/*
  matrix-fire.ino — Classic "doom fire" effect on the Arduino UNO Q LED matrix.

  What it does:
    Runs the classic PSX-Doom fire algorithm on the built-in 13x8 (13 cols x
    8 rows) blue LED matrix. A heat buffer is continuously seeded with random
    high heat along the BOTTOM row; every frame the heat propagates upward
    with random decay and slight horizontal jitter, and heat is mapped to
    LED brightness 0..7 (3-bit grayscale). A slow sine-driven intensity cycle
    (~25 s period) makes the fire breathe between calm embers and a raging
    blaze so it never looks static. Runs at ~30 fps with a non-blocking,
    millis()-paced loop.

  Buffer orientation:
    The 104-byte draw buffer is row-major, 8 rows of 13 columns, and row 0
    is the PHYSICAL TOP row. Heat is therefore seeded into row 7 (bottom)
    and pulled upward, so flames visibly rise from the bottom of the board.

  Wiring: no external hardware — uses only the built-in LED matrix.

  Compile:
    arduino-cli compile -b arduino:zephyr:unoq ./matrix-fire
  Upload:
    arduino-cli upload -b arduino:zephyr:unoq -p <board-ip> ./matrix-fire
*/

#include "Arduino_LED_Matrix.h"

// ---------- Matrix geometry ----------
constexpr int kWidth  = 13;   // columns
constexpr int kHeight = 8;    // rows; row 0 = physical top, row 7 = bottom
constexpr int kPixels = kWidth * kHeight;  // 104

// ---------- Fire tuning ----------
constexpr uint32_t kFramePeriodMs   = 33;     // ~30 fps
constexpr float    kIntensityCycleS = 25.0f;  // calm <-> raging period, seconds
constexpr uint8_t  kMaxHeat         = 255;    // internal heat scale
constexpr uint8_t  kBrightnessBits  = 3;      // 0..7 grayscale levels

// Intensity envelope: seed heat swings between these bounds over the cycle.
constexpr uint8_t kCalmSeedHeat   = 110;
constexpr uint8_t kRagingSeedHeat = 255;
// Fraction (0..255) of bottom-row cells re-seeded each frame, calm -> raging.
constexpr uint8_t kCalmSeedChance   = 90;
constexpr uint8_t kRagingSeedChance = 230;
// Random per-cell decay is 0..maxDecay; calmer fire decays faster (shorter flames).
constexpr uint8_t kCalmMaxDecay   = 60;
constexpr uint8_t kRagingMaxDecay = 28;

Arduino_LED_Matrix matrix;

static uint8_t heat[kPixels];      // row-major heat field, 0..255
static uint8_t frame[kPixels];     // brightness buffer handed to draw()

// ---------- Deterministic PRNG (32-bit LCG, Numerical Recipes constants) ----------
static uint32_t lcgState = 0xC0FFEE01u;

static inline uint32_t lcgNext() {
  lcgState = lcgState * 1664525u + 1013904223u;
  return lcgState;
}

// Uniform byte in [0, 255].
static inline uint8_t randByte() {
  return (uint8_t)(lcgNext() >> 24);
}

// Uniform value in [0, n-1] for small n.
static inline uint8_t randBelow(uint8_t n) {
  return (uint8_t)((lcgNext() >> 16) % n);
}

// ---------- Intensity cycle ----------
// Returns 0.0 (calm) .. 1.0 (raging) on a slow sine cycle.
static float intensityPhase(uint32_t nowMs) {
  const float t = (float)nowMs / 1000.0f;
  const float s = sinf(t * (2.0f * PI / kIntensityCycleS));
  return 0.5f + 0.5f * s;
}

static inline uint8_t lerpByte(uint8_t a, uint8_t b, float t) {
  return (uint8_t)((float)a + ((float)b - (float)a) * t);
}

// ---------- Fire simulation ----------

// Re-seed the bottom row with random high heat, scaled by current intensity.
static void seedBottomRow(uint8_t seedHeat, uint8_t seedChance) {
  uint8_t *bottom = &heat[(kHeight - 1) * kWidth];
  for (int x = 0; x < kWidth; x++) {
    if (randByte() < seedChance) {
      // Hot ember: seedHeat minus a little random flicker.
      uint8_t flicker = randBelow(48);
      bottom[x] = (seedHeat > flicker) ? (uint8_t)(seedHeat - flicker) : 0;
    } else {
      // Cool spot so the base of the fire dances.
      bottom[x] = randBelow(64);
    }
  }
}

// Propagate heat upward: each cell above the bottom row pulls from the row
// below with slight horizontal jitter and random decay.
static void propagateHeat(uint8_t maxDecay) {
  for (int y = 0; y < kHeight - 1; y++) {
    for (int x = 0; x < kWidth; x++) {
      // Horizontal jitter: -1, 0, or +1 source column (clamped at edges).
      int srcX = x + (int)randBelow(3) - 1;
      if (srcX < 0) srcX = 0;
      if (srcX >= kWidth) srcX = kWidth - 1;

      uint8_t src   = heat[(y + 1) * kWidth + srcX];
      uint8_t decay = randBelow((uint8_t)(maxDecay + 1));
      heat[y * kWidth + x] = (src > decay) ? (uint8_t)(src - decay) : 0;
    }
  }
}

// Map heat (0..255) to brightness (0..7) into the draw buffer.
static void renderFrame() {
  for (int i = 0; i < kPixels; i++) {
    frame[i] = (uint8_t)(heat[i] >> 5);  // 256 heat levels -> 8 brightness levels
  }
  matrix.draw(frame);
}

// ---------- Arduino entry points ----------

void setup() {
  Serial.begin(115200);
  matrix.begin();
  matrix.setGrayscaleBits(kBrightnessBits);
  memset(heat, 0, sizeof(heat));
  memset(frame, 0, sizeof(frame));
}

void loop() {
  static uint32_t lastFrameMs = 0;

  const uint32_t now = millis();
  if (now - lastFrameMs < kFramePeriodMs) {
    return;  // non-blocking pacing
  }
  lastFrameMs = now;

  const float phase = intensityPhase(now);
  const uint8_t seedHeat   = lerpByte(kCalmSeedHeat,   kRagingSeedHeat,   phase);
  const uint8_t seedChance = lerpByte(kCalmSeedChance, kRagingSeedChance, phase);
  const uint8_t maxDecay   = lerpByte(kCalmMaxDecay,   kRagingMaxDecay,   phase);

  seedBottomRow(seedHeat, seedChance);
  propagateHeat(maxDecay);
  renderFrame();
}
