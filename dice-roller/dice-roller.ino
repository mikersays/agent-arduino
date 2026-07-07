/*
  dice-roller.ino — Tabletop dice roller / decision maker on the Arduino UNO Q.

  What it does:
    Turns the built-in 13x8 blue LED matrix into a dice tray driven over the
    serial monitor (115200 baud). Every roll plays ~1 s of accelerating-then-
    decelerating random flicker before landing on the result:
      - d6 rolls show the classic pip patterns, centered on the matrix
      - every other die shows the number (static via NO_SCROLL for 1-2 digits;
        a result of 100 scrolls across once)
    The result is also printed to Serial, e.g. "d20 -> 17".

  Serial commands (case-insensitive, end with newline):
    ROLL d4|d6|d8|d10|d12|d20|d100   roll one die
    ROLL <N>d<S>                     e.g. ROLL 3d6 — N = 1..8 dice; each die is
                                     rolled and shown briefly, then the list
                                     and sum are printed and the sum displayed
    COIN                             flip a coin (HEADS/TAILS, matrix shows H/T)
    PICK <n>                         uniform pick of 1..n (n = 2..1000)
    AUTO ON | AUTO OFF               idle demo: a d6 rolls itself every ~8 s
    HELP                             reprint this command list
    (Bare "d20" or "2d6" without the ROLL keyword is accepted too.)

  Randomness (casual-fair, NOT cryptographic):
    A 32-bit LCG is seeded at boot from the low bits of repeated analogRead(A0)
    samples (A0 left floating picks up noise) mixed with micros(), and is
    re-stirred with fresh A0 low bits before every roll. Values are mapped to
    1..N with rejection sampling, so there is no modulo bias. This is plenty
    fair for board games — do not use it for anything security-related.

  Wiring: no external hardware needed — leave A0 floating for entropy.

  Compile:
    arduino-cli compile -b arduino:zephyr:unoq ./dice-roller
  Upload:
    arduino-cli upload -b arduino:zephyr:unoq -p <board-ip> ./dice-roller
*/

#include "ArduinoGraphics.h"    // MUST come before Arduino_LED_Matrix.h
#include "Arduino_LED_Matrix.h"

#include <ctype.h>
#include <string.h>
#include <stdlib.h>

// ---------- Configuration ----------
static const uint32_t SERIAL_BAUD       = 115200;
static const uint8_t  MATRIX_W          = 13;
static const uint8_t  MATRIX_H          = 8;
static const uint8_t  TEXT_ON           = 0xFF;  // blue-only matrix; R value = on
static const uint8_t  MAX_DICE          = 8;     // max count for NdS rolls
static const uint16_t PICK_MIN          = 2;
static const uint16_t PICK_MAX          = 1000;
static const uint32_t AUTO_INTERVAL_MS  = 8000;  // idle demo period
static const uint32_t HOLD_DIE_MS       = 650;   // per-die result hold in NdS rolls
static const uint32_t SCROLL_SPEED_MS   = 50;    // for 3+ digit results (blocking, once)
static const size_t   LINE_BUF_LEN      = 40;

// Flicker cadence: frame intervals in ms — fast in the middle (accelerate),
// slow at the end (decelerate). Single rolls get the full ~1 s ramp; each die
// of a multi-die roll gets the shorter ramp so 8d6 stays snappy.
static const uint16_t FLICKER_SINGLE_MS[] = {90, 70, 55, 45, 40, 40, 45, 55, 70, 95, 125, 160, 200};
static const uint16_t FLICKER_MULTI_MS[]  = {70, 50, 40, 45, 60, 90, 140};
static const uint8_t  FLICKER_SINGLE_N = sizeof(FLICKER_SINGLE_MS) / sizeof(FLICKER_SINGLE_MS[0]);
static const uint8_t  FLICKER_MULTI_N  = sizeof(FLICKER_MULTI_MS) / sizeof(FLICKER_MULTI_MS[0]);

// d6 pip geometry: 2x2 pip blocks. Columns: Left=2, Center=5, Right=9.
// Rows: Top=0, Middle=3, Bottom=6 (each block spans two rows/cols).
static const uint8_t PIP_COL_L = 2, PIP_COL_C = 5, PIP_COL_R = 9;
static const uint8_t PIP_ROW_T = 0, PIP_ROW_M = 3, PIP_ROW_B = 6;

// Pip mask bits: 0=TL 1=TR 2=ML 3=MR 4=BL 5=BR 6=Center. Index by face 1..6.
static const uint8_t PIP_MASK[7] = {
  0x00,  // (unused)
  0x40,  // 1: center
  0x21,  // 2: TL BR
  0x61,  // 3: TL center BR
  0x33,  // 4: TL TR BL BR
  0x73,  // 5: TL TR center BL BR
  0x3F,  // 6: TL TR ML MR BL BR
};

