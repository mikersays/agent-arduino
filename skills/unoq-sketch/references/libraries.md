# Bundled libraries available to UNO Q sketches

These ship **inside the Zephyr core** (verified at
`~/.arduino15/packages/arduino/hardware/zephyr/0.56.0/libraries/`) — no install needed:

| Library | Include | Use |
|---|---|---|
| Arduino_LED_Matrix | `<Arduino_LED_Matrix.h>` | the built-in 8×13 matrix (see `led-matrix.md`) |
| Wire | `<Wire.h>` | I²C master/slave |
| SPI | `<SPI.h>` | SPI bus |
| CAN | `<CAN.h>` | CAN bus |
| RTC | `<RTC.h>` | real-time clock |
| SocketWrapper | — | networking glue used by the bridge |
| ea_malloc | — | heap helper for llext |

Also bundled internally (auto-available to sketches / Apps):

| Library | Include | Use |
|---|---|---|
| Arduino_RouterBridge | `<Arduino_RouterBridge.h>` | RPC bridge MCU↔Linux **and** `Serial` transport. See unoq-app-bridge skill. |
| Arduino_Modulino | `<Modulino.h>` | Arduino's QWIIC modules: Knob, Buttons, Pixels, Buzzer, Movement, Distance, Thermo, LED matrix module, etc. |
| ArduinoGraphics | `<ArduinoGraphics.h>` | text/drawing primitives used by the LED matrix |

## Installing extra libraries
```bash
arduino-cli lib search <name>
arduino-cli lib install "<Library Name>"
```
Not every Arduino library supports the Zephyr/Cortex-M33 target — prefer the bundled ones and
official Arduino libraries. If a library fails to compile, it likely uses an unsupported
architecture; look for a Zephyr-compatible alternative.

## Modulino quick example (QWIIC plug-and-play sensors/actuators)
```cpp
#include <Modulino.h>
ModulinoKnob knob;
void setup() { Modulino.begin(); knob.begin(); }
void loop()  { int p = knob.get(); /* ... */ }
```
Modulino examples live on-device under
`~/.arduino15/internal/Arduino_Modulino_*/Arduino_Modulino/examples/`.
