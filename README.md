# Arduino UNO Q sketches — for people and coding agents

Run 30 standalone sketches on your UNO Q with **free Arduino tools**. You do not
need an AI subscription, API key, coding assistant, or GitHub account to download
the code. Beginners can start with the Arduino IDE; terminal users can use the
bundled `flash` helper. The optional agent skills remain available below.

**[Start here: setup and your first upload](https://mikersays.github.io/agent-arduino/getting-started.html)** ·
**[Instructions for every sketch](https://mikersays.github.io/agent-arduino/sketches.html)** ·
**[Browse the catalog](https://mikersays.github.io/agent-arduino/)**

## Upload your first light show

1. Complete [Arduino's UNO Q setup](https://docs.arduino.cc/tutorials/uno-q/user-manual/)
   with Arduino App Lab, then install the free [Arduino IDE 2](https://www.arduino.cc/en/software/).
2. Download this repository with **Code → Download ZIP** and extract it.
3. In IDE Boards Manager, search for **UNO Q** and install its Zephyr core.
   In Library Manager, install **Arduino_RouterBridge** and **ArduinoGraphics**.
4. Connect the UNO Q with a USB-C data cable. Select **Arduino UNO Q** and its port.
5. Open `matrix-light-show/matrix-light-show.ino`, click **Verify**, then **Upload**.
   You should see changing blue patterns on the built-in matrix.

No external components are needed for this first project. Uploading replaces the
current microcontroller sketch. Stop running App Lab apps first so they do not
replace your sketch. The [complete walkthrough](https://mikersays.github.io/agent-arduino/getting-started.html)
explains setup, dependencies, terminal uploads, serial commands, editing code,
wiring, and troubleshooting. Each sketch folder also has its own README.

The existing suite was compiled on the original board with `arduino:zephyr` core
0.56.0. That is historical compile evidence, not a hardware test of every sketch.
Typed serial input and most visual behavior still need hardware verification;
see [MAINTENANCE.md](MAINTENANCE.md). Current Arduino IDE supports USB uploads to
UNO Q; older agent notes describe the original board's network/local workflow.

## The board in one paragraph
The UNO Q is a hybrid: a **Qualcomm Dragonwing QRB2210** running Debian Linux (for apps and development tools) paired with an **STM32U585 microcontroller** that runs Arduino sketches. The two sides
talk over the RouterBridge. The board has a built-in **8×13 (104-LED) blue matrix**, 4 RGB LEDs,
and the standard UNO header. FQBN: `arduino:zephyr:unoq`.

## Optional skills for coding agents

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
(`arduino-cli compile -b arduino:zephyr:unoq ./<folder>`). These are historical checks. Only one sketch runs on the MCU
at a time.

**Flash from the UNO Q Linux terminal with the bundled CLI** — pick from an interactive list (arrow keys,
type-to-filter) or name a sketch directly:

```bash
./flash                    # interactive picker
./flash matrix-snake       # choose directly, then confirm
./flash --list             # table of all sketches + descriptions
```

The CLI auto-discovers every `<name>/<name>.ino` and takes its description from the sketch's
first header-comment line — new sketches appear automatically. (From another machine, use
`arduino-cli upload -b arduino:zephyr:unoq -p <board-ip> ./<folder>` instead.)

### On the built-in 8×13 LED matrix

| Sketch | What it does |
|---|---|
| **[matrix-light-show](matrix-light-show/README.md)** | 8-effect demo reel (orbiters, rain, sparkles, ripples, scanner, comet, interference waves, breathing checkerboard) with smooth cross-fades between effects. |
| **[matrix-life](matrix-life/README.md)** | Conway's Game of Life on a torus, with fading ghost trails and automatic reseed when the population dies out or stagnates. |
| **[matrix-fire](matrix-fire/README.md)** | Classic "doom fire": heat rises from the bottom row with random decay and jitter, slowly cycling between calm embers and a raging blaze. |
| **[matrix-plasma](matrix-plasma/README.md)** | Smooth ~30 fps plasma from four drifting sine fields (the M33's FPU makes `sinf` cheap). |
| **[matrix-snake](matrix-snake/README.md)** | Self-playing Snake — BFS pathfinding to the food, survival heuristic when boxed in, restarts on death. |
| **[matrix-pong](matrix-pong/README.md)** | Self-playing Pong with fixed-point ball physics, imperfect AI paddles (points do get scored), trails, and a dot-column scoreboard. |
| **[matrix-text-scroller](matrix-text-scroller/README.md)** | Scrolling text marquee via ArduinoGraphics (`Font_5x7`), rotating messages including live uptime. |
| **[matrix-analog-scope](matrix-analog-scope/README.md)** | Scrolling oscilloscope of `A0` with rolling auto-scale — plots floating-pin noise even with nothing wired. Also feeds the Serial Plotter. |
| **[rtc-matrix-clock](rtc-matrix-clock/README.md)** | Binary-coded clock driven by the bundled RTC library (seeded from compile time), with a blinking seconds heartbeat. |
| **[matrix-starfield](matrix-starfield/README.md)** | 3D starfield flying toward the viewer, brightness by depth, flight speed breathing between cruise and warp. |
| **[matrix-sand](matrix-sand/README.md)** | Falling-sand simulation: a wandering emitter drops grains that slide and settle, then the pile dissolves and it starts over. |
| **[matrix-maze-solver](matrix-maze-solver/README.md)** | Endless show: carve a perfect maze (recursive backtracker), solve it with animated BFS, flash the shortest path, repeat. |
| **[matrix-breakout](matrix-breakout/README.md)** | Self-playing Breakout — fixed-point ball physics, three brick rows, lives, levels, and a paddle AI that sometimes misses. |
| **[matrix-invaders](matrix-invaders/README.md)** | Self-playing Space Invaders — marching formation, bombs, and a cannon that dodges before it aims. |
| **[matrix-rule-automata](matrix-rule-automata/README.md)** | 1D cellular-automaton waterfall cycling Wolfram rules 30/90/110/184/54 with fading history. |
| **[matrix-wave-pool](matrix-wave-pool/README.md)** | Real 2D wave-equation water simulation — raindrops and stones ripple and reflect off the edges. |
| **[matrix-lissajous](matrix-lissajous/README.md)** | Lissajous curve tracer with antialiased head, decaying trails, and smooth transitions between frequency ratios. |
| **[matrix-dvd-bounce](matrix-dvd-bounce/README.md)** | The DVD screensaver — sub-pixel bouncing sprite, brightness patterns per bounce, and a celebration on perfect corner hits. |

### Applications (serial-controlled tools)

Practical tools, not demos. Each implements commands for the serial monitor
(`arduino-cli monitor -p <board-ip> -b arduino:zephyr:unoq -c baudrate=115200`), prints a help
banner on boot, and has a documented startup state. Some wait for commands; see
the README in each sketch folder. Serial input still needs a hardware check.

| Sketch | What it does |
|---|---|
| **[matrix-marquee](matrix-marquee/README.md)** | Scrolling message board: `TEXT <msg>` sets a persistent marquee, `ONCE <msg>` interjects, `SPEED <ms>` tunes it. |
| **[matrix-display-server](matrix-display-server/README.md)** | Turns the board into a scriptable status display — `BAR 75`, `METER 40 80`, `ROW`/`PIX`/`FILL`/`BLINK` with `OK`/`ERR` replies, drivable from any script. |
| **[pomodoro-timer](pomodoro-timer/README.md)** | Self-running 25/5 Pomodoro desk timer: draining pixel field, pomodoro tally, `PAUSE`/`SKIP`/`WORK n` over serial. |
| **[visual-metronome](visual-metronome/README.md)** | Silent musicians' metronome — drift-free anchored beat timing, sub-pixel pendulum, downbeat accents, `BPM`/`TS`/`TAP` tempo. |
| **[morse-beacon](morse-beacon/README.md)** | Text-to-Morse transmitter/trainer with ITU-verified table and timing; beacons `UNO Q` from boot, `SEND <text>` / `WPM n`. |
| **[analog-csv-logger](analog-csv-logger/README.md)** | Streams clean CSV from A0–A5 for `monitor \| tee data.csv`; live bar meters with peak-hold on the matrix; `RATE`/`CH`/`VOLTS`. |
| **[dice-roller](dice-roller/README.md)** | Tabletop dice: `ROLL 2d6`, `COIN`, `PICK n` — rolling animation, pip faces, A0-noise entropy, bias-free rejection sampling. |
| **[serial-stopwatch](serial-stopwatch/README.md)** | Millisecond stopwatch/lap timer (`START`/`STOP`/`LAP`, 20-lap table) with seconds bar and binary minutes on the matrix. |
| **[square-wave-generator](square-wave-generator/README.md)** | Two software square-wave channels on D2–D13 (0.1–1000 Hz, duty 1–99%, log `SWEEP`) for testing external circuits. |

### Zephyr RTOS

| Sketch | What it does |
|---|---|
| **[zephyr-threads-demo](zephyr-threads-demo/README.md)** | Teaching demo of this board's Zephyr superpower: two `k_thread_create` threads animate matrix halves under a `K_MUTEX_DEFINE` lock while `loop()` composites, with RGB-LED thread heartbeats. |

### Other peripherals

| Sketch | What it does |
|---|---|
| **[i2c-scanner](i2c-scanner/README.md)** | Scans the SDA/SCL header I²C bus (`Wire`; Qwiic uses `Wire1`) every 5 s, prints a table of found addresses (with known-device annotations) to Serial. |
| **[rgb-status-cycle](rgb-status-cycle/README.md)** | Cycles the two MCU-driven RGB LEDs (`LED3_*`/`LED4_*`) through complementary colors with an `LED_BUILTIN` heartbeat; polarity behind a single constant. |

## How these are made discoverable
The skills live at `.claude/skills/` inside this repo — the standard Claude Code **project
skills** location, auto-loaded whenever the agent works in this repository. No symlinks or
per-machine setup: clone the repo and the skills come with it. Edit the files in place; changes
are picked up on the next session.

## Explore the extra teaching examples

[Step-by-step instructions for all three extra examples](https://mikersays.github.io/agent-arduino/examples.html)
cover IDE uploads, terminal access, and the Python app workflow without AI.

The root catalog contains 30 sketches. Two additional standalone examples live in
[the sketch skill](.claude/skills/unoq-sketch/examples/): `blink` and `matrix-heart`.
After the same IDE setup, open `blink/blink.ino` or `matrix-heart/matrix-heart.ino`
in that directory, then Verify → Upload. Blink toggles the built-in LED; the heart
example displays a heart. These use no external components. The root `flash`
picker does not list these nested examples.

The [app bridge examples](.claude/skills/unoq-app-bridge/) pair Python and a sketch.
They are a separate, advanced app workflow, with instructions in that skill;
they are not standalone uploads from the root sketch catalog.

## Keep the user instructions current

Edit `tools/sketch-guides.json` when a sketch's behavior, wiring, or controls change.
Then run:

```bash
python3 tools/build-user-docs.py
python3 tools/build-user-docs.py --check
```

This generates the 30 sketch READMEs and `docs/sketches.html`, and checks that every
root sketch has instructions. Edit `docs/getting-started.html` for shared setup.
The catalog still uses the `SKETCHES` array in `docs/index.html`; keep that array and
the README index in sync when adding a sketch. Pages serves `docs/` on `main`.
