---
name: unoq-linux-hardware
description: Control and read THIS Arduino UNO Q board's Linux-side (Qualcomm Dragonwing MPU) hardware — the user RGB LED via sysfs (solid colors, heartbeat/timer blink triggers), CPU temperature, Wi-Fi/network info, and Docker/app services. Use when the user wants to light or blink the Linux-side LEDs, check board temperature or IP, or inspect what's running on the Linux side. For the LED matrix, LED3/LED4, or anything driven by a sketch, use unoq-sketch; for Python↔MCU apps, use unoq-app-bridge.
---

# Linux-side hardware on the Arduino UNO Q (this board)

You (the agent) run on the board's Linux side — a Qualcomm Dragonwing QRB2210 (quad Cortex-A53,
Debian). Some on-board hardware belongs to Linux, not to sketches. Everything below is verified
on this device and needs **no sudo** (works as the `arduino` user).

## The Linux-side RGB LEDs (sysfs)

Two of the four RGB LEDs are wired to the MPU. Channels are on/off only (`max_brightness` = 1):

| LED | sysfs names under `/sys/class/leds/` | Use |
|---|---|---|
| **LED 1 (user)** | `unoq:user-red1`, `unoq:user-green1`, `unoq:user-blue1` | free for any purpose |
| **LED 2 (status)** | `unoq:bt-blue2`, `unoq:panic-red2`, `unoq:wlan-green2` | system-status roles (Bluetooth/panic/Wi-Fi) — prefer leaving these alone |

(The other two RGB LEDs, LED3/LED4, belong to the MCU — drive them from a sketch via the
`LED3_R`…`LED4_B` pin macros; see the unoq-sketch skill.)

```bash
# Solid color: mix channels — e.g. magenta
echo 1 > "/sys/class/leds/unoq:user-red1/brightness"
echo 1 > "/sys/class/leds/unoq:user-blue1/brightness"

# Off
echo 0 > "/sys/class/leds/unoq:user-red1/brightness"
echo 0 > "/sys/class/leds/unoq:user-blue1/brightness"
```

### Kernel triggers (hardware-blink without a process)

Each channel has a `trigger` file. Verified available: `none`, `timer`, `heartbeat`,
`default-on`, `cpu`/`cpu0`–`cpu3`, `disk-activity`, plus bluetooth/rfkill/phy triggers.

```bash
# Heartbeat pulse on green (survives your shell exiting; kernel does the blinking)
echo heartbeat > "/sys/class/leds/unoq:user-green1/trigger"

# Custom blink: 100 ms on / 900 ms off
echo timer > "/sys/class/leds/unoq:user-blue1/trigger"
echo 100 > "/sys/class/leds/unoq:user-blue1/delay_on"
echo 900 > "/sys/class/leds/unoq:user-blue1/delay_off"

# Always restore when done: trigger back to none, brightness 0
echo none > "/sys/class/leds/unoq:user-blue1/trigger"
echo 0 > "/sys/class/leds/unoq:user-blue1/brightness"
```

See `examples/led-cycle.sh` for a tested demo (color wheel on LED1, then cleanup).

## Sensors & system info

```bash
# SoC temperature (millidegrees C; two zones)
cat /sys/class/thermal/thermal_zone0/type /sys/class/thermal/thermal_zone0/temp
#   e.g. mapss-thermal / 37000  → 37.0 °C

# Board IP — DHCP-assigned, changes between sessions; never hardcode it
ip -brief addr show wlan0

# CPU / memory
nproc; free -h; cat /proc/loadavg
```

## What's running on the Linux side

- `arduino-app-cli.service` — the app daemon (see unoq-app-bridge skill). Apps' Python runs in
  Docker containers (`docker ps` to inspect).
- MCU flashing is performed *by this side* over SWD/OpenOCD — that's why sketches can be flashed
  locally without a password (see unoq-sketch skill, workflow step 2).

## Rules

1. **On/off only** — no PWM on these sysfs LEDs; fake intensity with `timer` duty cycles.
2. **Clean up after demos**: set `trigger` to `none` and `brightness` to `0`, or the LED keeps
   blinking forever (kernel-driven).
3. Leave the LED2 status channels (`bt`, `panic`, `wlan`) on their system roles unless the user
   explicitly wants them.
4. Quote the sysfs paths in shell commands — the `:` in the LED names is fine, but tab-completion
   and some tools stumble on it.
