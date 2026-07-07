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
  its own address. The address is DHCP-assigned and **changes between sessions** — never reuse a
  remembered one; check with `ip -brief addr show wlan0` (or `arduino-cli board list`).

## The 30-second workflow

A sketch lives in a folder whose name matches the `.ino` file (e.g. `blink/blink.ino`).

```bash
# 0. One-time dependency check: the UNO Q core needs the real RouterBridge library
#    for Serial support, even if the sketch does not call Serial directly.
arduino-cli lib install Arduino_RouterBridge

# 1. Compile (the explicit --build-path is where step 2 finds the binary)
arduino-cli compile -b arduino:zephyr:unoq --build-path /tmp/build-blink ./blink

# 2. Flash — preferred path when running ON the board (non-interactive, no password):
PLAT=~/.arduino15/packages/arduino/hardware/zephyr/0.56.0
~/.arduino15/packages/arduino/tools/remoteocd/0.1.1/remoteocd upload \
  -f $PLAT/variants/arduino_uno_q_stm32u585xx/flash_sketch.cfg \
  $PLAT/firmwares/zephyr-arduino_uno_q_stm32u585xx.elf \
  /tmp/build-blink/blink.ino.elf-zsk.bin
#   (arg 1 = Zephyr firmware, only rewritten if changed; arg 2 = your sketch binary)

# 2-alt. Network upload (from another machine, or if you know the board password):
#   Prompts for the board's SSH password — FAILS in non-interactive sessions
#   unless you pass it with -F password=<pw>.
arduino-cli upload -b arduino:zephyr:unoq -p <board-wlan0-ip> ./blink

# 3. Watch MCU serial output
arduino-cli monitor -p <board-wlan0-ip> -b arduino:zephyr:unoq -c baudrate=115200
#   (or: arduino-app-cli monitor)
```

Confirm the board is detected first with `arduino-cli board list` — it should print
`Arduino UNO Q  arduino:zephyr:unoq`.

> Tip: `arduino-cli compile --clean` if you hit stale-build weirdness. The first Zephyr-core
> build takes tens of seconds; later builds reuse the shared core cache (~5 s).
>
> **Parallel compiles clash**: concurrent `arduino-cli compile` runs can collide on the shared
> build cache and fail with a spurious `exit status 1`. When compiling several sketches at once,
> give each its own `--build-path`; a clean re-run also recovers.

## Working examples to crib from

This repo (`/home/arduino/agent-arduino/`) contains **21 compile-verified sketches** for this
exact board — games with fixed-point physics (`matrix-pong`, `matrix-breakout`,
`matrix-invaders`, `matrix-snake`), buffer simulations (`matrix-fire`, `matrix-wave-pool`,
`matrix-sand`, `matrix-life`), float effects (`matrix-plasma`, `matrix-lissajous`,
`matrix-starfield`), peripherals (`i2c-scanner`, `rtc-matrix-clock`, `rgb-status-cycle`), and
Zephyr threading (`zephyr-threads-demo`). Match their style: non-blocking `millis()` pacing with
the rollover-safe subtraction pattern, LCG randomness, bounds-guarded `setPixel` helpers, and a
header comment with compile/upload commands.

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
3. **`Serial` requires the bridge library.** On this board `Serial` is routed through
   `Arduino_RouterBridge`; the core can error at compile time if the real Library Manager package
   is missing or outdated. Install/update it with `arduino-cli lib install Arduino_RouterBridge`.
   Plain `Serial.begin()/println()` works, but the monitor is reached via the board's network port
   or `arduino-app-cli monitor`, not a classic USB COM port.
4. **Upload over network, not a COM port.** The MCU is flashed via the Linux side; always use
   `-p <board-ip>`.
5. If a sketch needs to talk to Python/Linux (web UI, AI, files, network), it is no longer a
   standalone sketch — switch to the **unoq-app-bridge** skill.

## Zephyr/llext compile gotchas (all hit in practice on this board)

- **Some libc/newlib symbols don't link** in the llext environment even though they compile:
  `strtok_r` is missing (write a manual tokenizer), and libm functions that touch `errno`
  (e.g. `powf`) need a stub: `extern "C" int *__errno(void) { static int e; return &e; }`.
- **Arduino auto-prototypes vs your types**: generated prototypes are emitted *before* your
  `enum`/`struct` definitions, so a function taking a user-defined type (or reference to one)
  as a parameter fails to compile. Use built-in types in function signatures (pass an index or
  `uint8_t` instead of `MyEnum`/`Channel&`), or add an explicit forward declaration after the
  type definition.
- **Macro collisions**: common ALL-CAPS names like `LINE_MAX` are libc macros — pick another
  name if you get a bizarre redefinition error.

## When something fails

- `board not found` → `arduino-cli board list`; ensure you used the wlan0 IP and the board is on
  the network.
- Compile error mentioning `Arduino_RouterBridge` / Serial or a path under
  `libraries/stubs/Arduino_RouterBridge.h` → install/update the real bridge package with
  `arduino-cli lib install Arduino_RouterBridge`, then compile again.
- No serial output → confirm `baudrate=115200` and that `Serial.begin(115200)` is in `setup()`.
- `arduino-cli upload` fails with `Error getting user input: user input not supported in non
  interactive mode` → the network upload prompts for the board's SSH password. Use the
  passwordless local `remoteocd` flash from step 2 of the workflow above instead, or pass
  `-F password=<pw>`.
