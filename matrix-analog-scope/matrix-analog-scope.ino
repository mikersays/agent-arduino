/*
  matrix-analog-scope.ino  —  Arduino UNO Q (STM32U585 MCU side)

  A scrolling oscilloscope on the built-in 13x8 blue LED matrix, plotting
  analogRead(A0). New samples enter at the rightmost column and history
  scrolls left. The vertical scale auto-ranges: the min/max of the samples
  currently on screen are mapped onto the 8 rows. Each new sample is drawn
  as a bright pixel, with a dim vertical interpolation segment joining it
  to the previous column's value so the trace reads as a connected line.

  Wiring: none required. With nothing connected to A0 the pin floats, so
  you'll see mains hum / touch noise wander across the display — which
  still demos nicely. Touch the A0 pin or wire a pot/sensor for a real
  signal. The raw ADC value is also printed to Serial at ~10 Hz, ready
  for the Arduino IDE Serial Plotter (115200 baud).

  Compile:
    arduino-cli compile -b arduino:zephyr:unoq ./matrix-analog-scope
  Upload:
    arduino-cli upload -b arduino:zephyr:unoq -p <board-ip> ./matrix-analog-scope
*/

#include "Arduino_LED_Matrix.h"

// ---------- Display geometry ----------
const int MATRIX_W = 13;            // columns
const int MATRIX_H = 8;             // rows
const uint8_t TRACE_BRIGHT = 7;     // brightness of the sample pixel (0..7)
const uint8_t TRACE_DIM = 2;        // brightness of the connecting segment

// ---------- Timing ----------
const unsigned long SAMPLE_INTERVAL_MS = 50;   // 20 Hz scroll rate
const unsigned long SERIAL_INTERVAL_MS = 100;  // ~10 Hz Serial Plotter feed

// ---------- Auto-scale ----------
const int MIN_SPAN = 8;             // minimum ADC span so a flat signal
                                    // doesn't cause divide-by-zero jitter

Arduino_LED_Matrix matrix;

uint8_t frame[MATRIX_W * MATRIX_H];       // row-major, 8 rows x 13 cols
uint16_t history[MATRIX_W];               // raw ADC samples, oldest at [0]
int sampleCount = 0;                      // how many columns are valid

unsigned long lastSampleMs = 0;
unsigned long lastSerialMs = 0;

void setup() {
  Serial.begin(115200);
  pinMode(A0, INPUT);
  matrix.begin();
  matrix.setGrayscaleBits(3);
  memset(frame, 0, sizeof(frame));
  matrix.draw(frame);
}

// Push a new sample into the right end of the history, scrolling left.
void pushSample(uint16_t value) {
  if (sampleCount < MATRIX_W) {
    history[sampleCount++] = value;
  } else {
    for (int i = 0; i < MATRIX_W - 1; i++) {
      history[i] = history[i + 1];
    }
    history[MATRIX_W - 1] = value;
  }
}

// Rolling min/max over the samples currently on screen.
void windowRange(uint16_t &lo, uint16_t &hi) {
  lo = history[0];
  hi = history[0];
  for (int i = 1; i < sampleCount; i++) {
    if (history[i] < lo) lo = history[i];
    if (history[i] > hi) hi = history[i];
  }
  // Enforce a minimum span, centered on the signal, so a flat trace sits
  // steady mid-screen instead of flickering between rows.
  if ((int)(hi - lo) < MIN_SPAN) {
    int mid = ((int)hi + (int)lo) / 2;
    int newLo = mid - MIN_SPAN / 2;
    if (newLo < 0) newLo = 0;
    lo = (uint16_t)newLo;
    hi = lo + MIN_SPAN;
  }
}

// Map a raw sample to a matrix row: high values at the top (row 0).
int valueToRow(uint16_t value, uint16_t lo, uint16_t hi) {
  long num = (long)(value - lo) * (MATRIX_H - 1);
  int rowFromBottom = (int)(num / (hi - lo));
  if (rowFromBottom < 0) rowFromBottom = 0;
  if (rowFromBottom > MATRIX_H - 1) rowFromBottom = MATRIX_H - 1;
  return (MATRIX_H - 1) - rowFromBottom;
}

void setPixel(int row, int col, uint8_t level) {
  frame[row * MATRIX_W + col] = level;
}

// Redraw the whole frame from the sample history.
void renderTrace() {
  memset(frame, 0, sizeof(frame));
  if (sampleCount == 0) {
    matrix.draw(frame);
    return;
  }

  uint16_t lo, hi;
  windowRange(lo, hi);

  int firstCol = MATRIX_W - sampleCount;  // right-align a partial history
  int prevRow = -1;
  for (int i = 0; i < sampleCount; i++) {
    int col = firstCol + i;
    int row = valueToRow(history[i], lo, hi);

    // Dim vertical segment bridging the gap to the previous column's row,
    // drawn in this column so the trace is visually connected.
    if (prevRow >= 0 && prevRow != row) {
      int step = (prevRow < row) ? 1 : -1;
      for (int r = prevRow + step; r != row; r += step) {
        setPixel(r, col, TRACE_DIM);
      }
    }

    setPixel(row, col, TRACE_BRIGHT);
    prevRow = row;
  }

  matrix.draw(frame);
}

void loop() {
  unsigned long now = millis();

  if (now - lastSampleMs >= SAMPLE_INTERVAL_MS) {
    lastSampleMs = now;
    pushSample((uint16_t)analogRead(A0));
    renderTrace();
  }

  if (now - lastSerialMs >= SERIAL_INTERVAL_MS) {
    lastSerialMs = now;
    Serial.println(analogRead(A0));
  }
}
