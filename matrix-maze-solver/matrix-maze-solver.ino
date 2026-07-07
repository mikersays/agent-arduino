/*
  matrix-maze-solver.ino — Endless maze generate-and-solve show on the
  Arduino UNO Q built-in 13x8 LED matrix.

  What it does:
    Phase 1 (carve): a perfect maze is generated with an iterative
    recursive-backtracker on a 7x4 logical cell grid mapped onto the
    13x8 pixel matrix — cells live at even pixel coordinates
    (x = 0,2,..,12 ; y = 0,2,4,6) with wall pixels between them, which
    fits the 13-wide panel exactly (pixel row 7 stays dark as a border).
    The carving is animated: the backtracker head glows bright while
    carved passages stay dim.

    Phase 2 (solve): an animated BFS floods from the top-left cell to
    the bottom-right cell — explored cells at brightness 2, the frontier
    at brightness 4. When the exit is reached the shortest path flashes
    at full brightness 7 while everything else fades to black.

    After a ~1.5 s pause the maze regenerates with a new seed, forever.
    Both phases are paced at ~15 steps/sec so they are watchable.

  Wiring: no external hardware needed — uses only the built-in matrix.

  Compile:
    arduino-cli compile -b arduino:zephyr:unoq ./matrix-maze-solver
  Upload:
    arduino-cli upload -b arduino:zephyr:unoq -p <board-ip> ./matrix-maze-solver
*/

#include "Arduino_LED_Matrix.h"

// ---------- Matrix geometry ----------
constexpr int kWidth  = 13;
constexpr int kHeight = 8;
constexpr int kPixels = kWidth * kHeight;  // 104

// ---------- Logical maze geometry (cells on even pixel coords) ----------
constexpr int kCellsW    = 7;                   // pixels 0,2,4,6,8,10,12
constexpr int kCellsH    = 4;                   // pixels 0,2,4,6
constexpr int kMazeCells = kCellsW * kCellsH;   // 28
constexpr int kStartCell = 0;                   // top-left
constexpr int kGoalCell  = kMazeCells - 1;      // bottom-right

// ---------- Brightness levels (0..7) ----------
constexpr uint8_t kCarveHeadBrightness = 7;
constexpr uint8_t kCarvedBrightness    = 1;
constexpr uint8_t kExploredBrightness  = 2;
constexpr uint8_t kFrontierBrightness  = 4;
constexpr uint8_t kPathBrightness      = 7;

// ---------- Timing ----------
constexpr uint32_t kStepIntervalMs  = 66;    // ~15 steps/sec for both phases
constexpr uint32_t kFlashIntervalMs = 120;   // path flash half-period
constexpr uint8_t  kFlashSteps      = 16;    // ~1.9 s of flashing/fading
constexpr uint32_t kPauseMs         = 1500;  // dark pause before regenerating

// ---------- Deterministic pseudo-randomness (small LCG) ----------
uint32_t lcgState = 0xC0FFEE01u;

uint32_t lcgNext() {
  lcgState = lcgState * 1664525u + 1013904223u;
  return lcgState >> 8;
}

// ---------- Cell helpers ----------
// Directions: 0=up, 1=right, 2=down, 3=left.
constexpr int8_t kDx[4] = { 0, 1, 0, -1 };
constexpr int8_t kDy[4] = { -1, 0, 1, 0 };

inline int cellX(int c) { return c % kCellsW; }
inline int cellY(int c) { return c / kCellsW; }
inline int cellIndex(int cx, int cy) { return cy * kCellsW + cx; }

// Pixel index of a cell's LED (cells sit at even pixel coordinates).
inline int cellPixel(int c) { return (cellY(c) * 2) * kWidth + (cellX(c) * 2); }

// Pixel index of the wall/passage LED between two adjacent cells.
inline int betweenPixel(int a, int b) {
  int px = cellX(a) + cellX(b);          // = 2*cx +/- 1 → the odd pixel between
  int py = cellY(a) + cellY(b);
  return py * kWidth + px;
}

// Neighbor of cell c in direction d, or -1 if off the grid.
int neighborCell(int c, int d) {
  int cx = cellX(c) + kDx[d];
  int cy = cellY(c) + kDy[d];
  if (cx < 0 || cx >= kCellsW || cy < 0 || cy >= kCellsH) return -1;
  return cellIndex(cx, cy);
}

