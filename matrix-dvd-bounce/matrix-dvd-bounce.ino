/*
 * matrix-dvd-bounce.ino — Bouncing-DVD-logo screensaver on the Arduino UNO Q
 * LED matrix (13x8).
 *
 * What it does:
 *   A 3x2-pixel rounded "logo" sprite drifts diagonally using 8.8 fixed-point
 *   sub-pixel motion and bounces off the matrix edges. The matrix is blue-only,
 *   so instead of changing color on each bounce the logo cycles through a set
 *   of 3x2 brightness patterns (solid, bright-core, gradients, checker). The
 *   sprite leaves a very faint brightness-1 trail that slowly decays. Each
 *   bounce also re-rolls the speed of the reflected axis (within bounds) so
 *   the trajectory never locks into a short loop. When both axes bounce on
 *   the very same physics tick — the celebrated PERFECT CORNER HIT — a bright
 *   ripple expands across the whole matrix from that corner and a message with
 *   a running hit counter is printed to Serial.
 *
 * Wiring: no external hardware needed — uses only the built-in 13x8 matrix.
 *
 * Compile:
 *   arduino-cli compile -b arduino:zephyr:unoq ./matrix-dvd-bounce
 * Upload:
 *   arduino-cli upload -b arduino:zephyr:unoq -p <board-ip> ./matrix-dvd-bounce
 */

#include "Arduino_LED_Matrix.h"

// ---------------------------------------------------------------- matrix ---
static const int MAT_W = 13;
static const int MAT_H = 8;

Arduino_LED_Matrix matrix;
static uint8_t frame[MAT_W * MAT_H];   // composed frame, row-major 8x13
static uint8_t trail[MAT_W * MAT_H];   // decaying faint trail layer

// ---------------------------------------------------------- fixed point ----
// 8.8 fixed point: 1.0 == 256.
typedef int32_t fp_t;
static const fp_t FP_ONE = 256;
static inline fp_t fpFromInt(int v) { return (fp_t)v * FP_ONE; }
static inline int  fpToInt(fp_t v)  { return (int)(v >> 8); }

// -------------------------------------------------------------- tunables ---
static const uint32_t FRAME_MS       = 33;   // ~30 fps physics + render tick
static const uint32_t TRAIL_TICK_MS  = 200;  // trail decays 1 level per this
static const uint32_t RIPPLE_STEP_MS = 45;   // corner ripple expansion pace

static const int SPR_W = 3;                  // logo sprite size
static const int SPR_H = 2;
static const int MAX_X = MAT_W - SPR_W;      // top-left position bounds
static const int MAX_Y = MAT_H - SPR_H;

static const fp_t SPEED_MIN = (fp_t)(0.16f * FP_ONE);  // px per tick, per axis
static const fp_t SPEED_MAX = (fp_t)(0.34f * FP_ONE);

static const uint8_t TRAIL_BRIGHT = 1;       // very faint trail level
static const int     RIPPLE_MAX_R = MAT_W + MAT_H;  // covers whole matrix

// Logo brightness patterns, cycled on every bounce. Row-major 3x2, 0..7.
static const int NUM_PATTERNS = 4;
static const uint8_t PATTERNS[NUM_PATTERNS][SPR_W * SPR_H] = {
  { 7, 7, 7,  7, 7, 7 },   // solid
  { 3, 7, 3,  3, 7, 3 },   // bright core
  { 7, 5, 3,  3, 5, 7 },   // diagonal gradient
  { 7, 2, 7,  2, 7, 2 },   // checker
};

// ------------------------------------------------------------------- rng ---
// Small deterministic LCG (Numerical Recipes constants).
static uint32_t lcgState = 0xD1D5EED5u;
static uint32_t lcgNext() {
  lcgState = lcgState * 1664525u + 1013904223u;
  return lcgState;
}
// Uniform integer in [lo, hi] inclusive.
static int lcgRange(int lo, int hi) {
  return lo + (int)(lcgNext() % (uint32_t)(hi - lo + 1));
}
// Fresh per-axis speed magnitude within bounds.
static fp_t rollSpeed() { return (fp_t)lcgRange((int)SPEED_MIN, (int)SPEED_MAX); }

// ------------------------------------------------------------------ state --
enum Mode { DRIFTING, CORNER_RIPPLE };

static fp_t logoX, logoY;    // 8.8 position of the sprite's top-left pixel
static fp_t velX,  velY;     // 8.8 velocity, px per tick (sign = direction)
static int  patternIdx = 0;

static Mode     mode = DRIFTING;
static uint32_t lastFrameMs  = 0;
static uint32_t lastTrailMs  = 0;

static int      cornerHits    = 0;   // lifetime perfect-corner counter
static int      rippleCol, rippleRow;  // ripple origin (matrix corner)
static int      rippleRadius  = 0;
static uint32_t lastRippleMs  = 0;

// -------------------------------------------------------------- rendering --
static void setPixel(uint8_t *buf, int col, int row, uint8_t b) {
  if (col >= 0 && col < MAT_W && row >= 0 && row < MAT_H) {
    buf[row * MAT_W + col] = b;
  }
}

static void stampSprite(uint8_t *buf, int x, int y, const uint8_t *pat) {
  for (int r = 0; r < SPR_H; r++) {
    for (int c = 0; c < SPR_W; c++) {
      uint8_t b = pat[r * SPR_W + c];
      if (b) setPixel(buf, x + c, y + r, b);
    }
  }
}

