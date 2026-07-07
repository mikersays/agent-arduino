/*
  square-wave-generator.ino  —  Arduino UNO Q (STM32U585 MCU side)

  A poor-man's signal generator for testing and debugging external circuits.
  Up to 2 simultaneous software-timed square-wave channels on user-chosen
  digital pins (D2-D13), each with independent frequency (0.1-1000 Hz) and
  duty cycle (1-99%). A logarithmic frequency sweep is available per channel.

  Serial commands (115200 baud, newline-terminated, case-insensitive):
    OUT <ch> <pin> <freq> <duty>   e.g. OUT 1 D9 250 25   (duty may end in %)
    OFF <ch>                       stop a channel, pin driven LOW
    SWEEP <ch> <f1> <f2> <secs>    log sweep f1 -> f2, then hold f2
    STATUS                         show both channels
    HELP                           reprint this banner

  Honest accuracy limits: edges are timed with micros() deadlines served
  from loop(), NOT hardware timers. Jitter is loop-latency-bound — typically
  a few tens of microseconds, which is negligible below ~1 kHz but means the
  top of the range (1 kHz = 500 us half-periods) can wobble a few percent.
  Extreme duty at high frequency (e.g. 1% at 1 kHz asks for a 10 us phase)
  is faster than the loop can serve: those pulses stretch to one loop
  latency, though the generator stays live and the average rate recovers.
  The LED matrix refresh adds occasional extra jitter in the tens of
  microseconds. Long-term average frequency stays accurate because deadlines
  advance by exact period increments (rollover-safe uint32 arithmetic).
  The on-matrix waveform strip samples the output at 20 Hz, so it aliases
  above ~10 Hz — it is a "signal shape at a glance" aid, not a scope.

  Matrix display (8 rows x 13 cols, split per channel):
    Rows 0-3 = channel 1, rows 4-7 = channel 2. In each half, the top row is
    a filled bar showing the live duty cycle (full width = 100%), and the
    remaining 3 rows are a slow left-scrolling waveform strip: HIGH draws on
    the upper line, LOW on the lower line, with a dim pixel marking edges.

  Wiring: output on the chosen D-pin; scope/LED/logic analyzer to observe.
  No wiring needed to demo: channel 1 boots on D13 at 1 Hz / 50% and is
  mirrored on LED_BUILTIN (active-low), so the sketch blinks out of the box.

  Compile:
    arduino-cli compile -b arduino:zephyr:unoq ./square-wave-generator
  Upload:
    arduino-cli upload -b arduino:zephyr:unoq -p <board-ip> ./square-wave-generator
*/

#include "Arduino_LED_Matrix.h"
#include <string.h>
#include <stdlib.h>
#include <math.h>

// The llext link for this Zephyr core lacks newlib's __errno symbol, which
// libm's powf() references on domain errors. Provide a local stub.
extern "C" int *__errno(void) {
  static int llextErrno = 0;
  return &llextErrno;
}

// ---------- Channels & limits ----------
const int NUM_CHANNELS = 2;
const int PIN_MIN = 2;                 // D0/D1 reserved for serial on shields
const int PIN_MAX = 13;
const float FREQ_MIN_HZ = 0.1f;
const float FREQ_MAX_HZ = 1000.0f;
const int DUTY_MIN_PCT = 1;
const int DUTY_MAX_PCT = 99;
const float SWEEP_MIN_S = 0.1f;
const float SWEEP_MAX_S = 3600.0f;
const uint32_t MIN_PHASE_US = 1;       // never schedule a zero-length phase

// ---------- Boot default (self-demo, no wiring) ----------
const int DEFAULT_CH = 1;
const int DEFAULT_PIN = 13;
const float DEFAULT_FREQ_HZ = 1.0f;
const int DEFAULT_DUTY_PCT = 50;

// ---------- Display ----------
const int MATRIX_W = 13;
const int MATRIX_H = 8;
const int HALF_ROWS = 4;               // rows per channel
const uint8_t DUTY_BAR_LEVEL = 3;      // brightness of the duty-cycle bar
const uint8_t WAVE_LEVEL = 7;          // brightness of the waveform line
const uint8_t EDGE_LEVEL = 2;          // dim pixel marking a level transition