Arduino_LED_Matrix matrix;

// ---------- Entropy / RNG (casual-fair, not crypto) ----------

static uint32_t g_lcg = 0x2545F491UL;

static uint32_t lcgNext() {
  g_lcg = g_lcg * 1664525UL + 1013904223UL;
  return g_lcg;
}

// Fold a few floating-A0 low bits (plus the timer) into the LCG state.
static void stirEntropy(uint8_t reads) {
  for (uint8_t i = 0; i < reads; i++) {
    g_lcg ^= (uint32_t)(analogRead(A0) & 0x07) << ((i * 5) % 27);
    lcgNext();
  }
  g_lcg ^= micros();
  lcgNext();
}

// Uniform 1..n via rejection sampling on the LCG's top 16 bits (no modulo bias).
static uint16_t rollUniform(uint16_t n) {
  if (n < 2) return 1;
  const uint32_t limit = 65536UL - (65536UL % n);
  uint32_t r;
  do {
    r = lcgNext() >> 16;
  } while (r >= limit);
  return (uint16_t)(r % n) + 1;
}

// ---------- Matrix drawing ----------

static void canvasClear() {
  for (uint8_t y = 0; y < MATRIX_H; y++)
    for (uint8_t x = 0; x < MATRIX_W; x++)
      matrix.set(x, y, 0, 0, 0);
}

static void pipBlock(uint8_t x, uint8_t y) {  // 2x2 pip, top-left corner (x, y)
  matrix.set(x,     y,     TEXT_ON, 0, 0);
  matrix.set(x + 1, y,     TEXT_ON, 0, 0);
  matrix.set(x,     y + 1, TEXT_ON, 0, 0);
  matrix.set(x + 1, y + 1, TEXT_ON, 0, 0);
}

// Classic d6 pip pattern for face 1..6, centered on the matrix.
static void drawPips(uint8_t face) {
  const uint8_t m = PIP_MASK[face];
  matrix.beginDraw();
  canvasClear();
  if (m & 0x01) pipBlock(PIP_COL_L, PIP_ROW_T);
  if (m & 0x02) pipBlock(PIP_COL_R, PIP_ROW_T);
  if (m & 0x04) pipBlock(PIP_COL_L, PIP_ROW_M);
  if (m & 0x08) pipBlock(PIP_COL_R, PIP_ROW_M);
  if (m & 0x10) pipBlock(PIP_COL_L, PIP_ROW_B);
  if (m & 0x20) pipBlock(PIP_COL_R, PIP_ROW_B);
  if (m & 0x40) pipBlock(PIP_COL_C, PIP_ROW_M);
  matrix.endDraw();
}

// Static 1-2 character text, centered (Font_5x7: 2 chars fit on 13 px).
static void showTextStatic(const char* s) {
  const int x = (strlen(s) == 1) ? 4 : 1;
  matrix.beginText(x, 0, TEXT_ON, 0, 0);
  matrix.print(s);
  matrix.endText(NO_SCROLL);
}

// Show a number: static for 1-2 digits, one blocking left-scroll for 3+.
static void showNumber(uint16_t v) {
  char buf[8];
  snprintf(buf, sizeof(buf), "%u", (unsigned)v);
  if (v <= 99) {
    showTextStatic(buf);
  } else {
    matrix.beginText(0, 0, TEXT_ON, 0, 0);
    matrix.print(buf);
    matrix.endText(SCROLL_LEFT);  // blocks for one pass, then screen is clear
  }
}

// ---------- Roll job / animation state machine ----------

enum Mode  { MODE_DICE, MODE_COIN, MODE_PICK };
enum Phase { PH_IDLE, PH_FLICKER, PH_HOLD_DIE };

static Mode     g_mode      = MODE_DICE;
static Phase    g_phase     = PH_IDLE;
static uint8_t  g_count     = 1;       // number of dice in this job
static uint16_t g_sides     = 6;       // die sides / pick range / 2 for coin
static uint16_t g_results[MAX_DICE];
static uint8_t  g_dieIdx    = 0;       // die currently animating
static uint8_t  g_flickStep = 0;
static uint32_t g_phaseAt   = 0;       // millis() of last phase step
static bool     g_isAuto    = false;   // current job came from the idle demo
static bool     g_autoMode  = false;
static uint32_t g_lastDoneAt = 0;      // when the last job finished (auto timer)

static const uint16_t* flickerTable()  { return (g_count > 1) ? FLICKER_MULTI_MS : FLICKER_SINGLE_MS; }
static uint8_t         flickerSteps()  { return (g_count > 1) ? FLICKER_MULTI_N  : FLICKER_SINGLE_N;  }

