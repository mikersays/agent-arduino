/*
 * serial-stopwatch.ino — Serial-driven stopwatch / lap timer on the Arduino UNO Q
 *
 * What it does
 * ------------
 * A millisecond-exact stopwatch controlled over Serial (115200 baud) with the
 * live time visualized on the 13x8 LED matrix. Elapsed time uses an
 * accumulated-base + running-anchor design: while running, elapsed =
 * accumulatedMs + (millis() - anchorMs); STOP folds the running span into
 * accumulatedMs, so STOP/START never loses or double-counts a millisecond.
 * Times print as MM:SS.mmm.
 *
 * Serial commands (case-insensitive, end with newline)
 * ----------------------------------------------------
 *   START  - start / resume the stopwatch
 *   STOP   - pause (elapsed time is kept)
 *   RESET  - stop, zero the elapsed time, clear all laps
 *   LAP    - record a lap; prints lap number, lap time, total time (max 20 kept)
 *   LAPS   - reprint the full lap table
 *   STATUS - print state, elapsed time, lap count
 *
 * Matrix layout (13 columns x 8 rows, row 0 = top)
 * ------------------------------------------------
 *   Row 0            : run/pause indicator pixel at col 12 (top-right):
 *                      blinks bright at 1 Hz while running, solid dim when
 *                      paused/idle. On LAP the entire row 0 flashes at full
 *                      brightness for a moment as visual confirmation.
 *   Rows 1..6        : minutes, mixed-radix base-60, as up to 3 binary-coded
 *                      6-bit columns, LSB at the bottom (row 6), lit = 1:
 *                        col 6 : m % 60        (minutes within the hour, 0-59)
 *                        col 5 : (m / 60) % 60 (hours, 0-59)
 *                        col 4 : m / 3600      (60-hour units)
 *                      Higher columns appear only once nonzero; this covers
 *                      999 minutes (16h39m -> cols 5,6) and far beyond. A dim
 *                      baseline dot under col 6 keeps zero locatable.
 *   Row 7 (bottom)   : seconds progress bar. The 0-59.999 s position within
 *                      the current minute maps onto 13 pixels; whole pixels
 *                      are full brightness and the leading fractional pixel
 *                      glows dimmer in proportion, so the bar crawls smoothly.
 *
 * Wiring: no external hardware needed.
 *
 * Build / deploy
 * --------------
 *   arduino-cli compile -b arduino:zephyr:unoq ./serial-stopwatch
 *   arduino-cli upload  -b arduino:zephyr:unoq -p <board-ip> ./serial-stopwatch
 */

#include "Arduino_LED_Matrix.h"

// ---------- Matrix geometry ----------
constexpr int kMatrixWidth  = 13;
constexpr int kMatrixHeight = 8;
constexpr int kIndicatorRow = 0;                  // run/pause + lap-flash row
constexpr int kBottomRow    = kMatrixHeight - 1;  // seconds progress bar

// Minutes group: three 6-bit binary columns, bits on rows 1..6.
constexpr int kMinutesBitCount = 6;
constexpr int kMinutesLsbRow   = 6;   // LSB row; MSB ends up on row 1
constexpr int kColMinUnits     = 6;   // m % 60
constexpr int kColMinSixties   = 5;   // (m / 60) % 60  (hours)
constexpr int kColMin3600s     = 4;   // m / 3600       (60-hour units)

// Indicator pixel (top-right corner).
constexpr int kIndicatorCol = kMatrixWidth - 1;

// Brightness levels (0..7 with 3 grayscale bits).
constexpr uint8_t kFullBrightness     = 7;  // bar pixels, minute bits, flashes
constexpr uint8_t kPausedBrightness   = 2;  // solid dim indicator when paused
constexpr uint8_t kBaselineBrightness = 1;  // dim baseline under minutes group

// ---------- Timing ----------
constexpr uint32_t kFrameIntervalMs = 25;    // matrix refresh cadence
constexpr uint32_t kBlinkPeriodMs   = 1000;  // running indicator blink period
constexpr uint32_t kBlinkOnMs       = 500;   // indicator on-time per period
constexpr uint32_t kLapFlashMs      = 200;   // full-row flash duration on LAP
constexpr uint32_t kMsPerMinute     = 60000;

// ---------- Stopwatch / laps ----------
constexpr int kMaxLaps = 20;

Arduino_LED_Matrix matrix;
uint8_t frame[kMatrixWidth * kMatrixHeight];  // row-major, exactly 104 bytes