// Mark the sprite's footprint into the faint trail layer.
static void stampTrail(int x, int y) {
  for (int r = 0; r < SPR_H; r++) {
    for (int c = 0; c < SPR_W; c++) {
      uint8_t &px = trail[(y + r) * MAT_W + (x + c)];
      if (px < TRAIL_BRIGHT) px = TRAIL_BRIGHT;
    }
  }
}

static void decayTrail() {
  for (int i = 0; i < MAT_W * MAT_H; i++) {
    if (trail[i] > 0) trail[i]--;
  }
}

static void renderDrift() {
  memcpy(frame, trail, sizeof(frame));
  stampSprite(frame, fpToInt(logoX + FP_ONE / 2), fpToInt(logoY + FP_ONE / 2),
              PATTERNS[patternIdx]);
  matrix.draw(frame);
}

// Expanding diamond (Manhattan-distance ring) from the hit corner: a bright
// leading edge with a dimmer wake, drawn over the normal scene.
static void renderRipple() {
  memcpy(frame, trail, sizeof(frame));
  stampSprite(frame, fpToInt(logoX + FP_ONE / 2), fpToInt(logoY + FP_ONE / 2),
              PATTERNS[patternIdx]);
  for (int r = 0; r < MAT_H; r++) {
    for (int c = 0; c < MAT_W; c++) {
      int d = (c > rippleCol ? c - rippleCol : rippleCol - c) +
              (r > rippleRow ? r - rippleRow : rippleRow - r);
      int back = rippleRadius - d;        // how far behind the leading edge
      uint8_t b = 0;
      if (back == 0)      b = 7;          // leading edge
      else if (back == 1) b = 4;          // wake
      else if (back == 2) b = 2;
      uint8_t &px = frame[r * MAT_W + c];
      if (b > px) px = b;
    }
  }
  matrix.draw(frame);
}

// ---------------------------------------------------------------- physics --
// Advance one tick; reflect off edges. Cycles the pattern and re-rolls the
// reflected axis speed on every bounce. Starts the corner celebration when
// both axes bounce on the same tick.
static void stepLogo(uint32_t now) {
  logoX += velX;
  logoY += velY;

  bool bouncedX = false;
  bool bouncedY = false;

  if (logoX < 0) {
    logoX = -logoX;
    velX  = rollSpeed();
    bouncedX = true;
  } else if (logoX > fpFromInt(MAX_X)) {
    logoX = 2 * fpFromInt(MAX_X) - logoX;
    velX  = -rollSpeed();
    bouncedX = true;
  }

  if (logoY < 0) {
    logoY = -logoY;
    velY  = rollSpeed();
    bouncedY = true;
  } else if (logoY > fpFromInt(MAX_Y)) {
    logoY = 2 * fpFromInt(MAX_Y) - logoY;
    velY  = -rollSpeed();
    bouncedY = true;
  }

  if (bouncedX || bouncedY) {
    patternIdx = (patternIdx + 1) % NUM_PATTERNS;
  }

  if (bouncedX && bouncedY) {           // PERFECT CORNER HIT
    cornerHits++;
    rippleCol = (velX > 0) ? 0 : MAT_W - 1;  // corner we just left
    rippleRow = (velY > 0) ? 0 : MAT_H - 1;
    rippleRadius = 0;
    lastRippleMs = now;
    mode = CORNER_RIPPLE;
    Serial.print("PERFECT CORNER HIT #");
    Serial.print(cornerHits);
    Serial.print("  (corner col=");
    Serial.print(rippleCol);
    Serial.print(", row=");
    Serial.print(rippleRow);
    Serial.println(")");
  }
}

// ------------------------------------------------------------------ setup --
void setup() {
  Serial.begin(115200);
  matrix.begin();
  matrix.setGrayscaleBits(3);

  logoX = fpFromInt(lcgRange(1, MAX_X - 1));
  logoY = fpFromInt(lcgRange(1, MAX_Y - 1));
  velX  = (lcgNext() & 1u) ? rollSpeed() : -rollSpeed();
  velY  = (lcgNext() & 1u) ? rollSpeed() : -rollSpeed();

  uint32_t now = millis();
  lastFrameMs = now;
  lastTrailMs = now;
  renderDrift();
}

// ------------------------------------------------------------------- loop --
void loop() {
  uint32_t now = millis();

  // Faint trail decays on its own slow clock (rollover-safe subtraction).
  if ((uint32_t)(now - lastTrailMs) >= TRAIL_TICK_MS) {
    lastTrailMs += TRAIL_TICK_MS;
    decayTrail();
  }

  switch (mode) {
    case DRIFTING:
      if ((uint32_t)(now - lastFrameMs) >= FRAME_MS) {
        lastFrameMs = now;
        // Trail is stamped at the position being vacated, before the move.
        stampTrail(fpToInt(logoX + FP_ONE / 2), fpToInt(logoY + FP_ONE / 2));
        stepLogo(now);
        if (mode == DRIFTING) renderDrift();
        else                  renderRipple();  // corner hit this tick
      }
      break;

    case CORNER_RIPPLE:
      if ((uint32_t)(now - lastRippleMs) >= RIPPLE_STEP_MS) {
        lastRippleMs = now;
        rippleRadius++;
        if (rippleRadius > RIPPLE_MAX_R) {
          mode = DRIFTING;
          lastFrameMs = now;
          renderDrift();
        } else {
          renderRipple();
        }
      }
      break;
  }
}
