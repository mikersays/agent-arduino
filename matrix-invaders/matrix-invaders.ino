/*
  matrix-invaders.ino — Self-playing Space Invaders on the Arduino UNO Q
  LED matrix (13x8).

  What it does:
    A 4x2 formation of invaders (brightness 3, spaced one cell apart)
    marches left and right across the built-in 13x8 blue LED matrix,
    dropping one row and reversing direction at the edges. The march
    speeds up as invaders are destroyed. An AI-driven player cannon
    (brightness 7) sits on the bottom row: it slides under the nearest
    column that still holds a live invader and fires a shot (brightness 6,
    one alive at a time, moving up). Invaders randomly drop bombs
    (brightness 2, moving down); when a bomb would land on the cannon it
    dodges sideways — dodging takes priority over aiming. Hits remove
    invaders with a brief bright flash.

    Win (all invaders cleared): a sweep-up celebration plays, then a new,
    faster wave begins. Lose (an invader reaches the bottom row, or bombs
    hit the cannon 3 times): the screen fades out and the game restarts
    from wave 1.

    Timing: projectiles and the cannon update on a ~15 Hz tick; the
    invader march runs on its own, slower timer.

  Wiring: no external hardware needed — uses only the built-in LED matrix.

  Compile:
    arduino-cli compile -b arduino:zephyr:unoq ./matrix-invaders
  Upload:
    arduino-cli upload -b arduino:zephyr:unoq -p <board-ip> ./matrix-invaders
*/

#include "Arduino_LED_Matrix.h"

// ---------- Matrix geometry ----------
constexpr int kWidth  = 13;
constexpr int kHeight = 8;
constexpr int kCells  = kWidth * kHeight;  // 104

// ---------- Formation geometry ----------
constexpr int kInvaderCols   = 4;  // invaders per row
constexpr int kInvaderRows   = 2;  // rows of invaders
constexpr int kInvaderCount  = kInvaderCols * kInvaderRows;  // 8
constexpr int kInvaderPitch  = 2;  // one empty cell between invaders
constexpr int kFormWidth     = (kInvaderCols - 1) * kInvaderPitch + 1;  // 7
constexpr int kCannonRow     = kHeight - 1;  // bottom row

// ---------- Brightness levels (0..7) ----------
constexpr uint8_t kInvaderBrightness = 3;
constexpr uint8_t kCannonBrightness  = 7;
constexpr uint8_t kShotBrightness    = 6;
constexpr uint8_t kBombBrightness    = 2;
constexpr uint8_t kFlashBrightness   = 7;
constexpr uint8_t kSweepBrightness   = 5;

// ---------- Timing ----------
constexpr uint32_t kTickMs          = 66;   // ~15 Hz projectile/cannon tick
constexpr uint32_t kMarchBaseMs     = 520;  // full-formation march interval, wave 1
constexpr uint32_t kMarchMinMs      = 140;  // fastest allowed march step
constexpr uint32_t kWaveSpeedupMs   = 60;   // base march speedup per wave
constexpr uint32_t kSweepStepMs     = 90;   // celebration row sweep speed
constexpr uint32_t kFadeStepMs      = 120;  // lose-fade brightness step
constexpr uint8_t  kFlashTicks      = 4;    // hit flash duration in ticks
constexpr uint8_t  kBombMoveDivider = 2;    // bombs move every N ticks

// ---------- Gameplay tuning ----------
constexpr int      kMaxBombs        = 3;
constexpr uint32_t kBombDropPercent = 35;  // chance per march step
constexpr int      kDodgeRange      = 4;   // bomb within this many rows above
constexpr uint8_t  kStartLives      = 3;
constexpr int      kMaxFlashes      = 4;
constexpr int      kSweepPasses     = 2;

// ---------- Types ----------
struct Bomb {
  bool   active;
  int8_t x, y;
};

struct Flash {
  uint8_t ticksLeft;  // 0 = slot free
  int8_t  x, y;
};

enum class GameState : uint8_t { Playing, Celebrating, Losing };

// ---------- Game state ----------
Arduino_LED_Matrix matrix;

GameState gameState = GameState::Playing;

bool    invaderAlive[kInvaderCount];
int     aliveCount   = kInvaderCount;
int8_t  formX        = 0;   // left edge of formation
int8_t  formY        = 0;   // top edge of formation
int8_t  marchDir     = 1;   // +1 right, -1 left
uint8_t wave         = 1;

int8_t  cannonX      = kWidth / 2;
uint8_t lives        = kStartLives;

bool    shotActive   = false;
int8_t  shotX = 0, shotY = 0;

Bomb    bombs[kMaxBombs];
Flash   flashes[kMaxFlashes];

uint32_t lastTickMs   = 0;
uint32_t lastMarchMs  = 0;
uint32_t lastAnimMs   = 0;   // celebration / fade pacing
uint8_t  tickCount    = 0;

int8_t  sweepRow      = 0;   // celebration state
uint8_t sweepPassesLeft = 0;
uint8_t fadeLevel     = 0;   // lose-fade state (7 -> 0)
uint8_t frozenFrame[kCells];  // snapshot faded out on lose

