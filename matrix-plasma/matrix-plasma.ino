/*
  matrix-plasma.ino — Smooth plasma effect on the Arduino UNO Q LED matrix.

  What it does:
    Renders a classic "plasma" animation on the built-in 13x8 (13 cols x 8 rows)
    blue LED matrix. Four moving sine fields are combined per pixel:
      1. A sine over x (with a slowly rotating direction),
      2. A sine over y (phase-drifting),
      3. A diagonal sine over (x + y),
      4. A radial sine around a center point that itself drifts on a
         Lissajous path, so the pattern never repeats obviously.
    The sum is mapped to 8 grayscale levels (0..7) via setGrayscaleBits(3).
    Runs at ~30 fps with a non-blocking millis()-paced loop.

  Wiring: no external hardware — uses only the built-in LED matrix.

  Compile:
    arduino-cli compile -b arduino:zephyr:unoq ./matrix-plasma
  Upload:
    arduino-cli upload -b arduino:zephyr:unoq -p <board-ip> ./matrix-plasma
*/

#include "Arduino_LED_Matrix.h"

#include <math.h>

// ---------- Matrix geometry ----------
constexpr int kWidth  = 13;   // columns
constexpr int kHeight = 8;    // rows
constexpr int kPixels = kWidth * kHeight;   // 104 bytes, row-major

// ---------- Animation timing ----------
constexpr uint32_t kFrameIntervalMs = 33;   // ~30 fps
constexpr float    kTimeScale       = 0.001f; // ms -> plasma time units

// ---------- Plasma field tuning ----------
constexpr float kFreqX      = 0.55f;  // spatial frequency of the x field
constexpr float kFreqY      = 0.80f;  // spatial frequency of the y field
constexpr float kFreqDiag   = 0.45f;  // spatial frequency of the diagonal field
constexpr float kFreqRadial = 0.90f;  // spatial frequency of the radial field

constexpr float kSpeedX      = 1.10f; // phase speed of the x field
constexpr float kSpeedY      = 0.75f; // phase speed of the y field
constexpr float kSpeedDiag   = 0.50f; // phase speed of the diagonal field
constexpr float kSpeedRadial = 1.30f; // phase speed of the radial field

// Slow, incommensurate drift rates for the radial center (Lissajous path)
// and the x-field direction, so the overall pattern avoids obvious loops.
constexpr float kCenterDriftA = 0.170f;
constexpr float kCenterDriftB = 0.113f;
constexpr float kRotateSpeed  = 0.061f;

constexpr int kMaxBrightness = 7;     // 3-bit grayscale

Arduino_LED_Matrix matrix;

uint8_t frameBuf[kPixels];

// Render one plasma frame into frameBuf for plasma time t.
void renderPlasma(float t) {
  // Drifting center for the radial field (stays roughly on-panel).
  const float cx = (kWidth  - 1) * 0.5f + 4.5f * sinf(kCenterDriftA * t);
  const float cy = (kHeight - 1) * 0.5f + 2.5f * sinf(kCenterDriftB * t + 1.7f);

  // Slowly rotating direction for the "x" field.
  const float ang = kRotateSpeed * t;
  const float dirX = cosf(ang);
  const float dirY = sinf(ang);

  uint8_t *p = frameBuf;
  for (int y = 0; y < kHeight; y++) {
    for (int x = 0; x < kWidth; x++) {
      const float fx = (float)x;
      const float fy = (float)y;

      // Field 1: sine along a slowly rotating axis.
      const float v1 = sinf(kFreqX * (fx * dirX + fy * dirY) + kSpeedX * t);

      // Field 2: sine over y with drifting phase.
      const float v2 = sinf(kFreqY * fy + kSpeedY * t);

      // Field 3: diagonal sine.
      const float v3 = sinf(kFreqDiag * (fx + fy) + kSpeedDiag * t);

      // Field 4: radial sine around the drifting center.
      const float dx = fx - cx;
      const float dy = fy - cy;
      const float v4 = sinf(kFreqRadial * sqrtf(dx * dx + dy * dy)
                            + kSpeedRadial * t);

      // Sum in [-4, 4] -> normalize to [0, 1] -> brightness 0..7.
      const float norm = (v1 + v2 + v3 + v4 + 4.0f) * 0.125f;
      int level = (int)(norm * (kMaxBrightness + 1));
      if (level > kMaxBrightness) level = kMaxBrightness;
      if (level < 0)              level = 0;
      *p++ = (uint8_t)level;
    }
  }
}

void setup() {
  matrix.begin();
  matrix.setGrayscaleBits(3);
}

void loop() {
  static uint32_t lastFrameMs = 0;
  const uint32_t now = millis();
  if (now - lastFrameMs < kFrameIntervalMs) {
    return;
  }
  lastFrameMs = now;

  renderPlasma((float)now * kTimeScale);
  matrix.draw(frameBuf);
}
