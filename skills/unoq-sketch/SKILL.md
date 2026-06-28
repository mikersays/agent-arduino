---
name: unoq-sketch
description: Write, compile, and upload standalone MCU sketches (.ino) for THIS Arduino UNO Q board (STM32U585 microcontroller, Zephyr core, FQBN arduino:zephyr:unoq). Use whenever the user asks to write Arduino code/sketches for this device, blink LEDs, drive the built-in 8x13 LED matrix, read sensors, or use I2C/SPI/CAN/RTC on the microcontroller. For apps that connect Python on the Linux side to the sketch, see the unoq-app-bridge skill instead.
---

# Writing sketches for the Arduino UNO Q (this board)

This board is an **Arduino UNO Q** — a hybrid device. Sketches written here run on the
**STM32U585 microcontroller (MCU)**, *not* on the Linux side. The Linux side (Qualcomm
Dragonwing, Debian — where you, the agent, are running) talks to the MCU over a bridge.

- **MCU:** STM32U585, Arm Cortex-M33 @ up to 160 MHz, 2 MB flash, 786 KB SRAM
- **Core:** `arduino:zephyr` — sketches are compiled as **Zephyr llext loadable extensions**, so
  Zephyr kernel APIs (`#include <zephyr/kernel.h>`, mutexes, threads) are available alongside the
  normal Arduino API.
- **FQBN:** `arduino:zephyr:unoq`
- **This board's IP (upload target):** the board appears to `arduino-cli` as a *network port* at
  its own address. Find it with `ip -brief addr show wlan0` (currently `192.168.1.170`).

## The 30-second workflow

A sketch lives in a folder whose name matches the `.ino` file (e.g. `blink/blink.ino`).

```bash
# 1. Compile
arduino-cli compile -b arduino:zephyr:unoq ./blink

# 2. Upload to THIS board over the network (use the board's own wlan0 IP)
arduino-cli upload -b arduino:zephyr:unoq -p 192.168.1.170 ./blink

# 3. Watch MCU serial output
arduino-cli monitor -p 192.168.1.170 -b arduino:zephyr:unoq -c baudrate=115200
#   (or: arduino-app-cli monitor)
```

Confirm the board is detected first with `arduino-cli board list` — it should print
`Arduino UNO Q  arduino:zephyr:unoq`.

> Tip: `arduino-cli compile --clean` if you hit stale-build weirdness. Builds for the Zephyr
> core are slower than classic AVR — expect tens of seconds.

## Built-in hardware you can use immediately

| Feature | How | Reference |
|---|---|---|
| **8×13 blue LED matrix (104 LEDs)** | `#include <Arduino_LED_Matrix.h>` | `references/led-matrix.md` |
| **User RGB LEDs** | 4 on board — 2 driven by the MCU, 2 by the Linux MPU | `references/board.md` |
| `LED_BUILTIN` | `pinMode(LED_BUILTIN, OUTPUT)` — **active LOW** (LOW = on) | `references/board.md` |
| Digital/analog pins | Standard UNO header: `D0`–`D13`, `A0`–`A5` | `references/board.md` |
| I²C | `#include <Wire.h>` | `references/libraries.md` |
| SPI | `#include <SPI.h>` | `references/libraries.md` |
| CAN bus | `#include <CAN.h>` | `references/libraries.md` |
| RTC | `#include <RTC.h>` | `references/libraries.md` |
| Modulino QWIIC modules | `Arduino_Modulino` (knob, buttons, pixels, sensors) | `references/libraries.md` |

## Minimal sketch template

```cpp
void setup() {
  pinMode(LED_BUILTIN, OUTPUT);
  Serial.begin(115200);
}

void loop() {
  digitalWrite(LED_BUILTIN, LOW);   // active-low: LOW turns it ON
  delay(500);
  digitalWrite(LED_BUILTIN, HIGH);
  delay(500);
}
```

See `examples/blink/` and `examples/matrix-heart/` for complete, compile-tested sketches.

## Rules that matter on this board (don't skip)

1. **`LED_BUILTIN` is active-low.** `digitalWrite(LED_BUILTIN, LOW)` turns it **on**.
2. **The LED matrix is 13 wide × 8 tall (104 LEDs)** — *not* the 12×8 of the UNO R4. Pixel
   buffers passed to `matrix.draw()` must be exactly **104 bytes**.
3. **`Serial` requires the bridge.** On this board `Serial` is routed through
   `Arduino_RouterBridge`; the core errors at compile time if the library is outdated. Plain
   `Serial.begin()/println()` works, but the monitor is reached via the board's network port or
   `arduino-app-cli monitor`, not a classic USB COM port.
4. **Upload over network, not a COM port.** The MCU is flashed via the Linux side; always use
   `-p <board-ip>`.
5. If a sketch needs to talk to Python/Linux (web UI, AI, files, network), it is no longer a
   standalone sketch — switch to the **unoq-app-bridge** skill.

## When something fails

- `board not found` → `arduino-cli board list`; ensure you used the wlan0 IP and the board is on
  the network.
- Compile error mentioning `Arduino_RouterBridge` / Serial → the core wants the bundled bridge
  library; it ships in the core, so re-run `arduino-cli core list` and update `arduino:zephyr`.
- No serial output → confirm `baudrate=115200` and that `Serial.begin(115200)` is in `setup()`.