// ---------- Deterministic pseudo-randomness (small LCG) ----------
uint32_t lcgState = 0xC0FFEE01u;

uint32_t lcgNext() {
  lcgState = lcgState * 1664525u + 1013904223u;
  return lcgState >> 8;
}

// ---------- Invader helpers ----------
inline int invaderX(int i) { return formX + (i % kInvaderCols) * kInvaderPitch; }
inline int invaderY(int i) { return formY + (i / kInvaderCols) * kInvaderPitch; }

// Bottom-most row currently occupied by a live invader, or -1 if none.
int lowestAliveRow() {
  int lowest = -1;
  for (int i = 0; i < kInvaderCount; i++) {
    if (invaderAlive[i] && invaderY(i) > lowest) lowest = invaderY(i);
  }
  return lowest;
}

// March interval: shrinks with the wave number and with kills.
uint32_t marchIntervalMs() {
  uint32_t base = kMarchBaseMs;
  uint32_t speedup = (uint32_t)(wave - 1) * kWaveSpeedupMs;
  base = (base > speedup + kMarchMinMs) ? base - speedup : kMarchMinMs;
  if (aliveCount <= 1) return kMarchMinMs;
  uint32_t interval = kMarchMinMs +
      (base - kMarchMinMs) * (uint32_t)(aliveCount - 1) / (kInvaderCount - 1);
  return interval;
}

// ---------- Flash helpers ----------
void addFlash(int x, int y) {
  for (int i = 0; i < kMaxFlashes; i++) {
    if (flashes[i].ticksLeft == 0) {
      flashes[i] = { kFlashTicks, (int8_t)x, (int8_t)y };
      return;
    }
  }
}

// ---------- Game setup / reset ----------
void startWave(uint8_t newWave) {
  wave = newWave;
  for (int i = 0; i < kInvaderCount; i++) invaderAlive[i] = true;
  aliveCount = kInvaderCount;
  formX      = (int8_t)((kWidth - kFormWidth) / 2);
  formY      = 0;
  marchDir   = 1;
  shotActive = false;
  for (int i = 0; i < kMaxBombs; i++) bombs[i].active = false;
  for (int i = 0; i < kMaxFlashes; i++) flashes[i].ticksLeft = 0;
  gameState   = GameState::Playing;
  lastTickMs  = millis();
  lastMarchMs = lastTickMs;
}

void resetGame() {
  lives   = kStartLives;
  cannonX = kWidth / 2;
  startWave(1);
}

void startCelebration() {
  gameState       = GameState::Celebrating;
  sweepRow        = kHeight - 1;
  sweepPassesLeft = kSweepPasses;
  lastAnimMs      = millis() - kSweepStepMs;  // step immediately
}

void startLoseFade();

// ---------- Invader march ----------
void marchInvaders() {
  int nx = formX + marchDir;
  if (nx < 0 || nx + kFormWidth > kWidth) {
    marchDir = -marchDir;
    formY++;
    if (lowestAliveRow() >= kCannonRow) {  // invaders reached the bottom
      startLoseFade();
      return;
    }
  } else {
    formX = (int8_t)nx;
  }

  // Random bomb drop from a live invader.
  if (lcgNext() % 100 < kBombDropPercent) {
    int slot = -1;
    for (int i = 0; i < kMaxBombs; i++) {
      if (!bombs[i].active) { slot = i; break; }
    }
    if (slot >= 0 && aliveCount > 0) {
      int pick = (int)(lcgNext() % (uint32_t)aliveCount);
      for (int i = 0; i < kInvaderCount; i++) {
        if (!invaderAlive[i]) continue;
        if (pick-- == 0) {
          bombs[slot] = { true, (int8_t)invaderX(i), (int8_t)(invaderY(i) + 1) };
          break;
        }
      }
    }
  }
}

// ---------- Cannon AI ----------
// A bomb threatens the cannon if it shares a column and is close above it.
bool columnThreatened(int x) {
  for (int i = 0; i < kMaxBombs; i++) {
    if (!bombs[i].active) continue;
    if (bombs[i].x == x && kCannonRow - bombs[i].y <= kDodgeRange) return true;
  }
  return false;
}

void updateCannon() {
  // 1) Dodge: highest priority.
  if (columnThreatened(cannonX)) {
    bool leftOk  = cannonX > 0          && !columnThreatened(cannonX - 1);
    bool rightOk = cannonX < kWidth - 1 && !columnThreatened(cannonX + 1);
    if (leftOk && rightOk) cannonX += (lcgNext() & 1) ? 1 : -1;
    else if (leftOk)       cannonX--;
    else if (rightOk)      cannonX++;
    // Both sides blocked: stay and hope the shotgun spread misses.
    return;
  }

  // 2) Aim: slide under the nearest column with a live invader.
  int bestX = -1, bestDist = kWidth + 1;
  for (int i = 0; i < kInvaderCount; i++) {
    if (!invaderAlive[i]) continue;
    int dist = invaderX(i) - cannonX;
    if (dist < 0) dist = -dist;
    if (dist < bestDist) { bestDist = dist; bestX = invaderX(i); }
  }
  if (bestX < 0) return;

  if (cannonX < bestX)      cannonX++;
  else if (cannonX > bestX) cannonX--;

  // 3) Fire: one shot alive at a time, only when a live invader is overhead.
  if (!shotActive) {
    for (int i = 0; i < kInvaderCount; i++) {
      if (invaderAlive[i] && invaderX(i) == cannonX) {
        shotActive = true;
        shotX      = cannonX;
        shotY      = kCannonRow - 1;
        break;
      }
    }
  }
}

