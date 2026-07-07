/*
  matrix-marquee.ino — Serial-controlled scrolling message board for the
  Arduino UNO Q's built-in 13x8 blue LED matrix.

  What it does:
    Scrolls a persistent message across the LED matrix forever, and lets you
    reprogram it live over the serial monitor (115200 baud). Commands
    (case-insensitive, end lines with Enter — \r and \n both accepted):

      TEXT <message>   set the persistent scrolling message (max 120 chars,
                       longer input is truncated safely)
      SPEED <ms>       scroll step speed in ms/step, clamped to 20..500
                       (default 80; lower = faster)
      ONCE <message>   scroll a message exactly one time, then return to the
                       persistent message
      CLEAR            blank the display (empties the persistent message)
      HELP             reprint this command list

    With no input it scrolls a default greeting. Every state change is echoed
    to Serial, e.g. "msg set (23 chars), speed 80ms".

  Wiring: no external hardware needed — uses only the built-in LED matrix.

  A note on blocking (library limitation):
    matrix.endText(SCROLL_LEFT) BLOCKS until the whole message has scrolled
    off the display — that is how ArduinoGraphics works on this core. So the
    sketch polls serial input between scroll passes: a command typed while a
    pass is animating is buffered and takes effect right after the current
    pass finishes. Messages are capped at 120 chars and the pass length is
    proportional to message length x speed, which keeps each blocking window
    reasonable (a full 120-char message at the default 80 ms/step is the
    worst case — prefer shorter messages and/or faster speeds if you need
    snappier command response).

    Messages are padded with leading/trailing spaces so the text enters from
    the right edge and exits off the left edge smoothly.

  Compile:
    arduino-cli compile -b arduino:zephyr:unoq ./matrix-marquee
  Upload:
    arduino-cli upload -b arduino:zephyr:unoq -p <board-ip> ./matrix-marquee
*/

#include "ArduinoGraphics.h"   // MUST come before Arduino_LED_Matrix.h
#include "Arduino_LED_Matrix.h"

#include <string.h>
#include <stdlib.h>
#include <ctype.h>

// ---------- Configuration ----------
static const size_t   MAX_MSG_LEN       = 120;  // longest storable message
static const size_t   PAD_SPACES        = 2;    // blank cols worth of lead/trail padding
static const uint32_t DEFAULT_SPEED_MS  = 80;   // ms per scroll step
static const uint32_t MIN_SPEED_MS      = 20;
static const uint32_t MAX_SPEED_MS      = 500;
static const uint32_t PAUSE_BETWEEN_MS  = 250;  // idle gap between scroll passes
static const uint8_t  TEXT_BRIGHTNESS   = 0xFF; // matrix is blue-only; R value = brightness
static const size_t   LINE_BUF_LEN      = 160;  // serial input line buffer
static const char*    DEFAULT_MESSAGE   = "HELLO FROM UNO Q";

Arduino_LED_Matrix matrix;

// ---------- State ----------
static char     g_message[MAX_MSG_LEN + 1];   // persistent marquee message
static char     g_onceMsg[MAX_MSG_LEN + 1];   // one-shot message
static bool     g_oncePending = false;
static uint32_t g_speedMs     = DEFAULT_SPEED_MS;
static uint32_t g_nextPassAt  = 0;            // millis() gate between passes

// Scroll working buffer: padding + message + padding + NUL.
static char g_scrollBuf[PAD_SPACES + MAX_MSG_LEN + PAD_SPACES + 1];

// Serial line accumulator.
static char   g_line[LINE_BUF_LEN];
static size_t g_lineLen = 0;

// ---------- Small helpers ----------

// Case-insensitive comparison of a token against an uppercase keyword.
static bool tokenIs(const char* token, const char* keywordUpper) {
  while (*token && *keywordUpper) {
    if (toupper((unsigned char)*token) != *keywordUpper) return false;
    token++;
    keywordUpper++;
  }
  return *token == '\0' && *keywordUpper == '\0';
}

// Copy src into dst (capacity MAX_MSG_LEN + 1), truncating safely.
// Returns the stored length.
static size_t storeMessage(char* dst, const char* src) {
  size_t n = strlen(src);
  if (n > MAX_MSG_LEN) n = MAX_MSG_LEN;
  memcpy(dst, src, n);
  dst[n] = '\0';
  return n;
}

static void printHelp() {
  Serial.println();
  Serial.println("=== matrix-marquee: serial message board ===");
  Serial.println("Commands (case-insensitive):");
  Serial.println("  TEXT <message>  set persistent message (max 120 chars)");
  Serial.println("  SPEED <ms>      scroll step speed, 20..500 ms (default 80)");
  Serial.println("  ONCE <message>  scroll a message one time, then resume");
  Serial.println("  CLEAR           blank the display");
  Serial.println("  HELP            show this list");
  Serial.println("Note: commands typed mid-scroll apply after the current pass.");
  Serial.println();
}

