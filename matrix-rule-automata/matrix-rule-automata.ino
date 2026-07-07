/*
 * matrix-rule-automata.ino — Elementary cellular automaton waterfall on the
 * Arduino UNO Q LED matrix
 *
 * What it does:
 *   Runs 1D (elementary / Wolfram) cellular automata on the built-in 13x8
 *   blue LED matrix. Each 13-cell row is one generation: new generations
 *   appear at the BOTTOM row and history scrolls UPWARD, so the display reads
 *   like a waterfall in reverse — the freshest row enters at the bottom at
 *   full brightness (7) and rows dim as they age toward the top (oldest ~1).
 *   Row edges wrap, so the 13-cell universe is a ring.
 *
 *   The sketch cycles through a curated rule set every ~12 seconds:
 *   30 (chaos), 90 (Sierpinski), 110 (Turing-complete), 184 (traffic),
 *   and 54. Each rule change is announced on Serial and introduced with a
 *   brief ~1 second full-row "wipe" that sweeps a bright row up the matrix.
 *   Rules are seeded alternately with a single center cell or a random row
 *   (some rules only shine from a lone seed, others from noise). If a rule's
 *   state dies out, becomes uniform, or freezes, it is reseeded early with a
 *   random row. Runs at ~10 generations per second.
 *
 * Wiring: no external hardware — uses only the built-in LED matrix.
 *
 * Compile:
 *   arduino-cli compile -b arduino:zephyr:unoq ./matrix-rule-automata
 * Upload:
 *   arduino-cli upload -b arduino:zephyr:unoq -p <board-ip> ./matrix-rule-automata
 */

#include "Arduino_LED_Matrix.h"

// ---------- Matrix geometry ----------
constexpr int kWidth  = 13;
constexpr int kHeight = 8;
constexpr int kCells  = kWidth * kHeight;   // 104

// ---------- Brightness ----------
constexpr uint8_t kMaxBrightness = 7;  // newest generation (bottom row)
constexpr uint8_t kMinBrightness = 1;  // oldest generation (top row)
constexpr uint8_t kWipeBrightness = 7; // rule-change wipe row

// ---------- Timing ----------
constexpr uint32_t kGenerationIntervalMs = 100;    // ~10 generations/sec
constexpr uint32_t kRulePeriodMs         = 12000;  // time on each rule
constexpr uint32_t kWipeDurationMs       = 1000;   // rule-change wipe length
constexpr uint32_t kWipeStepMs           = kWipeDurationMs / kHeight;  // 125 ms/row

// ---------- Rule set ----------
constexpr uint8_t kRules[] = { 30, 90, 110, 184, 54 };
constexpr int kRuleCount = sizeof(kRules);

// Mask of the 13 valid cell bits in a row word.
constexpr uint16_t kRowMask = (1u << kWidth) - 1;

Arduino_LED_Matrix matrix;

// One uint16_t per display row; bit x = cell x. Index 0 is the physical top
// (oldest generation), index kHeight-1 the bottom (newest generation).
static uint16_t rows[kHeight];

// Displayed brightness per cell, row-major.
static uint8_t frame[kCells];

// ---------- State machine ----------
enum class CaState : uint8_t {
  Running,  // stepping generations
  Wipe,     // full-row sweep announcing a rule change
};

static CaState state = CaState::Wipe;  // start with a wipe into the first rule
static uint32_t lastStepMs = 0;
static uint32_t ruleStartMs = 0;
static int ruleIndex = 0;
static int wipeRow = kHeight - 1;      // wipe sweeps bottom -> top
static bool seedWithSingleCell = true; // alternates per rule change

// ---------- Deterministic PRNG (32-bit LCG, Numerical Recipes constants) ----------
static uint32_t lcgState = 0xACE0FBA5u;

static uint32_t lcgNext() {
  lcgState = lcgState * 1664525u + 1013904223u;
  return lcgState;
}

// ---------- Automaton core ----------
// Apply the current elementary rule to `row` with wraparound at the edges.
static uint16_t applyRule(uint16_t row, uint8_t rule) {
  uint16_t next = 0;
  for (int x = 0; x < kWidth; x++) {
    const int xl = (x == 0) ? kWidth - 1 : x - 1;
    const int xr = (x == kWidth - 1) ? 0 : x + 1;
    const uint8_t pattern = static_cast<uint8_t>(((row >> xl) & 1u) << 2 |
                                                 ((row >> x)  & 1u) << 1 |
                                                 ((row >> xr) & 1u));
    if ((rule >> pattern) & 1u) {
      next |= (1u << x);
    }
  }
  return next;
}