// ---------- Projectiles ----------
// Returns true if the shot at (shotX, shotY) killed an invader.
bool shotHitsInvader() {
  for (int i = 0; i < kInvaderCount; i++) {
    if (!invaderAlive[i]) continue;
    if (invaderX(i) == shotX && invaderY(i) == shotY) {
      invaderAlive[i] = false;
      aliveCount--;
      addFlash(shotX, shotY);
      return true;
    }
  }
  return false;
}

void updateShot() {
  if (!shotActive) return;
  if (shotHitsInvader()) { shotActive = false; return; }  // caught up by march
  shotY--;
  if (shotY < 0) { shotActive = false; return; }
  if (shotHitsInvader()) shotActive = false;
}

void updateBombs() {
  for (int i = 0; i < kMaxBombs; i++) {
    if (!bombs[i].active) continue;
    bombs[i].y++;
    if (bombs[i].y >= kHeight) { bombs[i].active = false; continue; }
    if (bombs[i].y == kCannonRow && bombs[i].x == cannonX) {
      bombs[i].active = false;
      addFlash(cannonX, kCannonRow);
      if (lives > 0) lives--;
      if (lives == 0) startLoseFade();
    }
  }
}

// ---------- Rendering ----------
void renderPlaying(uint8_t frame[kCells]) {
  memset(frame, 0, kCells);

  for (int i = 0; i < kInvaderCount; i++) {
    if (!invaderAlive[i]) continue;
    int x = invaderX(i), y = invaderY(i);
    if (y >= 0 && y < kHeight) frame[y * kWidth + x] = kInvaderBrightness;
  }
  for (int i = 0; i < kMaxBombs; i++) {
    if (bombs[i].active && bombs[i].y >= 0 && bombs[i].y < kHeight) {
      frame[bombs[i].y * kWidth + bombs[i].x] = kBombBrightness;
    }
  }
  if (shotActive) frame[shotY * kWidth + shotX] = kShotBrightness;
  frame[kCannonRow * kWidth + cannonX] = kCannonBrightness;

  for (int i = 0; i < kMaxFlashes; i++) {
    if (flashes[i].ticksLeft > 0) {
      frame[flashes[i].y * kWidth + flashes[i].x] = kFlashBrightness;
    }
  }
}

void startLoseFade() {
  renderPlaying(frozenFrame);  // snapshot the final scene
  gameState  = GameState::Losing;
  fadeLevel  = 7;
  lastAnimMs = millis();
}

// ---------- State updates ----------
void updatePlaying(uint32_t now) {
  if (now - lastMarchMs >= marchIntervalMs()) {
    lastMarchMs = now;
    marchInvaders();
    if (gameState != GameState::Playing) return;
  }

  if (now - lastTickMs >= kTickMs) {
    lastTickMs = now;
    tickCount++;

    updateShot();
    if (aliveCount == 0) { startCelebration(); return; }

    if (tickCount % kBombMoveDivider == 0) updateBombs();
    if (gameState != GameState::Playing) return;

    updateCannon();

    for (int i = 0; i < kMaxFlashes; i++) {
      if (flashes[i].ticksLeft > 0) flashes[i].ticksLeft--;
    }
  }

  uint8_t frame[kCells];
  renderPlaying(frame);
  matrix.draw(frame);
}

void updateCelebrating(uint32_t now) {
  if (now - lastAnimMs < kSweepStepMs) return;
  lastAnimMs = now;

  uint8_t frame[kCells];
  memset(frame, 0, sizeof(frame));
  for (int x = 0; x < kWidth; x++) frame[sweepRow * kWidth + x] = kSweepBrightness;
  matrix.draw(frame);

  if (--sweepRow < 0) {
    sweepRow = kHeight - 1;
    if (--sweepPassesLeft == 0) startWave((uint8_t)(wave < 255 ? wave + 1 : 255));
  }
}

void updateLosing(uint32_t now) {
  if (now - lastAnimMs < kFadeStepMs) return;
  lastAnimMs = now;

  uint8_t frame[kCells];
  for (int c = 0; c < kCells; c++) {
    frame[c] = (uint8_t)((frozenFrame[c] * fadeLevel) / 7);
  }
  matrix.draw(frame);

  if (fadeLevel == 0) resetGame();
  else fadeLevel--;
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
  switch (gameState) {
    case GameState::Playing:     updatePlaying(now);     break;
    case GameState::Celebrating: updateCelebrating(now); break;
    case GameState::Losing:      updateLosing(now);      break;
  }
}