// ---------- Command handling ----------

static void handleCommand(char* line) {
  // Split off the first token; the remainder (if any) is the argument.
  char* arg = strchr(line, ' ');
  if (arg != nullptr) {
    *arg = '\0';
    arg++;
    while (*arg == ' ') arg++;   // skip extra separator spaces
  } else {
    arg = line + strlen(line);   // empty argument
  }

  if (tokenIs(line, "TEXT")) {
    if (*arg == '\0') {
      Serial.println("usage: TEXT <message>");
      return;
    }
    size_t n = storeMessage(g_message, arg);
    Serial.print("msg set (");
    Serial.print((unsigned long)n);
    Serial.print(" chars), speed ");
    Serial.print((unsigned long)g_speedMs);
    Serial.println("ms");
    if (strlen(arg) > MAX_MSG_LEN) Serial.println("(truncated to 120 chars)");
  } else if (tokenIs(line, "SPEED")) {
    if (*arg == '\0') {
      Serial.println("usage: SPEED <ms>  (20..500)");
      return;
    }
    long v = strtol(arg, nullptr, 10);
    if (v < (long)MIN_SPEED_MS) v = MIN_SPEED_MS;
    if (v > (long)MAX_SPEED_MS) v = MAX_SPEED_MS;
    g_speedMs = (uint32_t)v;
    matrix.textScrollSpeed(g_speedMs);
    Serial.print("speed set to ");
    Serial.print((unsigned long)g_speedMs);
    Serial.println("ms per step");
  } else if (tokenIs(line, "ONCE")) {
    if (*arg == '\0') {
      Serial.println("usage: ONCE <message>");
      return;
    }
    size_t n = storeMessage(g_onceMsg, arg);
    g_oncePending = true;
    Serial.print("one-shot queued (");
    Serial.print((unsigned long)n);
    Serial.println(" chars)");
  } else if (tokenIs(line, "CLEAR")) {
    g_message[0] = '\0';
    matrix.clear();
    Serial.println("display cleared (persistent msg emptied)");
  } else if (tokenIs(line, "HELP")) {
    printHelp();
  } else {
    Serial.print("unknown command: ");
    Serial.println(line);
    Serial.println("type HELP for the command list");
  }
}

// Non-blocking serial poll: accumulate chars, dispatch on \r or \n.
static void pollSerial() {
  while (Serial.available() > 0) {
    int c = Serial.read();
    if (c < 0) break;
    if (c == '\r' || c == '\n') {
      if (g_lineLen > 0) {                 // ignore empty lines (and the
        g_line[g_lineLen] = '\0';          //  second half of \r\n pairs)
        handleCommand(g_line);
        g_lineLen = 0;
      }
    } else if (g_lineLen < LINE_BUF_LEN - 1) {
      g_line[g_lineLen++] = (char)c;
    }
    // Chars beyond the buffer are dropped; the line still terminates cleanly.
  }
}

// ---------- Scrolling ----------

// Scroll one padded pass of `text`. BLOCKS until the pass completes.
static void scrollPass(const char* text) {
  size_t pos = 0;
  for (size_t i = 0; i < PAD_SPACES; i++) g_scrollBuf[pos++] = ' ';
  size_t n = strlen(text);                 // callers cap this at MAX_MSG_LEN
  memcpy(&g_scrollBuf[pos], text, n);
  pos += n;
  for (size_t i = 0; i < PAD_SPACES; i++) g_scrollBuf[pos++] = ' ';
  g_scrollBuf[pos] = '\0';

  matrix.beginText(0, 0, TEXT_BRIGHTNESS, 0, 0);
  matrix.print(g_scrollBuf);
  matrix.endText(SCROLL_LEFT);             // blocking scroll animation
}

// ---------- Sketch ----------

void setup() {
  Serial.begin(115200);

  matrix.begin();
  matrix.textFont(Font_5x7);
  matrix.textScrollSpeed(g_speedMs);

  storeMessage(g_message, DEFAULT_MESSAGE);

  printHelp();
  Serial.print("scrolling default: \"");
  Serial.print(g_message);
  Serial.println("\"");
}

void loop() {
  pollSerial();  // runs between scroll passes; commands apply next pass

  // Rollover-safe millis() gate for the pause between passes.
  if ((int32_t)(millis() - g_nextPassAt) < 0) {
    return;
  }

  if (g_oncePending) {
    Serial.print("scrolling one-shot: \"");
    Serial.print(g_onceMsg);
    Serial.println("\"");
    scrollPass(g_onceMsg);                 // blocks for this pass
    g_oncePending = false;
    g_nextPassAt = millis() + PAUSE_BETWEEN_MS;
  } else if (g_message[0] != '\0') {
    scrollPass(g_message);                 // blocks for this pass
    g_nextPassAt = millis() + PAUSE_BETWEEN_MS;
  }
  // else: display cleared — just keep polling serial.
}
