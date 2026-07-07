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
| **`.claude/skills/unoq-sketch/`** | Writing a standalone microcontroller sketch — blink, LED matrix, sensors, I²C/SPI/CAN/RTC. Includes compile/upload workflow and the matrix API. |
| **`.claude/skills/unoq-app-bridge/`** | Building an "Arduino App" where Python on Linux and a sketch on the MCU work together (web UI, AI, networking + real-time I/O) via RouterBridge RPC. |
| **`.claude/skills/unoq-linux-hardware/`** | Controlling the Linux side's own hardware: the user RGB LED via sysfs (solid colors, kernel blink triggers), CPU temperature, network info, and the app/Docker services. |

Each skill has a `SKILL.md`, supporting `references/`, and compile-ready `examples/`.

## Sketch suite

**Browse the catalog: [mikersays.github.io/agent-arduino](https://mikersays.github.io/agent-arduino/)**
(GitHub Pages from `docs/` — when adding a sketch, add it to the `SKETCHES` array in
`docs/index.html` as well as the tables below).

Standalone MCU sketches, one folder each, all compile-verified on this board
(`arduino-cli compile -b arduino:zephyr:unoq ./<folder>`). Only one sketch runs on the MCU
at a time.

**Flash from the board with the bundled CLI** — pick from an interactive list (arrow keys,
type-to-filter) or name a sketch directly:

```bash
./flash                    # interactive picker
./flash matrix-snake -y    # direct, no confirmation
./flash --list             # table of all sketches + descriptions
```

The CLI auto-discovers every `<name>/<name>.ino` and takes its description from the sketch's
first header-comment line — new sketches appear automatically. (From another machine, use
`arduino-cli upload -b arduino:zephyr:unoq -p <board-ip> ./<folder>` instead.)

### On the built-in 8×13 LED matrix

| Sketch | What it does |
|---|---|
| **`matrix-light-show/`** | 8-effect demo reel (orbiters, rain, sparkles, ripples, scanner, comet, interference waves, breathing checkerboard) with smooth cross-fades between effects. |
| **`matrix-life/`** | Conway's Game of Life on a torus, with fading ghost trails and automatic reseed when the population dies out or stagnates. |
| **`matrix-fire/`** | Classic "doom fire": heat rises from the bottom row with random decay and jitter, slowly cycling between calm embers and a raging blaze. |
| **`matrix-plasma/`** | Smooth ~30 fps plasma from four drifting sine fields (the M33's FPU makes `sinf` cheap). |
| **`matrix-snake/`** | Self-playing Snake — BFS pathfinding to the food, survival heuristic when boxed in, restarts on death. |
| **`matrix-pong/`** | Self-playing Pong with fixed-point ball physics, imperfect AI paddles (points do get scored), trails, and a dot-column scoreboard. |
| **`matrix-text-scroller/`** | Scrolling text marquee via ArduinoGraphics (`Font_5x7`), rotating messages including live uptime. |
| **`matrix-analog-scope/`** | Scrolling oscilloscope of `A0` with rolling auto-scale — plots floating-pin noise even with nothing wired. Also feeds the Serial Plotter. |
| **`rtc-matrix-clock/`** | Binary-coded clock driven by the bundled RTC library (seeded from compile time), with a blinking seconds heartbeat. |
| **`matrix-starfield/`** | 3D starfield flying toward the viewer, brightness by depth, flight speed breathing between cruise and warp. |
| **`matrix-sand/`** | Falling-sand simulation: a wandering emitter drops grains that slide and settle, then the pile dissolves and it starts over. |
| **`matrix-maze-solver/`** | Endless show: carve a perfect maze (recursive backtracker), solve it with animated BFS, flash the shortest path, repeat. |
| **`matrix-breakout/`** | Self-playing Breakout — fixed-point ball physics, three brick rows, lives, levels, and a paddle AI that sometimes misses. |
| **`matrix-invaders/`** | Self-playing Space Invaders — marching formation, bombs, and a cannon that dodges before it aims. |
| **`matrix-rule-automata/`** | 1D cellular-automaton waterfall cycling Wolfram rules 30/90/110/184/54 with fading history. |
| **`matrix-wave-pool/`** | Real 2D wave-equation water simulation — raindrops and stones ripple and reflect off the edges. |
| **`matrix-lissajous/`** | Lissajous curve tracer with antialiased head, decaying trails, and smooth transitions between frequency ratios. |
| **`matrix-dvd-bounce/`** | The DVD screensaver — sub-pixel bouncing sprite, brightness patterns per bounce, and a celebration on perfect corner hits. |

### Applications (serial-controlled tools)

Practical tools, not demos. Each takes commands over the serial monitor
(`arduino-cli monitor -p <board-ip> -b arduino:zephyr:unoq -c baudrate=115200`), prints a help
banner on boot, and does something useful with no input at all.

| Sketch | What it does |
|---|---|
| **`matrix-marquee/`** | Scrolling message board: `TEXT <msg>` sets a persistent marquee, `ONCE <msg>` interjects, `SPEED <ms>` tunes it. |
| **`matrix-display-server/`** | Turns the board into a scriptable status display — `BAR 75`, `METER 40 80`, `ROW`/`PIX`/`FILL`/`BLINK` with `OK`/`ERR` replies, drivable from any script. |
| **`pomodoro-timer/`** | Self-running 25/5 Pomodoro desk timer: draining pixel field, pomodoro tally, `PAUSE`/`SKIP`/`WORK n` over serial. |
| **`visual-metronome/`** | Silent musicians' metronome — drift-free anchored beat timing, sub-pixel pendulum, downbeat accents, `BPM`/`TS`/`TAP` tempo. |
| **`morse-beacon/`** | Text-to-Morse transmitter/trainer with ITU-verified table and timing; beacons `UNO Q` from boot, `SEND <text>` / `WPM n`. |
| **`analog-csv-logger/`** | Streams clean CSV from A0–A5 for `monitor \| tee data.csv`; live bar meters with peak-hold on the matrix; `RATE`/`CH`/`VOLTS`. |
| **`dice-roller/`** | Tabletop dice: `ROLL 2d6`, `COIN`, `PICK n` — rolling animation, pip faces, A0-noise entropy, bias-free rejection sampling. |
| **`serial-stopwatch/`** | Millisecond stopwatch/lap timer (`START`/`STOP`/`LAP`, 20-lap table) with seconds bar and binary minutes on the matrix. |
| **`square-wave-generator/`** | Two software square-wave channels on any D-pin (0.1–1000 Hz, duty 1–99%, log `SWEEP`) for testing external circuits. |

### Zephyr RTOS

| Sketch | What it does |
|---|---|
| **`zephyr-threads-demo/`** | Teaching demo of this board's Zephyr superpower: two `k_thread_create` threads animate matrix halves under a `K_MUTEX_DEFINE` lock while `loop()` composites, with RGB-LED thread heartbeats. |

### Other peripherals

| Sketch | What it does |
|---|---|
| **`i2c-scanner/`** | Scans the I²C/QWIIC bus every 5 s, prints a table of found addresses (with known-device annotations) to Serial. |
| **`rgb-status-cycle/`** | Cycles the two MCU-driven RGB LEDs (`LED3_*`/`LED4_*`) through complementary colors with an `LED_BUILTIN` heartbeat; polarity behind a single constant. |

## How these are made discoverable
The skills live at `.claude/skills/` inside this repo — the standard Claude Code **project
skills** location, auto-loaded whenever the agent works in this repository. No symlinks or
per-machine setup: clone the repo and the skills come with it. Edit the files in place; changes
are picked up on the next session.

## Quick start (standalone sketch)
```bash
arduino-cli board list                                   # confirm: Arduino UNO Q  arduino:zephyr:unoq
arduino-cli lib install Arduino_RouterBridge             # one-time Serial/bridge dependency
arduino-cli compile -b arduino:zephyr:unoq ./.claude/skills/unoq-sketch/examples/matrix-heart
arduino-cli upload  -b arduino:zephyr:unoq -p <board-ip> ./.claude/skills/unoq-sketch/examples/matrix-heart
```
Find `<board-ip>` with `ip -brief addr show wlan0`.
