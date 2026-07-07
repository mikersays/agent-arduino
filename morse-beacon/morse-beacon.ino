/*
  morse-beacon.ino — Morse code transmitter + trainer for the Arduino UNO Q.

  What it does:
    Converts text to International Morse code (A-Z, 0-9, common punctuation;
    lookup tables live in flash const arrays) and "transmits" it visually:
    the full 13x8 LED matrix lights at brightness 7 during marks (dit = 1
    unit, dah = 3 units) with LED_BUILTIN blinking in sync, and standard
    gaps are honored (1 unit between elements, 3 between letters, 7 between
    words). As a trainer aid, the current letter's dit/dah pattern is shown
    as dots on the TOP ROW while that letter is being sent (dim dot = dit,
    bright dot = dah), and each letter is printed to Serial as it starts,
    e.g. "E ." or "Q --.-". The timing engine is a non-blocking millis()
    state machine — no delay() anywhere.

  Serial commands (115200 baud, newline-terminated, case-insensitive):
    SEND <text>    queue a message (up to 200 chars) and transmit it
    WPM <5-30>     set speed; one unit = 1200 / WPM ms (PARIS standard)
    REPEAT ON|OFF  loop the last message forever as a beacon
    STOP           stop transmitting (last message is kept for REPEAT/SEND)
    HELP           print this command list

  Default behavior from boot: beacons "UNO Q" at 12 WPM on repeat.

  Wiring: no external hardware needed — uses only the built-in LED matrix,
  LED_BUILTIN (active-low on this board) and the serial monitor.

  Compile:
    arduino-cli compile -b arduino:zephyr:unoq ./morse-beacon
  Upload:
    arduino-cli upload -b arduino:zephyr:unoq -p <board-ip> ./morse-beacon
*/

#include "Arduino_LED_Matrix.h"
#include <ctype.h>
#include <string.h>

// ---------- Matrix geometry ----------
constexpr int kWidth  = 13;               // columns
constexpr int kHeight = 8;                // rows; row 0 = physical top
constexpr int kPixels = kWidth * kHeight; // 104

// ---------- Display levels ----------
constexpr uint8_t kBrightnessBits = 3;    // 0..7 grayscale
constexpr uint8_t kMarkLevel      = 7;    // whole matrix during a mark
constexpr uint8_t kDitDotLevel    = 3;    // top-row trainer dot for a dit
constexpr uint8_t kDahDotLevel    = 7;    // top-row trainer dot for a dah
constexpr int     kDotColStride   = 2;    // top-row spacing between elements

// ---------- Morse timing (all in "units"; 1 unit = 1200 / WPM ms) ----------
constexpr uint8_t  kDitUnits     = 1;
constexpr uint8_t  kDahUnits     = 3;
constexpr uint8_t  kElemGapUnits = 1;     // between elements of one letter
constexpr uint8_t  kCharGapUnits = 3;     // between letters
constexpr uint8_t  kWordGapUnits = 7;     // between words (total, replaces 3)
constexpr uint16_t kUnitNumerMs  = 1200;  // unitMs = 1200 / WPM (PARIS)

constexpr uint8_t kDefaultWpm = 12;
constexpr uint8_t kMinWpm     = 5;
constexpr uint8_t kMaxWpm     = 30;

// ---------- Buffers ----------
constexpr int kMaxMessage = 200;          // queued message length limit
constexpr int kMaxLine    = 240;          // serial input line limit

static const char kDefaultMessage[] = "UNO Q";

// ---------- Morse tables (flash const; validated against ITU-R M.1677-1) ----------
static const char* const kMorseAlpha[26] = {
  ".-",    // A
  "-...",  // B
  "-.-.",  // C
  "-..",   // D
  ".",     // E
  "..-.",  // F
  "--.",   // G
  "....",  // H
  "..",    // I
  ".---",  // J
  "-.-",   // K
  ".-..",  // L
  "--",    // M
  "-.",    // N
  "---",   // O
  ".--.",  // P
  "--.-",  // Q
  ".-.",   // R
  "...",   // S
  "-",     // T
  "..-",   // U
  "...-",  // V
  ".--",   // W
  "-..-",  // X
  "-.--",  // Y
  "--.."   // Z
};

