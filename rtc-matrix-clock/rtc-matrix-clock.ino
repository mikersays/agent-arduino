/*
 * rtc-matrix-clock.ino — Binary-coded RTC clock on the Arduino UNO Q LED matrix
 *
 * What it does
 * ------------
 * Uses the STM32U585 hardware RTC (bundled RTC library, class `Rtc`) as the
 * timebase. At boot, if the RTC is not already running, it is initialized from
 * the sketch's compile timestamp (__DATE__ / __TIME__). Every second the full
 * date-time is printed to Serial (115200 baud), and the 13x8 LED matrix shows
 * a BCD "binary clock" plus a blinking heartbeat pixel.
 *
 * Matrix layout (13 columns x 8 rows, row 0 = top)
 * ------------------------------------------------
 * Each time digit is one column of bits, least-significant bit at the BOTTOM
 * row (row 7), growing upward. Lit bit = binary 1.
 *
 *   col  1 : hours   tens  (0..2, 2 bits)   \  hours group
 *   col  2 : hours   units (0..9, 4 bits)   /
 *   col  5 : minutes tens  (0..5, 3 bits)   \  minutes group
 *   col  6 : minutes units (0..9, 4 bits)   /
 *   col  9 : seconds tens  (0..5, 3 bits)   \  seconds group
 *   col 10 : seconds units (0..9, 4 bits)   /
 *
 * A dim marker on the bottom row under each group's columns acts as a baseline
 * so zero digits are still locatable. The heartbeat pixel at row 0, col 12
 * (top-right corner) blinks with a 1 Hz cadence (on for the first half of each
 * second).
 *
 * Wiring: no external hardware required.
 *
 * Build / deploy
 * --------------
 *   arduino-cli compile -b arduino:zephyr:unoq ./rtc-matrix-clock
 *   arduino-cli upload  -b arduino:zephyr:unoq -p <board-ip> ./rtc-matrix-clock
 */

#include "Arduino_LED_Matrix.h"
#include <RTC.h>

// ---------- Matrix geometry ----------
constexpr int kMatrixWidth  = 13;
constexpr int kMatrixHeight = 8;
constexpr int kBottomRow    = kMatrixHeight - 1;

// Digit column assignments (see header comment).
constexpr int kColHourTens   = 1;
constexpr int kColHourUnits  = 2;
constexpr int kColMinTens    = 5;
constexpr int kColMinUnits   = 6;
constexpr int kColSecTens    = 9;
constexpr int kColSecUnits   = 10;

// Heartbeat pixel position (top-right corner).
constexpr int kHeartbeatRow = 0;
constexpr int kHeartbeatCol = kMatrixWidth - 1;

// Brightness levels (0..7 with 3 grayscale bits).
constexpr uint8_t kBitBrightness       = 7;  // lit binary digit bits
constexpr uint8_t kBaselineBrightness  = 1;  // dim group baseline markers
constexpr uint8_t kHeartbeatBrightness = 5;

// ---------- Timing ----------
constexpr uint32_t kPollIntervalMs   = 50;   // how often we sample the RTC
constexpr uint32_t kHeartbeatOnMs    = 500;  // heartbeat on-time within each second

Arduino_LED_Matrix matrix;
Rtc rtc;

uint8_t frame[kMatrixWidth * kMatrixHeight];  // row-major, exactly 104 bytes

uint32_t lastPollMs = 0;
int lastPrintedSecond = -1;
uint32_t secondStartMs = 0;  // millis() when the current RTC second began

// ---------- Compile-time clock seed ----------

