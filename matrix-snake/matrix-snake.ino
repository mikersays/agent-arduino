/*
  matrix-snake.ino — Self-playing Snake on the Arduino UNO Q LED matrix (13x8).

  What it does:
    A snake plays itself on the built-in 13x8 blue LED matrix. Each move it
    runs a BFS from its head to the food across the 104-cell grid, avoiding
    its own body. If no path to the food exists, it falls back to the safe
    move whose resulting region (flood fill) has the most free cells — a
    simple survival heuristic. Eating food grows the snake by one segment.
    When the snake is fully trapped (no safe move), the whole board flashes
    a few times and the game restarts.

    Rendering: head at brightness 7, body at brightness 3, food blinks
    between bright and dim so it is easy to spot.

  Wiring: no external hardware — uses only the built-in LED matrix.

  Compile:
    arduino-cli compile -b arduino:zephyr:unoq ./matrix-snake
  Upload:
    arduino-cli upload -b arduino:zephyr:unoq -p <board-ip> ./matrix-snake
*/

#include "Arduino_LED_Matrix.h"

// ---------- Matrix geometry ----------
constexpr int kWidth  = 13;
constexpr int kHeight = 8;
constexpr int kCells  = kWidth * kHeight;  // 104

// ---------- Brightness levels (0..7) ----------
constexpr uint8_t kHeadBrightness     = 7;
constexpr uint8_t kBodyBrightness     = 3;
constexpr uint8_t kFoodBrightBright   = 7;
constexpr uint8_t kFoodBrightDim      = 1;
constexpr uint8_t kFlashBrightness    = 5;

// ---------- Timing ----------
constexpr uint32_t kMoveIntervalMs   = 150;  // ~6.7 cells/sec
constexpr uint32_t kFoodBlinkMs      = 120;  // food blink half-period
constexpr uint32_t kFlashIntervalMs  = 180;  // death flash half-period
constexpr uint8_t  kFlashHalfCycles  = 6;    // 3 on/off flashes

// ---------- Game state ----------
Arduino_LED_Matrix matrix;

enum class GameState : uint8_t { Playing, Flashing };
GameState gameState = GameState::Playing;

// Snake body as cell indices; index 0 is the head.
uint8_t snakeCells[kCells];
int     snakeLength = 0;
uint8_t foodCell    = 0;

uint32_t lastMoveMs    = 0;
uint32_t lastFlashMs   = 0;
uint8_t  flashesLeft   = 0;
bool     flashOn       = false;

// ---------- Deterministic pseudo-randomness (small LCG) ----------
uint32_t lcgState = 0x1234ABCDu;

uint32_t lcgNext() {
  lcgState = lcgState * 1664525u + 1013904223u;
  return lcgState >> 8;
}

// ---------- Grid helpers ----------
inline int cellX(uint8_t c) { return c % kWidth; }
inline int cellY(uint8_t c) { return c / kWidth; }
inline uint8_t cellIndex(int x, int y) { return (uint8_t)(y * kWidth + x); }

// Neighbor of cell c in direction d (0=up,1=right,2=down,3=left).
// Returns -1 if off the board.
int neighborCell(uint8_t c, int d) {
  int x = cellX(c);
  int y = cellY(c);
  switch (d) {
    case 0: y--; break;
    case 1: x++; break;
    case 2: y++; break;
    default: x--; break;
  }
  if (x < 0 || x >= kWidth || y < 0 || y >= kHeight) return -1;
  return cellIndex(x, y);
}

// Mark cells occupied by the snake body. If ignoreTail is true, the tail
// cell is treated as free (it vacates on the next non-growing move).
void buildOccupied(bool occupied[kCells], bool ignoreTail) {
  memset(occupied, 0, kCells);
  int last = ignoreTail ? snakeLength - 1 : snakeLength;
  for (int i = 0; i < last; i++) occupied[snakeCells[i]] = true;
}

// ---------- Pathfinding ----------
// BFS from the head toward the food, avoiding the body.
// Returns the first-step cell toward the food, or -1 if unreachable.
int bfsFirstStepToFood() {
  bool occupied[kCells];
  // The head will move; only eating extends past the tail, and only when
  // the food is adjacent — treating the tail as occupied stays safe.
  buildOccupied(occupied, false);

  uint8_t queue[kCells];
  int16_t parent[kCells];
  bool    visited[kCells] = {false};
  int qHead = 0, qTail = 0;

  uint8_t start = snakeCells[0];
  visited[start] = true;
  parent[start]  = -1;
  queue[qTail++] = start;

  while (qHead < qTail) {
    uint8_t c = queue[qHead++];
    if (c == foodCell) {
      // Walk back to the cell adjacent to the head.
      uint8_t step = c;
      while (parent[step] != (int16_t)start) step = (uint8_t)parent[step];
      return step;
    }
    for (int d = 0; d < 4; d++) {
      int n = neighborCell(c, d);
      if (n < 0 || visited[n] || occupied[n]) continue;
      visited[n]      = true;
      parent[n]       = c;
      queue[qTail++]  = (uint8_t)n;
    }
  }
  return -1;
}

