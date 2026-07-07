/*
 * matrix-breakout.ino — Self-playing Breakout on the Arduino UNO Q LED matrix
 * (13 columns x 8 rows).
 *
 * What it does:
 *   Three rows of bricks (rows 0/1/2 at brightness 4/3/2) sit at the top.
 *   A 3-pixel AI paddle on the bottom row returns a ball that uses 8.8
 *   fixed-point sub-pixel physics. The AI predicts the ball's landing column
 *   (simulating wall bounces), but tracks with limited speed and a small
 *   random aim error, so it occasionally misses. Brick hits remove the brick
 *   with a brief bright flash; the paddle's return angle depends on where the
 *   ball strikes it. A miss dims the screen briefly and costs a life (3 lives
 *   shown as dim pixels in the bottom-left corner). Clearing all bricks
 *   flashes the screen, rebuilds the wall and speeds the ball up slightly.
 *   On the last miss the screen fades out and the game resets. ~30 fps.
 *
 * Wiring: no external hardware needed — uses only the built-in LED matrix.
 *
 * Compile:
 *   arduino-cli compile -b arduino:zephyr:unoq ./matrix-breakout
 * Upload:
 *   arduino-cli upload -b arduino:zephyr:unoq -p <board-ip> ./matrix-breakout
 */

#include "Arduino_LED_Matrix.h"

// ---------------------------------------------------------------- matrix ---
static const int MAT_W = 13;
static const int MAT_H = 8;

Arduino_LED_Matrix matrix;
static uint8_t frame[MAT_W * MAT_H];   // composed frame, row-major 8x13
static uint8_t flashBuf[MAT_W * MAT_H]; // decaying brick-hit flash layer

// ---------------------------------------------------------- fixed point ----
// 8.8 fixed point: 1.0 == 256.
typedef int32_t fp_t;
static const fp_t FP_ONE = 256;
static inline fp_t fpFromInt(int v) { return (fp_t)v * FP_ONE; }
static inline int  fpToInt(fp_t v)  { return (int)(v >> 8); }
static inline int  fpRound(fp_t v)  { return fpToInt(v + FP_ONE / 2); }

// -------------------------------------------------------------- tunables ---
static const uint32_t PHYSICS_MS     = 33;   // physics + render tick (~30 fps)
static const uint32_t AI_MOVE_MS     = 90;   // min ms between paddle steps
static const uint32_t SERVE_PAUSE_MS = 600;  // dead time before a serve
static const uint32_t MISS_DIM_MS    = 450;  // screen-dim time after a miss
static const uint32_t LEVEL_FLASH_MS = 400;  // full-screen flash on clear
static const uint32_t FADE_STEP_MS   = 90;   // per-step delay of game-over fade

static const int BRICK_ROWS   = 3;
static const int PADDLE_LEN   = 3;
static const int PADDLE_ROW   = MAT_H - 1;             // bottom row
static const fp_t PADDLE_PLANE = fpFromInt(PADDLE_ROW - 1); // bounce plane
static const int START_LIVES  = 3;

static const fp_t BASE_SPEED_Y     = (fp_t)(0.30f * FP_ONE); // px per tick
static const fp_t LEVEL_SPEED_UP   = (fp_t)(0.05f * FP_ONE); // per level
static const fp_t MAX_SPEED_Y      = (fp_t)(0.55f * FP_ONE);
static const fp_t MAX_VX           = (fp_t)(0.45f * FP_ONE);
static const fp_t MIN_VX           = (fp_t)(0.06f * FP_ONE); // avoid verticals
static const fp_t ANGLE_PER_OFFSET = (fp_t)(0.20f * FP_ONE); // vx per px off center

static const uint8_t BRIGHT_BALL    = 7;
static const uint8_t BRIGHT_PADDLE  = 6;
static const uint8_t BRIGHT_HIT     = 7;  // brick-hit flash peak
static const uint8_t BRIGHT_LIFE    = 1;  // dim corner life pips
static const uint8_t BRIGHT_LVFLASH = 5;  // level-clear full-screen flash
static const uint8_t FLASH_DECAY    = 2;  // flash brightness lost per tick
// Brick brightness by row: row 0 = 4, row 1 = 3, row 2 = 2.
static const uint8_t BRICK_BRIGHT[BRICK_ROWS] = { 4, 3, 2 };

