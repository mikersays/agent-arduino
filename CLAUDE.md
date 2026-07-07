# Agent guide — Arduino UNO Q repo

You are running ON the board (its Debian/Linux side). Sketches you write run on the OTHER half
(STM32U585 MCU). Everything in this repo was built and verified on this exact device.

## Read the skill before writing code

Three project skills auto-load from `.claude/skills/` — they contain the verified APIs,
commands, and gotchas. **Do not write UNO Q code from generic Arduino knowledge; it will be
wrong** (this board's matrix is 13×8 not 12×8, `LED_BUILTIN` is active-low, uploads don't use
COM ports, and the Zephyr/llext toolchain has linking quirks).

| Task | Skill |
|---|---|
| Any `.ino` sketch: matrix, pins, I²C/SPI/CAN/RTC | `unoq-sketch` |
| Python (Linux) ↔ sketch (MCU) apps | `unoq-app-bridge` |
| Linux-side LEDs, temperature, system | `unoq-linux-hardware` |

## Non-negotiable rules

1. **Compile everything you write, on this board, before calling it done:**
   `arduino-cli compile -b arduino:zephyr:unoq ./<sketch-folder>` (folder name must match the
   `.ino` name). Iterate until clean. Never deliver an unverified sketch.
2. **Matrix buffers are exactly 104 bytes** (8 rows × 13 cols, row-major `y*13+x`), brightness
   0–7 after `matrix.setGrayscaleBits(3)`.
3. **`LED_BUILTIN` is ACTIVE-LOW** (`LOW` = on).
4. **The board IP is DHCP** — never reuse a remembered address; `ip -brief addr show wlan0`.
5. **Flash locally, not over the network**: `arduino-cli upload -p <ip>` prompts for an SSH
   password and fails non-interactively. Use the passwordless `remoteocd` flow in
   `unoq-sketch` SKILL.md (workflow step 2). Flashing replaces whatever sketch is running.
6. **Parallel compiles need separate `--build-path`s** or they clash on the shared cache.
7. llext linking quirks (missing `strtok_r`, `__errno` stub for libm, auto-prototype vs
   enum/struct ordering, `LINE_MAX` collision): see "Zephyr/llext compile gotchas" in
   `unoq-sketch` SKILL.md.

## House style for sketches (see any existing sketch — they all follow it)

- Header comment: what it does, hardware needed (or "no external hardware needed"), exact
  compile + upload commands.
- Non-blocking `loop()` paced by the rollover-safe pattern `now - last >= interval` (unsigned
  subtraction). No long `delay()`s except where a library forces it (document that).
- Deterministic LCG for randomness; constants over magic numbers; bounds-guarded `setPixel`
  helpers; brightness clamped ≤ 7.
- Serial-interactive sketches: `Serial.begin(115200)`, help banner in `setup()`, non-blocking
  line reader (handles `\r`, `\n`, overflow-safe, case-insensitive commands), and sensible
  default behavior with no input at all.

## How this repo was built (repeat this process)

1. Write the sketch (crib from the closest existing one — 30 to choose from, indexed in
   README.md).
2. Compile-fix loop until clean; note flash/RAM from the output.
3. **Adversarially review your own work** before delivering: trace the core update/state
   machine by hand; hunt uint8_t underflow, signed/unsigned traps, off-by-one at x=12/y=7,
   millis()/micros() rollover, out-of-bounds frame writes, pause/resume double-counting,
   parser overflow. Real bugs were found this way in ~1 of 3 sketches, always by tracing.
4. Add the sketch to the README table in the right section.
5. Verify the full suite still compiles before shipping (loop `arduino-cli compile` over all
   sketch folders).

## Repo layout

- `<sketch-name>/<sketch-name>.ino` — one folder per sketch, all at repo root, all
  compile-verified. README.md has the full index (matrix demos, games, applications, Zephyr).
- `.claude/skills/` — the three project skills (this is their home; no symlinks elsewhere).
- New durable board knowledge goes INTO the skills, not into ad-hoc notes.
