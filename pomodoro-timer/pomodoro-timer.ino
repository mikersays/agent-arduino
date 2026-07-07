/*
 * pomodoro-timer.ino — Pomodoro desk timer on the Arduino UNO Q LED matrix
 *
 * What it does:
 *   Runs a standalone Pomodoro timer from the moment the board boots:
 *   25 minutes of work, 5 minutes of break, and after 4 completed pomodoros
 *   a 15 minute long break (then the cycle repeats). Remaining time is shown
 *   as a draining pixel field on the 13x8 matrix — all 104 pixels are lit at
 *   the start of a phase and drain away smoothly as time passes. Work phases
 *   glow at brightness 5, breaks at a dim 2, so a glance tells the phase.
 *   Completed pomodoros appear as a 1-pixel tally in the bottom-left corner,
 *   and a slow heartbeat pixel in the bottom-right corner shows the timer is
 *   alive even when the field barely moves. Phase transitions play 3
 *   full-screen flashes and print a Serial announcement.
 *
 *   Serial commands (case-insensitive, newline-terminated, 115200 baud):
 *     START        start/resume the timer
 *     PAUSE        freeze the timer (drain field holds still)
 *     RESUME       continue after PAUSE
 *     SKIP         jump to the next phase immediately
 *     RESET        back to pomodoro 0, fresh work phase, running
 *     STATUS       print phase + mm:ss remaining (also auto-printed every minute)
 *     WORK <min>   set work length in minutes   (clamped 1..120)
 *     BREAK <min>  set short-break length       (clamped 1..120)
 *     LONG <min>   set long-break length        (clamped 1..120)
 *
 * Wiring: no external hardware needed — uses only the built-in LED matrix.
 *
 * Compile:
 *   arduino-cli compile -b arduino:zephyr:unoq ./pomodoro-timer
 * Upload:
 *   arduino-cli upload -b arduino:zephyr:unoq -p <board-ip> ./pomodoro-timer
 */

#include "Arduino_LED_Matrix.h"

// ---------- Matrix geometry ----------
constexpr int kWidth  = 13;
constexpr int kHeight = 8;
constexpr int kPixels = kWidth * kHeight;   // 104

// ---------- Pomodoro defaults ----------
constexpr uint32_t kDefaultWorkMin  = 25;
constexpr uint32_t kDefaultBreakMin = 5;
constexpr uint32_t kDefaultLongMin  = 15;
constexpr int      kPomodorosPerSet = 4;    // long break after this many
constexpr uint32_t kMinMinutes      = 1;    // clamp range for WORK/BREAK/LONG
constexpr uint32_t kMaxMinutes      = 120;
constexpr uint32_t kMsPerMinute     = 60000u;

// ---------- Brightness (0..7) ----------
constexpr uint8_t kWorkBrightness      = 5;  // drain field during work
constexpr uint8_t kBreakBrightness     = 2;  // drain field during any break
constexpr uint8_t kTallyBrightness     = 7;  // completed-pomodoro pips
constexpr uint8_t kHeartbeatBrightness = 7;  // alive indicator
constexpr uint8_t kFlashBrightness     = 7;  // phase-transition flash

// ---------- Fixed pixels ----------
// Tally pips grow along the bottom-left of the bottom row; the heartbeat sits
// in the bottom-right corner. The field drains from the bottom-right upward
// (highest index first), so both corners clear early in each phase.
constexpr int kTallyRow       = kHeight - 1;             // bottom row
constexpr int kHeartbeatIndex = kPixels - 1;             // (12, 7)

// ---------- Timing ----------
constexpr uint32_t kRenderIntervalMs  = 100;    // display refresh
constexpr uint32_t kHeartbeatHalfMs   = 500;    // heartbeat toggles every 500 ms
constexpr uint32_t kFlashStepMs       = 150;    // on/off dwell during flashes
constexpr int      kFlashSteps        = 6;      // 3 flashes = 3x (on + off)
constexpr uint32_t kAutoStatusMs      = 60000;  // STATUS line every minute

// ---------- Serial line reader ----------
constexpr size_t kLineBufLen = 32;

// ---------- Phase machine ----------
enum class Phase : uint8_t { Work, ShortBreak, LongBreak };

Arduino_LED_Matrix matrix;

// Configurable durations (minutes)
static uint32_t workMin  = kDefaultWorkMin;
static uint32_t breakMin = kDefaultBreakMin;
static uint32_t longMin  = kDefaultLongMin;

// Timer state
static Phase    phase              = Phase::Work;
static uint32_t phaseTotalMs       = kDefaultWorkMin * kMsPerMinute;
static uint32_t phaseElapsedMs     = 0;
static int      completedPomodoros = 0;      // 0..kPomodorosPerSet
static bool     running            = true;   // false = paused
static uint32_t lastAccumMs        = 0;      // last time elapsed was accumulated

// Transition flash state
static bool     flashing       = false;
static int      flashStep      = 0;
static uint32_t flashStepStart = 0;

