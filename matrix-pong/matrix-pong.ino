/*
 * matrix-pong.ino — Self-playing Pong on the Arduino UNO Q LED matrix (13x8).
 *
 * What it does:
 *   Two AI-driven 3-pixel paddles (column 0 and column 12) rally a ball whose
 *   position/velocity use 8.8 fixed-point math for smooth sub-pixel motion.
 *   The bounce angle depends on where the ball strikes the paddle. Each AI has
 *   a limited tracking speed and a small random aim error, so points do get
 *   scored. The ball leaves a short dim trail. When a point is scored the
 *   scorer's half of the matrix flashes briefly, then the ball is re-served
 *   toward the loser. Every few total points the score is shown for ~1 s as
 *   two dot columns (tally style: bright dot = 5 points, dim dot = 1 point).
 *
 * Wiring: no external hardware — uses only the built-in 13x8 LED matrix.
 *
 * Compile:
 *   arduino-cli compile -b arduino:zephyr:unoq ./matrix-pong
 * Upload:
 *   arduino-cli upload -b arduino:zephyr:unoq -p <board-ip> ./matrix-pong
 */

#include "Arduino_LED_Matrix.h"

// Types are defined before any function so the IDE's auto-generated
// prototypes (inserted ahead of the first function) can see them.
static const int PADDLE_LEN = 3;

struct Paddle {
  int      topRow;      // top pixel row, 0 .. MAT_H - PADDLE_LEN
  int      aimError;    // pixels of deliberate mis-aim, re-rolled per rally
  uint32_t lastMoveMs;  // paces the limited tracking speed
};

// ---------------------------------------------------------------- matrix ---
static const int MAT_W = 13;
static const int MAT_H = 8;
static const uint8_t MAX_BRIGHT = 7;   // grayscale range 0..7 (3 bits)

Arduino_LED_Matrix matrix;
static uint8_t frame[MAT_W * MAT_H];   // composed frame, row-major 8x13
static uint8_t trail[MAT_W * MAT_H];   // decaying ball trail layer

// ---------------------------------------------------------- fixed point ----
// 8.8 fixed point: 1.0 == 256.
typedef int32_t fp_t;
static const fp_t FP_ONE = 256;
static inline fp_t fpFromInt(int v) { return (fp_t)v * FP_ONE; }
static inline int  fpToInt(fp_t v)  { return (int)(v >> 8); }

// -------------------------------------------------------------- tunables ---
static const uint32_t PHYSICS_MS     = 33;    // ball physics + render tick
static const uint32_t AI_MOVE_MS     = 110;   // min ms between paddle steps
static const uint32_t FLASH_MS       = 350;   // scorer-side flash duration
static const uint32_t SERVE_PAUSE_MS = 500;   // dead time before re-serve
static const uint32_t SCORE_SHOW_MS  = 1000;  // score display duration
static const int      SCORE_SHOW_EVERY = 3;   // show score every N total points

static const int LEFT_COL   = 0;
static const int RIGHT_COL  = MAT_W - 1;
// Bounce planes: the columns adjacent to each paddle.
static const fp_t LEFT_PLANE  = fpFromInt(LEFT_COL + 1);
static const fp_t RIGHT_PLANE = fpFromInt(RIGHT_COL - 1);

static const fp_t BALL_SPEED_X   = (fp_t)(0.36f * FP_ONE);  // px per tick
static const fp_t SPEED_UP       = (fp_t)(0.012f * FP_ONE); // per paddle hit
static const fp_t MAX_SPEED_X    = (fp_t)(0.60f * FP_ONE);
static const fp_t MAX_VY         = (fp_t)(0.45f * FP_ONE);
static const fp_t ANGLE_PER_OFFSET = (fp_t)(0.22f * FP_ONE); // vy per pixel off paddle center

static const uint8_t BRIGHT_BALL   = 7;
static const uint8_t BRIGHT_PADDLE = 5;
static const uint8_t BRIGHT_FLASH  = 3;
static const uint8_t TRAIL_DECAY   = 2;   // brightness lost per tick -> short trail

// ------------------------------------------------------------------- rng ---
// Small deterministic LCG (Numerical Recipes constants).
static uint32_t lcgState = 0xC0FFEE42u;
static uint32_t lcgNext() {
  lcgState = lcgState * 1664525u + 1013904223u;
  return lcgState;
}
// Uniform integer in [lo, hi] inclusive.
static int lcgRange(int lo, int hi) {
  return lo + (int)(lcgNext() % (uint32_t)(hi - lo + 1));
}

// ------------------------------------------------------------ game state ---
enum GameState { PLAYING, SCORE_FLASH, SCORE_SHOW, SERVE_PAUSE };

static Paddle leftPad, rightPad;

static fp_t ballX, ballY;   // 8.8 position in pixels
static fp_t ballVX, ballVY; // 8.8 velocity in pixels per physics tick

static int scoreLeft  = 0;
static int scoreRight = 0;

