// UNO Q onboard LED matrix light show.
// Uses the built-in 13x8 blue LED matrix as a 3-bit grayscale display
// (brightness 0..7) and cycles through eight effects, cross-fading the
// frame brightness over ~500 ms at every effect boundary:
//   0 orbit        - two comet heads chasing around the matrix border
//   1 rain         - drops spawning at the top and falling off screen
//   2 sparkles     - random flashes with decaying glow halos
//   3 ripples      - interfering triangle-wave rings from three centers
//   4 scanner      - Larson-style ping-pong beam with drifting embers
//   5 comet        - bouncing head that leaves a persistent decaying trail
//   6 waves        - two counter-moving wave sources forming a moire pattern
//   7 checkerboard - two interleaved pixel sets breathing in anti-phase
//
// No external hardware required.
//
// Compile:
//   arduino-cli compile -b arduino:zephyr:unoq ./matrix-light-show
//
// Upload:
//   arduino-cli upload -b arduino:zephyr:unoq -p <board-ip> ./matrix-light-show

#include "Arduino_LED_Matrix.h"

Arduino_LED_Matrix matrix;

const uint8_t WIDTH = 13;
const uint8_t HEIGHT = 8;
const uint8_t PIXELS = WIDTH * HEIGHT;
const uint8_t PERIMETER = 2 * (WIDTH + HEIGHT) - 4;  // 38 border cells
const uint8_t MAX_BRIGHT = 7;
const uint16_t FRAME_MS = 45;
const uint16_t EFFECT_MS = 6000;
const uint16_t FADE_MS = 500;  // Cross-fade window at each end of an effect.
const uint8_t NUM_EFFECTS = 8;

uint8_t frame[PIXELS];
uint32_t lastFrameMs = 0;
uint32_t effectStartMs = 0;
uint8_t effect = 0;
uint32_t rng = 0xC0FFEE42;

uint8_t sparkle[PIXELS];  // Decaying flash intensities for drawSparkles.
uint8_t trail[PIXELS];    // Persistent decaying trail for drawComet.
uint8_t drops[WIDTH];     // Head row of each rain column (>= DROP_DONE = idle).
int8_t cometX = 0;
int8_t cometY = 0;
int8_t cometDX = 1;
int8_t cometDY = 1;

const uint8_t DROP_DONE = HEIGHT + 2;  // Row where the whole trail has left.

void setup() {
  matrix.begin();
  matrix.setGrayscaleBits(3);  // Brightness values are 0..7.
  resetEffect();
}

void loop() {
  const uint32_t now = millis();

  if (now - effectStartMs >= EFFECT_MS) {
    effect = (effect + 1) % NUM_EFFECTS;
    resetEffect();
  }

  if (now - lastFrameMs < FRAME_MS) {
    return;
  }

  lastFrameMs = now;
  const uint32_t elapsed = now - effectStartMs;
  const uint16_t t = elapsed / FRAME_MS;

  clearFrame();

  switch (effect) {
    case 0: drawOrbit(t); break;
    case 1: drawRain(t); break;
    case 2: drawSparkles(t); break;
    case 3: drawRipples(t); break;
    case 4: drawScanner(t); break;
    case 5: drawComet(t); break;
    case 6: drawWaves(t); break;
    default: drawCheckerboard(t); break;
  }

  applyFade(elapsed);
  matrix.draw(frame);
}

void resetEffect() {
  effectStartMs = millis();
  lastFrameMs = 0;  // Forces the next loop() pass to render immediately.

  for (uint8_t i = 0; i < PIXELS; i++) {
    sparkle[i] = 0;
    trail[i] = 0;
  }

  for (uint8_t x = 0; x < WIDTH; x++) {
    drops[x] = randomByte() % (DROP_DONE + 1);
  }

  cometX = 2 + (randomByte() % (WIDTH - 4));
  cometY = 1 + (randomByte() % (HEIGHT - 2));
  cometDX = (randomByte() & 1) ? 1 : -1;
  cometDY = (randomByte() & 1) ? 1 : -1;
}

