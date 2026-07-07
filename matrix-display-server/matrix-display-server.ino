/*
  matrix-display-server.ino — Serial-driven status display server for the
  Arduino UNO Q LED matrix.

  What it does:
    Turns the board into a generic status display peripheral. A host (a person
    in a serial monitor, or a script) drives the built-in 13x8 blue LED matrix
    with one-line commands at 115200 baud. Every command is answered with
    "OK" or "ERR <reason>". The frame is persistent state: PIX/ROW edit the
    current picture, TEXT scrolls a message once and then restores it, and
    BLINK flashes whatever is currently shown.

  Protocol (one command per line, case-insensitive command word):
    TEXT <msg>               scroll the message once, then restore the frame
    BAR <0-100>              horizontal progress bar on a dim backdrop row
    METER <a> <b>            two vertical 0-100 meters (left / right halves)
    ROW <y> <13 digits 0-7>  set one row's pixels directly
    PIX <x> <y> <0-7>        set one pixel (x 0-12, y 0-7, 0=top row)
    FILL <0-7>               fill the whole matrix at one brightness
    BLINK <n>                blink current content n times (clamped 1..10)
    CLEAR                    all pixels off
    HELP                     list commands

  Example session (host lines prefixed with >):
    > HELP
    ... command list ...
    OK
    > FILL 0
    OK
    > BAR 40
    OK
    > TEXT BUILD 40%
    OK                       (replies after the scroll finishes)
    > METER 75 30
    OK
    > PIX 6 0 7
    OK
    > ROW 7 0007770000700
    OK
    > BLINK 3
    OK
    > BAR 999
    ERR value out of range 0-100

  Driving it from Linux (copy-paste; interactive monitor over the network port):
    arduino-cli monitor -p <board-ip> -b arduino:zephyr:unoq -c baudrate=115200
    # then type commands, e.g.:  BAR 66   or   TEXT DEPLOY OK

  Wiring: no external hardware needed — uses only the built-in LED matrix
  and the bridged serial port.

  Boot state: a single dim "ready" indicator pixel in the bottom-right corner.

  Compile:
    arduino-cli compile -b arduino:zephyr:unoq ./matrix-display-server
  Upload:
    arduino-cli upload -b arduino:zephyr:unoq -p <board-ip> ./matrix-display-server
*/

#include "ArduinoGraphics.h"   // MUST come before Arduino_LED_Matrix.h
#include "Arduino_LED_Matrix.h"

#include <string.h>
#include <ctype.h>

// ---------- Matrix geometry ----------
constexpr int kWidth  = 13;                  // columns, x = 0..12
constexpr int kHeight = 8;                   // rows,    y = 0..7 (0 = top)
constexpr int kPixels = kWidth * kHeight;    // 104

// ---------- Brightness ----------
constexpr uint8_t kGrayscaleBits = 3;        // 8 levels
constexpr uint8_t kMaxBrightness = 7;        // per-pixel value 0..7
constexpr uint8_t kDimBackdrop   = 1;        // "unfilled" cells of BAR/METER

// ---------- BAR ----------
constexpr int     kBarRow    = 3;            // row the progress bar lives on
constexpr uint8_t kBarBright = kMaxBrightness;

// ---------- METER (two vertical meters on the left/right halves) ----------
constexpr int     kMeterLeftFirstCol  = 0;
constexpr int     kMeterLeftLastCol   = 5;   // cols 0..5  = meter A
constexpr int     kMeterRightFirstCol = 7;   // col 6 is the untouched gap
constexpr int     kMeterRightLastCol  = 12;  // cols 7..12 = meter B
constexpr uint8_t kMeterBright        = kMaxBrightness;

// ---------- BLINK ----------
constexpr int      kBlinkMin     = 1;
constexpr int      kBlinkMax     = 10;
constexpr uint32_t kBlinkPhaseMs = 140;      // off-time and on-time per blink