// ---------- Pacing (all non-blocking) ----------
const unsigned long MATRIX_INTERVAL_MS = 40;   // 25 Hz redraw
const unsigned long SCROLL_INTERVAL_MS = 50;   // 20 Hz waveform sampling
const unsigned long SWEEP_UPDATE_MS = 25;      // sweep freq recompute rate

// ---------- Serial ----------
const size_t LINE_BUF_LEN = 64;
const int MAX_TOKENS = 6;

struct Channel {
  bool active;
  int pin;
  float freqHz;
  int dutyPct;
  bool level;                 // current output level
  uint32_t nextEdgeUs;        // micros() deadline of the next edge
  uint32_t highUs;            // duration of the HIGH phase
  uint32_t lowUs;             // duration of the LOW phase
  // Sweep state
  bool sweeping;
  float sweepF1, sweepF2;
  unsigned long sweepStartMs;
  unsigned long sweepDurMs;
  unsigned long lastSweepUpdateMs;
  // Waveform strip history (sampled levels, oldest at [0])
  uint8_t wave[MATRIX_W];
  int waveCount;
};

Arduino_LED_Matrix matrix;
Channel channels[NUM_CHANNELS];
uint8_t frame[MATRIX_W * MATRIX_H];

unsigned long lastMatrixMs = 0;
unsigned long lastScrollMs = 0;

char lineBuf[LINE_BUF_LEN];
size_t lineLen = 0;

// ---------------------------------------------------------------- helpers

void printHelp() {
  Serial.println(F(""));
  Serial.println(F("=== Square Wave Generator (UNO Q) ==="));
  Serial.println(F("2 software-timed channels, D2-D13. Commands:"));
  Serial.println(F("  OUT <ch 1|2> <pin D2-D13> <freq 0.1-1000 Hz> <duty 1-99>%"));
  Serial.println(F("  OFF <ch>"));
  Serial.println(F("  SWEEP <ch> <f1> <f2> <seconds>   (log sweep, then hold f2)"));
  Serial.println(F("  STATUS"));
  Serial.println(F("  HELP"));
  Serial.println(F("Timing is loop-served micros() deadlines: clean below ~1 kHz,"));
  Serial.println(F("jitter is loop-latency-bound (matrix refresh adds ~10s of us)."));
  Serial.println(F("Matrix: top half = CH1, bottom = CH2 (duty bar + waveform strip)."));
  Serial.println(F(""));
}

// Recompute a channel's HIGH/LOW phase durations from freqHz and dutyPct.
// (Index-based rather than Channel& because the Arduino auto-prototypes are
// emitted before the struct definition.)
void recomputePhases(int idx) {
  Channel &ch = channels[idx];
  float periodUs = 1000000.0f / ch.freqHz;
  uint32_t high = (uint32_t)(periodUs * ch.dutyPct / 100.0f + 0.5f);
  uint32_t period = (uint32_t)(periodUs + 0.5f);
  if (high < MIN_PHASE_US) high = MIN_PHASE_US;
  if (high > period - MIN_PHASE_US) high = period - MIN_PHASE_US;
  ch.highUs = high;
  ch.lowUs = period - high;
}

void mirrorBuiltin(int idx) {
  // LED_BUILTIN is active-low: channel HIGH -> LED on -> write LOW.
  digitalWrite(LED_BUILTIN, channels[idx].level ? LOW : HIGH);
}

// (Re)start a channel with validated parameters.
void startChannel(int idx, int pin, float freqHz, int dutyPct) {
  Channel &ch = channels[idx];
  if (ch.active && ch.pin != pin) {
    digitalWrite(ch.pin, LOW);        // release the old pin
  }
  ch.active = true;
  ch.pin = pin;
  ch.freqHz = freqHz;
  ch.dutyPct = dutyPct;
  ch.sweeping = false;
  ch.level = false;
  ch.waveCount = 0;
  recomputePhases(idx);
  pinMode(pin, OUTPUT);
  digitalWrite(pin, LOW);
  ch.nextEdgeUs = micros();           // rising edge served on the next pass
  if (idx == 0) mirrorBuiltin(idx);
}

void stopChannel(int idx) {
  Channel &ch = channels[idx];
  if (ch.active) digitalWrite(ch.pin, LOW);
  ch.active = false;
  ch.sweeping = false;
  ch.level = false;
  ch.waveCount = 0;
  if (idx == 0) digitalWrite(LED_BUILTIN, HIGH);   // active-low: off
}

