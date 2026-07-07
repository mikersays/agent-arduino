/*
  matrix-wave-pool.ino — 2D water ripple simulation on the Arduino UNO Q LED matrix.

  What it does:
    Runs a real discrete 2D wave-equation simulation (the classic two-buffer
    "water ripple" integrator, not a distance-function fake) on the built-in
    13x8 blue LED matrix. Two int16_t fixed-point height fields (current and
    previous) are stepped every frame:

        newH(x,y) = (left + right + up + down) / 2 - prevH(x,y)

    then damped by ~0.94 so ripples fade out. Edge cells reflect by clamping
    neighbor reads, so waves bounce off the walls of the "pool". Random
    raindrops strike every 0.5-2 s, injecting a downward impulse; occasionally
    a heavier "stone" lands instead, with a bigger impulse and a small plus-
    shaped splash pattern. Height maps to brightness 0..7 centered on calm
    water at level 1, so ripple crests glow bright and troughs go dark.
    Runs at ~30 fps with a non-blocking, millis()-paced loop and prints a
    running drop counter to Serial after each strike.

  Buffer orientation:
    The 104-byte draw buffer is row-major, 8 rows of 13 columns, and row 0
    is the PHYSICAL TOP row.

  Wiring: no external hardware needed — uses only the built-in LED matrix.

  Compile:
    arduino-cli compile -b arduino:zephyr:unoq ./matrix-wave-pool
  Upload:
    arduino-cli upload -b arduino:zephyr:unoq -p <board-ip> ./matrix-wave-pool
*/

#include "Arduino_LED_Matrix.h"

// ---------- Matrix geometry ----------
constexpr int kWidth  = 13;   // columns
constexpr int kHeight = 8;    // rows; row 0 = physical top
constexpr int kPixels = kWidth * kHeight;  // 104

// ---------- Timing ----------
constexpr uint32_t kFramePeriodMs = 33;    // ~30 fps
constexpr uint32_t kMinDropGapMs  = 500;   // next raindrop in 0.5 s ...
constexpr uint32_t kMaxDropGapMs  = 2000;  // ... to 2.0 s

// ---------- Wave tuning (int16_t fixed point height units) ----------
// Damping: newH -= newH/16, i.e. multiply by 15/16 = 0.9375 (~0.94).
constexpr int      kDampShift     = 4;
constexpr int16_t  kDropImpulse   = -7000;   // raindrop pushes water DOWN
constexpr int16_t  kStoneImpulse  = -14000;  // heavier stone, bigger hole
constexpr int16_t  kSplashLift    = 3500;    // stone splash ring lifts neighbors
constexpr uint8_t  kStoneChance   = 42;      // /256 odds a drop is a stone (~1 in 6)
constexpr int16_t  kHeightClamp   = 20000;   // keep sums safely inside int16_t

// ---------- Brightness mapping ----------
constexpr uint8_t  kBrightnessBits = 3;      // 0..7 grayscale levels
constexpr uint8_t  kCalmLevel      = 1;      // brightness of flat water
constexpr int16_t  kBrightDivisor  = 900;    // height units per brightness step
constexpr uint8_t  kMaxBrightness  = 7;

Arduino_LED_Matrix matrix;

static int16_t heightCur[kPixels];   // current height field
static int16_t heightPrev[kPixels];  // previous height field (wave memory)
static uint8_t frame[kPixels];       // brightness buffer handed to draw()

static uint32_t dropCount = 0;

// ---------- Deterministic PRNG (32-bit LCG, Numerical Recipes constants) ----------
static uint32_t lcgState = 0x5EED1234u;

static inline uint32_t lcgNext() {
  lcgState = lcgState * 1664525u + 1013904223u;
  return lcgState;
}

// Uniform byte in [0, 255].
static inline uint8_t randByte() {
  return (uint8_t)(lcgNext() >> 24);
}

// Uniform value in [lo, hi] (hi > lo).
static inline uint32_t randRange(uint32_t lo, uint32_t hi) {
  return lo + (lcgNext() >> 8) % (hi - lo + 1);
}

// ---------- Water simulation ----------

// Neighbor read with reflecting edges: out-of-range coordinates clamp back
// onto the border cell, so waves bounce off the pool walls.
static inline int16_t sampleClamped(const int16_t *h, int x, int y) {
  if (x < 0) x = 0;
  if (x >= kWidth) x = kWidth - 1;
  if (y < 0) y = 0;
  if (y >= kHeight) y = kHeight - 1;
  return h[y * kWidth + x];
}

