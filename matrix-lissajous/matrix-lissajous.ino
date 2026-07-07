/*
  matrix-lissajous.ino — Lissajous curve tracer on the Arduino UNO Q LED matrix.

  What it does:
    A tracer dot sweeps a Lissajous figure on the built-in 13x8 (13 cols x
    8 rows) blue LED matrix:
        x = cx + Ax * sinf(phaseX + phase)
        y = cy + Ay * sinf(phaseY)
    where phaseX/phaseY advance at rates a*omega / b*omega. The head draws at
    full brightness (7) and leaves a persistent trail: instead of clearing the
    frame, every few frames the whole buffer is decayed by one brightness
    level, so the figure fades gracefully behind the dot.

    Every ~10 s the (a, b) frequency ratio smoothly cross-fades to a new one
    from a curated list of pretty ratios (1:2, 2:3, 3:4, 3:5, 4:5, 5:6),
    chosen by a small deterministic LCG. The x phase also drifts continuously
    so the figure precesses and never sits still. Frequencies are integrated
    into phase accumulators, so ratio transitions are click-free.

    The tracer runs ~60 steps/sec; each step is subdivided so consecutive
    plot points are <= ~0.35 px apart (no gaps at any speed). Cheap
    antialiasing: besides the nearest pixel, the adjacent pixel in x and in y
    is lit at a lower level proportional to the fractional position.

  Wiring: no external hardware needed — uses only the built-in LED matrix.

  Compile:
    arduino-cli compile -b arduino:zephyr:unoq ./matrix-lissajous
  Upload:
    arduino-cli upload -b arduino:zephyr:unoq -p <board-ip> ./matrix-lissajous
*/

#include "Arduino_LED_Matrix.h"

#include <math.h>

// ---------- Matrix geometry ----------
constexpr int kWidth  = 13;   // columns
constexpr int kHeight = 8;    // rows
constexpr int kPixels = kWidth * kHeight;   // 104 bytes, row-major

// ---------- Tracer geometry ----------
constexpr float kCenterX = (kWidth  - 1) * 0.5f;  // 6.0
constexpr float kCenterY = (kHeight - 1) * 0.5f;  // 3.5
constexpr float kAmpX    = (kWidth  - 1) * 0.5f;  // fill the panel edge to edge
constexpr float kAmpY    = (kHeight - 1) * 0.5f;

// ---------- Timing ----------
constexpr uint32_t kStepIntervalMs   = 16;      // ~60 tracer steps/sec
constexpr uint32_t kDecayEveryFrames = 3;       // trail loses 1 level / 3 frames
constexpr uint32_t kRatioHoldMs      = 10000;   // ~10 s per frequency ratio
constexpr uint32_t kRatioBlendMs     = 2000;    // cross-fade duration a->a', b->b'

// ---------- Motion tuning ----------
constexpr float kBaseOmega     = 1.05f;   // rad/s per unit of the ratio numbers
constexpr float kPhaseDriftHz  = 0.055f;  // slow precession of the x phase
constexpr float kMaxStepPx     = 0.35f;   // max pixel gap between plot points
constexpr int   kMaxBrightness = 7;       // 3-bit grayscale
constexpr float kTwoPi         = 6.28318530718f;

// Curated pretty frequency ratios (a : b).
constexpr uint8_t kRatios[][2] = {
  {1, 2}, {2, 3}, {3, 4}, {3, 5}, {4, 5}, {5, 6},
};
constexpr int kRatioCount = sizeof(kRatios) / sizeof(kRatios[0]);

// ---------- Deterministic pseudo-randomness (LCG, Numerical Recipes) ----------
constexpr uint32_t kLcgMul  = 1664525u;
constexpr uint32_t kLcgAdd  = 1013904223u;
constexpr uint32_t kLcgSeed = 0xC0FFEE01u;

uint32_t lcgState = kLcgSeed;

uint32_t lcgNext() {
  lcgState = lcgState * kLcgMul + kLcgAdd;
  return lcgState >> 8;   // drop the weakest low bits
}

Arduino_LED_Matrix matrix;

uint8_t frameBuf[kPixels];

// ---------- Tracer state ----------
float phaseX = 0.0f;   // integrated x phase (rad)
float phaseY = 0.0f;   // integrated y phase (rad)
float drift  = 0.0f;   // slowly precessing extra x phase (rad)

int      ratioIdx      = 0;       // current entry in kRatios
float    freqFromA     = 1.0f;    // blend endpoints (ratio numbers, pre-omega)
float    freqFromB     = 2.0f;
float    freqToA       = 1.0f;
float    freqToB       = 2.0f;
uint32_t ratioStartMs  = 0;       // when the current ratio period began

// Blend-smoothed current ratio numbers.
float curA = 1.0f;
float curB = 2.0f;

