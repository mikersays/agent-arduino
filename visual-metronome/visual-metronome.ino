/*
 * visual-metronome.ino — Silent visual metronome on the Arduino UNO Q LED
 * matrix (13x8) for practicing musicians.
 *
 * What it does:
 *   A pendulum dot sweeps left-right across the middle rows in perfect time.
 *   Its position is a triangle wave of the beat phase (sub-pixel anti-aliased,
 *   rendered at ~60 fps) so it hits an edge exactly on every beat. On each
 *   beat the edge it lands on flashes a bright 3-pixel tick column and the
 *   built-in LED pulses. On beat 1 of the bar the full edge column flashes
 *   brighter and the entire top row lights for 2 frames, so the downbeat is
 *   unmistakable. The bottom row shows one counter dot per beat of the bar,
 *   with the current beat brightest. Beat timing is derived from an anchored
 *   start time (never from accumulated frame deltas), so it cannot drift.
 *
 * Serial commands (115200 baud, case-insensitive, newline-terminated):
 *   BPM <40-240>   set tempo (re-anchors on the change instant)
 *   TS <2-7>       set beats per bar (time signature numerator)
 *   TAP            call twice or more in tempo; averages the last 3 intervals
 *   START / STOP   run / freeze the metronome
 *   STATUS         print current settings
 *   Defaults: 100 BPM, 4/4, running.
 *
 * Wiring: no external hardware needed — uses only the built-in 13x8 LED
 * matrix and the built-in LED.
 *
 * Compile:
 *   arduino-cli compile -b arduino:zephyr:unoq ./visual-metronome
 * Upload:
 *   arduino-cli upload -b arduino:zephyr:unoq -p <board-ip> ./visual-metronome
 */

#include "Arduino_LED_Matrix.h"

// ---------------------------------------------------------------- matrix ---
static const int     MAT_W      = 13;
static const int     MAT_H      = 8;
static const uint8_t MAX_BRIGHT = 7;      // grayscale range 0..7 (3 bits)

Arduino_LED_Matrix matrix;
static uint8_t frame[MAT_W * MAT_H];      // composed frame, row-major

// -------------------------------------------------------------- tunables ---
static const uint32_t FRAME_MS        = 16;   // ~60 fps render tick
static const uint32_t TICK_FLASH_MS   = 100;  // edge tick column duration
static const uint32_t LED_PULSE_MS    = 60;   // built-in LED pulse per beat
static const int      ACCENT_FRAMES   = 2;    // top-row accent frame count

static const int      BPM_MIN         = 40;
static const int      BPM_MAX         = 240;
static const int      TS_MIN          = 2;
static const int      TS_MAX          = 7;
static const int      BPM_DEFAULT     = 100;
static const int      TS_DEFAULT      = 4;

static const uint32_t TAP_TIMEOUT_MS  = 2500; // gap that resets tap history
static const int      TAP_MAX_INTERVALS = 3;  // average up to last 3 gaps

static const uint32_t ANCHOR_SLIDE_MS = 3600000UL; // re-base anchor hourly

// Pendulum rides on the two middle rows.
static const int PEND_ROW_TOP = 3;
static const int PEND_ROW_BOT = 4;
// Beat tick: 3-pixel column centered on the pendulum rows.
static const int TICK_ROW_FIRST = 2;
static const int TICK_ROW_LAST  = 4;

static const uint8_t BRIGHT_PEND      = 7;  // pendulum dot (split sub-pixel)
static const uint8_t BRIGHT_TICK      = 6;  // ordinary beat edge tick
static const uint8_t BRIGHT_DOWNBEAT  = 7;  // downbeat full edge column
static const uint8_t BRIGHT_ACCENT    = 7;  // downbeat top-row accent
static const uint8_t BRIGHT_CNT_CUR   = 7;  // current beat counter dot
static const uint8_t BRIGHT_CNT_OTHER = 1;  // other beat counter dots
static const uint8_t BRIGHT_STOPPED   = 2;  // idle dot when stopped