// One random in-between frame of the rolling animation.
static void renderFlickerFrame() {
  char buf[4];
  switch (g_mode) {
    case MODE_COIN:
      showTextStatic(rollUniform(2) == 1 ? "H" : "T");
      break;
    case MODE_DICE:
      if (g_sides == 6) {
        drawPips((uint8_t)rollUniform(6));
        break;
      }
      // fall through to number flash for d4/d8/d10/d12/d20/d100
    case MODE_PICK: {
      const uint16_t span = (g_sides > 99) ? 99 : g_sides;  // keep it 2 digits
      snprintf(buf, sizeof(buf), "%u", (unsigned)rollUniform(span));
      showTextStatic(buf);
      break;
    }
  }
}

// Land the display on one die's final value.
static void showDieResult(uint16_t value) {
  if (g_mode == MODE_COIN) {
    showTextStatic(value == 1 ? "H" : "T");
  } else if (g_mode == MODE_DICE && g_sides == 6) {
    drawPips((uint8_t)value);
  } else {
    showNumber(value);
  }
}

// Print the Serial result line and, for multi-die rolls, display the sum.
static void finishJob() {
  if (g_isAuto) Serial.print("[auto] ");
  if (g_mode == MODE_COIN) {
    Serial.print("COIN -> ");
    Serial.println(g_results[0] == 1 ? "HEADS" : "TAILS");
  } else if (g_mode == MODE_PICK) {
    Serial.print("PICK ");
    Serial.print(g_sides);
    Serial.print(" -> ");
    Serial.println(g_results[0]);
  } else if (g_count == 1) {
    Serial.print("d");
    Serial.print(g_sides);
    Serial.print(" -> ");
    Serial.println(g_results[0]);
  } else {
    uint16_t sum = 0;
    Serial.print(g_count);
    Serial.print("d");
    Serial.print(g_sides);
    Serial.print(" -> ");
    for (uint8_t i = 0; i < g_count; i++) {
      if (i) Serial.print(" + ");
      Serial.print(g_results[i]);
      sum += g_results[i];
    }
    Serial.print(" = ");
    Serial.println(sum);
    showNumber(sum);  // sums over 99 (e.g. 8d20) scroll once
  }
  g_phase = PH_IDLE;
  g_lastDoneAt = millis();
}

// Roll everything up front, then start the flicker animation.
// (mode is a Mode value; typed uint8_t so the Arduino preprocessor's
// auto-generated prototype compiles before the enum definition.)
static void startJob(uint8_t mode, uint8_t count, uint16_t sides, bool isAuto) {
  stirEntropy(4);
  g_mode   = (Mode)mode;
  g_count  = count;
  g_sides  = sides;
  g_isAuto = isAuto;
  for (uint8_t i = 0; i < count; i++) g_results[i] = rollUniform(sides);
  g_dieIdx    = 0;
  g_flickStep = 0;
  g_phase     = PH_FLICKER;
  g_phaseAt   = millis();
  renderFlickerFrame();
}

static void updateAnimation() {
  const uint32_t now = millis();
  switch (g_phase) {
    case PH_IDLE:
      if (g_autoMode && (now - g_lastDoneAt) >= AUTO_INTERVAL_MS) {
        startJob(MODE_DICE, 1, 6, true);
      }
      break;

    case PH_FLICKER:
      if (now - g_phaseAt >= flickerTable()[g_flickStep]) {
        g_phaseAt = now;
        g_flickStep++;
        if (g_flickStep < flickerSteps()) {
          renderFlickerFrame();
        } else {
          showDieResult(g_results[g_dieIdx]);  // landed
          if (g_count > 1) {
            g_phase = PH_HOLD_DIE;  // hold every die, including the last
          } else {
            finishJob();
          }
        }
      }
      break;

    case PH_HOLD_DIE:
      if (now - g_phaseAt >= HOLD_DIE_MS) {
        if (g_dieIdx + 1 < g_count) {
          g_dieIdx++;
          g_flickStep = 0;
          g_phaseAt   = now;
          g_phase     = PH_FLICKER;
          renderFlickerFrame();
        } else {
          finishJob();  // last die had its hold — now print list + show sum
        }
      }
      break;
  }
}

// ---------- Command parsing ----------

static void printHelp() {
  Serial.println();
  Serial.println("UNO Q Dice Roller — commands (case-insensitive):");
  Serial.println("  ROLL d4|d6|d8|d10|d12|d20|d100   roll one die");
  Serial.println("  ROLL <N>d<S>   e.g. ROLL 3d6 (N = 1..8): list + sum");
  Serial.println("  COIN           flip a coin (H/T on the matrix)");
  Serial.println("  PICK <n>       uniform 1..n (n = 2..1000)");
  Serial.println("  AUTO ON|OFF    idle demo: a d6 rolls itself every ~8 s");
  Serial.println("  HELP           show this list");
  Serial.println("RNG: A0-noise-seeded LCG + rejection sampling — casual-fair, not crypto.");
}