void clearFrame() {
  for (uint8_t i = 0; i < PIXELS; i++) {
    frame[i] = 0;
  }
}

// Scales the whole frame down near effect boundaries: ramps 0->full over
// the first FADE_MS and full->0 over the last FADE_MS of each effect.
void applyFade(uint32_t elapsed) {
  uint32_t scale = 8;

  if (elapsed < FADE_MS) {
    scale = (elapsed * 8) / FADE_MS;
  } else if (EFFECT_MS - elapsed < FADE_MS) {
    scale = ((EFFECT_MS - elapsed) * 8) / FADE_MS;
  }

  if (scale >= 8) {
    return;
  }

  for (uint8_t i = 0; i < PIXELS; i++) {
    frame[i] = (frame[i] * scale) / 8;
  }
}

void setPixel(int8_t x, int8_t y, uint8_t brightness) {
  if (x < 0 || x >= WIDTH || y < 0 || y >= HEIGHT) {
    return;
  }

  uint8_t &pixel = frame[y * WIDTH + x];
  if (brightness > pixel) {
    pixel = brightness > MAX_BRIGHT ? MAX_BRIGHT : brightness;
  }
}

void addPixel(int8_t x, int8_t y, uint8_t brightness) {
  if (x < 0 || x >= WIDTH || y < 0 || y >= HEIGHT) {
    return;
  }

  uint8_t &pixel = frame[y * WIDTH + x];
  const uint8_t value = pixel + brightness;
  pixel = value > MAX_BRIGHT ? MAX_BRIGHT : value;
}

uint8_t randomByte() {
  rng = (rng * 1664525UL) + 1013904223UL;
  return (rng >> 24) & 0xFF;
}

// Maps a 0..255 phase onto a 0..7 triangle: rises then falls.
uint8_t triangleWave(uint8_t value) {
  return value < 128 ? value / 18 : (255 - value) / 18;
}

// Maps a border position (0..PERIMETER-1) to matrix coordinates,
// walking clockwise from the top-left corner with each corner visited once:
// top row 0..12, right column 13..19, bottom row 20..31, left column 32..37.
void borderPixel(uint8_t pos, int8_t &x, int8_t &y) {
  pos %= PERIMETER;

  if (pos < WIDTH) {                          // Top row, left to right.
    x = pos;
    y = 0;
  } else if (pos < WIDTH + HEIGHT - 1) {      // Right column, top to bottom.
    x = WIDTH - 1;
    y = pos - (WIDTH - 1);
  } else if (pos < 2 * WIDTH + HEIGHT - 2) {  // Bottom row, right to left.
    x = (2 * WIDTH + HEIGHT - 3) - pos;
    y = HEIGHT - 1;
  } else {                                    // Left column, bottom to top.
    x = 0;
    y = PERIMETER - pos;
  }
}

void drawOrbit(uint16_t t) {
  const int8_t cx = 6;
  const int8_t cy = 3;
  const uint8_t TAIL = 6;
  const uint8_t head = (t * 2) % PERIMETER;

  for (uint8_t i = 0; i < TAIL; i++) {
    int8_t x;
    int8_t y;

    // Clockwise orbiter.
    borderPixel((head + PERIMETER - i) % PERIMETER, x, y);
    setPixel(x, y, MAX_BRIGHT - i);

    // Counter-clockwise orbiter, offset half a lap.
    borderPixel((PERIMETER + PERIMETER / 2 - head + i) % PERIMETER, x, y);
    setPixel(x, y, MAX_BRIGHT - i);
  }

  // Pulsing core.
  setPixel(cx, cy, 5 + (t % 3));
  setPixel(cx, cy + 1, 4);
}