// ---------- TEXT ----------
constexpr uint32_t kScrollSpeedMs  = 60;     // ms per scroll step
constexpr uint8_t  kTextBrightness = 0xFF;   // blue-only matrix; R = brightness

// ---------- Boot "ready" indicator ----------
constexpr int     kReadyX      = 12;         // bottom-right corner
constexpr int     kReadyY      = 7;
constexpr uint8_t kReadyBright = 1;          // dim

// ---------- Serial protocol ----------
constexpr uint32_t kBaudRate    = 115200;
constexpr size_t   kLineBufLen  = 96;        // max command line length
constexpr int      kPercentMax  = 100;

Arduino_LED_Matrix matrix;

// Persistent frame: row-major, index y * kWidth + x, values 0..7.
static uint8_t frame[kPixels];
static const uint8_t kBlankFrame[kPixels] = { 0 };

// ---------- Frame helpers ----------

static void show() {
  matrix.draw(frame);
}

// Bounds-guarded pixel write into the persistent frame.
static void setPixel(int x, int y, uint8_t bright) {
  if (x < 0 || x >= kWidth || y < 0 || y >= kHeight) return;
  if (bright > kMaxBrightness) bright = kMaxBrightness;
  frame[y * kWidth + x] = bright;
}

// Round percent (0..100) to a cell count out of `cells`.
static int percentToCells(long percent, int cells) {
  return (int)((percent * cells + kPercentMax / 2) / kPercentMax);
}

// ---------- Replies ----------

static void replyOk() {
  Serial.println("OK");
}

static void replyErr(const char* reason) {
  Serial.print("ERR ");
  Serial.println(reason);
}

// ---------- Strict argument parsing ----------

// Parse a non-negative decimal integer; the whole token must be digits.
static bool parseUint(const char* s, long* out) {
  if (s == nullptr || *s == '\0') return false;
  long v = 0;
  for (const char* p = s; *p != '\0'; ++p) {
    if (!isdigit((unsigned char)*p)) return false;
    v = v * 10 + (*p - '0');
    if (v > 1000000L) return false;  // guard absurd values / overflow
  }
  *out = v;
  return true;
}

// Split `rest` (already past the command word) into exactly `want` space-
// separated tokens. Returns false if there are fewer or extra tokens.
static bool splitArgs(char* rest, char* argv[], int want) {
  int count = 0;
  char* p = rest;
  while (p != nullptr && *p != '\0') {
    while (*p == ' ') ++p;             // skip separators
    if (*p == '\0') break;
    if (count >= want) return false;   // extra token
    argv[count++] = p;
    while (*p != '\0' && *p != ' ') ++p;
    if (*p != '\0') *p++ = '\0';       // terminate token
  }
  return count == want;
}

// Parse one token as an integer in [lo, hi].
static bool parseRanged(const char* tok, long lo, long hi, long* out) {
  long v;
  if (!parseUint(tok, &v)) return false;
  if (v < lo || v > hi) return false;
  *out = v;
  return true;
}

// ---------- Command handlers ----------

static void cmdHelp() {
  Serial.println("matrix-display-server commands (one per line, reply OK/ERR):");
  Serial.println("  TEXT <msg>               scroll message once, then restore frame");
  Serial.println("  BAR <0-100>              progress bar on row 3 (dim backdrop)");
  Serial.println("  METER <a> <b>            two vertical 0-100 meters (left/right)");
  Serial.println("  ROW <y> <13 digits 0-7>  set one row, e.g. ROW 3 0007770000700");
  Serial.println("  PIX <x> <y> <0-7>        set one pixel (x 0-12, y 0-7, 0=top)");
  Serial.println("  FILL <0-7>               fill whole matrix");
  Serial.println("  BLINK <n>                blink current content n times (1-10)");
  Serial.println("  CLEAR                    all off");
  Serial.println("  HELP                     this list");
  replyOk();
}

