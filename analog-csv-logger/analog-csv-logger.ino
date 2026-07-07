/*
  analog-csv-logger.ino  —  Arduino UNO Q (STM32U585 MCU side)

  Multi-channel analog data logger that streams clean CSV over Serial
  (115200 baud). Enabled channels are sampled on an anchored schedule at a
  configurable rate and printed as "millis,A0,..." rows, ready to capture on
  the Linux side:

      arduino-cli monitor -p <board-ip> -b arduino:zephyr:unoq \
        -c baudrate=115200 | tee data.csv

  or to view live in the Arduino IDE Serial Plotter. The built-in 13x8 LED
  matrix shows six live vertical bar meters (one per channel A0..A5, two
  columns each, brightness 4) with a peak-hold pixel at brightness 7 that
  decays over ~1 second — at-a-glance signal levels while you log.

  Wiring: none required. Floating pins read noise, which still demos nicely;
  connect sensors (or pots) to A0-A5 for real data.

  Serial commands (replies are prefixed with "# " so they never corrupt the
  CSV stream):
    START            begin logging            STOP             pause logging
    RATE <0.2-200>   sample rate in Hz (float ok, e.g. RATE 12.5)
    CH <spec>        enable channels: mask "CH 111000" or list "CH A0,A3"
    RAW / VOLTS      output raw ADC counts, or calibrated 0.000-3.300 V
    HEADER           reprint the CSV header row
    STATUS           show current settings

  Defaults from boot: logging A0-A2 at 2 Hz, RAW mode.

  Sample timing is derived from an anchored schedule (next-due += period),
  so the rate never drifts; if the loop falls behind, missed samples are
  skipped rather than emitted in a burst.

  Compile:
    arduino-cli compile -b arduino:zephyr:unoq ./analog-csv-logger
  Upload:
    arduino-cli upload -b arduino:zephyr:unoq -p <board-ip> ./analog-csv-logger
*/

#include "Arduino_LED_Matrix.h"

// ---------- Channels ----------
const int NUM_CHANNELS = 6;
const uint8_t CHANNEL_PIN[NUM_CHANNELS] = { A0, A1, A2, A3, A4, A5 };
const uint8_t DEFAULT_MASK = 0b000111;      // bit i = Ai; boot with A0-A2

// ---------- ADC / calibration ----------
const int ADC_MAX = 1023;                   // 10-bit analogRead()
const long VREF_MILLIVOLTS = 3300;          // assumed 3.3 V reference

// ---------- Sample rate ----------
const float RATE_MIN_HZ = 0.2f;
const float RATE_MAX_HZ = 200.0f;
const float DEFAULT_RATE_HZ = 2.0f;

// ---------- Display geometry ----------
const int MATRIX_W = 13;                    // columns
const int MATRIX_H = 8;                     // rows
const int METER_COLS = 2;                   // columns per channel meter
const uint8_t BAR_BRIGHT = 4;               // bar body brightness (0..7)
const uint8_t PEAK_BRIGHT = 7;              // peak-hold pixel brightness

// ---------- Display timing ----------
const unsigned long FRAME_INTERVAL_MS = 33;             // ~30 fps meters
const unsigned long PEAK_DECAY_STEP_MS = 1000 / MATRIX_H; // full-scale fall ~1 s

// ---------- Serial command input ----------
const int CMD_BUF_LEN = 48;

Arduino_LED_Matrix matrix;
uint8_t frame[MATRIX_W * MATRIX_H];         // row-major, 8 rows x 13 cols

// Logger state
uint8_t channelMask = DEFAULT_MASK;
float rateHz = DEFAULT_RATE_HZ;
unsigned long periodUs = (unsigned long)(1000000.0f / DEFAULT_RATE_HZ);
bool logging = true;
bool voltsMode = false;
unsigned long nextDueUs = 0;                // anchored schedule (micros)

// Meter state
uint8_t barHeight[NUM_CHANNELS];            // lit pixels from bottom, 0..8
uint8_t peakHeight[NUM_CHANNELS];           // peak-hold height, 0..8
unsigned long lastFrameMs = 0;
unsigned long lastPeakDecayMs = 0;

// Command input state
char cmdBuf[CMD_BUF_LEN];
int cmdLen = 0;

// ---------------------------------------------------------------------------
// Serial output helpers
// ---------------------------------------------------------------------------

// Print an ADC reading in the current output mode. VOLTS is rendered with
// integer math (millivolts) to avoid float-formatting surprises: 0.000-3.300.
void printReading(int raw) {
  if (!voltsMode) {
    Serial.print(raw);
    return;
  }
  long mv = ((long)raw * VREF_MILLIVOLTS + ADC_MAX / 2) / ADC_MAX;
  Serial.print(mv / 1000);
  Serial.print('.');
  long frac = mv % 1000;
  if (frac < 100) Serial.print('0');
  if (frac < 10) Serial.print('0');
  Serial.print(frac);
}