// ----------------------------------------------------------------- state ---
static int      bpm        = BPM_DEFAULT;
static int      beatsPerBar = TS_DEFAULT;
static bool     running    = true;

// Drift-free anchored clock: absolute beat index = anchorBeat + elapsed/beatMs.
static uint32_t beatMs     = 60000UL / BPM_DEFAULT;
static uint32_t anchorMs   = 0;     // millis() at the anchor instant
static uint32_t anchorBeat = 0;     // absolute beat index at the anchor

static uint32_t lastBeatIndex   = 0xFFFFFFFFu;  // forces beat 0 to fire
static int      beatInBar       = 0;
static uint32_t tickFlashUntil  = 0;   // millis deadline of edge tick
static int      tickFlashCol    = 0;   // which edge column is flashing
static bool     tickIsDownbeat  = false;
static int      accentFramesLeft = 0;  // top-row accent countdown (frames)
static uint32_t ledOffAt        = 0;   // built-in LED pulse deadline
static bool     ledOn           = false;

static uint32_t lastFrameMs = 0;

// Tap tempo.
static uint32_t tapTimes[TAP_MAX_INTERVALS + 1];
static int      tapCount = 0;

// Serial line reader.
static const int CMD_LINE_MAX = 32;
static char lineBuf[CMD_LINE_MAX];
static int  lineLen = 0;

// --------------------------------------------------------------- helpers ---
static inline void setPixel(int x, int y, uint8_t b) {
  if (x < 0 || x >= MAT_W || y < 0 || y >= MAT_H) return;
  uint8_t &p = frame[y * MAT_W + x];
  if (b > p) p = b;                 // additive-max compositing
}

// Re-anchor the beat clock at 'now', preserving the position within the bar.
static void reAnchor(uint32_t now) {
  uint32_t idx = (lastBeatIndex == 0xFFFFFFFFu) ? 0 : lastBeatIndex + 1;
  anchorMs   = now;
  anchorBeat = idx;                 // a fresh beat starts right now
  lastBeatIndex = idx - 1;          // so the beat at 'idx' fires immediately
}

static void applyBpm(int newBpm, uint32_t now) {
  bpm    = newBpm;
  beatMs = 60000UL / (uint32_t)bpm;
  reAnchor(now);
}

static void printStatus() {
  Serial.print("STATUS: ");
  Serial.print(bpm);
  Serial.print(" BPM, ");
  Serial.print(beatsPerBar);
  Serial.print("/4, ");
  Serial.print(running ? "RUNNING" : "STOPPED");
  Serial.print(", beat ");
  Serial.print(beatInBar + 1);
  Serial.print("/");
  Serial.println(beatsPerBar);
}

static void printHelp() {
  Serial.println();
  Serial.println("=== visual-metronome ===");
  Serial.println("Silent visual metronome on the 13x8 LED matrix.");
  Serial.println("Commands (newline-terminated, case-insensitive):");
  Serial.println("  BPM <40-240>  set tempo");
  Serial.println("  TS <2-7>      beats per bar");
  Serial.println("  TAP           tap 2+ times in tempo to set BPM");
  Serial.println("  START / STOP  run or freeze");
  Serial.println("  STATUS        show settings");
  printStatus();
}

// ------------------------------------------------------------- tap tempo ---
static void handleTap(uint32_t now) {
  // Rollover-safe staleness check.
  if (tapCount > 0 && (uint32_t)(now - tapTimes[tapCount - 1]) > TAP_TIMEOUT_MS) {
    tapCount = 0;
  }
  if (tapCount == TAP_MAX_INTERVALS + 1) {      // slide the window
    for (int i = 0; i < TAP_MAX_INTERVALS; i++) tapTimes[i] = tapTimes[i + 1];
    tapCount = TAP_MAX_INTERVALS;
  }
  tapTimes[tapCount++] = now;

  if (tapCount < 2) {
    Serial.println("TAP: first tap, keep tapping...");
    return;
  }
  uint32_t sum = 0;
  int intervals = tapCount - 1;                 // up to TAP_MAX_INTERVALS
  for (int i = 0; i < intervals; i++) {
    sum += (uint32_t)(tapTimes[tapCount - 1 - i] - tapTimes[tapCount - 2 - i]);
  }
  uint32_t avg = sum / (uint32_t)intervals;
  if (avg == 0) avg = 1;
  int newBpm = (int)(60000UL / avg);
  if (newBpm < BPM_MIN) newBpm = BPM_MIN;
  if (newBpm > BPM_MAX) newBpm = BPM_MAX;
  applyBpm(newBpm, now);
  Serial.print("TAP: ");
  Serial.print(intervals);
  Serial.print(" interval(s) -> ");
  Serial.print(newBpm);
  Serial.println(" BPM");
}