static void cmdText(char* msg) {
  if (msg == nullptr || *msg == '\0') {
    replyErr("TEXT needs a message");
    return;
  }
  // Blocking scroll, then restore the persistent frame.
  matrix.beginText(0, 0, kTextBrightness, 0, 0);
  matrix.print(msg);
  matrix.endText(SCROLL_LEFT);
  show();
  replyOk();
}

static void cmdBar(char* rest) {
  char* argv[1];
  long pct;
  if (!splitArgs(rest, argv, 1)) {
    replyErr("usage: BAR <0-100>");
    return;
  }
  if (!parseRanged(argv[0], 0, kPercentMax, &pct)) {
    replyErr("value out of range 0-100");
    return;
  }
  const int lit = percentToCells(pct, kWidth);
  for (int x = 0; x < kWidth; ++x) {
    setPixel(x, kBarRow, (x < lit) ? kBarBright : kDimBackdrop);
  }
  show();
  replyOk();
}

// Draw one vertical meter across [firstCol..lastCol], filled bottom-up.
static void drawMeter(long pct, int firstCol, int lastCol) {
  const int litRows = percentToCells(pct, kHeight);
  for (int y = 0; y < kHeight; ++y) {
    const bool lit = (kHeight - y) <= litRows;  // y=7 is the bottom row
    for (int x = firstCol; x <= lastCol; ++x) {
      setPixel(x, y, lit ? kMeterBright : kDimBackdrop);
    }
  }
}

static void cmdMeter(char* rest) {
  char* argv[2];
  long a, b;
  if (!splitArgs(rest, argv, 2)) {
    replyErr("usage: METER <a 0-100> <b 0-100>");
    return;
  }
  if (!parseRanged(argv[0], 0, kPercentMax, &a) ||
      !parseRanged(argv[1], 0, kPercentMax, &b)) {
    replyErr("values out of range 0-100");
    return;
  }
  drawMeter(a, kMeterLeftFirstCol, kMeterLeftLastCol);
  drawMeter(b, kMeterRightFirstCol, kMeterRightLastCol);
  show();
  replyOk();
}

static void cmdRow(char* rest) {
  char* argv[2];
  long y;
  if (!splitArgs(rest, argv, 2)) {
    replyErr("usage: ROW <y> <13 digits 0-7>");
    return;
  }
  if (!parseRanged(argv[0], 0, kHeight - 1, &y)) {
    replyErr("row out of range 0-7");
    return;
  }
  const char* digits = argv[1];
  if (strlen(digits) != (size_t)kWidth) {
    replyErr("need exactly 13 digits");
    return;
  }
  for (int x = 0; x < kWidth; ++x) {
    if (digits[x] < '0' || digits[x] > '7') {
      replyErr("digits must be 0-7");
      return;
    }
  }
  for (int x = 0; x < kWidth; ++x) {
    setPixel(x, (int)y, (uint8_t)(digits[x] - '0'));
  }
  show();
  replyOk();
}

static void cmdPix(char* rest) {
  char* argv[3];
  long x, y, b;
  if (!splitArgs(rest, argv, 3)) {
    replyErr("usage: PIX <x> <y> <0-7>");
    return;
  }
  if (!parseRanged(argv[0], 0, kWidth - 1, &x)) {
    replyErr("x out of range 0-12");
    return;
  }
  if (!parseRanged(argv[1], 0, kHeight - 1, &y)) {
    replyErr("y out of range 0-7");
    return;
  }
  if (!parseRanged(argv[2], 0, kMaxBrightness, &b)) {
    replyErr("brightness out of range 0-7");
    return;
  }
  setPixel((int)x, (int)y, (uint8_t)b);
  show();
  replyOk();
}

static void cmdFill(char* rest) {
  char* argv[1];
  long b;
  if (!splitArgs(rest, argv, 1)) {
    replyErr("usage: FILL <0-7>");
    return;
  }
  if (!parseRanged(argv[0], 0, kMaxBrightness, &b)) {
    replyErr("brightness out of range 0-7");
    return;
  }
  memset(frame, (uint8_t)b, sizeof(frame));
  show();
  replyOk();
}