static const char* const kMorseDigit[10] = {
  "-----", // 0
  ".----", // 1
  "..---", // 2
  "...--", // 3
  "....-", // 4
  ".....", // 5
  "-....", // 6
  "--...", // 7
  "---..", // 8
  "----."  // 9
};

static const char kPunctChars[] = {
  '.', ',', '?', '\'', '!', '/', '(', ')', '&',
  ':', ';', '=', '+', '-', '_', '"', '@'
};

static const char* const kPunctPatterns[] = {
  ".-.-.-", // .
  "--..--", // ,
  "..--..", // ?
  ".----.", // '
  "-.-.--", // !
  "-..-.",  // /
  "-.--.",  // (
  "-.--.-", // )
  ".-...",  // &
  "---...", // :
  "-.-.-.", // ;
  "-...-",  // =
  ".-.-.",  // +
  "-....-", // -
  "..--.-", // _
  ".-..-.", // "
  ".--.-."  // @
};

constexpr int kNumPunct = sizeof(kPunctChars) / sizeof(kPunctChars[0]);

// Look up the Morse pattern for a character, or nullptr if unsendable.
static const char* morseLookup(char c) {
  const char u = (char)toupper((unsigned char)c);
  if (u >= 'A' && u <= 'Z') return kMorseAlpha[u - 'A'];
  if (u >= '0' && u <= '9') return kMorseDigit[u - '0'];
  for (int i = 0; i < kNumPunct; i++) {
    if (kPunctChars[i] == u) return kPunctPatterns[i];
  }
  return nullptr;
}

// ---------- Transmitter state machine ----------
enum TxPhase : uint8_t {
  TX_IDLE,      // nothing to send
  TX_MARK,      // matrix + LED on for a dit or dah
  TX_ELEM_GAP,  // 1 unit off, inside a letter
  TX_CHAR_GAP,  // 3 units off, between letters
  TX_WORD_GAP   // 7 units off, between words / repeat wraps
};

// Explicit prototype: keeps the IDE's auto-generated one (which would land
// above the enum definition) from breaking the build.
static void enterPhase(TxPhase p, uint32_t durUnits, uint32_t now);

Arduino_LED_Matrix matrix;

static uint8_t frame[kPixels];            // draw buffer handed to matrix.draw()

static char     message[kMaxMessage + 1]; // last queued message
static int      messageLen = 0;
static int      msgPos     = 0;           // index of the char being sent
static const char* pattern = nullptr;     // dit/dah string of current letter
static int      elemIdx    = 0;           // index into pattern

static TxPhase  phase        = TX_IDLE;
static uint32_t phaseStartMs = 0;
static uint32_t phaseDurMs   = 0;

static bool     txActive   = false;
static bool     repeatMode = false;
static uint8_t  wpm        = kDefaultWpm;
static uint32_t unitMs     = kUnitNumerMs / kDefaultWpm;

// ---------- LED helpers ----------
static inline void builtinLed(bool on) {
  digitalWrite(LED_BUILTIN, on ? LOW : HIGH);  // active-low
}

static void drawAllOff() {
  memset(frame, 0, sizeof(frame));
  matrix.draw(frame);
}

static void drawAllOn() {
  memset(frame, kMarkLevel, sizeof(frame));
  matrix.draw(frame);
}

// Show the current letter's pattern as dots on the top row:
// dim dot = dit, bright dot = dah, one skipped column between elements.
static void drawPatternRow() {
  memset(frame, 0, sizeof(frame));
  if (pattern != nullptr) {
    for (int i = 0; pattern[i] != '\0'; i++) {
      const int col = i * kDotColStride;
      if (col >= kWidth) break;  // patterns here are <= 6 elements; safety only
      frame[col] = (pattern[i] == '-') ? kDahDotLevel : kDitDotLevel;
    }
  }
  matrix.draw(frame);
}

