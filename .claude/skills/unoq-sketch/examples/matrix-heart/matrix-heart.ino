// UNO Q — draw a pulsing heart on the built-in 8x13 LED matrix (104 LEDs).
// Demonstrates the grayscale buffer API (brightness 0..7).
//
//   arduino-cli compile -b arduino:zephyr:unoq ./matrix-heart
//   arduino-cli upload  -b arduino:zephyr:unoq -p <board-wlan0-ip> ./matrix-heart

#include "Arduino_LED_Matrix.h"

Arduino_LED_Matrix matrix;

// 8 rows x 13 cols, row-major. 1 = lit pixel of the heart shape.
const uint8_t heart[8][13] = {
  {0,0,0,0,0,0,0,0,0,0,0,0,0},
  {0,0,1,1,0,0,0,0,0,1,1,0,0},
  {0,1,1,1,1,0,0,0,1,1,1,1,0},
  {0,1,1,1,1,1,0,1,1,1,1,1,0},
  {0,0,1,1,1,1,1,1,1,1,1,0,0},
  {0,0,0,1,1,1,1,1,1,1,0,0,0},
  {0,0,0,0,1,1,1,1,1,0,0,0,0},
  {0,0,0,0,0,1,1,1,0,0,0,0,0},
};

void setup() {
  matrix.begin();
  matrix.setGrayscaleBits(3);   // brightness range 0..7
}

void loop() {
  // Breathe: ramp brightness up then down.
  for (int b = 1; b <= 7; b++) { render(b); delay(60); }
  for (int b = 7; b >= 1; b--) { render(b); delay(60); }
  delay(200);
}

void render(uint8_t brightness) {
  uint8_t frame[104];                 // 8 * 13
  for (int y = 0; y < 8; y++)
    for (int x = 0; x < 13; x++)
      frame[y * 13 + x] = heart[y][x] ? brightness : 0;
  matrix.draw(frame);
}