static GameState state = SERVE_PAUSE;
static uint32_t  stateSince = 0;
static bool      leftScoredLast = false;  // who won the last point
static bool      pendingScoreShow = false;
static uint32_t  lastPhysicsMs = 0;

// -------------------------------------------------------------- helpers ----
static inline int clampInt(int v, int lo, int hi) {
  return v < lo ? lo : (v > hi ? hi : v);
}

static void setPixel(uint8_t *buf, int col, int row, uint8_t b) {
  if (col >= 0 && col < MAT_W && row >= 0 && row < MAT_H) {
    buf[row * MAT_W + col] = b;
  }
}

static int paddleCenter(const Paddle &p) { return p.topRow + PADDLE_LEN / 2; }

static bool paddleCovers(const Paddle &p, int row) {
  return row >= p.topRow && row < p.topRow + PADDLE_LEN;
}

static void rollAimError(Paddle &p) {
  p.aimError = lcgRange(-2, 2);  // up to 2 pixels of mis-aim
}

// ------------------------------------------------------------------ serve --
static void serveBall(bool towardLeft) {
  ballX = fpFromInt(MAT_W / 2);
  ballY = fpFromInt(lcgRange(2, MAT_H - 3));
  ballVX = towardLeft ? -BALL_SPEED_X : BALL_SPEED_X;
  ballVY = (fp_t)lcgRange(-(int)(MAX_VY / 2), (int)(MAX_VY / 2));
  rollAimError(leftPad);
  rollAimError(rightPad);
}

// --------------------------------------------------------------------- AI --
// Limited tracking speed (one pixel per AI_MOVE_MS at most) plus the rolled
// aim error. A paddle only chases the ball while it approaches; otherwise it
// drifts back toward center.
static void updatePaddle(Paddle &p, bool isLeft, uint32_t now) {
  if (now - p.lastMoveMs < AI_MOVE_MS) return;

  bool ballApproaching = isLeft ? (ballVX < 0) : (ballVX > 0);
  int target;
  if (ballApproaching) {
    target = fpToInt(ballY) + p.aimError;
  } else {
    target = MAT_H / 2;  // drift home between rallies
  }
  target = clampInt(target, 0, MAT_H - 1);

  int center = paddleCenter(p);
  if (target > center)      p.topRow++;
  else if (target < center) p.topRow--;
  p.topRow = clampInt(p.topRow, 0, MAT_H - PADDLE_LEN);
  p.lastMoveMs = now;
}

// ---------------------------------------------------------------- physics --
// Returns +1 if left scores, -1 if right scores, 0 otherwise.
static int stepBall() {
  ballX += ballVX;
  ballY += ballVY;

  // Bounce off top/bottom walls.
  if (ballY < 0) {
    ballY = -ballY;
    ballVY = -ballVY;
  } else if (ballY > fpFromInt(MAT_H - 1)) {
    ballY = 2 * fpFromInt(MAT_H - 1) - ballY;
    ballVY = -ballVY;
  }

  int row = fpToInt(ballY + FP_ONE / 2);
  row = clampInt(row, 0, MAT_H - 1);

  // Left paddle face.
  if (ballVX < 0 && ballX <= LEFT_PLANE) {
    if (paddleCovers(leftPad, row)) {
      ballX = 2 * LEFT_PLANE - ballX;
      bounceOffPaddle(leftPad, row);
    } else if (ballX < fpFromInt(LEFT_COL)) {
      return -1;  // right player scores
    }
  }
  // Right paddle face.
  if (ballVX > 0 && ballX >= RIGHT_PLANE) {
    if (paddleCovers(rightPad, row)) {
      ballX = 2 * RIGHT_PLANE - ballX;
      bounceOffPaddle(rightPad, row);
    } else if (ballX > fpFromInt(RIGHT_COL)) {
      return +1;  // left player scores
    }
  }
  return 0;
}

// Reverse X, steer Y by strike offset from paddle center, speed up slightly.
static void bounceOffPaddle(Paddle &p, int strikeRow) {
  int offset = strikeRow - paddleCenter(p);      // -1, 0, +1 (3-px paddle)
  ballVY += (fp_t)offset * ANGLE_PER_OFFSET;
  ballVY = ballVY > MAX_VY ? MAX_VY : (ballVY < -MAX_VY ? -MAX_VY : ballVY);

  fp_t speed = (ballVX < 0 ? -ballVX : ballVX) + SPEED_UP;
  if (speed > MAX_SPEED_X) speed = MAX_SPEED_X;
  ballVX = (ballVX < 0) ? speed : -speed;        // reflect

  rollAimError(p);  // fresh mis-aim for the next return
}

// -------------------------------------------------------------- rendering --
static void decayTrail() {
  for (int i = 0; i < MAT_W * MAT_H; i++) {
    trail[i] = (trail[i] > TRAIL_DECAY) ? (uint8_t)(trail[i] - TRAIL_DECAY) : 0;
  }
}