// ---------------------------------------------------------------- serial ---
static void handleLine(char *s, uint32_t now) {
  // Trim leading spaces, uppercase in place.
  while (*s == ' ') s++;
  for (char *p = s; *p; p++) *p = toupper((unsigned char)*p);
  if (*s == '\0') return;

  if (strncmp(s, "BPM", 3) == 0) {
    int v = atoi(s + 3);
    if (v >= BPM_MIN && v <= BPM_MAX) {
      applyBpm(v, now);
      Serial.print("OK: ");
      Serial.print(bpm);
      Serial.println(" BPM");
    } else {
      Serial.println("ERR: BPM range is 40-240");
    }
  } else if (strncmp(s, "TS", 2) == 0) {
    int v = atoi(s + 2);
    if (v >= TS_MIN && v <= TS_MAX) {
      beatsPerBar = v;
      if (beatInBar >= beatsPerBar) beatInBar = 0;
      Serial.print("OK: ");
      Serial.print(beatsPerBar);
      Serial.println(" beats per bar");
    } else {
      Serial.println("ERR: TS range is 2-7");
    }
  } else if (strcmp(s, "TAP") == 0) {
    handleTap(now);
  } else if (strcmp(s, "START") == 0) {
    if (!running) {
      running = true;
      reAnchor(now);
    }
    Serial.println("OK: running");
  } else if (strcmp(s, "STOP") == 0) {
    running = false;
    Serial.println("OK: stopped");
  } else if (strcmp(s, "STATUS") == 0) {
    printStatus();
  } else {
    Serial.print("ERR: unknown command '");
    Serial.print(s);
    Serial.println("' (BPM/TS/TAP/START/STOP/STATUS)");
  }
}

static void pollSerial(uint32_t now) {
  while (Serial.available() > 0) {
    char c = (char)Serial.read();
    if (c == '\n' || c == '\r') {
      if (lineLen > 0) {
        lineBuf[lineLen] = '\0';
        handleLine(lineBuf, now);
        lineLen = 0;
      }
    } else if (lineLen < CMD_LINE_MAX - 1) {
      lineBuf[lineLen++] = c;
    }
  }
}

// --------------------------------------------------------------- drawing ---
static void drawCounterDots() {
  // One dot per beat of the bar along the bottom row, spaced 2 columns.
  for (int i = 0; i < beatsPerBar; i++) {
    uint8_t b = (running && i == beatInBar) ? BRIGHT_CNT_CUR : BRIGHT_CNT_OTHER;
    setPixel(i * 2, MAT_H - 1, b);
  }
}

static void drawPendulum(float x) {
  // Sub-pixel anti-aliased dot: brightness split across the two columns
  // straddling x, on both middle rows.
  int   xi   = (int)x;
  float fracX = x - (float)xi;
  uint8_t b0 = (uint8_t)((float)BRIGHT_PEND * (1.0f - fracX) + 0.5f);
  uint8_t b1 = (uint8_t)((float)BRIGHT_PEND * fracX + 0.5f);
  setPixel(xi,     PEND_ROW_TOP, b0);
  setPixel(xi,     PEND_ROW_BOT, b0);
  setPixel(xi + 1, PEND_ROW_TOP, b1);
  setPixel(xi + 1, PEND_ROW_BOT, b1);
}

