/*
  matrix-sand.ino — Falling-sand simulation on the Arduino UNO Q LED matrix.

  What it does:
    Grains of sand spawn at the TOP of the built-in 13x8 (13 cols x 8 rows)
    blue LED matrix from an emitter column that slowly wanders left/right.
    Each simulation tick (~20 ticks/sec) a falling grain drops one cell; if
    the cell directly below is occupied it tries below-left / below-right in
    RANDOM order (to avoid directional bias), and if neither is free it
    settles. Falling grains render bright (7), settled grains render dim (3),
    and a freshly settled grain briefly glows, fading 7 -> 3 over a few
    ticks. The grid is updated from the BOTTOM row upward so every grain
    moves at most one cell per tick. When the pile reaches near the emitter
    (settled sand in the top two rows) or after a grain budget is exhausted,
    the pile dissolves bottom-up over ~2 s — each row flashes bright, then
    vanishes — and the simulation restarts. Non-blocking, millis()-paced loop.

  Buffer orientation:
    The 104-byte draw buffer is row-major, 8 rows of 13 columns, and row 0
    is the PHYSICAL TOP row. Sand therefore spawns in row 0 and piles up on
    row 7 (bottom).

  Wiring: no external hardware needed — uses only the built-in LED matrix.

  Compile:
    arduino-cli compile -b arduino:zephyr:unoq ./matrix-sand
  Upload:
    arduino-cli upload -b arduino:zephyr:unoq -p <board-ip> ./matrix-sand
*/

#include "Arduino_LED_Matrix.h"

// ---------- Matrix geometry ----------
constexpr int kWidth  = 13;   // columns
constexpr int kHeight = 8;    // rows; row 0 = physical top, row 7 = bottom
constexpr int kPixels = kWidth * kHeight;  // 104

// ---------- Timing ----------
constexpr uint32_t kTickPeriodMs = 50;   // ~20 simulation ticks per second
constexpr uint8_t  kBrightnessBits = 3;  // 0..7 grayscale levels

// ---------- Sand tuning ----------
constexpr uint8_t kSpawnPeriodTicks   = 3;    // one grain every N ticks
constexpr uint8_t kWanderPeriodTicks  = 4;    // emitter drifts every N ticks
constexpr uint16_t kMaxGrains         = 70;   // grain budget before a reset
constexpr int      kTriggerRow        = 1;    // settled sand this high -> reset
constexpr uint8_t  kGlowTicks         = 4;    // fresh-settle glow duration

// ---------- Brightness levels ----------
constexpr uint8_t kBrightFalling = 7;  // grains in flight
constexpr uint8_t kBrightSettled = 3;  // resting pile
constexpr uint8_t kBrightEmitter = 1;  // faint marker at the emitter column
constexpr uint8_t kBrightDissolve = 7; // row being wiped away

// ---------- Dissolve wipe: 8 rows x 5 ticks x 50 ms = 2000 ms ----------
constexpr uint8_t kDissolveTicksPerRow = 5;

// ---------- Cell states ----------
constexpr uint8_t kEmpty   = 0;
constexpr uint8_t kFalling = 1;
constexpr uint8_t kSettled = 2;

enum class Mode : uint8_t { Running, Dissolving };

Arduino_LED_Matrix matrix;

static uint8_t grid[kPixels];   // row-major cell states
static uint8_t glow[kPixels];   // per-cell fresh-settle glow countdown
static uint8_t frame[kPixels];  // brightness buffer handed to draw()

static Mode     mode = Mode::Running;
static int      emitterX = kWidth / 2;
static uint16_t grainsSpawned = 0;
static uint8_t  spawnTick = 0;
static uint8_t  wanderTick = 0;
static int      dissolveRow = 0;      // row currently being wiped (bottom-up)
static uint8_t  dissolveTick = 0;

// ---------- Deterministic PRNG (32-bit LCG, Numerical Recipes constants) ----------
static uint32_t lcgState = 0x5EEDBA5Eu;

static inline uint32_t lcgNext() {
  lcgState = lcgState * 1664525u + 1013904223u;
  return lcgState;
}

// Uniform value in [0, n-1] for small n.
static inline uint8_t randBelow(uint8_t n) {
  return (uint8_t)((lcgNext() >> 16) % n);
}

static inline int idx(int x, int y) {
  return y * kWidth + x;
}

// ---------- Simulation ----------

static void resetSimulation() {
  memset(grid, kEmpty, sizeof(grid));
  memset(glow, 0, sizeof(glow));
  grainsSpawned = 0;
  spawnTick = 0;
  wanderTick = 0;
  emitterX = kWidth / 2;
  mode = Mode::Running;
}

static void settleGrain(int x, int y) {
  grid[idx(x, y)] = kSettled;
  glow[idx(x, y)] = kGlowTicks;
}