// Dead (all 0) or uniform (all 1) — the automaton has nothing left to show.
static bool isDegenerate(uint16_t row) {
  return row == 0 || row == kRowMask;
}

// ---------- Seeding ----------
// Random row, guaranteed non-degenerate.
static uint16_t randomRow() {
  uint16_t row;
  do {
    row = static_cast<uint16_t>(lcgNext() >> 8) & kRowMask;
  } while (isDegenerate(row));
  return row;
}

// Clear the history and place the seed generation on the bottom row.
static void seedAutomaton(bool singleCell) {
  memset(rows, 0, sizeof(rows));
  rows[kHeight - 1] = singleCell ? (1u << (kWidth / 2)) : randomRow();
}

// ---------- Rendering ----------
// Brightness by generation age: bottom (newest) = 7, dimming to 1 at the top.
static uint8_t rowBrightness(int y) {
  const int age = (kHeight - 1) - y;  // 0 = newest
  const int level = kMaxBrightness - age;
  return static_cast<uint8_t>((level < kMinBrightness) ? kMinBrightness : level);
}

static void drawRows() {
  for (int y = 0; y < kHeight; y++) {
    const uint8_t bright = rowBrightness(y);
    for (int x = 0; x < kWidth; x++) {
      frame[y * kWidth + x] = ((rows[y] >> x) & 1u) ? bright : 0;
    }
  }
  matrix.draw(frame);
}

// One frame of the rule-change wipe: a single full bright row on a blank
// field, sweeping from the bottom row up to the top.
static void drawWipeRow(int y) {
  memset(frame, 0, kCells);
  for (int x = 0; x < kWidth; x++) {
    frame[y * kWidth + x] = kWipeBrightness;
  }
  matrix.draw(frame);
}

// ---------- Generation step ----------
// Scroll history up one row and compute the new generation on the bottom.
static void stepGeneration() {
  const uint16_t next = applyRule(rows[kHeight - 1], kRules[ruleIndex]);
  const bool frozen = (next == rows[kHeight - 1]);

  memmove(&rows[0], &rows[1], (kHeight - 1) * sizeof(rows[0]));
  rows[kHeight - 1] = next;

  if (isDegenerate(next) || frozen) {
    Serial.print("Rule ");
    Serial.print(kRules[ruleIndex]);
    Serial.println(next == 0 ? " died out — reseeding"
                             : " went uniform/static — reseeding");
    rows[kHeight - 1] = randomRow();
  }
}

// ---------- Rule changes ----------
static void beginWipe() {
  state = CaState::Wipe;
  wipeRow = kHeight - 1;
  lastStepMs = millis();
  drawWipeRow(wipeRow);
}

// Called when the wipe finishes: seed and announce the current rule.
static void beginRule() {
  seedAutomaton(seedWithSingleCell);
  Serial.print("Rule ");
  Serial.print(kRules[ruleIndex]);
  Serial.println(seedWithSingleCell ? " — single center seed" : " — random row seed");
  seedWithSingleCell = !seedWithSingleCell;

  state = CaState::Running;
  ruleStartMs = millis();
  lastStepMs = ruleStartMs;
  drawRows();
}

// ---------- Arduino entry points ----------
void setup() {
  Serial.begin(115200);
  matrix.begin();
  matrix.setGrayscaleBits(3);

  memset(frame, 0, kCells);
  beginWipe();
}

void loop() {
  const uint32_t now = millis();

  switch (state) {
    case CaState::Running:
      if (now - ruleStartMs >= kRulePeriodMs) {
        ruleIndex = (ruleIndex + 1) % kRuleCount;
        beginWipe();
        break;
      }
      if (now - lastStepMs >= kGenerationIntervalMs) {
        lastStepMs = now;
        stepGeneration();
        drawRows();
      }
      break;

    case CaState::Wipe:
      if (now - lastStepMs >= kWipeStepMs) {
        lastStepMs = now;
        if (wipeRow > 0) {
          wipeRow--;
          drawWipeRow(wipeRow);
        } else {
          beginRule();
        }
      }
      break;
  }
}