static const int AIM_ERR_MAX   = 2;    // paddle mis-aim, pixels
static const int PREDICT_TICKS = 400;  // cap on landing-point simulation

// ------------------------------------------------------------------- rng ---
// Small deterministic LCG (Numerical Recipes constants).
static uint32_t lcgState = 0xB16B00B5u;
static uint32_t lcgNext() {
  lcgState = lcgState * 1664525u + 1013904223u;
  return lcgState;
}
// Uniform integer in [lo, hi] inclusive.
static int lcgRange(int lo, int hi) {
  return lo + (int)(lcgNext() % (uint32_t)(hi - lo + 1));
}

// ------------------------------------------------------------ game state ---
enum GameState { SERVE_PAUSE, PLAYING, MISS_DIM, LEVEL_FLASH, GAME_OVER_FADE };

static uint8_t bricks[BRICK_ROWS][MAT_W];  // nonzero = alive
static int     brickCount = 0;

static fp_t ballX, ballY;    // 8.8 position in pixels
static fp_t ballVX, ballVY;  // 8.8 velocity in pixels per physics tick

static int      paddleLeft;   // leftmost paddle column, 0 .. MAT_W-PADDLE_LEN
static int      aimError;     // deliberate mis-aim, re-rolled per return
static uint32_t lastAiMoveMs = 0;

static int lives = START_LIVES;
static int level = 0;   // 0-based; raises ball speed

static GameState state = SERVE_PAUSE;
static uint32_t  stateSince = 0;
static uint32_t  lastPhysicsMs = 0;
static int       fadeStep = 0;  // game-over fade progress

// -------------------------------------------------------------- helpers ----
static inline int clampInt(int v, int lo, int hi) {
  return v < lo ? lo : (v > hi ? hi : v);
}

static void setPixel(uint8_t *buf, int col, int row, uint8_t b) {
  if (col >= 0 && col < MAT_W && row >= 0 && row < MAT_H) {
    buf[row * MAT_W + col] = b;
  }
}

static int paddleCenter() { return paddleLeft + PADDLE_LEN / 2; }

static bool paddleCovers(int col) {
  return col >= paddleLeft && col < paddleLeft + PADDLE_LEN;
}

static void rollAimError() {
  aimError = lcgRange(-AIM_ERR_MAX, AIM_ERR_MAX);
}

static fp_t levelSpeedY() {
  fp_t s = BASE_SPEED_Y + (fp_t)level * LEVEL_SPEED_UP;
  return s > MAX_SPEED_Y ? MAX_SPEED_Y : s;
}

static void buildBricks() {
  for (int r = 0; r < BRICK_ROWS; r++) {
    for (int c = 0; c < MAT_W; c++) bricks[r][c] = 1;
  }
  brickCount = BRICK_ROWS * MAT_W;
}

// ------------------------------------------------------------------ serve --
static void serveBall() {
  ballX  = fpFromInt(lcgRange(3, MAT_W - 4));
  ballY  = fpFromInt(MAT_H - 3);
  ballVY = -levelSpeedY();  // upward
  ballVX = (fp_t)lcgRange(-(int)(MAX_VX * 3 / 4), (int)(MAX_VX * 3 / 4));
  if (ballVX >= 0 && ballVX < MIN_VX)  ballVX = MIN_VX;
  if (ballVX < 0  && ballVX > -MIN_VX) ballVX = -MIN_VX;
  rollAimError();
}

// --------------------------------------------------------------------- AI --
// Predict the column where the ball will cross the paddle plane, simulating
// wall bounces but ignoring bricks (an imperfect but plausible player model).
static int predictLandingCol() {
  if (ballVY <= 0) return MAT_W / 2;  // heading up: drift home
  fp_t x = ballX, y = ballY, vx = ballVX;
  for (int i = 0; i < PREDICT_TICKS && y < PADDLE_PLANE; i++) {
    x += vx;
    y += ballVY;
    if (x < 0) { x = -x; vx = -vx; }
    else if (x > fpFromInt(MAT_W - 1)) {
      x = 2 * fpFromInt(MAT_W - 1) - x;
      vx = -vx;
    }
  }
  return clampInt(fpRound(x), 0, MAT_W - 1);
}