// ---------- Maze / show state ----------
Arduino_LED_Matrix matrix;

enum class Phase : uint8_t { Carving, Solving, PathFlash, Pause };
Phase phase = Phase::Carving;

uint8_t openDirs[kMazeCells];   // bitmask per cell: bit d set = passage in dir d

// Carving (iterative backtracker)
bool    carved[kMazeCells];
uint8_t carveStack[kMazeCells];
int     carveTop = 0;           // stack size; head is carveStack[carveTop-1]

// Solving (BFS)
bool   bfsVisited[kMazeCells];
int8_t bfsParent[kMazeCells];
uint8_t bfsQueue[kMazeCells];
int    bfsHead = 0, bfsTail = 0;
bool   goalReached = false;

// Path flash
bool    onPath[kMazeCells];
uint8_t flashStep = 0;
bool    flashOn   = false;

uint32_t lastStepMs = 0;

// ---------- Phase transitions ----------
void startCarving() {
  memset(openDirs, 0, sizeof(openDirs));
  memset(carved, 0, sizeof(carved));
  carveStack[0] = (uint8_t)(lcgNext() % kMazeCells);  // random start cell
  carved[carveStack[0]] = true;
  carveTop   = 1;
  phase      = Phase::Carving;
  lastStepMs = millis();
}

void startSolving() {
  memset(bfsVisited, 0, sizeof(bfsVisited));
  bfsHead = bfsTail = 0;
  bfsVisited[kStartCell] = true;
  bfsParent[kStartCell]  = -1;
  bfsQueue[bfsTail++]    = kStartCell;
  goalReached = false;
  phase       = Phase::Solving;
}

void startPathFlash() {
  memset(onPath, 0, sizeof(onPath));
  for (int c = kGoalCell; c != -1; c = bfsParent[c]) onPath[c] = true;
  flashStep = 0;
  flashOn   = true;
  phase     = Phase::PathFlash;
}

// ---------- Phase steps ----------
// One backtracker step: carve toward a random unvisited neighbor, or backtrack.
void stepCarve() {
  if (carveTop == 0) {  // maze complete
    startSolving();
    return;
  }
  int head = carveStack[carveTop - 1];

  int choices[4];
  int nChoices = 0;
  for (int d = 0; d < 4; d++) {
    int n = neighborCell(head, d);
    if (n >= 0 && !carved[n]) choices[nChoices++] = d;
  }

  if (nChoices == 0) {
    carveTop--;  // dead end — backtrack
    return;
  }

  int d = choices[lcgNext() % (uint32_t)nChoices];
  int n = neighborCell(head, d);
  openDirs[head] |= (uint8_t)(1u << d);
  openDirs[n]    |= (uint8_t)(1u << ((d + 2) & 3));  // opposite direction
  carved[n] = true;
  carveStack[carveTop++] = (uint8_t)n;
}

// One BFS step: expand a single dequeued cell through open passages.
void stepSolve() {
  if (goalReached || bfsHead >= bfsTail) {  // solved (perfect maze: always solvable)
    startPathFlash();
    return;
  }
  int c = bfsQueue[bfsHead++];
  if (c == kGoalCell) {
    goalReached = true;
    return;
  }
  for (int d = 0; d < 4; d++) {
    if (!(openDirs[c] & (1u << d))) continue;
    int n = neighborCell(c, d);
    if (n < 0 || bfsVisited[n]) continue;
    bfsVisited[n]       = true;
    bfsParent[n]        = (int8_t)c;
    bfsQueue[bfsTail++] = (uint8_t)n;
  }
}

// ---------- Rendering ----------
// Light a cell's pixel and, per open passage to an already-lit-alike
// neighbor, the odd wall pixel between them (only paint upward, never dim).
void paintCell(uint8_t frame[kPixels], int c, uint8_t level) {
  int p = cellPixel(c);
  if (frame[p] < level) frame[p] = level;
}

void paintPassage(uint8_t frame[kPixels], int a, int b, uint8_t level) {
  int p = betweenPixel(a, b);
  if (frame[p] < level) frame[p] = level;
}