bool     running       = false;
uint32_t accumulatedMs = 0;  // elapsed time banked by STOP
uint32_t anchorMs      = 0;  // millis() at the most recent START

uint32_t lapTotalMs[kMaxLaps];  // total elapsed at the moment of each lap
int      lapCount = 0;

uint32_t lastFrameMs   = 0;
uint32_t lapFlashUntil = 0;  // millis() deadline for the LAP row flash
bool     lapFlashArmed = false;

// ---------- Serial line reader (non-blocking) ----------
constexpr size_t kLineBufSize = 32;
char   lineBuf[kLineBufSize];
size_t lineLen = 0;

// ---------- Elapsed time ----------

// Total elapsed ms right now. Rollover-safe: (now - anchorMs) is well defined
// in unsigned arithmetic even across the 32-bit millis() wrap.
uint32_t elapsedMs() {
  uint32_t total = accumulatedMs;
  if (running) {
    total += millis() - anchorMs;
  }
  return total;
}

// Format ms as MM:SS.mmm (minutes field widens past 99 as needed).
void formatTime(uint32_t ms, char *out, size_t outSize) {
  uint32_t minutes = ms / kMsPerMinute;
  uint32_t seconds = (ms % kMsPerMinute) / 1000;
  uint32_t millisPart = ms % 1000;
  snprintf(out, outSize, "%02lu:%02lu.%03lu",
           (unsigned long)minutes, (unsigned long)seconds,
           (unsigned long)millisPart);
}

// ---------- Frame drawing ----------

void setPixel(int row, int col, uint8_t level) {
  if (row >= 0 && row < kMatrixHeight && col >= 0 && col < kMatrixWidth) {
    frame[row * kMatrixWidth + col] = level;
  }
}

// One 6-bit binary value as a vertical column, LSB at kMinutesLsbRow.
void drawMinuteColumn(int col, uint32_t value) {
  for (int bit = 0; bit < kMinutesBitCount; bit++) {
    if (value & (1UL << bit)) {
      setPixel(kMinutesLsbRow - bit, col, kFullBrightness);
    }
  }
}

void renderFrame(uint32_t now) {
  memset(frame, 0, sizeof(frame));
  uint32_t elapsed = elapsedMs();

  // Bottom row: smooth seconds progress bar (0..59.999 s -> 13 pixels).
  uint32_t msInMinute = elapsed % kMsPerMinute;
  uint32_t scaled = msInMinute * (uint32_t)kMatrixWidth;  // < 780000, no overflow
  int      fullPixels = scaled / kMsPerMinute;            // 0..12
  uint32_t remainder  = scaled % kMsPerMinute;
  uint8_t  fracLevel  = (uint8_t)((remainder * kFullBrightness) / kMsPerMinute);
  for (int x = 0; x < fullPixels; x++) {
    setPixel(kBottomRow, x, kFullBrightness);
  }
  if (fullPixels < kMatrixWidth) {
    setPixel(kBottomRow, fullPixels, fracLevel);
  }

  // Minutes group: mixed-radix base-60 binary columns.
  uint32_t minutes = elapsed / kMsPerMinute;
  uint32_t units   = minutes % 60;
  uint32_t sixties = (minutes / 60) % 60;
  uint32_t big     = minutes / 3600;
  if (big > 63) big = 63;  // clamp to what 6 bits can show
  drawMinuteColumn(kColMinUnits, units);
  if (sixties > 0 || big > 0) drawMinuteColumn(kColMinSixties, sixties);
  if (big > 0)                drawMinuteColumn(kColMin3600s, big);
  if (units == 0) {  // keep zero locatable
    setPixel(kMinutesLsbRow, kColMinUnits, kBaselineBrightness);
  }

  // Indicator row: LAP flash overrides the run/pause pixel.
  if (lapFlashArmed && (int32_t)(lapFlashUntil - now) > 0) {
    for (int x = 0; x < kMatrixWidth; x++) {
      setPixel(kIndicatorRow, x, kFullBrightness);
    }
  } else {
    lapFlashArmed = false;
    if (running) {
      if ((now % kBlinkPeriodMs) < kBlinkOnMs) {
        setPixel(kIndicatorRow, kIndicatorCol, kFullBrightness);
      }
    } else {
      setPixel(kIndicatorRow, kIndicatorCol, kPausedBrightness);
    }
  }

  matrix.draw(frame);
}

// ---------- Command handling ----------

