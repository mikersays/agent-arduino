# Arduino UNO Q — board reference

## Architecture: two brains
| | Linux side (MPU) | Sketch side (MCU) |
|---|---|---|
| Chip | Qualcomm Dragonwing **QRB2210** | **STM32U585** |
| Cores | Quad Arm Cortex-A53 @ up to 2.0 GHz + Adreno GPU | Arm Cortex-M33 @ up to 160 MHz |
| Memory | 2 GB LPDDR4, 16 GB eMMC | 2 MB flash, 786 KB SRAM |
| Runs | Debian Linux (Python apps, AI, networking — where the agent lives) | Arduino sketches / Zephyr (real-time I/O) |
| Connectivity | Wi-Fi 5 (2.4/5 GHz), Bluetooth 5.1 | — |

Sketches (this skill) target the **MCU**. The two sides talk via `Arduino_RouterBridge`
(see the unoq-app-bridge skill).

## On-board LEDs — who controls what
- **8×13 blue LED matrix (104 LEDs)** — MCU (sketches). See `led-matrix.md`.
- **RGB LEDs 3 and 4** — MCU (sketches), via variant pin macros `LED3_R/G/B`, `LED4_R/G/B`
  (plain digital pins; assume active-low like `LED_BUILTIN` — see `rgb-status-cycle/` in the
  repo for a working example).
- **RGB LEDs 1 and 2** — Linux side (NOT reachable from sketches): sysfs at
  `/sys/class/leds/unoq:user-{red,green,blue}1` (free for use) and
  `unoq:{bt-blue,panic-red,wlan-green}2` (system-status roles). See the **unoq-linux-hardware**
  skill.
- **`LED_BUILTIN`** — MCU, **active LOW**: `digitalWrite(LED_BUILTIN, LOW)` turns it ON.

## Pins
The board exposes the **standard Arduino UNO header**: digital `D0`–`D13` and analog `A0`–`A5`,
plus I²C (SDA/SCL), SPI, and a QWIIC/Modulino connector. Use the normal Arduino names in code:
```cpp
pinMode(D2, INPUT_PULLUP);
int v = analogRead(A0);
```
For the exact silk-screen-to-GPIO mapping and electrical limits, check the official pinout
(do not guess specific GPIO numbers): https://docs.arduino.cc/hardware/uno-q

## Board menu options (FQBN suffixes)
The `unoq` board defines selectable menus (from `boards.txt`):
- `link_mode` = `dynamic` (default) or `static` — how the llext sketch is linked.
- `wait_linux_boot` = `yes` (wait for Linux) / `no` (immediate) / `app` (wait for the App).

Append as needed, e.g.:
```bash
arduino-cli compile -b arduino:zephyr:unoq:link_mode=static,wait_linux_boot=yes ./sketch
```
Defaults are fine for normal use.

## Toolchain facts (verified on-device)
- Core: `arduino:zephyr`, hardware dir `.../hardware/zephyr/0.56.0`.
- Cross-compiler: `arm-zephyr-eabi-` (Zephyr SDK).
- Sketches compile to **llext loadable extensions**; you may include Zephyr headers
  (`<zephyr/kernel.h>`, `K_MUTEX_DEFINE`, `k_mutex_lock`, threads, etc.).
- Upload/flash goes through the Linux side via OpenOCD (`flash_sketch.cfg`); always upload to the
  board's **network port** (`-p <wlan0-ip>`), not a USB COM port.
