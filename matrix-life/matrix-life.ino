/*
 * matrix-life.ino — Conway's Game of Life on the Arduino UNO Q LED matrix
 *
 * What it does:
 *   Runs Conway's Game of Life on the built-in 13x8 blue LED matrix, treating
 *   the grid as a torus (both axes wrap). Live cells glow at full brightness
 *   (7); when a cell dies it leaves a fading "ghost" trail that decays to 0
 *   over a few frames, so the display looks organic rather than binary.
 *
 *   The simulation runs at ~8 generations per second. It detects extinction
 *   (no live cells) and stagnation (still lifes and period-2 oscillators, by
 *   comparing each new generation against the previous two). When the world
 *   goes stale, it pauses briefly, plays a subtle "breath" pulse across the
 *   ghost field as a reseed cue, then sows a fresh random soup (~35% density)
 *   from a deterministic LCG.
 *
 * Wiring: no external hardware — uses only the built-in LED matrix.
 *
 * Compile:
 *   arduino-cli compile -b arduino:zephyr:unoq ./matrix-life
 * Upload:
 *   arduino-cli upload -b arduino:zephyr:unoq -p <board-ip> ./matrix-life
 */

#include "Arduino_LED_Matrix.h"

// ---------- Matrix geometry ----------
constexpr int kWidth  = 13;
constexpr int kHeight = 8;
constexpr int kCells  = kWidth * kHeight;   // 104

// ---------- Brightness / ghost trail ----------
constexpr uint8_t kLiveBrightness = 7;   // brightness of a live cell (0..7)
constexpr uint8_t kGhostDecayStep = 2;   // brightness lost per frame after death (7->0 in ~4 frames)

// ---------- Timing ----------
constexpr uint32_t kGenerationIntervalMs = 125;  // ~8 generations/sec
constexpr uint32_t kReseedPauseMs        = 900;  // pause before reseeding
constexpr uint32_t kCuePulseMs           = 60;   // per-step pacing of the reseed cue pulse

// ---------- Reseed ----------
constexpr uint8_t kSoupDensityPercent = 35;      // ~35% of cells start alive

Arduino_LED_Matrix matrix;

// Cell state for the current generation plus the two before it
// (prev1 = last generation, prev2 = the one before that).
static uint8_t cells[kCells];
static uint8_t prev1[kCells];
static uint8_t prev2[kCells];

// Displayed brightness per cell (live level or decaying ghost).
static uint8_t frame[kCells];

// ---------- State machine ----------
enum class LifeState : uint8_t {
  Running,     // stepping generations
  StalePause,  // world died/stagnated; letting ghosts linger
  ReseedCue,   // subtle pulse announcing the reseed
};

static LifeState state = LifeState::Running;
static uint32_t lastStepMs = 0;
static uint32_t stateEnterMs = 0;
static int cuePhase = 0;  // steps through the cue pulse animation

// ---------- Deterministic PRNG (32-bit LCG, Numerical Recipes constants) ----------
static uint32_t lcgState = 0xC0FFEE42u;

static uint32_t lcgNext() {
  lcgState = lcgState * 1664525u + 1013904223u;
  return lcgState;
}

// Uniform-ish percent roll 0..99.
static uint8_t lcgPercent() {
  return static_cast<uint8_t>((lcgNext() >> 16) % 100u);
}

// ---------- Grid helpers ----------
static inline int cellIndex(int x, int y) {
  return y * kWidth + x;
}

// Count the 8 neighbors of (x, y) with toroidal wrap on both axes.
static int neighborCount(int x, int y) {
  const int xl = (x == 0) ? kWidth - 1 : x - 1;
  const int xr = (x == kWidth - 1) ? 0 : x + 1;
  const int yu = (y == 0) ? kHeight - 1 : y - 1;
  const int yd = (y == kHeight - 1) ? 0 : y + 1;

  return cells[cellIndex(xl, yu)] + cells[cellIndex(x, yu)] + cells[cellIndex(xr, yu)] +
         cells[cellIndex(xl, y)]                            + cells[cellIndex(xr, y)] +
         cells[cellIndex(xl, yd)] + cells[cellIndex(x, yd)] + cells[cellIndex(xr, yd)];
}

