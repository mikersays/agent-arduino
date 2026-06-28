// UNO Q onboard LED matrix light show.
// Uses the built-in 8x13 blue LED matrix as a grayscale display.
//
// Compile:
//   arduino-cli compile -b arduino:zephyr:unoq ./matrix-light-show
//
// Upload:
//   arduino-cli upload -b arduino:zephyr:unoq -p <board-wlan0-ip> ./matrix-light-show

#include "Arduino_LED_Matrix.h"

Arduino_LED_Matrix matrix;

const uint8_t WIDTH = 13;
const uint8_t HEIGHT = 8;
const uint8_t PIXELS = WIDTH * HEIGHT;
const uint16_t FRAME_MS = 45;
const uint16_t EFFECT_MS = 6000;

uint8_t frame[PIXELS];
uint32_t lastFrameMs = 0;
uint32_t effectStartMs = 0;
uint8_t effect = 0;
uint32_t rng = 0xC0FFEE42;

uint8_t sparkle[PIXELS];
uint8_t drops[WIDTH];

void setup() {
  matrix.begin();
  matrix.setGrayscaleBits(3);  // Brightness values are 0..7.
  resetEffect();
}

void loop() {
  const uint32_t now = millis();

  if (now - effectStartMs >= EFFECT_MS) {
    effect = (effect + 1) % 5;
    resetEffect();
  }

  if (now - lastFrameMs < FRAME_MS) {
    return;
  }

  lastFrameMs = now;
  const uint16_t t = (now - effectStartMs) / FRAME_MS;

  clearFrame();

  switch (effect) {
    case 0: drawOrbit(t); break;
    case 1: drawRain(t); break;
    case 2: drawSparkles(t); break;
    case 3: drawRipples(t); break;
    default: drawScanner(t); break;
  }

  matrix.draw(frame);
}

void resetEffect() {
  effectStartMs = millis();
  lastFrameMs = 0;

  for (uint8_t i = 0; i < PIXELS; i++) {
    sparkle[i] = 0;
  }

  for (uint8_t x = 0; x < WIDTH; x++) {
    drops[x] = randomByte() % HEIGHT;
  }
}

void clearFrame() {
  for (uint8_t i = 0; i < PIXELS; i++) {
    frame[i] = 0;
  }
}

void setPixel(int8_t x, int8_t y, uint8_t brightness) {
  if (x < 0 || x >= WIDTH || y < 0 || y >= HEIGHT) {
    return;
  }

  uint8_t &pixel = frame[y * WIDTH + x];
  if (brightness > pixel) {
    pixel = brightness > 7 ? 7 : brightness;
  }
}

void addPixel(int8_t x, int8_t y, uint8_t brightness) {
  if (x < 0 || x >= WIDTH || y < 0 || y >= HEIGHT) {
    return;
  }

  uint8_t &pixel = frame[y * WIDTH + x];
  const uint8_t value = pixel + brightness;
  pixel = value > 7 ? 7 : value;
}

uint8_t randomByte() {
  rng = (rng * 1664525UL) + 1013904223UL;
  return (rng >> 24) & 0xFF;
}

uint8_t triangleWave(uint8_t value) {
  return value < 128 ? value / 18 : (255 - value) / 18;
}

void drawOrbit(uint16_t t) {
  const int8_t cx = 6;
  const int8_t cy = 3;

  for (uint8_t i = 0; i < 10; i++) {
    const uint16_t phase = (t * 13 + i * 19) % 104;
    int8_t x;
    int8_t y;

    if (phase < 13) {
      x = phase;
      y = 0;
    } else if (phase < 20) {
      x = 12;
      y = phase - 12;
    } else if (phase < 33) {
      x = 32 - phase;
      y = 7;
    } else if (phase < 40) {
      x = 0;
      y = 39 - phase;
    } else {
      const uint8_t p = phase - 40;
      x = (p * 5 + t) % WIDTH;
      y = 3 + ((p + t) % 3) - 1;
    }

    const uint8_t b = i < 3 ? 7 - i : 2;
    setPixel(x, y, b);
    addPixel((x + cx) / 2, (y + cy) / 2, 1);
  }

  setPixel(cx, cy, 5 + (t % 3));
  setPixel(cx, cy + 1, 4);
}

void drawRain(uint16_t t) {
  for (uint8_t x = 0; x < WIDTH; x++) {
    if ((randomByte() & 0x1F) == 0) {
      drops[x] = 0;
    }

    const uint8_t head = (drops[x] + t + x) % HEIGHT;
    setPixel(x, head, 7);
    setPixel(x, head - 1, 4);
    setPixel(x, head - 2, 2);
  }

  for (uint8_t i = 0; i < 4; i++) {
    const uint8_t x = randomByte() % WIDTH;
    const uint8_t y = randomByte() % HEIGHT;
    addPixel(x, y, 1);
  }
}

void drawSparkles(uint16_t t) {
  for (uint8_t i = 0; i < PIXELS; i++) {
    if (sparkle[i] > 0) {
      sparkle[i]--;
    }
  }

  const uint8_t bursts = 3 + (t % 3);
  for (uint8_t i = 0; i < bursts; i++) {
    if ((randomByte() & 0x03) == 0) {
      sparkle[randomByte() % PIXELS] = 7;
    }
  }

  for (uint8_t y = 0; y < HEIGHT; y++) {
    for (uint8_t x = 0; x < WIDTH; x++) {
      const uint8_t b = sparkle[y * WIDTH + x];
      if (b > 0) {
        setPixel(x, y, b);
        if (b > 5) {
          addPixel(x - 1, y, 2);
          addPixel(x + 1, y, 2);
          addPixel(x, y - 1, 2);
          addPixel(x, y + 1, 2);
        }
      }
    }
  }
}

void drawRipples(uint16_t t) {
  const int8_t centers[3][2] = {
    {2, 2},
    {10, 5},
    {6, 3},
  };

  for (uint8_t y = 0; y < HEIGHT; y++) {
    for (uint8_t x = 0; x < WIDTH; x++) {
      uint8_t brightness = 0;

      for (uint8_t c = 0; c < 3; c++) {
        const int8_t dx = abs(x - centers[c][0]);
        const int8_t dy = abs(y - centers[c][1]);
        const uint8_t distance = dx + dy;
        const uint8_t wave = (distance * 36 + t * 15 + c * 55) & 0xFF;
        const uint8_t b = triangleWave(wave);
        if (b > brightness) {
          brightness = b;
        }
      }

      setPixel(x, y, brightness);
    }
  }
}

void drawScanner(uint16_t t) {
  const int8_t x = t % ((WIDTH - 1) * 2);
  const int8_t beam = x < WIDTH ? x : ((WIDTH - 1) * 2) - x;

  for (uint8_t y = 0; y < HEIGHT; y++) {
    setPixel(beam, y, 7);
    setPixel(beam - 1, y, 4);
    setPixel(beam + 1, y, 4);
    setPixel(beam - 2, y, 1);
    setPixel(beam + 2, y, 1);
  }

  for (uint8_t i = 0; i < 12; i++) {
    const int8_t x0 = (i * 5 + t / 2) % WIDTH;
    const int8_t y0 = (i * 3 + t) % HEIGHT;
    addPixel(x0, y0, 2);
  }
}