// Parse __DATE__ ("Jul  6 2026") and __TIME__ ("14:05:09") into calendar fields.
void parseCompileTimestamp(int &year, int &month, int &day,
                           int &hour, int &minute, int &second) {
  static const char kMonths[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
  const char *date = __DATE__;
  const char *time = __TIME__;

  month = 1;
  for (int m = 0; m < 12; m++) {
    if (strncmp(date, &kMonths[m * 3], 3) == 0) {
      month = m + 1;
      break;
    }
  }
  day    = atoi(date + 4);
  year   = atoi(date + 7);
  hour   = atoi(time);
  minute = atoi(time + 3);
  second = atoi(time + 6);
}

// ---------- Frame drawing ----------

void clearFrame() {
  memset(frame, 0, sizeof(frame));
}

void setPixel(int row, int col, uint8_t level) {
  if (row >= 0 && row < kMatrixHeight && col >= 0 && col < kMatrixWidth) {
    frame[row * kMatrixWidth + col] = level;
  }
}

// Draw one BCD digit as a vertical bit column, LSB on the bottom row.
void drawDigitColumn(int col, int value, int bitCount) {
  for (int bit = 0; bit < bitCount; bit++) {
    if (value & (1 << bit)) {
      setPixel(kBottomRow - bit, col, kBitBrightness);
    }
  }
}

// Dim baseline marker under a two-column digit group so zeros stay locatable.
void drawGroupBaseline(int tensCol, int unitsCol) {
  if (frame[kBottomRow * kMatrixWidth + tensCol] == 0) {
    setPixel(kBottomRow, tensCol, kBaselineBrightness);
  }
  if (frame[kBottomRow * kMatrixWidth + unitsCol] == 0) {
    setPixel(kBottomRow, unitsCol, kBaselineBrightness);
  }
}

void renderClock(int hour, int minute, int second, bool heartbeatOn) {
  clearFrame();

  drawDigitColumn(kColHourTens,  hour / 10,   2);
  drawDigitColumn(kColHourUnits, hour % 10,   4);
  drawDigitColumn(kColMinTens,   minute / 10, 3);
  drawDigitColumn(kColMinUnits,  minute % 10, 4);
  drawDigitColumn(kColSecTens,   second / 10, 3);
  drawDigitColumn(kColSecUnits,  second % 10, 4);

  drawGroupBaseline(kColHourTens, kColHourUnits);
  drawGroupBaseline(kColMinTens,  kColMinUnits);
  drawGroupBaseline(kColSecTens,  kColSecUnits);

  if (heartbeatOn) {
    setPixel(kHeartbeatRow, kHeartbeatCol, kHeartbeatBrightness);
  }

  matrix.draw(frame);
}

// ---------- Sketch ----------

void setup() {
  Serial.begin(115200);
  uint32_t serialWaitStart = millis();
  while (!Serial && (millis() - serialWaitStart) < 3000) {
    // Give the host a moment to attach, but do not block forever.
  }

  matrix.begin();
  matrix.setGrayscaleBits(3);

  if (!rtc.begin()) {
    Serial.println("ERROR: RTC failed to initialize");
    return;
  }

  if (!rtc.isRunning()) {
    int year, month, day, hour, minute, second;
    parseCompileTimestamp(year, month, day, hour, minute, second);
    rtc.setTime(year, month, day, hour, minute, second);
    Serial.print("RTC was not running; seeded from compile time: ");
    Serial.print(__DATE__);
    Serial.print(" ");
    Serial.println(__TIME__);
  } else {
    Serial.println("RTC already running; keeping stored time.");
  }
}

void loop() {
  uint32_t now = millis();
  if (now - lastPollMs < kPollIntervalMs) {
    return;
  }
  lastPollMs = now;

  int year, month, day, hour, minute, second;
  if (rtc.getTime(year, month, day, hour, minute, second) != 0) {
    return;  // transient read failure; try again next poll
  }

  if (second != lastPrintedSecond) {
    lastPrintedSecond = second;
    secondStartMs = now;

    char line[40];
    snprintf(line, sizeof(line), "%04d-%02d-%02d %02d:%02d:%02d",
             year, month, day, hour, minute, second);
    Serial.println(line);
  }

  bool heartbeatOn = (now - secondStartMs) < kHeartbeatOnMs;
  renderClock(hour, minute, second, heartbeatOn);
}