// ---------- Phase transitions ----------
static void enterPhase(TxPhase p, uint32_t durUnits, uint32_t now) {
  phase        = p;
  phaseStartMs = now;
  phaseDurMs   = durUnits * unitMs;

  switch (p) {
    case TX_MARK:
      builtinLed(true);
      drawAllOn();
      break;
    case TX_ELEM_GAP:
      builtinLed(false);
      drawPatternRow();     // keep trainer dots up between elements
      break;
    case TX_CHAR_GAP:
    case TX_WORD_GAP:
      builtinLed(false);
      drawAllOff();
      break;
    case TX_IDLE:
    default:
      builtinLed(false);
      drawAllOff();
      break;
  }
}

static void stopTransmit(uint32_t now) {
  txActive = false;
  pattern  = nullptr;
  enterPhase(TX_IDLE, 0, now);
}

// Begin sending message[msgPos]. Skips spaces (as word gaps) and any
// unsendable characters. Announces each letter on Serial as it starts.
static void startNextChar(uint32_t now) {
  while (msgPos < messageLen) {
    const char c = message[msgPos];

    if (c == ' ') {
      // Consume the whole run of spaces as a single 7-unit word gap.
      while (msgPos < messageLen && message[msgPos] == ' ') msgPos++;
      pattern = nullptr;
      enterPhase(TX_WORD_GAP, kWordGapUnits, now);
      return;
    }

    pattern = morseLookup(c);
    if (pattern == nullptr) {   // unknown char: skip it silently
      msgPos++;
      continue;
    }

    Serial.print((char)toupper((unsigned char)c));
    Serial.print(' ');
    Serial.println(pattern);

    elemIdx = 0;
    enterPhase(TX_MARK, (pattern[0] == '-') ? kDahUnits : kDitUnits, now);
    return;
  }

  // End of message.
  if (repeatMode) {
    msgPos  = 0;
    pattern = nullptr;
    enterPhase(TX_WORD_GAP, kWordGapUnits, now);  // beacon pause, then wrap
  } else {
    Serial.println("Done.");
    stopTransmit(now);
  }
}

static void startTransmit(uint32_t now) {
  if (messageLen == 0) {
    Serial.println("Nothing to send.");
    stopTransmit(now);
    return;
  }
  msgPos   = 0;
  txActive = true;
  Serial.print("Sending at ");
  Serial.print(wpm);
  Serial.print(" WPM: ");
  Serial.println(message);
  startNextChar(now);
}

// Advance the state machine when the current phase's time is up.
static void serviceTransmitter(uint32_t now) {
  if (!txActive || phase == TX_IDLE) return;
  if ((uint32_t)(now - phaseStartMs) < phaseDurMs) return;  // rollover-safe

  switch (phase) {
    case TX_MARK:
      elemIdx++;
      if (pattern != nullptr && pattern[elemIdx] != '\0') {
        enterPhase(TX_ELEM_GAP, kElemGapUnits, now);
      } else {
        // Letter finished. Peek past unsendable chars: a 3-unit letter gap
        // only if another sendable letter follows in the same word —
        // otherwise it would stack onto the 7-unit word gap.
        msgPos++;
        int look = msgPos;
        while (look < messageLen && message[look] != ' ' &&
               morseLookup(message[look]) == nullptr) look++;
        if (look < messageLen && message[look] != ' ') {
          pattern = nullptr;
          enterPhase(TX_CHAR_GAP, kCharGapUnits, now);
        } else {
          pattern = nullptr;
          startNextChar(now);   // handles spaces, end-of-message, repeat
        }
      }
      break;

    case TX_ELEM_GAP:
      enterPhase(TX_MARK, (pattern[elemIdx] == '-') ? kDahUnits : kDitUnits, now);
      break;

    case TX_CHAR_GAP:
    case TX_WORD_GAP:
      startNextChar(now);
      break;

    case TX_IDLE:
    default:
      break;
  }
}

// ---------- Serial command handling ----------
static void printHelp() {
  Serial.println();
  Serial.println("=== Morse Beacon — UNO Q ===");
  Serial.println("Commands (case-insensitive):");
  Serial.println("  SEND <text>    transmit text (max 200 chars, A-Z 0-9 punctuation)");
  Serial.println("  WPM <5-30>     set speed (unit = 1200/WPM ms)");
  Serial.println("  REPEAT ON|OFF  loop the last message as a beacon");
  Serial.println("  STOP           stop transmitting");
  Serial.println("  HELP           show this help");
  Serial.println("Matrix = full-on during marks; top row previews the letter");
  Serial.println("(dim dot = dit, bright dot = dah). LED_BUILTIN blinks in sync.");
  Serial.println();
}