void printChannelList() {
  bool first = true;
  for (int i = 0; i < NUM_CHANNELS; i++) {
    if (channelMask & (1 << i)) {
      if (!first) Serial.print(',');
      Serial.print('A');
      Serial.print(i);
      first = false;
    }
  }
}

// CSV header row: part of the data stream, so no "# " prefix.
void printHeader() {
  Serial.print("millis,");
  printChannelList();
  Serial.println();
}

void printStatus() {
  Serial.print("# status: ");
  Serial.print(logging ? "LOGGING" : "STOPPED");
  Serial.print(", rate=");
  Serial.print(rateHz, 2);
  Serial.print(" Hz, mode=");
  Serial.print(voltsMode ? "VOLTS" : "RAW");
  Serial.print(", channels=");
  printChannelList();
  Serial.println();
}

void printHelp() {
  Serial.println("# analog-csv-logger — Arduino UNO Q");
  Serial.println("# CSV rows stream below; '#'-prefixed lines are replies/info.");
  Serial.println("# Commands: START | STOP | RATE <0.2-200> | CH <111000 or A0,A3>");
  Serial.println("#           RAW | VOLTS | HEADER | STATUS");
  Serial.println("# Nothing wired? Floating pins read noise. Sensors go on A0-A5.");
}

// ---------------------------------------------------------------------------
// Command parsing
// ---------------------------------------------------------------------------

// Parse a CH argument: either a bit mask like "111000" (leftmost char = A0)
// or a comma-separated list like "A0,A3". Returns true and fills maskOut on
// success.
bool parseChannelSpec(const char *arg, uint8_t &maskOut) {
  // Try mask form first: 1-6 chars, all '0'/'1'.
  int len = strlen(arg);
  bool isMask = (len >= 1 && len <= NUM_CHANNELS);
  for (int i = 0; i < len && isMask; i++) {
    if (arg[i] != '0' && arg[i] != '1') isMask = false;
  }
  if (isMask) {
    uint8_t mask = 0;
    for (int i = 0; i < len; i++) {
      if (arg[i] == '1') mask |= (1 << i);
    }
    maskOut = mask;
    return mask != 0;
  }

  // List form: A0,A3 (case already uppercased; tolerate spaces).
  uint8_t mask = 0;
  const char *p = arg;
  while (*p) {
    while (*p == ' ' || *p == ',') p++;
    if (*p == '\0') break;
    if (*p != 'A') return false;
    p++;
    if (*p < '0' || *p > '0' + NUM_CHANNELS - 1) return false;
    mask |= (1 << (*p - '0'));
    p++;
    if (*p != '\0' && *p != ',' && *p != ' ') return false;
  }
  maskOut = mask;
  return mask != 0;
}

void handleCommand(char *line) {
  // Uppercase in place for case-insensitive matching.
  for (char *p = line; *p; p++) *p = toupper((unsigned char)*p);

  // Split "VERB ARG".
  char *arg = strchr(line, ' ');
  if (arg) {
    *arg++ = '\0';
    while (*arg == ' ') arg++;
  }

  if (strcmp(line, "START") == 0) {
    if (!logging) {
      logging = true;
      nextDueUs = micros();               // re-anchor the schedule
      printHeader();
    }
    Serial.println("# logging started");
  } else if (strcmp(line, "STOP") == 0) {
    logging = false;
    Serial.println("# logging stopped");
  } else if (strcmp(line, "RATE") == 0) {
    float r = arg ? (float)atof(arg) : 0.0f;
    if (r < RATE_MIN_HZ || r > RATE_MAX_HZ) {
      Serial.println("# error: RATE takes 0.2-200 (Hz)");
    } else {
      rateHz = r;
      periodUs = (unsigned long)(1000000.0f / rateHz);
      nextDueUs = micros();               // re-anchor at the new rate
      Serial.print("# rate = ");
      Serial.print(rateHz, 2);
      Serial.println(" Hz");
    }
  } else if (strcmp(line, "CH") == 0) {
    uint8_t mask;
    if (arg && parseChannelSpec(arg, mask)) {
      channelMask = mask;
      Serial.print("# channels = ");
      printChannelList();
      Serial.println();
      printHeader();                      // column layout changed
    } else {
      Serial.println("# error: CH takes a mask (e.g. 111000) or list (e.g. A0,A3)");
    }
  } else if (strcmp(line, "RAW") == 0) {
    voltsMode = false;
    Serial.println("# mode = RAW (ADC counts 0-1023)");
  } else if (strcmp(line, "VOLTS") == 0) {
    voltsMode = true;
    Serial.println("# mode = VOLTS (0.000-3.300, 3.3V ref)");
  } else if (strcmp(line, "HEADER") == 0) {
    printHeader();
  } else if (strcmp(line, "STATUS") == 0) {
    printStatus();
  } else {
    Serial.print("# unknown command: ");
    Serial.println(line);
    Serial.println("# try: START STOP RATE CH RAW VOLTS HEADER STATUS");
  }
}

