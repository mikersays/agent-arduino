/*
  matrix-starfield.ino — 3D starfield flying toward the viewer on the
  Arduino UNO Q LED matrix.

  What it does:
    Simulates ~14 stars in a 3D volume in front of the camera on the
    built-in 13x8 (13 cols x 8 rows) blue LED matrix. Each star has float
    x, y, z; every frame z decreases (the star flies toward the viewer)
    and the star is perspective-projected around the screen center.
    Brightness grows as the star approaches: far stars glow at level 1,
    near stars blaze at level 7 (3-bit grayscale). When a star leaves the
    screen or passes the camera it respawns at far z with a new random
    x, y. The flight speed itself oscillates on a slow sine (~20 s
    period), so the field breathes between a gentle cruise and a warp
    rush. Runs at ~30 fps with a non-blocking, millis()-paced loop.

  Buffer orientation:
    The 104-byte draw buffer is row-major, 8 rows of 13 columns, and
    row 0 is the PHYSICAL TOP row. Stars are projected around the matrix
    center (col 6, row 3.5).

  Wiring: no external hardware — uses only the built-in LED matrix.

  Compile:
    arduino-cli compile -b arduino:zephyr:unoq ./matrix-starfield
  Upload:
    arduino-cli upload -b arduino:zephyr:unoq -p <board-ip> ./matrix-starfield
*/

#include "Arduino_LED_Matrix.h"

// ---------- Matrix geometry ----------
constexpr int   kWidth   = 13;                  // columns
constexpr int   kHeight  = 8;                   // rows; row 0 = physical top
constexpr int   kPixels  = kWidth * kHeight;    // 104
constexpr float kCenterX = (kWidth  - 1) * 0.5f;  // 6.0
constexpr float kCenterY = (kHeight - 1) * 0.5f;  // 3.5

// ---------- Starfield tuning ----------
constexpr uint32_t kFramePeriodMs   = 33;      // ~30 fps
constexpr int      kNumStars        = 14;      // stars kept alive
constexpr float    kFarZ            = 1.0f;    // spawn depth (arbitrary units)
constexpr float    kNearZ           = 0.05f;   // "passes the camera" threshold
constexpr float    kFocalLength     = 1.2f;    // projection scale factor
constexpr float    kSpawnHalfWidth  = 0.9f;    // |x| range at spawn
constexpr float    kSpawnHalfHeight = 0.55f;   // |y| range at spawn
constexpr float    kSpeedCycleS     = 20.0f;   // cruise <-> warp period, seconds
constexpr float    kCruiseSpeed     = 0.25f;   // z units per second (calm)
constexpr float    kWarpSpeed       = 1.6f;    // z units per second (rush)
constexpr uint8_t  kBrightnessBits  = 3;       // 0..7 grayscale levels
constexpr uint8_t  kMinBrightness   = 1;       // far star
constexpr uint8_t  kMaxBrightness   = 7;       // near star

Arduino_LED_Matrix matrix;

struct Star {
  float x, y, z;
};

static Star    stars[kNumStars];
static uint8_t frame[kPixels];   // brightness buffer handed to draw()

// ---------- Deterministic PRNG (32-bit LCG, Numerical Recipes constants) ----------
static uint32_t lcgState = 0x5EEDBA5Eu;

static inline uint32_t lcgNext() {
  lcgState = lcgState * 1664525u + 1013904223u;
  return lcgState;
}

// Uniform float in [0, 1).
static inline float randUnit() {
  return (float)(lcgNext() >> 8) * (1.0f / 16777216.0f);  // 24-bit mantissa
}

// Uniform float in [-1, 1).
static inline float randSigned() {
  return randUnit() * 2.0f - 1.0f;
}

// ---------- Star lifecycle ----------

// Place a star at far depth with random lateral position. Stagger the
// initial depth so the field doesn't respawn in lockstep waves.
static void respawnStar(Star &s, bool staggerDepth) {
  s.x = randSigned() * kSpawnHalfWidth;
  s.y = randSigned() * kSpawnHalfHeight;
  s.z = staggerDepth ? (kNearZ + randUnit() * (kFarZ - kNearZ)) : kFarZ;
}

// ---------- Speed envelope ----------
// Slow sine between cruise and warp, period kSpeedCycleS.
static float currentSpeed(uint32_t nowMs) {
  const float t = (float)nowMs / 1000.0f;
  const float s = 0.5f + 0.5f * sinf(t * (2.0f * PI / kSpeedCycleS));
  return kCruiseSpeed + (kWarpSpeed - kCruiseSpeed) * s;
}

// ---------- Simulation + render ----------

static void stepStarfield(float speed, float dtSeconds) {
  memset(frame, 0, sizeof(frame));

  for (int i = 0; i < kNumStars; i++) {
    Star &s = stars[i];
    s.z -= speed * dtSeconds;

    if (s.z <= kNearZ) {          // flew past the camera
      respawnStar(s, false);
    }

    // Perspective projection around the matrix center.
    const float invZ = kFocalLength / s.z;
    const int px = (int)lroundf(kCenterX + s.x * invZ * kCenterX);
    const int py = (int)lroundf(kCenterY + s.y * invZ * kCenterY);

    if (px < 0 || px >= kWidth || py < 0 || py >= kHeight) {
      respawnStar(stars[i], false);  // drifted off-screen
      continue;
    }

    // Brightness grows as z shrinks: far (kFarZ) -> 1, near (kNearZ) -> 7.
    float depth = (kFarZ - s.z) / (kFarZ - kNearZ);  // 0 far .. 1 near
    if (depth < 0.0f) depth = 0.0f;
    if (depth > 1.0f) depth = 1.0f;
    const uint8_t level =
        (uint8_t)(kMinBrightness +
                  depth * (float)(kMaxBrightness - kMinBrightness) + 0.5f);

    uint8_t &px8 = frame[py * kWidth + px];
    if (level > px8) px8 = level;  // brightest star wins a shared pixel
  }

  matrix.draw(frame);
}

// ---------- Arduino entry points ----------

void setup() {
  Serial.begin(115200);
  matrix.begin();
  matrix.setGrayscaleBits(kBrightnessBits);

  for (int i = 0; i < kNumStars; i++) {
    respawnStar(stars[i], true);  // staggered depths for a natural start
  }
}

void loop() {
  static uint32_t lastFrameMs = 0;

  const uint32_t now = millis();
  if (now - lastFrameMs < kFramePeriodMs) {
    return;  // non-blocking pacing (rollover-safe subtraction)
  }
  const float dtSeconds = (float)(now - lastFrameMs) / 1000.0f;
  lastFrameMs = now;

  stepStarfield(currentSpeed(now), dtSeconds > 0.1f ? 0.033f : dtSeconds);
}