void printHelp() {
  Serial.println();
  Serial.println("=== Serial Stopwatch / Lap Timer ===");
  Serial.println("Commands (case-insensitive, end with newline):");
  Serial.println("  START  - start / resume");
  Serial.println("  STOP   - pause (elapsed kept)");
  Serial.println("  RESET  - zero elapsed, clear laps");
  Serial.println("  LAP    - record a lap (up to 20 kept)");
  Serial.println("  LAPS   - reprint lap table");
  Serial.println("  STATUS - state, elapsed, lap count");
  Serial.println("Times shown as MM:SS.mmm");
  Serial.println();
}

void printLapLine(int index) {
  uint32_t total = lapTotalMs[index];
  uint32_t prev  = (index > 0) ? lapTotalMs[index - 1] : 0;
  char lapStr[16], totalStr[16];
  formatTime(total - prev, lapStr, sizeof(lapStr));
  formatTime(total, totalStr, sizeof(totalStr));
  char line[48];
  snprintf(line, sizeof(line), "Lap %2d  lap %s  total %s",
           index + 1, lapStr, totalStr);
  Serial.println(line);
}

void handleStart() {
  if (running) {
    Serial.println("Already running.");
    return;
  }
  anchorMs = millis();
  running = true;
  Serial.println("Started.");
}

void handleStop() {
  if (!running) {
    Serial.println("Already stopped.");
    return;
  }
  accumulatedMs += millis() - anchorMs;  // bank the running span exactly once
  running = false;
  char buf[16];
  formatTime(accumulatedMs, buf, sizeof(buf));
  Serial.print("Stopped at ");
  Serial.println(buf);
}

void handleReset() {
  running = false;
  accumulatedMs = 0;
  lapCount = 0;
  Serial.println("Reset. Elapsed 00:00.000, laps cleared.");
}

void handleLap(uint32_t now) {
  if (!running) {
    Serial.println("Not running - START first.");
    return;
  }
  if (lapCount >= kMaxLaps) {
    Serial.println("Lap memory full (20 laps).");
    return;
  }
  lapTotalMs[lapCount] = elapsedMs();
  lapCount++;
  printLapLine(lapCount - 1);
  lapFlashUntil = now + kLapFlashMs;
  lapFlashArmed = true;
}

void handleLaps() {
  if (lapCount == 0) {
    Serial.println("No laps recorded.");
    return;
  }
  Serial.println("--- Laps ---");
  for (int i = 0; i < lapCount; i++) {
    printLapLine(i);
  }
}

void handleStatus() {
  char buf[16];
  formatTime(elapsedMs(), buf, sizeof(buf));
  Serial.print("State: ");
  Serial.print(running ? "RUNNING" : "STOPPED");
  Serial.print("  elapsed ");
  Serial.print(buf);
  Serial.print("  laps ");
  Serial.print(lapCount);
  Serial.print("/");
  Serial.println(kMaxLaps);
}

bool commandIs(const char *cmd) {
  size_t i = 0;
  for (; cmd[i] != '\0'; i++) {
    if (toupper((unsigned char)lineBuf[i]) != cmd[i]) return false;
  }
  return lineBuf[i] == '\0';
}

void processLine(uint32_t now) {
  if (lineLen == 0) return;
  if      (commandIs("START"))  handleStart();
  else if (commandIs("STOP"))   handleStop();
  else if (commandIs("RESET"))  handleReset();
  else if (commandIs("LAPS"))   handleLaps();
  else if (commandIs("LAP"))    handleLap(now);
  else if (commandIs("STATUS")) handleStatus();
  else if (commandIs("HELP"))   printHelp();
  else {
    Serial.print("Unknown command: ");
    Serial.println(lineBuf);
    Serial.println("Try START, STOP, RESET, LAP, LAPS, STATUS, HELP.");
  }
}

// Non-blocking reader: accepts \n, \r, or \r\n line endings.
void pollSerial(uint32_t now) {
  while (Serial.available() > 0) {
    char c = (char)Serial.read();
    if (c == '\n' || c == '\r') {
      lineBuf[lineLen] = '\0';
      processLine(now);
      lineLen = 0;
    } else if (lineLen < kLineBufSize - 1) {
      lineBuf[lineLen++] = c;
    }
    // Overlong lines: extra chars dropped; terminator still ends the line.
  }
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

  printHelp();
  Serial.println("Ready. Send START to begin.");
}

void loop() {
  uint32_t now = millis();

  pollSerial(now);

  if (now - lastFrameMs >= kFrameIntervalMs) {
    lastFrameMs = now;
    renderFrame(now);
  }
}