// Non-blocking line reader; handles \r, \n, and \r\n endings.
void pollSerialCommands() {
  while (Serial.available() > 0) {
    char c = (char)Serial.read();
    if (c == '\n' || c == '\r') {
      if (cmdLen > 0) {
        cmdBuf[cmdLen] = '\0';
        handleCommand(cmdBuf);
        cmdLen = 0;
      }
    } else if (cmdLen < CMD_BUF_LEN - 1) {
      cmdBuf[cmdLen++] = c;
    }
  }
}

// ---------------------------------------------------------------------------
// Logging
// ---------------------------------------------------------------------------

void emitCsvRow() {
  Serial.print(millis());
  for (int i = 0; i < NUM_CHANNELS; i++) {
    if (channelMask & (1 << i)) {
      Serial.print(',');
      printReading(analogRead(CHANNEL_PIN[i]));
    }
  }
  Serial.println();
}

// Anchored scheduler: fire when the due time passes, then advance the anchor
// by whole periods. If we've fallen more than one period behind, jump the
// anchor forward (skipping the missed samples) instead of bursting.
void serviceLogger() {
  if (!logging) return;
  unsigned long nowUs = micros();
  if ((long)(nowUs - nextDueUs) < 0) return;

  emitCsvRow();
  nextDueUs += periodUs;
  if ((long)(nowUs - nextDueUs) >= 0) {
    unsigned long missed = (nowUs - nextDueUs) / periodUs + 1;
    nextDueUs += missed * periodUs;
  }
}

// ---------------------------------------------------------------------------
// Matrix bar meters
// ---------------------------------------------------------------------------

void setPixel(int row, int col, uint8_t level) {
  if (row < 0 || row >= MATRIX_H || col < 0 || col >= MATRIX_W) return;
  frame[row * MATRIX_W + col] = level;
}

void updateMeters(unsigned long nowMs) {
  // Fresh readings for the display (independent of the logging schedule).
  for (int i = 0; i < NUM_CHANNELS; i++) {
    int raw = analogRead(CHANNEL_PIN[i]);
    uint8_t h = (uint8_t)(((long)raw * MATRIX_H + ADC_MAX / 2) / ADC_MAX);
    if (h > MATRIX_H) h = MATRIX_H;
    barHeight[i] = h;
    if (h >= peakHeight[i]) peakHeight[i] = h;
  }

  // Peak decay: one row per step => full-scale fall in ~1 second.
  if (nowMs - lastPeakDecayMs >= PEAK_DECAY_STEP_MS) {
    lastPeakDecayMs = nowMs;
    for (int i = 0; i < NUM_CHANNELS; i++) {
      if (peakHeight[i] > barHeight[i]) peakHeight[i]--;
    }
  }

  // Render: channel i owns columns 2i and 2i+1; column 12 stays dark.
  memset(frame, 0, sizeof(frame));
  for (int i = 0; i < NUM_CHANNELS; i++) {
    int colBase = i * METER_COLS;
    for (int p = 0; p < barHeight[i]; p++) {
      int row = MATRIX_H - 1 - p;
      for (int c = 0; c < METER_COLS; c++) setPixel(row, colBase + c, BAR_BRIGHT);
    }
    if (peakHeight[i] > 0) {
      int peakRow = MATRIX_H - peakHeight[i];
      for (int c = 0; c < METER_COLS; c++) setPixel(peakRow, colBase + c, PEAK_BRIGHT);
    }
  }
  matrix.draw(frame);
}

// ---------------------------------------------------------------------------

void setup() {
  Serial.begin(115200);
  for (int i = 0; i < NUM_CHANNELS; i++) {
    pinMode(CHANNEL_PIN[i], INPUT);
    barHeight[i] = 0;
    peakHeight[i] = 0;
  }
  matrix.begin();
  matrix.setGrayscaleBits(3);
  memset(frame, 0, sizeof(frame));
  matrix.draw(frame);

  printHelp();
  printStatus();
  printHeader();
  nextDueUs = micros();                    // anchor the sample schedule
}

void loop() {
  pollSerialCommands();
  serviceLogger();

  unsigned long nowMs = millis();
  if (nowMs - lastFrameMs >= FRAME_INTERVAL_MS) {
    lastFrameMs = nowMs;
    updateMeters(nowMs);
  }
}