static void queueMessage(const char* text) {
  int n = 0;
  while (text[n] != '\0' && n < kMaxMessage) {
    message[n] = text[n];
    n++;
  }
  message[n] = '\0';
  messageLen = n;
  if (text[n] != '\0') {
    Serial.println("Note: message truncated to 200 chars.");
  }
}

static bool tokenEquals(const char* tok, const char* upperWord) {
  while (*upperWord != '\0') {
    if (toupper((unsigned char)*tok) != *upperWord) return false;
    tok++;
    upperWord++;
  }
  return *tok == '\0';
}

static void handleLine(char* line, uint32_t now) {
  // Trim leading spaces.
  while (*line == ' ') line++;
  // Trim trailing whitespace/CR.
  int len = (int)strlen(line);
  while (len > 0 && (line[len - 1] == ' ' || line[len - 1] == '\r')) {
    line[--len] = '\0';
  }
  if (len == 0) return;

  // Split off the first token.
  char* args = line;
  while (*args != '\0' && *args != ' ') args++;
  if (*args == ' ') {
    *args = '\0';
    args++;
    while (*args == ' ') args++;
  }

  if (tokenEquals(line, "HELP")) {
    printHelp();
  } else if (tokenEquals(line, "SEND")) {
    if (*args == '\0') {
      Serial.println("Usage: SEND <text>");
      return;
    }
    queueMessage(args);
    startTransmit(now);
  } else if (tokenEquals(line, "WPM")) {
    const long v = atol(args);
    if (v < kMinWpm || v > kMaxWpm) {
      Serial.println("WPM must be 5-30.");
      return;
    }
    wpm    = (uint8_t)v;
    unitMs = kUnitNumerMs / wpm;
    Serial.print("Speed: ");
    Serial.print(wpm);
    Serial.print(" WPM (unit ");
    Serial.print(unitMs);
    Serial.println(" ms).");
  } else if (tokenEquals(line, "REPEAT")) {
    if (tokenEquals(args, "ON")) {
      repeatMode = true;
      Serial.println("Repeat: ON (beacon mode).");
      if (!txActive && messageLen > 0) startTransmit(now);
    } else if (tokenEquals(args, "OFF")) {
      repeatMode = false;
      Serial.println("Repeat: OFF.");
    } else {
      Serial.println("Usage: REPEAT ON|OFF");
    }
  } else if (tokenEquals(line, "STOP")) {
    stopTransmit(now);
    Serial.println("Stopped.");
  } else {
    Serial.print("Unknown command: ");
    Serial.println(line);
    Serial.println("Type HELP for the command list.");
  }
}

// Non-blocking serial line reader.
static void pollSerial(uint32_t now) {
  static char lineBuf[kMaxLine + 1];
  static int  lineLen = 0;

  while (Serial.available() > 0) {
    const char c = (char)Serial.read();
    if (c == '\n' || c == '\r') {
      if (lineLen > 0) {
        lineBuf[lineLen] = '\0';
        lineLen = 0;
        handleLine(lineBuf, now);
      }
    } else if (lineLen < kMaxLine) {
      lineBuf[lineLen++] = c;
    }
    // Overlong lines: extra chars are dropped until the newline.
  }
}

// ---------- Arduino entry points ----------
void setup() {
  pinMode(LED_BUILTIN, OUTPUT);
  builtinLed(false);

  Serial.begin(115200);

  matrix.begin();
  matrix.setGrayscaleBits(kBrightnessBits);
  drawAllOff();

  printHelp();

  // Boot default: beacon "UNO Q" at 12 WPM on repeat.
  queueMessage(kDefaultMessage);
  repeatMode = true;
  startTransmit(millis());
}

void loop() {
  const uint32_t now = millis();
  pollSerial(now);
  serviceTransmitter(now);
}