void drawRain(uint16_t t) {
  (void)t;

  for (uint8_t x = 0; x < WIDTH; x++) {
    if (drops[x] >= DROP_DONE) {
      // Column is idle; randomly spawn a new drop at the top.
      if ((randomByte() & 0x07) == 0) {
        drops[x] = 0;
      }
      continue;
    }

    const int8_t head = drops[x];
    setPixel(x, head, 7);
    setPixel(x, head - 1, 4);
    setPixel(x, head - 2, 2);
    drops[x]++;
  }

  // Faint ambient splashes.
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
      sparkle[randomByte() % PIXELS] = MAX_BRIGHT;
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
  const uint8_t span = (WIDTH - 1) * 2;
  const int8_t x = t % span;
  const int8_t beam = x < WIDTH ? x : span - x;

  for (uint8_t y = 0; y < HEIGHT; y++) {
    setPixel(beam, y, 7);
    setPixel(beam - 1, y, 4);
    setPixel(beam + 1, y, 4);
    setPixel(beam - 2, y, 1);
    setPixel(beam + 2, y, 1);
  }

  // Drifting embers behind the beam.
  for (uint8_t i = 0; i < 12; i++) {
    const int8_t x0 = (i * 5 + t / 2) % WIDTH;
    const int8_t y0 = (i * 3 + t) % HEIGHT;
    addPixel(x0, y0, 2);
  }
}

// Bouncing comet head leaving a persistent trail that decays to black.
void drawComet(uint16_t t) {
  // Decay the trail every other frame (~14 frames = ~630 ms of afterglow).
  if ((t & 1) == 0) {
    for (uint8_t i = 0; i < PIXELS; i++) {
      if (trail[i] > 0) {
        trail[i]--;
      }
    }
  }

  cometX += cometDX;
  if (cometX <= 0) {
    cometX = 0;
    cometDX = 1;
  } else if (cometX >= WIDTH - 1) {
    cometX = WIDTH - 1;
    cometDX = -1;
  }

  cometY += cometDY;
  if (cometY <= 0) {
    cometY = 0;
    cometDY = 1;
    // Occasional horizontal kick so the path does not settle into a loop.
    if ((randomByte() & 0x03) == 0) {
      cometDX = -cometDX;
    }
  } else if (cometY >= HEIGHT - 1) {
    cometY = HEIGHT - 1;
    cometDY = -1;
  }

  trail[cometY * WIDTH + cometX] = MAX_BRIGHT;

  for (uint8_t y = 0; y < HEIGHT; y++) {
    for (uint8_t x = 0; x < WIDTH; x++) {
      setPixel(x, y, trail[y * WIDTH + x]);
    }
  }
}

// Two wave sources moving in opposite directions; their sum forms a
// slowly shifting interference (moire) pattern.
void drawWaves(uint16_t t) {
  const int8_t sx1 = 3;
  const int8_t sy1 = 2;
  const int8_t sx2 = 9;
  const int8_t sy2 = 5;

  for (uint8_t y = 0; y < HEIGHT; y++) {
    for (uint8_t x = 0; x < WIDTH; x++) {
      const uint8_t d1 = abs(x - sx1) + abs(y - sy1);
      const uint8_t d2 = abs(x - sx2) + abs(y - sy2);
      const uint8_t w1 = triangleWave((uint8_t)(d1 * 40 - t * 11));
      const uint8_t w2 = triangleWave((uint8_t)(d2 * 40 + t * 7));
      setPixel(x, y, (w1 + w2) / 2);
    }
  }
}

// Checkerboard whose two interleaved pixel sets breathe in anti-phase:
// as the "black" squares brighten, the "white" squares dim, and vice versa.
void drawCheckerboard(uint16_t t) {
  const uint8_t phase = (uint8_t)(t * 5);
  const uint8_t a = triangleWave(phase);
  const uint8_t b = triangleWave((uint8_t)(phase + 128));

  for (uint8_t y = 0; y < HEIGHT; y++) {
    for (uint8_t x = 0; x < WIDTH; x++) {
      setPixel(x, y, ((x + y) & 1) ? a : b);
    }
  }
}