// Flood-fill count of free cells reachable from 'from' (which must be free).
int floodFillCount(const bool occupied[kCells], uint8_t from) {
  bool visited[kCells] = {false};
  uint8_t queue[kCells];
  int qHead = 0, qTail = 0;
  visited[from]  = true;
  queue[qTail++] = from;
  int count = 0;
  while (qHead < qTail) {
    uint8_t c = queue[qHead++];
    count++;
    for (int d = 0; d < 4; d++) {
      int n = neighborCell(c, d);
      if (n < 0 || visited[n] || occupied[n]) continue;
      visited[n]     = true;
      queue[qTail++] = (uint8_t)n;
    }
  }
  return count;
}

// Survival fallback: pick the safe neighbor whose region has the most
// free space. Returns the chosen cell, or -1 if fully trapped.
int survivalMove() {
  bool occupied[kCells];
  buildOccupied(occupied, true);  // tail vacates on a non-growing move

  int bestCell  = -1;
  int bestSpace = -1;
  for (int d = 0; d < 4; d++) {
    int n = neighborCell(snakeCells[0], d);
    if (n < 0 || occupied[n]) continue;
    int space = floodFillCount(occupied, (uint8_t)n);
    if (space > bestSpace) {
      bestSpace = space;
      bestCell  = n;
    }
  }
  return bestCell;
}

// ---------- Game logic ----------
void placeFood() {
  bool occupied[kCells];
  buildOccupied(occupied, false);
  int freeCount = kCells - snakeLength;
  if (freeCount <= 0) { foodCell = snakeCells[0]; return; }  // board full
  int pick = (int)(lcgNext() % (uint32_t)freeCount);
  for (int c = 0; c < kCells; c++) {
    if (occupied[c]) continue;
    if (pick-- == 0) { foodCell = (uint8_t)c; return; }
  }
}

void resetGame() {
  snakeLength   = 3;
  snakeCells[0] = cellIndex(6, 4);  // head near center, body to the left
  snakeCells[1] = cellIndex(5, 4);
  snakeCells[2] = cellIndex(4, 4);
  placeFood();
  gameState  = GameState::Playing;
  lastMoveMs = millis();
}

void startDeathFlash() {
  gameState   = GameState::Flashing;
  flashesLeft = kFlashHalfCycles;
  flashOn     = false;
  lastFlashMs = millis() - kFlashIntervalMs;  // flash immediately
}

void stepSnake() {
  int next = bfsFirstStepToFood();
  if (next < 0) next = survivalMove();
  if (next < 0) {  // fully trapped
    startDeathFlash();
    return;
  }

  bool ate = ((uint8_t)next == foodCell);
  if (ate && snakeLength < kCells) snakeLength++;

  // Shift body back one cell and place the new head.
  for (int i = snakeLength - 1; i > 0; i--) snakeCells[i] = snakeCells[i - 1];
  snakeCells[0] = (uint8_t)next;

  if (ate) {
    if (snakeLength >= kCells) {  // won the whole board — celebrate & restart
      startDeathFlash();
      return;
    }
    placeFood();
  }
}

// ---------- Rendering ----------
void renderGame() {
  uint8_t frame[kCells];
  memset(frame, 0, sizeof(frame));

  for (int i = 1; i < snakeLength; i++) frame[snakeCells[i]] = kBodyBrightness;
  frame[snakeCells[0]] = kHeadBrightness;

  bool blinkPhase = (millis() / kFoodBlinkMs) & 1;
  frame[foodCell] = blinkPhase ? kFoodBrightBright : kFoodBrightDim;

  matrix.draw(frame);
}

void renderFlash(bool on) {
  uint8_t frame[kCells];
  memset(frame, on ? kFlashBrightness : 0, sizeof(frame));
  matrix.draw(frame);
}

// ---------- Arduino entry points ----------
void setup() {
  Serial.begin(115200);
  matrix.begin();
  matrix.setGrayscaleBits(3);
  resetGame();
}

void loop() {
  uint32_t now = millis();

  if (gameState == GameState::Playing) {
    if (now - lastMoveMs >= kMoveIntervalMs) {
      lastMoveMs = now;
      stepSnake();
    }
    if (gameState == GameState::Playing) renderGame();
  } else {  // Flashing
    if (now - lastFlashMs >= kFlashIntervalMs) {
      lastFlashMs = now;
      flashOn = !flashOn;
      renderFlash(flashOn);
      if (--flashesLeft == 0) resetGame();
    }
  }
}
