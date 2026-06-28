# arduino/ — Agent skills for this Arduino UNO Q

This folder holds Claude Code **agent skills** that teach the agent how to write code for *this
specific board* (an Arduino UNO Q), plus runnable example sketches/apps.

Everything here was derived from the actual software installed on this board (the Zephyr core,
`Arduino_LED_Matrix`, `Arduino_RouterBridge`, and the on-device example apps), not from generic
assumptions — so the APIs and commands match what's really here.

## The board in one paragraph
The UNO Q is a hybrid: a **Qualcomm Dragonwing QRB2210** running Debian Linux (where the agent
lives) paired with an **STM32U585 microcontroller** that runs Arduino sketches. The two sides
talk over the RouterBridge. The board has a built-in **8×13 (104-LED) blue matrix**, 4 RGB LEDs,
and the standard UNO header. FQBN: `arduino:zephyr:unoq`.

## Skills

| Skill | Use it when… |
|---|---|
| **`skills/unoq-sketch/`** | Writing a standalone microcontroller sketch — blink, LED matrix, sensors, I²C/SPI/CAN/RTC. Includes compile/upload workflow and the matrix API. |
| **`skills/unoq-app-bridge/`** | Building an "Arduino App" where Python on Linux and a sketch on the MCU work together (web UI, AI, networking + real-time I/O) via RouterBridge RPC. |

Each skill has a `SKILL.md`, supporting `references/`, and compile-ready `examples/`.

## How these are made discoverable
The two skill directories are symlinked into `~/.claude/skills/`, which is where Claude Code
auto-loads personal skills from. Edit the files here; the symlinks pick up changes. To remove a
skill from the agent, delete its symlink in `~/.claude/skills/`.

## Quick start (standalone sketch)
```bash
arduino-cli board list                                   # confirm: Arduino UNO Q  arduino:zephyr:unoq
arduino-cli compile -b arduino:zephyr:unoq ./skills/unoq-sketch/examples/matrix-heart
arduino-cli upload  -b arduino:zephyr:unoq -p <board-ip> ./skills/unoq-sketch/examples/matrix-heart
```
Find `<board-ip>` with `ip -brief addr show wlan0`.