// ---------------------------------------------------------------- output

// Serve any due edges. Called every loop pass; rollover-safe signed diff.
void serveEdges() {
  for (int i = 0; i < NUM_CHANNELS; i++) {
    Channel &ch = channels[i];
    if (!ch.active) continue;
    uint32_t now = micros();
    if ((int32_t)(now - ch.nextEdgeUs) >= 0) {
      ch.level = !ch.level;
      digitalWrite(ch.pin, ch.level ? HIGH : LOW);
      if (i == 0) mirrorBuiltin(i);
      uint32_t dur = ch.level ? ch.highUs : ch.lowUs;
      ch.nextEdgeUs += dur;           // advance from the scheduled time: no drift
      // If the loop stalled past the whole phase, resync instead of bursting.
      if ((int32_t)(now - ch.nextEdgeUs) >= 0) ch.nextEdgeUs = now + dur;
    }
  }
}

// Advance any active logarithmic sweeps.
void serviceSweeps() {
  unsigned long nowMs = millis();
  for (int i = 0; i < NUM_CHANNELS; i++) {
    Channel &ch = channels[i];
    if (!ch.active || !ch.sweeping) continue;
    if (nowMs - ch.lastSweepUpdateMs < SWEEP_UPDATE_MS) continue;
    ch.lastSweepUpdateMs = nowMs;
    unsigned long elapsed = nowMs - ch.sweepStartMs;
    if (elapsed >= ch.sweepDurMs) {
      ch.freqHz = ch.sweepF2;         // hold f2
      ch.sweeping = false;
      Serial.print(F("CH"));
      Serial.print(i + 1);
      Serial.print(F(": sweep done, holding "));
      Serial.print(ch.sweepF2, 3);
      Serial.println(F(" Hz"));
    } else {
      float t = (float)elapsed / (float)ch.sweepDurMs;
      ch.freqHz = ch.sweepF1 * powf(ch.sweepF2 / ch.sweepF1, t);
    }
    recomputePhases(i);
  }
}

// ---------------------------------------------------------------- display

void setPixel(int row, int col, uint8_t level) {
  if (row < 0 || row >= MATRIX_H || col < 0 || col >= MATRIX_W) return;
  frame[row * MATRIX_W + col] = level;
}

void pushWaveSamples() {
  for (int i = 0; i < NUM_CHANNELS; i++) {
    Channel &ch = channels[i];
    if (!ch.active) continue;
    if (ch.waveCount < MATRIX_W) {
      ch.wave[ch.waveCount++] = ch.level ? 1 : 0;
    } else {
      for (int k = 0; k < MATRIX_W - 1; k++) ch.wave[k] = ch.wave[k + 1];
      ch.wave[MATRIX_W - 1] = ch.level ? 1 : 0;
    }
  }
}

void renderMatrix() {
  memset(frame, 0, sizeof(frame));
  for (int i = 0; i < NUM_CHANNELS; i++) {
    Channel &ch = channels[i];
    if (!ch.active) continue;
    int base = i * HALF_ROWS;

    // Duty bar on the half's top row: filled width proportional to duty.
    int cols = (ch.dutyPct * MATRIX_W + 50) / 100;
    if (cols < 1) cols = 1;
    if (cols > MATRIX_W) cols = MATRIX_W;
    for (int c = 0; c < cols; c++) setPixel(base, c, DUTY_BAR_LEVEL);

    // Scrolling waveform strip: HIGH on upper line, LOW on lower line,
    // dim middle pixel where the level changed (edge marker).
    int rowHigh = base + 1;
    int rowMid = base + 2;
    int rowLow = base + 3;
    int firstCol = MATRIX_W - ch.waveCount;   // right-align partial history
    int prev = -1;
    for (int s = 0; s < ch.waveCount; s++) {
      int col = firstCol + s;
      int lvl = ch.wave[s];
      setPixel(lvl ? rowHigh : rowLow, col, WAVE_LEVEL);
      if (prev >= 0 && prev != lvl) setPixel(rowMid, col, EDGE_LEVEL);
      prev = lvl;
    }
  }
  matrix.draw(frame);
}

// ---------------------------------------------------------------- commands