void renderCarving() {
  uint8_t frame[kPixels];
  memset(frame, 0, sizeof(frame));
  for (int c = 0; c < kMazeCells; c++) {
    if (!carved[c]) continue;
    paintCell(frame, c, kCarvedBrightness);
    for (int d = 0; d < 2; d++) {  // up/right only: each passage painted once
      if (openDirs[c] & (1u << d)) paintPassage(frame, c, neighborCell(c, d), kCarvedBrightness);
    }
  }
  if (carveTop > 0) paintCell(frame, carveStack[carveTop - 1], kCarveHeadBrightness);
  matrix.draw(frame);
}

// Base maze at kCarvedBrightness plus BFS overlay (explored + frontier).
void renderSolving() {
  uint8_t frame[kPixels];
  memset(frame, 0, sizeof(frame));
  for (int c = 0; c < kMazeCells; c++) {
    paintCell(frame, c, kCarvedBrightness);
    for (int d = 0; d < 2; d++) {
      if (openDirs[c] & (1u << d)) paintPassage(frame, c, neighborCell(c, d), kCarvedBrightness);
    }
  }
  for (int c = 0; c < kMazeCells; c++) {
    if (!bfsVisited[c]) continue;
    paintCell(frame, c, kExploredBrightness);
    if (bfsParent[c] >= 0) paintPassage(frame, c, bfsParent[c], kExploredBrightness);
  }
  for (int i = bfsHead; i < bfsTail; i++) {  // frontier = still queued
    int c = bfsQueue[i];
    paintCell(frame, c, kFrontierBrightness);
    if (bfsParent[c] >= 0) paintPassage(frame, c, bfsParent[c], kFrontierBrightness);
  }
  matrix.draw(frame);
}

// Shortest path blinking at 7 while the rest of the maze fades to black.
void renderPathFlash() {
  uint8_t fade = 0;  // non-path brightness, kExploredBrightness → 0
  if (flashStep < kFlashSteps) {
    fade = (uint8_t)((uint32_t)kExploredBrightness * (kFlashSteps - flashStep) / kFlashSteps);
  }

  uint8_t frame[kPixels];
  memset(frame, 0, sizeof(frame));
  if (fade > 0) {
    for (int c = 0; c < kMazeCells; c++) {
      paintCell(frame, c, fade);
      for (int d = 0; d < 2; d++) {
        if (openDirs[c] & (1u << d)) paintPassage(frame, c, neighborCell(c, d), fade);
      }
    }
  }
  if (flashOn) {
    for (int c = 0; c < kMazeCells; c++) {
      if (!onPath[c]) continue;
      paintCell(frame, c, kPathBrightness);
      if (bfsParent[c] >= 0 && onPath[bfsParent[c]]) {
        paintPassage(frame, c, bfsParent[c], kPathBrightness);
      }
    }
  }
  matrix.draw(frame);
}

void renderDark() {
  uint8_t frame[kPixels];
  memset(frame, 0, sizeof(frame));
  matrix.draw(frame);
}

// ---------- Arduino entry points ----------
void setup() {
  Serial.begin(115200);
  matrix.begin();
  matrix.setGrayscaleBits(3);
  lcgState ^= millis();  // tiny bit of run-to-run variety, still LCG-driven
  startCarving();
}

void loop() {
  uint32_t now = millis();

  switch (phase) {
    case Phase::Carving:
      if (now - lastStepMs >= kStepIntervalMs) {
        lastStepMs = now;
        stepCarve();
      }
      if (phase == Phase::Carving) renderCarving();
      break;

    case Phase::Solving:
      if (now - lastStepMs >= kStepIntervalMs) {
        lastStepMs = now;
        stepSolve();
      }
      if (phase == Phase::Solving) renderSolving();
      break;

    case Phase::PathFlash:
      if (now - lastStepMs >= kFlashIntervalMs) {
        lastStepMs = now;
        flashStep++;
        flashOn = !flashOn;
        if (flashStep >= kFlashSteps) {
          phase      = Phase::Pause;
          lastStepMs = now;
          renderDark();
          break;
        }
      }
      renderPathFlash();
      break;

    case Phase::Pause:
      if (now - lastStepMs >= kPauseMs) startCarving();  // new seed: LCG rolls on
      break;
  }
}