// Housekeeping timers
static uint32_t lastRenderMs = 0;
static uint32_t lastStatusMs = 0;
static bool     heartbeatOn  = false;
static uint32_t heartbeatMs  = 0;

// Serial input buffer
static char   lineBuf[kLineBufLen];
static size_t lineLen = 0;

// ---------- Phase helpers ----------
static const char* phaseName(Phase p) {
  switch (p) {
    case Phase::Work:       return "WORK";
    case Phase::ShortBreak: return "BREAK";
    default:                return "LONG BREAK";
  }
}

static uint32_t phaseDurationMs(Phase p) {
  switch (p) {
    case Phase::Work:       return workMin * kMsPerMinute;
    case Phase::ShortBreak: return breakMin * kMsPerMinute;
    default:                return longMin * kMsPerMinute;
  }
}

static uint8_t phaseBrightness(Phase p) {
  return (p == Phase::Work) ? kWorkBrightness : kBreakBrightness;
}

// ---------- Serial output ----------
static void printHelp() {
  Serial.println("Pomodoro Timer — Arduino UNO Q LED matrix");
  Serial.println("Field drains as time passes (bright=work, dim=break).");
  Serial.println("Bottom-left pips = completed pomodoros; bottom-right blink = heartbeat.");
  Serial.println("Commands: START PAUSE RESUME SKIP RESET STATUS");
  Serial.println("          WORK <min>  BREAK <min>  LONG <min>   (1..120)");
}

static void printMmSs(uint32_t ms) {
  const uint32_t totalSec = ms / 1000u;
  const uint32_t mm = totalSec / 60u;
  const uint32_t ss = totalSec % 60u;
  if (mm < 10) Serial.print('0');
  Serial.print(mm);
  Serial.print(':');
  if (ss < 10) Serial.print('0');
  Serial.print(ss);
}

static void printStatus() {
  const uint32_t remaining =
      (phaseElapsedMs < phaseTotalMs) ? phaseTotalMs - phaseElapsedMs : 0;
  Serial.print("STATUS ");
  Serial.print(phaseName(phase));
  Serial.print(' ');
  printMmSs(remaining);
  Serial.print(" remaining, pomodoros=");
  Serial.print(completedPomodoros);
  Serial.print('/');
  Serial.print(kPomodorosPerSet);
  Serial.println(running ? (flashing ? " (transition)" : " (running)")
                         : " (paused)");
}

// ---------- Phase control ----------
static void startPhase(Phase p, uint32_t now) {
  phase          = p;
  phaseTotalMs   = phaseDurationMs(p);
  phaseElapsedMs = 0;
  lastAccumMs    = now;
  lastStatusMs   = now;

  Serial.print("Phase: ");
  Serial.print(phaseName(p));
  Serial.print(" (");
  printMmSs(phaseTotalMs);
  Serial.println(")");
}

static Phase nextPhaseAfter(Phase p) {
  if (p == Phase::Work) {
    completedPomodoros++;
    if (completedPomodoros >= kPomodorosPerSet) return Phase::LongBreak;
    return Phase::ShortBreak;
  }
  if (p == Phase::LongBreak) {
    completedPomodoros = 0;  // fresh set
  }
  return Phase::Work;
}

// Announce the finished phase and kick off the transition flashes; the next
// phase actually starts when the flash sequence ends.
static Phase pendingPhase = Phase::Work;

static void beginTransition(uint32_t now, bool skipped) {
  Serial.print(skipped ? "Skipped " : "Completed ");
  Serial.println(phaseName(phase));
  pendingPhase   = nextPhaseAfter(phase);
  flashing       = true;
  flashStep      = 0;
  flashStepStart = now;
  if (phase == Phase::Work) {
    Serial.print("Pomodoros this set: ");
    Serial.print(completedPomodoros);
    Serial.print('/');
    Serial.println(kPomodorosPerSet);
  }
}

static void resetSession(uint32_t now) {
  completedPomodoros = 0;
  flashing           = false;
  running            = true;
  Serial.println("Session reset");
  startPhase(Phase::Work, now);
}

// ---------- Rendering ----------
static void renderFrame() {
  uint8_t frame[kPixels];

  // Draining field: lit pixel count is ceil(remaining/total * 104), so the
  // field is full at phase start and empties exactly at 00:00. Pixels drain
  // highest-index first (bottom-right upward).
  const uint32_t remaining =
      (phaseElapsedMs < phaseTotalMs) ? phaseTotalMs - phaseElapsedMs : 0;
  const uint32_t lit =
      (remaining * (uint32_t)kPixels + phaseTotalMs - 1) / phaseTotalMs;
  const uint8_t level = phaseBrightness(phase);
  for (int i = 0; i < kPixels; i++) {
    frame[i] = ((uint32_t)i < lit) ? level : 0;
  }

  // Tally pips: one bright pixel per completed pomodoro, bottom-left corner.
  for (int t = 0; t < completedPomodoros && t < kPomodorosPerSet; t++) {
    frame[kTallyRow * kWidth + t] = kTallyBrightness;
  }

  // Heartbeat pixel: slow blink so the display visibly isn't frozen.
  frame[kHeartbeatIndex] = heartbeatOn ? kHeartbeatBrightness : 0;

  matrix.draw(frame);
}