// Move one falling grain at (x, y). Bottom-up scan order guarantees the
// destination row was already processed, so a grain moves at most one cell
// per tick and is never updated twice.
static void stepGrain(int x, int y) {
  if (y == kHeight - 1) {          // on the floor
    settleGrain(x, y);
    return;
  }

  const int below = idx(x, y + 1);
  if (grid[below] == kEmpty) {     // straight down
    grid[idx(x, y)] = kEmpty;
    grid[below] = kFalling;
    return;
  }

  // Below is occupied: try the two diagonals in random order to avoid bias.
  const int firstDx = (randBelow(2) == 0) ? -1 : 1;
  for (int attempt = 0; attempt < 2; attempt++) {
    const int dx = (attempt == 0) ? firstDx : -firstDx;
    const int nx = x + dx;
    if (nx < 0 || nx >= kWidth) continue;
    const int diag = idx(nx, y + 1);
    if (grid[diag] == kEmpty) {
      grid[idx(x, y)] = kEmpty;
      grid[diag] = kFalling;
      return;
    }
  }

  settleGrain(x, y);               // nowhere to go
}

static void wanderEmitter() {
  if (++wanderTick < kWanderPeriodTicks) return;
  wanderTick = 0;
  emitterX += (int)randBelow(3) - 1;   // -1, 0, or +1
  if (emitterX < 0) emitterX = 0;
  if (emitterX >= kWidth) emitterX = kWidth - 1;
}

static void spawnGrain() {
  if (++spawnTick < kSpawnPeriodTicks) return;
  spawnTick = 0;
  if (grainsSpawned >= kMaxGrains) return;
  const int top = idx(emitterX, 0);
  if (grid[top] == kEmpty) {
    grid[top] = kFalling;
    grainsSpawned++;
  }
}

// True when the pile is tall enough (or the budget is spent) to trigger a wipe.
static bool resetDue() {
  for (int y = 0; y <= kTriggerRow; y++) {
    for (int x = 0; x < kWidth; x++) {
      if (grid[idx(x, y)] == kSettled) return true;
    }
  }
  if (grainsSpawned >= kMaxGrains) {
    // Budget spent: wait until nothing is still in flight, then wipe.
    for (int i = 0; i < kPixels; i++) {
      if (grid[i] == kFalling) return false;
    }
    return true;
  }
  return false;
}

static void beginDissolve() {
  // Freeze anything still in flight so the wipe eats a static pile.
  for (int i = 0; i < kPixels; i++) {
    if (grid[i] == kFalling) grid[i] = kSettled;
  }
  memset(glow, 0, sizeof(glow));
  dissolveRow = kHeight - 1;
  dissolveTick = 0;
  mode = Mode::Dissolving;
}

static void tickRunning() {
  wanderEmitter();
  spawnGrain();

  // Fade fresh-settle glows from previous ticks BEFORE moving grains, so a
  // grain that settles this tick keeps its full glow (renders at 7) for one
  // frame before fading.
  for (int i = 0; i < kPixels; i++) {
    if (glow[i] > 0) glow[i]--;
  }

  // Update from the bottom row upward.
  for (int y = kHeight - 1; y >= 0; y--) {
    for (int x = 0; x < kWidth; x++) {
      if (grid[idx(x, y)] == kFalling) stepGrain(x, y);
    }
  }

  if (resetDue()) beginDissolve();
}

static void tickDissolving() {
  if (++dissolveTick >= kDissolveTicksPerRow) {
    dissolveTick = 0;
    for (int x = 0; x < kWidth; x++) {
      grid[idx(x, dissolveRow)] = kEmpty;   // wipe this row away
    }
    if (--dissolveRow < 0) resetSimulation();
  }
}

// ---------- Rendering ----------

static void renderFrame() {
  for (int i = 0; i < kPixels; i++) {
    switch (grid[i]) {
      case kFalling: frame[i] = kBrightFalling; break;
      case kSettled: frame[i] = (uint8_t)(kBrightSettled + glow[i]); break;
      default:       frame[i] = 0; break;
    }
  }

  if (mode == Mode::Dissolving && dissolveRow >= 0) {
    // Flash the row currently being eaten by the wipe.
    for (int x = 0; x < kWidth; x++) {
      const int i = idx(x, dissolveRow);
      if (grid[i] == kSettled) frame[i] = kBrightDissolve;
    }
  } else if (mode == Mode::Running) {
    // Faint marker showing where the emitter is pointing.
    const int top = idx(emitterX, 0);
    if (frame[top] == 0) frame[top] = kBrightEmitter;
  }

  matrix.draw(frame);
}

// ---------- Arduino entry points ----------

void setup() {
  Serial.begin(115200);
  matrix.begin();
  matrix.setGrayscaleBits(kBrightnessBits);
  resetSimulation();
}

void loop() {
  static uint32_t lastTickMs = 0;

  const uint32_t now = millis();
  if (now - lastTickMs < kTickPeriodMs) {
    return;  // non-blocking pacing (rollover-safe subtraction)
  }
  lastTickMs = now;

  if (mode == Mode::Running) {
    tickRunning();
  } else {
    tickDissolving();
  }
  renderFrame();
}