// One discrete wave-equation step: for every cell,
//   new = (sum of 4 neighbors)/2 - previous, then damp by 15/16.
// Result lands in heightPrev, then the buffers swap roles via memcpy-free
// pointerless swap: we write into prev and exchange meaning each frame.
static void stepWave() {
  for (int y = 0; y < kHeight; y++) {
    for (int x = 0; x < kWidth; x++) {
      const int32_t sum = (int32_t)sampleClamped(heightCur, x - 1, y) +
                          (int32_t)sampleClamped(heightCur, x + 1, y) +
                          (int32_t)sampleClamped(heightCur, x, y - 1) +
                          (int32_t)sampleClamped(heightCur, x, y + 1);
      int32_t v = (sum >> 1) - (int32_t)heightPrev[y * kWidth + x];
      v -= v >> kDampShift;  // damping ~0.94
      if (v > kHeightClamp)  v = kHeightClamp;
      if (v < -kHeightClamp) v = -kHeightClamp;
      heightPrev[y * kWidth + x] = (int16_t)v;
    }
  }
  // Swap: what we just wrote becomes "current", old current becomes "previous".
  for (int i = 0; i < kPixels; i++) {
    const int16_t tmp = heightCur[i];
    heightCur[i]  = heightPrev[i];
    heightPrev[i] = tmp;
  }
}

// Add an impulse at (x, y), saturating to the clamp range.
static void addImpulse(int x, int y, int16_t amount) {
  if (x < 0 || x >= kWidth || y < 0 || y >= kHeight) return;
  int32_t v = (int32_t)heightCur[y * kWidth + x] + amount;
  if (v > kHeightClamp)  v = kHeightClamp;
  if (v < -kHeightClamp) v = -kHeightClamp;
  heightCur[y * kWidth + x] = (int16_t)v;
}

// Drop a raindrop (or occasionally a stone) somewhere random.
static void spawnDrop() {
  const int x = (int)randRange(0, kWidth - 1);
  const int y = (int)randRange(0, kHeight - 1);
  const bool stone = randByte() < kStoneChance;

  if (stone) {
    // Heavy stone: deep central hole plus a small plus-shaped splash where
    // displaced water is thrown upward around the impact point.
    addImpulse(x, y, kStoneImpulse);
    addImpulse(x - 1, y, kSplashLift);
    addImpulse(x + 1, y, kSplashLift);
    addImpulse(x, y - 1, kSplashLift);
    addImpulse(x, y + 1, kSplashLift);
  } else {
    addImpulse(x, y, kDropImpulse);
  }

  dropCount++;
  Serial.print(stone ? "stone " : "drop  ");
  Serial.print("#");
  Serial.print(dropCount);
  Serial.print(" at (");
  Serial.print(x);
  Serial.print(",");
  Serial.print(y);
  Serial.println(")");
}

// Map height to brightness 0..7 centered on calm water at kCalmLevel:
// crests glow brighter, troughs go dark.
static void renderFrame() {
  for (int i = 0; i < kPixels; i++) {
    int32_t b = (int32_t)kCalmLevel + (int32_t)heightCur[i] / kBrightDivisor;
    if (b < 0) b = 0;
    if (b > kMaxBrightness) b = kMaxBrightness;
    frame[i] = (uint8_t)b;
  }
  matrix.draw(frame);
}

// ---------- Arduino entry points ----------

void setup() {
  Serial.begin(115200);
  matrix.begin();
  matrix.setGrayscaleBits(kBrightnessBits);
  memset(heightCur, 0, sizeof(heightCur));
  memset(heightPrev, 0, sizeof(heightPrev));
  memset(frame, 0, sizeof(frame));
}

void loop() {
  static uint32_t lastFrameMs = 0;
  static uint32_t nextDropDueMs = kMinDropGapMs;

  const uint32_t now = millis();
  if (now - lastFrameMs < kFramePeriodMs) {
    return;  // non-blocking pacing (rollover-safe subtraction)
  }
  lastFrameMs = now;

  if ((int32_t)(now - nextDropDueMs) >= 0) {
    spawnDrop();
    nextDropDueMs = now + randRange(kMinDropGapMs, kMaxDropGapMs);
  }

  stepWave();
  renderFrame();
}