static void renderPlayfield(bool drawBall) {
  memcpy(frame, trail, sizeof(frame));
  for (int i = 0; i < PADDLE_LEN; i++) {
    setPixel(frame, LEFT_COL,  leftPad.topRow + i,  BRIGHT_PADDLE);
    setPixel(frame, RIGHT_COL, rightPad.topRow + i, BRIGHT_PADDLE);
  }
  if (drawBall) {
    setPixel(frame, fpToInt(ballX + FP_ONE / 2), fpToInt(ballY + FP_ONE / 2),
             BRIGHT_BALL);
  }
  matrix.draw(frame);
}

// Overlay a solid flash on the scorer's half of the matrix.
static void renderFlash(bool leftSide) {
  renderCommonForFlash();
  int c0 = leftSide ? 0 : MAT_W / 2 + 1;
  int c1 = leftSide ? MAT_W / 2 - 1 : MAT_W - 1;
  for (int r = 0; r < MAT_H; r++) {
    for (int c = c0; c <= c1; c++) {
      uint8_t &px = frame[r * MAT_W + c];
      if (px < BRIGHT_FLASH) px = BRIGHT_FLASH;
    }
  }
  matrix.draw(frame);
}

static void renderCommonForFlash() {
  memcpy(frame, trail, sizeof(frame));
  for (int i = 0; i < PADDLE_LEN; i++) {
    setPixel(frame, LEFT_COL,  leftPad.topRow + i,  BRIGHT_PADDLE);
    setPixel(frame, RIGHT_COL, rightPad.topRow + i, BRIGHT_PADDLE);
  }
}

// Score display: two dot columns growing up from the bottom row.
// Tally coding: each bright dot (7) = 5 points, each dim dot (2) = 1 point.
static void drawScoreColumn(int col, int score) {
  int fives = score / 5;
  int ones  = score % 5;
  int row = MAT_H - 1;
  for (int i = 0; i < fives && row >= 0; i++, row--) setPixel(frame, col, row, 7);
  for (int i = 0; i < ones  && row >= 0; i++, row--) setPixel(frame, col, row, 2);
}

static void renderScore() {
  memset(frame, 0, sizeof(frame));
  drawScoreColumn(4, scoreLeft);   // left score, left-of-center column
  drawScoreColumn(8, scoreRight);  // right score, right-of-center column
  matrix.draw(frame);
}

// ------------------------------------------------------------------ setup --
void setup() {
  Serial.begin(115200);
  matrix.begin();
  matrix.setGrayscaleBits(3);

  leftPad.topRow  = (MAT_H - PADDLE_LEN) / 2;
  rightPad.topRow = (MAT_H - PADDLE_LEN) / 2;
  leftPad.lastMoveMs = rightPad.lastMoveMs = 0;
  rollAimError(leftPad);
  rollAimError(rightPad);

  leftScoredLast = (lcgNext() & 1u) != 0;  // random first serve direction
  serveBall(!leftScoredLast);
  state = SERVE_PAUSE;
  stateSince = millis();
}

// ------------------------------------------------------------------- loop --
void loop() {
  uint32_t now = millis();

  switch (state) {
    case PLAYING: {
      updatePaddle(leftPad,  true,  now);
      updatePaddle(rightPad, false, now);

      if (now - lastPhysicsMs >= PHYSICS_MS) {
        lastPhysicsMs = now;
        decayTrail();

        int result = stepBall();
        if (result != 0) {
          leftScoredLast = (result > 0);
          if (leftScoredLast) scoreLeft++; else scoreRight++;
          Serial.print("Score  L ");
          Serial.print(scoreLeft);
          Serial.print(" : ");
          Serial.print(scoreRight);
          Serial.println(" R");
          pendingScoreShow =
              ((scoreLeft + scoreRight) % SCORE_SHOW_EVERY) == 0;
          state = SCORE_FLASH;
          stateSince = now;
          renderFlash(leftScoredLast);
          break;
        }

        // Stamp the ball into the trail layer so it fades behind the ball.
        setPixel(trail, fpToInt(ballX + FP_ONE / 2),
                 fpToInt(ballY + FP_ONE / 2), BRIGHT_BALL);
        renderPlayfield(true);
      }
      break;
    }

    case SCORE_FLASH:
      if (now - stateSince >= FLASH_MS) {
        if (pendingScoreShow) {
          pendingScoreShow = false;
          renderScore();
          state = SCORE_SHOW;
        } else {
          memset(trail, 0, sizeof(trail));
          renderPlayfield(false);
          state = SERVE_PAUSE;
        }
        stateSince = now;
      }
      break;

    case SCORE_SHOW:
      if (now - stateSince >= SCORE_SHOW_MS) {
        memset(trail, 0, sizeof(trail));
        renderPlayfield(false);
        state = SERVE_PAUSE;
        stateSince = now;
      }
      break;

    case SERVE_PAUSE:
      if (now - stateSince >= SERVE_PAUSE_MS) {
        serveBall(!leftScoredLast);  // serve toward the loser
        lastPhysicsMs = now;
        state = PLAYING;
      }
      break;
  }
}
