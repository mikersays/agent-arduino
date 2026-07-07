# UNO Q built-in LED matrix (`Arduino_LED_Matrix`)

The UNO Q has a built-in **8 rows × 13 columns = 104 blue LEDs**, driven by the STM32 MCU.
Library is bundled in the Zephyr core: `Arduino_LED_Matrix` (verified on-device at
`~/.arduino15/packages/arduino/hardware/zephyr/0.56.0/libraries/Arduino_LED_Matrix`).

Geometry (from the header): `canvasWidth = 13`, `canvasHeight = 8`.

## Three ways to drive it

### 1. Scrolling text (needs ArduinoGraphics)
```cpp
#include "ArduinoGraphics.h"
#include "Arduino_LED_Matrix.h"
Arduino_LED_Matrix matrix;

void setup() {
  matrix.begin();
  matrix.textFont(Font_5x7);
  matrix.textScrollSpeed(100);   // ms per step
}

void loop() {
  matrix.beginText(0, 0, 127, 0, 0);     // x, y, R, G, B (brightness; matrix is blue-only)
  matrix.print("  hello UNO Q  ");
  matrix.endText(SCROLL_LEFT);           // SCROLL_LEFT/RIGHT/UP/DOWN or NO_SCROLL
  delay(500);
}
```

### 2. Grayscale pixel buffer (104 bytes, brightness 0–7)
Each byte is one LED's brightness. With 3 grayscale bits the valid range is `0..7`.
Buffer is row-major: 8 rows of 13 columns.
```cpp
matrix.setGrayscaleBits(3);             // 0..7 brightness per pixel
uint8_t frame[104];                     // 8 * 13, row-major
// ... fill frame ...
matrix.draw(frame);                     // push to the matrix
matrix.clear();                         // all off
```

### 3. Packed bitmap frames + animations
A frame is **four 32-bit words** (104 bits used) plus a duration. Sequences are arrays of
`{w0, w1, w2, w3, duration_ms}`:
```cpp
const uint32_t animation[][5] = {
  { 0x38022020, 0x810408a0, 0x2200e800, 0x20000000, 66 },
  { 0x1c011010, 0x40820450, 0x11007400, 0x10000000, 66 },
  // ...
};

matrix.loadSequence(animation);
matrix.playSequence(/*loop=*/true);     // or call matrix.next() yourself for precise timing
```
Single static frame:
```cpp
const uint32_t heart[] = { 0x00198a44, 0x44282810, 0x10000000, 0x00000000 };
matrix.loadFrame(heart);                // uint32_t[4]
```

## API quick reference (verified from the on-device header)

| Method | Purpose |
|---|---|
| `begin()` / `end()` | start / stop the matrix |
| `clear()` | turn all LEDs off |
| `setGrayscaleBits(n)` | brightness depth (use 3 → values 0..7) |
| `draw(uint8_t* buf)` | render a 104-byte grayscale buffer |
| `loadFrame(uint32_t[4])` | render one packed bitmap frame |
| `loadSequence(arr)` / `playSequence(loop)` | load + play an animation array |
| `next()` | advance one frame (manual timing) |
| `sequenceDone()` | true when a non-looping sequence finished |
| `set(x,y,r,g,b)` | set one pixel (with ArduinoGraphics) |
| `beginText/print/endText(scroll)` | scrolling/static text (with ArduinoGraphics) |
| `matrixWrite(uint32_t[4])` | low-level write — bits must be bit-reversed via `reverse()` |

## Generating frames

The on-device example app **led-matrix-painter**
(`/var/lib/arduino-app-cli/examples/led-matrix-painter/`) is a web tool that lets you draw frames
and export them as the `uint32_t[][5]` C arrays above. For ad-hoc art, fill a 104-byte buffer and
use `draw()` — it's the simplest path.

**18 compile-verified matrix sketches** live in this repo (`/home/arduino/agent-arduino/`),
covering the grayscale-buffer pattern (`matrix-fire`, `matrix-life`, `matrix-plasma`, games…)
and the ArduinoGraphics text path (`matrix-text-scroller`). Start from one of those rather than
from scratch.

## Gotchas
- **104 bytes exactly** for `draw()`. Wrong length = garbage/overrun.
- It's a **blue** matrix; the R/G/B args are really just brightness — non-zero = on.
- When driving the matrix from Bridge RPC providers (App model), the provider runs on a different
  thread than `loop()`. Guard matrix writes with a Zephyr mutex (see the led-toggle/painter
  examples and the unoq-app-bridge skill).