static void render(uint32_t now) {
  memset(frame, 0, sizeof(frame));

  if (running) {
    // Triangle wave of the beat phase: even beats sweep left->right, odd
    // beats right->left, so the dot is at an edge exactly on each beat.
    uint32_t elapsed  = (uint32_t)(now - anchorMs);       // rollover-safe
    uint32_t beatIdx  = anchorBeat + elapsed / beatMs;
    float    phase    = (float)(elapsed % beatMs) / (float)beatMs;
    float    x        = ((beatIdx & 1u) == 0u) ? phase * (float)(MAT_W - 1)
                                               : (1.0f - phase) * (float)(MAT_W - 1);
    drawPendulum(x);

    // Beat tick / downbeat edge flash.
    if ((uint32_t)(tickFlashUntil - now) <= TICK_FLASH_MS) {  // still active
      if (tickIsDownbeat) {
        for (int y = 0; y < MAT_H; y++) setPixel(tickFlashCol, y, BRIGHT_DOWNBEAT);
      } else {
        for (int y = TICK_ROW_FIRST; y <= TICK_ROW_LAST; y++) {
          setPixel(tickFlashCol, y, BRIGHT_TICK);
        }
      }
    }

    // 2-frame full-top-row downbeat accent.
    if (accentFramesLeft > 0) {
      for (int x2 = 0; x2 < MAT_W; x2++) setPixel(x2, 0, BRIGHT_ACCENT);
      accentFramesLeft--;
    }
  } else {
    // Stopped: dim idle dot parked at the left edge.
    setPixel(0, PEND_ROW_TOP, BRIGHT_STOPPED);
    setPixel(0, PEND_ROW_BOT, BRIGHT_STOPPED);
  }

  drawCounterDots();
  matrix.draw(frame);
}

// ------------------------------------------------------------------ beat ---
static void fireBeat(uint32_t beatIdx, uint32_t now) {
  beatInBar = (int)(beatIdx % (uint32_t)beatsPerBar);
  tickFlashCol   = ((beatIdx & 1u) == 0u) ? 0 : MAT_W - 1;
  tickFlashUntil = now + TICK_FLASH_MS;
  tickIsDownbeat = (beatInBar == 0);
  if (tickIsDownbeat) accentFramesLeft = ACCENT_FRAMES;

  digitalWrite(LED_BUILTIN, LOW);   // active-low: ON
  ledOn    = true;
  ledOffAt = now + LED_PULSE_MS;
}

// ------------------------------------------------------------- setup/loop --
void setup() {
  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, HIGH);  // active-low: OFF

  matrix.begin();
  matrix.setGrayscaleBits(3);

  Serial.begin(115200);
  printHelp();

  uint32_t now = millis();
  anchorMs    = now;
  anchorBeat  = 0;
  lastBeatIndex = 0xFFFFFFFFu;      // beat 0 fires on the first loop pass
  lastFrameMs = now;
}

void loop() {
  uint32_t now = millis();

  pollSerial(now);

  if (running) {
    // Drift-free beat detection from the anchored clock.
    uint32_t elapsed = (uint32_t)(now - anchorMs);
    // Slide the anchor forward by whole beats once an hour so 'elapsed' can
    // never approach the 32-bit wrap on multi-week runs. Whole-beat integer
    // steps leave beatIdx and phase unchanged, so this cannot cause drift.
    if (elapsed >= ANCHOR_SLIDE_MS) {
      uint32_t wholeBeats = elapsed / beatMs;
      anchorMs   += wholeBeats * beatMs;
      anchorBeat += wholeBeats;
      elapsed = (uint32_t)(now - anchorMs);
    }
    uint32_t beatIdx = anchorBeat + elapsed / beatMs;
    if (beatIdx != lastBeatIndex) {
      lastBeatIndex = beatIdx;
      fireBeat(beatIdx, now);
    }
  }

  // End the built-in LED pulse (rollover-safe).
  if (ledOn && (int32_t)(now - ledOffAt) >= 0) {
    digitalWrite(LED_BUILTIN, HIGH);
    ledOn = false;
  }

  // ~60 fps render tick.
  if ((uint32_t)(now - lastFrameMs) >= FRAME_MS) {
    lastFrameMs = now;
    render(now);
  }
}