static void cmdBlink(char* rest) {
  char* argv[1];
  long n;
  if (!splitArgs(rest, argv, 1)) {
    replyErr("usage: BLINK <n>");
    return;
  }
  if (!parseUint(argv[0], &n)) {
    replyErr("count must be a number");
    return;
  }
  if (n < kBlinkMin) n = kBlinkMin;   // clamp 1..10 per protocol
  if (n > kBlinkMax) n = kBlinkMax;
  for (long i = 0; i < n; ++i) {
    matrix.draw(const_cast<uint8_t*>(kBlankFrame));
    delay(kBlinkPhaseMs);
    show();
    delay(kBlinkPhaseMs);
  }
  replyOk();
}

static void cmdClear(char* rest) {
  // CLEAR takes no arguments — reject trailing junk.
  if (rest != nullptr && *rest != '\0') {
    replyErr("CLEAR takes no arguments");
    return;
  }
  memset(frame, 0, sizeof(frame));
  show();
  replyOk();
}

// ---------- Line dispatch ----------

static void processLine(char* line) {
  // Trim leading spaces; ignore empty lines silently.
  while (*line == ' ') ++line;
  if (*line == '\0') return;

  // Trim trailing spaces.
  size_t len = strlen(line);
  while (len > 0 && line[len - 1] == ' ') line[--len] = '\0';

  // Split off the command word; `rest` is everything after it.
  char* rest = line;
  while (*rest != '\0' && *rest != ' ') ++rest;
  if (*rest != '\0') {
    *rest++ = '\0';
    while (*rest == ' ') ++rest;
  }

  // Case-insensitive command word.
  for (char* p = line; *p != '\0'; ++p) *p = (char)toupper((unsigned char)*p);

  if      (strcmp(line, "TEXT")  == 0) cmdText(rest);
  else if (strcmp(line, "BAR")   == 0) cmdBar(rest);
  else if (strcmp(line, "METER") == 0) cmdMeter(rest);
  else if (strcmp(line, "ROW")   == 0) cmdRow(rest);
  else if (strcmp(line, "PIX")   == 0) cmdPix(rest);
  else if (strcmp(line, "FILL")  == 0) cmdFill(rest);
  else if (strcmp(line, "BLINK") == 0) cmdBlink(rest);
  else if (strcmp(line, "CLEAR") == 0) cmdClear(rest);
  else if (strcmp(line, "HELP")  == 0) cmdHelp();
  else                                 replyErr("unknown command (try HELP)");
}

// Non-blocking serial line reader.
static void pollSerial() {
  static char buf[kLineBufLen];
  static size_t pos = 0;
  static bool overflow = false;

  while (Serial.available() > 0) {
    const char c = (char)Serial.read();
    if (c == '\r' || c == '\n') {
      if (overflow) {
        replyErr("line too long");
      } else if (pos > 0) {
        buf[pos] = '\0';
        processLine(buf);
      }
      pos = 0;
      overflow = false;
    } else if (pos < kLineBufLen - 1) {
      buf[pos++] = c;
    } else {
      overflow = true;  // discard until end of line
    }
  }
}

// ---------- Sketch ----------

void setup() {
  Serial.begin(kBaudRate);

  matrix.begin();
  matrix.setGrayscaleBits(kGrayscaleBits);
  matrix.textFont(Font_5x7);
  matrix.textScrollSpeed(kScrollSpeedMs);

  // Boot state: dim "ready" indicator pixel in the bottom-right corner.
  memset(frame, 0, sizeof(frame));
  setPixel(kReadyX, kReadyY, kReadyBright);
  show();

  Serial.println("matrix-display-server ready (13x8 matrix, brightness 0-7)");
  Serial.println("Type HELP for the command list.");
}

void loop() {
  pollSerial();
}
