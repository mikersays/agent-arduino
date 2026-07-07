/*
  matrix-text-scroller.ino — Scrolling text marquee on the Arduino UNO Q LED matrix.

  What it does:
    Rotates through a small playlist of messages on the built-in 13x8 blue LED
    matrix using ArduinoGraphics text rendering with Font_5x7:
      1. "HELLO UNO Q"
      2. "13x8 = 104 LEDS"
      3. The current uptime in minutes (rendered with snprintf), e.g. "UP 12 MIN"
    Each message scrolls in from the right and exits to the left (SCROLL_LEFT).

  Wiring: no external hardware — uses only the built-in LED matrix.

  A note on blocking:
    matrix.endText(SCROLL_LEFT) blocks until the entire message has finished
    scrolling off the display. That is the idiomatic usage for this library, so
    the loop is structured as "pick next message -> scroll it (blocking) ->
    repeat". Pacing between messages uses millis() rather than delay(), so the
    only "blocking" is the scroll animation itself.

  Compile:
    arduino-cli compile -b arduino:zephyr:unoq ./matrix-text-scroller
  Upload:
    arduino-cli upload -b arduino:zephyr:unoq -p <board-ip> ./matrix-text-scroller
*/

#include "ArduinoGraphics.h"   // MUST come before Arduino_LED_Matrix.h
#include "Arduino_LED_Matrix.h"

// ---------- Configuration ----------
static const uint32_t SCROLL_SPEED_MS   = 60;   // ms per scroll step (lower = faster)
static const uint32_t PAUSE_BETWEEN_MS  = 400;  // pause between messages
static const uint8_t  TEXT_BRIGHTNESS   = 0xFF; // matrix is blue-only; R value = brightness
static const size_t   UPTIME_BUF_LEN    = 24;

Arduino_LED_Matrix matrix;

// Fixed messages in the rotation. The uptime message is generated on the fly.
static const char* const FIXED_MESSAGES[] = {
  "HELLO UNO Q",
  "13x8 = 104 LEDS",
};
static const size_t NUM_FIXED = sizeof(FIXED_MESSAGES) / sizeof(FIXED_MESSAGES[0]);
static const size_t NUM_SLOTS = NUM_FIXED + 1;  // +1 for the uptime message

// ---------- Helpers ----------

// Render the uptime-in-minutes message into the caller's buffer.
static void formatUptime(char* buf, size_t len) {
  unsigned long minutes = millis() / 60000UL;
  snprintf(buf, len, "UP %lu MIN", minutes);
}

// Scroll one message across the matrix. Blocks until the scroll completes
// (that is how ArduinoGraphics' endText(SCROLL_LEFT) works — see header note).
static void scrollMessage(const char* text) {
  matrix.beginText(0, 0, TEXT_BRIGHTNESS, 0, 0);
  matrix.print(text);
  matrix.endText(SCROLL_LEFT);  // blocking scroll animation
}

// ---------- Sketch ----------

void setup() {
  Serial.begin(115200);

  matrix.begin();
  matrix.textFont(Font_5x7);
  matrix.textScrollSpeed(SCROLL_SPEED_MS);
}

void loop() {
  static size_t slot = 0;                 // which message is next
  static uint32_t nextScrollAt = 0;       // millis() timestamp gate
  static char uptimeBuf[UPTIME_BUF_LEN];

  // millis()-paced gate between (blocking) scrolls — no delay() calls here.
  if ((int32_t)(millis() - nextScrollAt) < 0) {
    return;
  }

  const char* text;
  if (slot < NUM_FIXED) {
    text = FIXED_MESSAGES[slot];
  } else {
    formatUptime(uptimeBuf, sizeof(uptimeBuf));
    text = uptimeBuf;
  }

  Serial.print("Scrolling: ");
  Serial.println(text);
  scrollMessage(text);  // blocks for the duration of the scroll

  slot = (slot + 1) % NUM_SLOTS;
  nextScrollAt = millis() + PAUSE_BETWEEN_MS;
}