static void renderFlash(bool on) {
  uint8_t frame[kPixels];
  memset(frame, on ? kFlashBrightness : 0, kPixels);
  matrix.draw(frame);
}

// ---------- Command handling ----------
static uint32_t clampMinutes(long v) {
  if (v < (long)kMinMinutes) return kMinMinutes;
  if (v > (long)kMaxMinutes) return kMaxMinutes;
  return (uint32_t)v;
}

// Apply a new duration; if it changes the phase currently on screen, resize
// that phase in place (elapsed time is kept).
static void setDuration(uint32_t* target, long value, Phase affects,
                        const char* label) {
  *target = clampMinutes(value);
  if (phase == affects && !flashing) {
    phaseTotalMs = phaseDurationMs(phase);
  }
  Serial.print(label);
  Serial.print(" length set to ");
  Serial.print(*target);
  Serial.println(" min");
}

static void handleLine(char* line, uint32_t now) {
  // Uppercase in place; trim leading spaces.
  for (char* c = line; *c; c++) {
    if (*c >= 'a' && *c <= 'z') *c -= 32;
  }
  while (*line == ' ') line++;
  if (*line == '\0') return;

  // Split "CMD ARG"
  char* arg = strchr(line, ' ');
  if (arg) {
    *arg++ = '\0';
    while (*arg == ' ') arg++;
  }

  if (strcmp(line, "START") == 0 || strcmp(line, "RESUME") == 0) {
    if (running) {
      Serial.println("Already running");
    } else {
      running      = true;
      lastAccumMs  = now;  // don't count paused time
      lastStatusMs = now;  // re-arm the minute cadence; no instant auto-STATUS
      Serial.println("Running");
      printStatus();
    }
  } else if (strcmp(line, "PAUSE") == 0) {
    if (!running) {
      Serial.println("Already paused");
    } else {
      running = false;
      Serial.println("Paused");
      printStatus();
    }
  } else if (strcmp(line, "SKIP") == 0) {
    if (!flashing) beginTransition(now, true);
  } else if (strcmp(line, "RESET") == 0) {
    resetSession(now);
  } else if (strcmp(line, "STATUS") == 0) {
    printStatus();
  } else if (strcmp(line, "WORK") == 0 && arg) {
    setDuration(&workMin, atol(arg), Phase::Work, "Work");
  } else if (strcmp(line, "BREAK") == 0 && arg) {
    setDuration(&breakMin, atol(arg), Phase::ShortBreak, "Break");
  } else if (strcmp(line, "LONG") == 0 && arg) {
    setDuration(&longMin, atol(arg), Phase::LongBreak, "Long break");
  } else {
    Serial.print("Unknown command: ");
    Serial.println(line);
    printHelp();
  }
}

// Non-blocking line reader: accumulates until \r or \n, then dispatches.
static void pollSerial(uint32_t now) {
  while (Serial.available() > 0) {
    const char c = (char)Serial.read();
    if (c == '\n' || c == '\r') {
      if (lineLen > 0) {
        lineBuf[lineLen] = '\0';
        lineLen = 0;
        handleLine(lineBuf, now);
      }
    } else if (lineLen < kLineBufLen - 1) {
      lineBuf[lineLen++] = c;
    }
  }
}

// ---------- Arduino entry points ----------
void setup() {
  Serial.begin(115200);
  matrix.begin();
  matrix.setGrayscaleBits(3);

  printHelp();
  const uint32_t now = millis();
  startPhase(Phase::Work, now);
  heartbeatMs  = now;
  lastRenderMs = now;
}

void loop() {
  const uint32_t now = millis();

  pollSerial(now);

  // Heartbeat blinks unconditionally — even when paused or flashing.
  if (now - heartbeatMs >= kHeartbeatHalfMs) {
    heartbeatMs += kHeartbeatHalfMs;
    heartbeatOn = !heartbeatOn;
  }

  if (flashing) {
    // 3 full-screen flashes, then the pending phase begins.
    if (now - flashStepStart >= kFlashStepMs) {
      flashStepStart = now;
      flashStep++;
      if (flashStep >= kFlashSteps) {
        flashing = false;
        startPhase(pendingPhase, now);
      }
    }
    if (flashing) {
      renderFlash((flashStep % 2) == 0);
      return;
    }
  }

  // Accumulate elapsed time only while running — pause/resume safe and
  // robust to millis() rollover (unsigned subtraction).
  if (running) {
    phaseElapsedMs += now - lastAccumMs;
  }
  lastAccumMs = now;

  // Phase complete?
  if (running && phaseElapsedMs >= phaseTotalMs) {
    beginTransition(now, false);
    return;
  }

  // Automatic STATUS line every minute while running.
  if (running && now - lastStatusMs >= kAutoStatusMs) {
    lastStatusMs = now;
    printStatus();
  }

  if (now - lastRenderMs >= kRenderIntervalMs) {
    lastRenderMs = now;
    renderFrame();
  }
}