void printStatus() {
  for (int i = 0; i < NUM_CHANNELS; i++) {
    Channel &ch = channels[i];
    Serial.print(F("CH"));
    Serial.print(i + 1);
    Serial.print(F(": "));
    if (!ch.active) {
      Serial.println(F("off"));
      continue;
    }
    Serial.print(F("D"));
    Serial.print(ch.pin);
    Serial.print(F("  "));
    Serial.print(ch.freqHz, 3);
    Serial.print(F(" Hz  "));
    Serial.print(ch.dutyPct);
    Serial.print(F("%"));
    if (ch.sweeping) {
      unsigned long elapsed = millis() - ch.sweepStartMs;
      unsigned long leftMs = (elapsed < ch.sweepDurMs) ? ch.sweepDurMs - elapsed : 0;
      Serial.print(F("  [sweep "));
      Serial.print(ch.sweepF1, 2);
      Serial.print(F("->"));
      Serial.print(ch.sweepF2, 2);
      Serial.print(F(" Hz, "));
      Serial.print(leftMs / 1000.0f, 1);
      Serial.print(F(" s left]"));
    }
    Serial.println();
  }
}

// Parse a channel token ("1" or "2"); returns 0-based index or -1.
int parseChannel(const char *tok) {
  if (strcmp(tok, "1") == 0) return 0;
  if (strcmp(tok, "2") == 0) return 1;
  return -1;
}

// Parse a pin token ("D9" or "9"); returns pin number or -1.
int parsePin(const char *tok) {
  if (tok[0] == 'D') tok++;
  if (*tok == '\0') return -1;
  for (const char *p = tok; *p; p++) {
    if (*p < '0' || *p > '9') return -1;
  }
  int pin = atoi(tok);
  if (pin < PIN_MIN || pin > PIN_MAX) return -1;
  return pin;
}

// Parse a duty token ("25" or "25%"); returns percent or -1.
int parseDuty(const char *tok) {
  char tmp[8];
  size_t n = strlen(tok);
  if (n == 0 || n >= sizeof(tmp)) return -1;
  strcpy(tmp, tok);
  if (tmp[n - 1] == '%') tmp[n - 1] = '\0';
  if (tmp[0] == '\0') return -1;
  for (const char *p = tmp; *p; p++) {
    if (*p < '0' || *p > '9') return -1;
  }
  int duty = atoi(tmp);
  if (duty < DUTY_MIN_PCT || duty > DUTY_MAX_PCT) return -1;
  return duty;
}

void cmdOut(char *tokens[], int count) {
  if (count != 5) {
    Serial.println(F("ERR usage: OUT <ch> <pin> <freq> <duty>%"));
    return;
  }
  int idx = parseChannel(tokens[1]);
  int pin = parsePin(tokens[2]);
  float freq = atof(tokens[3]);
  int duty = parseDuty(tokens[4]);
  if (idx < 0) { Serial.println(F("ERR channel must be 1 or 2")); return; }
  if (pin < 0) { Serial.println(F("ERR pin must be D2-D13")); return; }
  if (!(freq >= FREQ_MIN_HZ && freq <= FREQ_MAX_HZ)) {
    Serial.println(F("ERR freq must be 0.1-1000 Hz"));
    return;
  }
  if (duty < 0) { Serial.println(F("ERR duty must be 1-99 (%)")); return; }
  int other = 1 - idx;
  if (channels[other].active && channels[other].pin == pin) {
    Serial.println(F("ERR pin already in use by the other channel"));
    return;
  }
  startChannel(idx, pin, freq, duty);
  Serial.print(F("OK CH"));
  Serial.print(idx + 1);
  Serial.print(F(" -> D"));
  Serial.print(pin);
  Serial.print(F(" "));
  Serial.print(freq, 3);
  Serial.print(F(" Hz "));
  Serial.print(duty);
  Serial.println(F("%"));
}

void cmdOff(char *tokens[], int count) {
  if (count != 2) { Serial.println(F("ERR usage: OFF <ch>")); return; }
  int idx = parseChannel(tokens[1]);
  if (idx < 0) { Serial.println(F("ERR channel must be 1 or 2")); return; }
  stopChannel(idx);
  Serial.print(F("OK CH"));
  Serial.print(idx + 1);
  Serial.println(F(" off (pin LOW)"));
}