// Limited tracking speed (one pixel per AI_MOVE_MS at most) plus mis-aim.
static void updatePaddle(uint32_t now) {
  if (now - lastAiMoveMs < AI_MOVE_MS) return;
  int target = clampInt(predictLandingCol() + aimError, 0, MAT_W - 1);
  int center = paddleCenter();
  if (target > center)      paddleLeft++;
  else if (target < center) paddleLeft--;
  paddleLeft = clampInt(paddleLeft, 0, MAT_W - PADDLE_LEN);
  lastAiMoveMs = now;
}

// ---------------------------------------------------------------- physics --
// Reverse Y, steer X by strike offset from paddle center.
static void bounceOffPaddle() {
  fp_t offset = ballX - fpFromInt(paddleCenter());  // sub-pixel strike offset
  ballVX += (fp_t)((offset * ANGLE_PER_OFFSET) >> 8);
  ballVX = ballVX > MAX_VX ? MAX_VX : (ballVX < -MAX_VX ? -MAX_VX : ballVX);
  if (ballVX >= 0 && ballVX < MIN_VX)  ballVX = MIN_VX;
  if (ballVX < 0  && ballVX > -MIN_VX) ballVX = -MIN_VX;
  ballY  = 2 * PADDLE_PLANE - ballY;
  ballVY = -ballVY;
  rollAimError();  // fresh mis-aim for the next return
}

// Remove a brick, leave a bright flash behind, and reflect the ball. The
// bounce axis is chosen from which cell edge the ball crossed this tick.
static void hitBrick(int col, int row, int prevCol, int prevRow) {
  bricks[row][col] = 0;
  brickCount--;
  setPixel(flashBuf, col, row, BRIGHT_HIT);
  if (prevRow == row && prevCol != col) {
    ballVX = -ballVX;  // entered through a side face
  } else {
    ballVY = -ballVY;  // entered through top/bottom face
  }
}

// Returns true when the ball is lost past the bottom edge.
static bool stepBall() {
  int prevCol = clampInt(fpRound(ballX), 0, MAT_W - 1);
  int prevRow = clampInt(fpRound(ballY), 0, MAT_H - 1);

  ballX += ballVX;
  ballY += ballVY;

  // Side walls.
  if (ballX < 0) {
    ballX = -ballX;
    ballVX = -ballVX;
  } else if (ballX > fpFromInt(MAT_W - 1)) {
    ballX = 2 * fpFromInt(MAT_W - 1) - ballX;
    ballVX = -ballVX;
  }
  // Ceiling.
  if (ballY < 0) {
    ballY = -ballY;
    ballVY = -ballVY;
  }

  // Bricks: test the cell the ball's center now occupies.
  int col = clampInt(fpRound(ballX), 0, MAT_W - 1);
  int row = fpRound(ballY);
  if (row >= 0 && row < BRICK_ROWS && bricks[row][col]) {
    hitBrick(col, row, prevCol, prevRow);
  }

  // Paddle.
  if (ballVY > 0 && ballY >= PADDLE_PLANE && ballY <= fpFromInt(PADDLE_ROW) &&
      paddleCovers(clampInt(fpRound(ballX), 0, MAT_W - 1))) {
    bounceOffPaddle();
  }

  return ballY > fpFromInt(MAT_H - 1) + FP_ONE / 2;  // fell past the bottom
}

// -------------------------------------------------------------- rendering --
static void decayFlash() {
  for (int i = 0; i < MAT_W * MAT_H; i++) {
    flashBuf[i] = (flashBuf[i] > FLASH_DECAY)
                      ? (uint8_t)(flashBuf[i] - FLASH_DECAY) : 0;
  }
}