// Max-blend a brightness level into one pixel (with bounds check).
void plotPixel(int x, int y, int level) {
  if (x < 0 || x >= kWidth || y < 0 || y >= kHeight || level <= 0) {
    return;
  }
  uint8_t &cell = frameBuf[y * kWidth + x];
  if (level > cell) {
    cell = (uint8_t)level;
  }
}

// Draw the tracer head at float position (fx, fy): nearest pixel at full
// brightness, plus the adjacent pixel in x and in y at a level proportional
// to the fractional offset (cheap antialiasing).
void plotHead(float fx, float fy) {
  const int ix = (int)floorf(fx + 0.5f);   // nearest pixel
  const int iy = (int)floorf(fy + 0.5f);
  const float dx = fx - (float)ix;         // fractional offset, [-0.5, 0.5]
  const float dy = fy - (float)iy;

  plotPixel(ix, iy, kMaxBrightness);

  // Neighbor toward the fractional offset, scaled so |off| = 0.5 -> level 7.
  const int lvlX = (int)(fabsf(dx) * 2.0f * kMaxBrightness + 0.5f);
  const int lvlY = (int)(fabsf(dy) * 2.0f * kMaxBrightness + 0.5f);
  plotPixel(ix + (dx >= 0.0f ? 1 : -1), iy, lvlX);
  plotPixel(ix, iy + (dy >= 0.0f ? 1 : -1), lvlY);
}

// Decay every pixel by one brightness level (trail fade).
void decayFrame() {
  for (int i = 0; i < kPixels; i++) {
    if (frameBuf[i] > 0) {
      frameBuf[i]--;
    }
  }
}

// Pick the next ratio (never the same one twice in a row) and start a blend.
void startNextRatio(uint32_t now) {
  int next = (int)(lcgNext() % (uint32_t)(kRatioCount - 1));
  if (next >= ratioIdx) {
    next++;                       // skip the current index
  }
  ratioIdx = next;

  freqFromA = curA;
  freqFromB = curB;
  freqToA   = (float)kRatios[ratioIdx][0];
  freqToB   = (float)kRatios[ratioIdx][1];

  // Occasionally swap which axis gets the faster frequency, for variety.
  if (lcgNext() & 1u) {
    const float tmp = freqToA;
    freqToA = freqToB;
    freqToB = tmp;
  }
  ratioStartMs = now;
}

// Update curA/curB with a smoothstep cross-fade at the start of each period.
void updateRatioBlend(uint32_t now) {
  const uint32_t elapsed = now - ratioStartMs;
  if (elapsed >= kRatioBlendMs) {
    curA = freqToA;
    curB = freqToB;
    return;
  }
  float u = (float)elapsed / (float)kRatioBlendMs;
  u = u * u * (3.0f - 2.0f * u);            // smoothstep
  curA = freqFromA + (freqToA - freqFromA) * u;
  curB = freqFromB + (freqToB - freqFromB) * u;
}

// Advance the tracer by dtSec, subdividing so plot points never gap.
void stepTracer(float dtSec) {
  const float omegaA = curA * kBaseOmega;   // rad/s on each axis
  const float omegaB = curB * kBaseOmega;

  // Worst-case pixel speed of the head, used to size the substeps.
  const float maxPxPerSec = kAmpX * omegaA + kAmpY * omegaB;
  int substeps = 1 + (int)(maxPxPerSec * dtSec / kMaxStepPx);
  const float subDt = dtSec / (float)substeps;

  while (substeps-- > 0) {
    phaseX += omegaA * subDt;
    phaseY += omegaB * subDt;
    drift  += kPhaseDriftHz * kTwoPi * subDt;

    // Keep the accumulators bounded (preserves float precision forever).
    if (phaseX > kTwoPi) phaseX -= kTwoPi;
    if (phaseY > kTwoPi) phaseY -= kTwoPi;
    if (drift  > kTwoPi) drift  -= kTwoPi;

    const float fx = kCenterX + kAmpX * sinf(phaseX + drift);
    const float fy = kCenterY + kAmpY * sinf(phaseY);
    plotHead(fx, fy);
  }
}

void setup() {
  matrix.begin();
  matrix.setGrayscaleBits(3);
  Serial.begin(115200);
  ratioStartMs = millis();
  startNextRatio(ratioStartMs);   // jump straight into a random pretty ratio
}

void loop() {
  static uint32_t lastStepMs = 0;
  static uint32_t frameCount = 0;

  const uint32_t now = millis();
  if (now - lastStepMs < kStepIntervalMs) {
    return;                       // non-blocking pacing, rollover-safe
  }
  lastStepMs = now;
  frameCount++;

  if (now - ratioStartMs >= kRatioHoldMs) {
    startNextRatio(now);
  }
  updateRatioBlend(now);

  if (frameCount % kDecayEveryFrames == 0) {
    decayFrame();
  }

  stepTracer((float)kStepIntervalMs * 0.001f);
  matrix.draw(frameBuf);
}