void cmdSweep(char *tokens[], int count) {
  if (count != 5) {
    Serial.println(F("ERR usage: SWEEP <ch> <f1> <f2> <seconds>"));
    return;
  }
  int idx = parseChannel(tokens[1]);
  float f1 = atof(tokens[2]);
  float f2 = atof(tokens[3]);
  float secs = atof(tokens[4]);
  if (idx < 0) { Serial.println(F("ERR channel must be 1 or 2")); return; }
  if (!channels[idx].active) {
    Serial.println(F("ERR channel is off: configure it with OUT first"));
    return;
  }
  if (!(f1 >= FREQ_MIN_HZ && f1 <= FREQ_MAX_HZ) ||
      !(f2 >= FREQ_MIN_HZ && f2 <= FREQ_MAX_HZ)) {
    Serial.println(F("ERR freqs must be 0.1-1000 Hz"));
    return;
  }
  if (!(secs >= SWEEP_MIN_S && secs <= SWEEP_MAX_S)) {
    Serial.println(F("ERR seconds must be 0.1-3600"));
    return;
  }
  Channel &ch = channels[idx];
  ch.sweeping = true;
  ch.sweepF1 = f1;
  ch.sweepF2 = f2;
  ch.sweepDurMs = (unsigned long)(secs * 1000.0f);
  ch.sweepStartMs = millis();
  ch.lastSweepUpdateMs = ch.sweepStartMs;
  ch.freqHz = f1;
  recomputePhases(idx);
  Serial.print(F("OK CH"));
  Serial.print(idx + 1);
  Serial.print(F(" log sweep "));
  Serial.print(f1, 3);
  Serial.print(F(" -> "));
  Serial.print(f2, 3);
  Serial.print(F(" Hz over "));
  Serial.print(secs, 1);
  Serial.println(F(" s, then hold f2"));
}

void processLine(char *line) {
  // Uppercase in place (case-insensitive commands).
  for (char *p = line; *p; p++) {
    if (*p >= 'a' && *p <= 'z') *p -= 32;
  }
  // Tokenize on whitespace.
  char *tokens[MAX_TOKENS];
  int count = 0;
  char *tok = strtok(line, " \t");
  while (tok && count < MAX_TOKENS) {
    tokens[count++] = tok;
    tok = strtok(NULL, " \t");
  }
  if (count == 0) return;

  if (strcmp(tokens[0], "OUT") == 0) cmdOut(tokens, count);
  else if (strcmp(tokens[0], "OFF") == 0) cmdOff(tokens, count);
  else if (strcmp(tokens[0], "SWEEP") == 0) cmdSweep(tokens, count);
  else if (strcmp(tokens[0], "STATUS") == 0) printStatus();
  else if (strcmp(tokens[0], "HELP") == 0) printHelp();
  else Serial.println(F("ERR unknown command (try HELP)"));
}

// Non-blocking line reader: accepts \n or \r as terminators.
void pollSerial() {
  while (Serial.available() > 0) {
    char c = (char)Serial.read();
    if (c == '\n' || c == '\r') {
      if (lineLen > 0) {
        lineBuf[lineLen] = '\0';
        processLine(lineBuf);
        lineLen = 0;
      }
    } else if (lineLen < LINE_BUF_LEN - 1) {
      lineBuf[lineLen++] = c;
    }
  }
}

// ---------------------------------------------------------------- sketch

void setup() {
  Serial.begin(115200);
  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, HIGH);    // active-low: start off

  matrix.begin();
  matrix.setGrayscaleBits(3);
  memset(frame, 0, sizeof(frame));
  matrix.draw(frame);

  memset(channels, 0, sizeof(channels));

  printHelp();

  // Self-demo default: CH1 on D13 at 1 Hz / 50%, mirrored on LED_BUILTIN.
  startChannel(DEFAULT_CH - 1, DEFAULT_PIN, DEFAULT_FREQ_HZ, DEFAULT_DUTY_PCT);
  Serial.println(F("Default: CH1 on D13, 1 Hz, 50% (mirrored on LED_BUILTIN)"));
}

void loop() {
  serveEdges();                       // highest priority: output timing
  pollSerial();
  serviceSweeps();

  unsigned long nowMs = millis();
  if (nowMs - lastScrollMs >= SCROLL_INTERVAL_MS) {
    lastScrollMs = nowMs;
    pushWaveSamples();
  }
  if (nowMs - lastMatrixMs >= MATRIX_INTERVAL_MS) {
    lastMatrixMs = nowMs;
    renderMatrix();
  }
}
