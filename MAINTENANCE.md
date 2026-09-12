# Maintenance backlog — prioritized plan for the next session

Grounded in an on-device repo analysis (2026-07-07): 30 sketches / ~8.7k lines of `.ino`,
`./flash` CLI (643 lines), Pages catalog in `docs/`, 3 project skills. Work top-down; each item
has acceptance criteria. Check items off (`[x]`) and prune completed sections as you go.

## P1 — correctness & visibility gaps

- [x] **Enable GitHub Pages** — confirmed via GitHub API on 2026-09-12:
  Pages serves `main` `/docs` at https://mikersays.github.io/agent-arduino/.

- [x] **Make standalone sketches accessible without AI** — free IDE and on-board
  terminal setup in `docs/getting-started.html`; all 30 sketch guides in
  `docs/sketches.html` and individual READMEs. Regenerate with
  `python3 tools/build-user-docs.py`; check with `--check`.

- [ ] **Verify I2C bus guidance on hardware.** The scanner uses `Wire`; current
  Arduino documentation assigns Qwiic to `Wire1`. User guides document the header
  bus and the untested edit for Qwiic. Correct the original sketch header when
  the bus behavior has been verified; do not claim it scans both buses.

- [ ] **Runtime-verify the serial-input path.** The 9 application sketches' command handling
  is compile-verified and logic-reviewed, but nobody has confirmed that typed input actually
  reaches `Serial.read()` through the bridge monitor. Flash `matrix-marquee` (`./flash
  matrix-marquee -y`), then try scripted input, e.g. `printf 'TEXT HI\n' | arduino-cli monitor
  -p <ip> -b arduino:zephyr:unoq -c baudrate=115200` (also try `arduino-app-cli monitor`).
  *Accept:* marquee text visibly changes → note "verified" in `unoq-sketch` SKILL.md. If input
  does NOT work, that's a major finding: document the limitation in the skill + the 9 app
  headers, and prototype a Bridge-RPC alternative (see `unoq-app-bridge`).

- [ ] **Add a one-command compile sweep.** The ship checklist's "compile everything" step is an
  ad-hoc bash loop retyped each session. Add `--sweep` to `./flash` (reuse its discovery; also
  sweep `.claude/skills/*/examples/*`), print a PASS/FAIL table, nonzero exit on any failure.
  Update CLAUDE.md step 5 to say `./flash --sweep`.
  *Accept:* one command, 32 compiles (30 sketches + 2 skill examples), correct exit code.

## P2 — single source of truth for sketch metadata

- [ ] **Generate the catalog instead of hand-syncing it.** Sketch descriptions live in 3
  places: `.ino` headers (canonical, feeds `./flash`), `docs/index.html` `SKETCHES` array, and
  README tables. Add a `// @category: effects|sims|games|apps|periph|rtos` tag line to each
  sketch header, then a `tools/gen-catalog` script that regenerates the `SKETCHES` array
  (between `/* GEN:BEGIN */ ... /* GEN:END */` markers, keeping the page self-contained) and
  optionally the README tables from the headers. Replace the "add one entry to docs" step in
  CLAUDE.md with "run `tools/gen-catalog`".
  *Accept:* running the generator twice is a no-op; deleting a docs entry and regenerating
  restores it; CLAUDE.md updated.

- [ ] **CI compile check (experiment).** No `.github/workflows`. A GitHub Action that installs
  `arduino-cli` + the `arduino:zephyr` core and runs the sweep would compile-check pushes from
  agents working off-board. Unknowns: core availability/size in runners — timebox it, cache
  `~/.arduino15`, and add a README badge if it works.
  *Accept:* green action on a trivial push, or a documented "not feasible because X" note here.

## P3 — hardware verification & code health

- [ ] **Eyes-on hardware pass with Mike.** Only `matrix-light-show` and `matrix-marquee` have
  ever run on the matrix; `rgb-status-cycle` and `zephyr-threads-demo` assume LED3/LED4 are
  active-low (never visually confirmed). Flash a few sketches, ask Mike if colors/behavior
  match the Serial output; flip the polarity constants if wrong and record the truth in
  `unoq-sketch/references/board.md`.
  *Accept:* polarity fact verified (either way) and documented.

- [ ] **Bless canonical snippets instead of deduplicating.** 15 sketches carry the same LCG,
  11 a `setPixel`, 9 a serial line reader. This duplication is *deliberate* (self-contained
  teaching sketches — do NOT extract a shared library retroactively). Instead, add
  `references/snippets.md` to `unoq-sketch` with the one blessed version of each helper so
  future sketches copy consistent code.
  *Accept:* skill reference exists; CLAUDE.md house-style section points to it.

- [ ] **Runtime-verify the app-bridge path.** The `led-toggle` example app has never been
  started (`arduino-app-cli app start`). Verify the full Python↔MCU loop once and correct the
  skill where reality disagrees. Note: starting an app flashes its sketch, replacing whatever
  is running — reflash the marquee (or whatever Mike had) after.
  *Accept:* app runs, LED toggles (per logs), skill corrections committed.

## P4 — nice-to-haves (only if P1–P3 are done)

- [ ] Per-card mini-animations on the Pages site (tiny JS sims of fire/life/snake — the hero
  plasma pattern shows the approach; keep the page self-contained).
- [ ] Tag a `v1.0` release (30 sketches, CLI, skills) for a stable reference point.
- [ ] Ask Mike about the git author identity (commits are authored as "Codex").
- [ ] Cross-reference `matrix-text-scroller` (minimal text demo) and `matrix-marquee`
  (interactive successor) in each other's headers — keep both.

## Standing context for the next agent

- Read `CLAUDE.md` first; skills auto-load from `.claude/skills/`.
- Board IP is DHCP (`ip -brief addr show wlan0`); flash locally via `./flash` — network
  upload prompts for a password.
- As of 2026-07-07, `matrix-marquee` is running on the MCU.
- When you finish or invalidate an item here, update this file in the same commit.