// Advance one generation. Returns the number of live cells afterwards.
static int stepGeneration() {
  static uint8_t next[kCells];

  int alive = 0;
  for (int y = 0; y < kHeight; y++) {
    for (int x = 0; x < kWidth; x++) {
      const int i = cellIndex(x, y);
      const int n = neighborCount(x, y);
      const uint8_t live = cells[i] ? (n == 2 || n == 3) : (n == 3);
      next[i] = live;
      alive += live;
    }
  }

  memcpy(prev2, prev1, kCells);
  memcpy(prev1, cells, kCells);
  memcpy(cells, next, kCells);
  return alive;
}

// True if the world is a still life or a period-2 oscillator.
static bool isStagnant() {
  return memcmp(cells, prev1, kCells) == 0 ||
         memcmp(cells, prev2, kCells) == 0;
}

// ---------- Rendering ----------
// Fold cell state into the brightness frame: live cells at full brightness,
// dead cells decay toward 0.
static void updateFrame() {
  for (int i = 0; i < kCells; i++) {
    if (cells[i]) {
      frame[i] = kLiveBrightness;
    } else if (frame[i] > kGhostDecayStep) {
      frame[i] -= kGhostDecayStep;
    } else {
      frame[i] = 0;
    }
  }
  matrix.draw(frame);
}

// ---------- Reseeding ----------
static void seedSoup() {
  memset(prev1, 0, kCells);
  memset(prev2, 0, kCells);
  for (int i = 0; i < kCells; i++) {
    cells[i] = (lcgPercent() < kSoupDensityPercent) ? 1 : 0;
  }
}

// One step of the reseed cue: a soft brightness "breath" (all remaining
// ghosts lift to a dim glow, then fade) so the reseed doesn't pop in cold.
// Returns true when the cue has finished.
static bool stepReseedCue() {
  constexpr uint8_t pulseLevels[] = { 1, 2, 3, 2, 1, 0 };
  constexpr int pulseSteps = sizeof(pulseLevels);

  if (cuePhase >= pulseSteps) {
    return true;
  }

  const uint8_t level = pulseLevels[cuePhase];
  uint8_t cue[kCells];
  for (int i = 0; i < kCells; i++) {
    cue[i] = (frame[i] > level) ? frame[i] : level;
  }
  matrix.draw(cue);
  cuePhase++;
  return false;
}

// ---------- Arduino entry points ----------
void setup() {
  Serial.begin(115200);
  matrix.begin();
  matrix.setGrayscaleBits(3);

  memset(frame, 0, kCells);
  seedSoup();
  updateFrame();
  lastStepMs = millis();
}

void loop() {
  const uint32_t now = millis();

  switch (state) {
    case LifeState::Running:
      if (now - lastStepMs >= kGenerationIntervalMs) {
        lastStepMs = now;
        const int alive = stepGeneration();
        updateFrame();
        if (alive == 0 || isStagnant()) {
          state = LifeState::StalePause;
          stateEnterMs = now;
          Serial.println(alive == 0 ? "Extinction — reseeding soon"
                                    : "Stagnation — reseeding soon");
        }
      }
      break;

    case LifeState::StalePause:
      // Keep ghosts fading during the pause so the screen stays alive.
      if (now - lastStepMs >= kGenerationIntervalMs) {
        lastStepMs = now;
        updateFrame();
      }
      if (now - stateEnterMs >= kReseedPauseMs) {
        state = LifeState::ReseedCue;
        stateEnterMs = now;
        cuePhase = 0;
      }
      break;

    case LifeState::ReseedCue:
      if (now - stateEnterMs >= kCuePulseMs) {
        stateEnterMs = now;
        if (stepReseedCue()) {
          seedSoup();
          updateFrame();
          state = LifeState::Running;
          lastStepMs = now;
        }
      }
      break;
  }
}