// Compose bricks, life pips, flash layer, paddle and (optionally) the ball.
static void composeFrame(bool drawBall) {
  memset(frame, 0, sizeof(frame));
  for (int r = 0; r < BRICK_ROWS; r++) {
    for (int c = 0; c < MAT_W; c++) {
      if (bricks[r][c]) frame[r * MAT_W + c] = BRICK_BRIGHT[r];
    }
  }
  for (int i = 0; i < lives; i++) {  // dim pips in the bottom-left corner
    setPixel(frame, i, PADDLE_ROW, BRIGHT_LIFE);
  }
  for (int i = 0; i < MAT_W * MAT_H; i++) {
    if (flashBuf[i] > frame[i]) frame[i] = flashBuf[i];
  }
  for (int i = 0; i < PADDLE_LEN; i++) {
    setPixel(frame, paddleLeft + i, PADDLE_ROW, BRIGHT_PADDLE);
  }
  if (drawBall) {
    setPixel(frame, fpRound(ballX), clampInt(fpRound(ballY), 0, MAT_H - 1),
             BRIGHT_BALL);
  }
}

// Render the playfield with brightness cut by `shift` (0 = full, 1 = half...).
static void renderDimmed(int shift, bool drawBall) {
  composeFrame(drawBall);
  if (shift > 0) {
    for (int i = 0; i < MAT_W * MAT_H; i++) frame[i] >>= shift;
  }
  matrix.draw(frame);
}

static void renderLevelFlash() {
  composeFrame(false);
  for (int i = 0; i < MAT_W * MAT_H; i++) {
    if (frame[i] < BRIGHT_LVFLASH) frame[i] = BRIGHT_LVFLASH;
  }
  matrix.draw(frame);
}

// ------------------------------------------------------------------ setup --
void setup() {
  Serial.begin(115200);
  matrix.begin();
  matrix.setGrayscaleBits(3);

  buildBricks();
  paddleLeft = (MAT_W - PADDLE_LEN) / 2;
  lives = START_LIVES;
  level = 0;
  serveBall();
  state = SERVE_PAUSE;
  stateSince = millis();
}

// ------------------------------------------------------------------- loop --
void loop() {
  uint32_t now = millis();

  switch (state) {
    case SERVE_PAUSE:
      if (now - stateSince >= SERVE_PAUSE_MS) {
        serveBall();
        lastPhysicsMs = now;
        state = PLAYING;
      }
      break;

    case PLAYING: {
      updatePaddle(now);
      if (now - lastPhysicsMs < PHYSICS_MS) break;
      lastPhysicsMs = now;
      decayFlash();

      bool lost = stepBall();

      if (brickCount == 0) {  // level cleared
        level++;
        Serial.print("Level cleared -> ");
        Serial.println(level + 1);
        renderLevelFlash();
        state = LEVEL_FLASH;
        stateSince = now;
        break;
      }
      if (lost) {
        lives--;
        Serial.print("Miss. Lives left: ");
        Serial.println(lives);
        renderDimmed(2, false);  // brief screen dim
        state = MISS_DIM;
        stateSince = now;
        break;
      }
      renderDimmed(0, true);
      break;
    }

    case MISS_DIM:
      if (now - stateSince >= MISS_DIM_MS) {
        if (lives > 0) {
          memset(flashBuf, 0, sizeof(flashBuf));
          renderDimmed(0, false);
          state = SERVE_PAUSE;
        } else {
          Serial.println("Game over.");
          fadeStep = 0;
          state = GAME_OVER_FADE;
        }
        stateSince = now;
      }
      break;

    case LEVEL_FLASH:
      if (now - stateSince >= LEVEL_FLASH_MS) {
        buildBricks();  // rebuild the wall, ball speeds up via levelSpeedY()
        memset(flashBuf, 0, sizeof(flashBuf));
        renderDimmed(0, false);
        state = SERVE_PAUSE;
        stateSince = now;
      }
      break;

    case GAME_OVER_FADE:
      if (now - stateSince >= FADE_STEP_MS) {
        fadeStep++;
        renderDimmed(fadeStep, false);
        stateSince = now;
        if (fadeStep >= 4) {  // fully dark at 3-bit depth: reset everything
          buildBricks();
          memset(flashBuf, 0, sizeof(flashBuf));
          lives = START_LIVES;
          level = 0;
          paddleLeft = (MAT_W - PADDLE_LEN) / 2;
          state = SERVE_PAUSE;
        }
      }
      break;
  }
}