static bool validSides(uint16_t s) {
  return s == 4 || s == 6 || s == 8 || s == 10 || s == 12 || s == 20 || s == 100;
}

// Parse "D6" / "2D6" (already uppercased). Returns false if malformed.
static bool parseDiceSpec(const char* s, uint8_t& count, uint16_t& sides) {
  if (s == NULL || *s == '\0') return false;
  uint32_t n = 0;
  const char* p = s;
  while (isdigit((unsigned char)*p)) {
    n = n * 10 + (uint32_t)(*p++ - '0');
    if (n > 1000) return false;  // absurd count — also blocks uint32 wraparound
  }
  const uint32_t cnt = (p == s) ? 1 : n;  // no leading digits -> 1 die
  if (*p++ != 'D') return false;
  if (!isdigit((unsigned char)*p)) return false;
  uint32_t sd = 0;
  while (isdigit((unsigned char)*p)) {
    sd = sd * 10 + (uint32_t)(*p++ - '0');
    if (sd > 1000) return false;  // absurd sides — also blocks uint32 wraparound
  }
  if (*p != '\0') return false;
  if (cnt < 1 || cnt > MAX_DICE || !validSides((uint16_t)sd)) return false;
  count = (uint8_t)cnt;
  sides = (uint16_t)sd;
  return true;
}

static void handleLine(char* line) {
  char* cmd = strtok(line, " \t");
  if (cmd == NULL) return;
  char* arg = strtok(NULL, " \t");
  uint8_t  count;
  uint16_t sides;

  if (strcmp(cmd, "HELP") == 0) {
    printHelp();
  } else if (strcmp(cmd, "ROLL") == 0) {
    if (parseDiceSpec(arg, count, sides)) {
      startJob(MODE_DICE, count, sides, false);
    } else {
      Serial.println("ERR: usage ROLL d4|d6|d8|d10|d12|d20|d100 or ROLL <1-8>d<S>");
    }
  } else if (strcmp(cmd, "COIN") == 0) {
    startJob(MODE_COIN, 1, 2, false);
  } else if (strcmp(cmd, "PICK") == 0) {
    const long n = (arg != NULL) ? atol(arg) : 0;
    if (n >= PICK_MIN && n <= PICK_MAX) {
      startJob(MODE_PICK, 1, (uint16_t)n, false);
    } else {
      Serial.println("ERR: usage PICK <n> with n = 2..1000");
    }
  } else if (strcmp(cmd, "AUTO") == 0) {
    if (arg != NULL && strcmp(arg, "ON") == 0) {
      g_autoMode = true;
      g_lastDoneAt = millis();
      Serial.println("AUTO on — idle d6 every ~8 s");
    } else if (arg != NULL && strcmp(arg, "OFF") == 0) {
      g_autoMode = false;
      Serial.println("AUTO off");
    } else {
      Serial.println("ERR: usage AUTO ON|OFF");
    }
  } else if (parseDiceSpec(cmd, count, sides)) {  // bare "d20" / "2d6" shortcut
    startJob(MODE_DICE, count, sides, false);
  } else {
    Serial.print("ERR: unknown command '");
    Serial.print(cmd);
    Serial.println("' — try HELP");
  }
}

// Non-blocking line reader: accepts \r, \n or \r\n, uppercases as it goes.
static void pollSerial() {
  static char    buf[LINE_BUF_LEN];
  static uint8_t len = 0;
  while (Serial.available() > 0) {
    const char c = (char)Serial.read();
    if (c == '\r' || c == '\n') {
      if (len > 0) {
        buf[len] = '\0';
        len = 0;
        handleLine(buf);
      }
    } else if (len < LINE_BUF_LEN - 1) {
      buf[len++] = (char)toupper((unsigned char)c);
    }
  }
}

// ---------- Sketch ----------

void setup() {
  Serial.begin(SERIAL_BAUD);

  matrix.begin();
  matrix.textFont(Font_5x7);
  matrix.textScrollSpeed(SCROLL_SPEED_MS);

  // Seed the LCG: floating-A0 noise low bits folded together with micros().
  for (uint8_t i = 0; i < 16; i++) {
    g_lcg = g_lcg * 31 + (uint32_t)(analogRead(A0) & 0x0F);
    lcgNext();
  }
  g_lcg ^= micros();
  lcgNext();

  printHelp();

  drawPips(6);  // something friendly on the tray while idle
  g_lastDoneAt = millis();
}

void loop() {
  pollSerial();
  updateAnimation();
}
