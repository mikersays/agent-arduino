/*
  rgb-status-cycle.ino — Arduino UNO Q (STM32U585 MCU side)

  What it does:
    Cycles the two MCU-driven RGB status LEDs through complementary colors.
    LED3 steps through the 7 "on" combinations of its R/G/B channels
    (Red, Green, Blue, Yellow, Magenta, Cyan, White) every ~600 ms, while
    LED4 always shows the complementary color (White <-> Off included).
    LED_BUILTIN gives a heartbeat double-blink every 2 seconds.
    The current LED3 color name (and LED4's complement) is printed to
    Serial on every step.

  Wiring:
    No external hardware — uses the on-board RGB LEDs (LED3/LED4 pin
    macros from the board variant) and LED_BUILTIN only.

  Polarity:
    All LEDs are assumed ACTIVE-LOW (write LOW to turn ON), which matches
    the UNO Q board. If your board revision differs, flip the single
    constant LED_ACTIVE_LOW below to false — nothing else needs changing.

  Build:
    arduino-cli compile -b arduino:zephyr:unoq ./rgb-status-cycle
    arduino-cli upload  -b arduino:zephyr:unoq -p <board-ip> ./rgb-status-cycle
*/

#include <Arduino.h>

// ---------- Configuration ----------------------------------------------

// Set to false if your LEDs turn out to be active-high.
const bool LED_ACTIVE_LOW = true;

const unsigned long COLOR_STEP_MS = 600;   // time per color step
const unsigned long HEARTBEAT_PERIOD_MS = 2000; // double-blink period
const unsigned long HEARTBEAT_PULSE_MS = 100;   // width of each blink

// Channel bitmask layout: bit0 = R, bit1 = G, bit2 = B
const uint8_t MASK_R = 0x01;
const uint8_t MASK_G = 0x02;
const uint8_t MASK_B = 0x04;
const uint8_t MASK_ALL = MASK_R | MASK_G | MASK_B;

// The 7 "on" combinations LED3 cycles through, in order.
const uint8_t COLOR_SEQUENCE[] = {
  MASK_R,                    // Red
  MASK_G,                    // Green
  MASK_B,                    // Blue
  MASK_R | MASK_G,           // Yellow
  MASK_R | MASK_B,           // Magenta
  MASK_G | MASK_B,           // Cyan
  MASK_ALL                   // White
};
const uint8_t NUM_COLORS = sizeof(COLOR_SEQUENCE) / sizeof(COLOR_SEQUENCE[0]);

// ---------- Helpers -----------------------------------------------------

// Name for any 3-bit R/G/B mask (index 0..7).
const char* colorName(uint8_t mask) {
  static const char* const NAMES[8] = {
    "Off", "Red", "Green", "Yellow", "Blue", "Magenta", "Cyan", "White"
  };
  return NAMES[mask & MASK_ALL];
}

// Write one LED channel honoring the configured polarity.
void writeLed(pin_size_t pin, bool on) {
  const bool level = LED_ACTIVE_LOW ? !on : on;
  digitalWrite(pin, level ? HIGH : LOW);
}

// Apply an R/G/B bitmask to one RGB LED's three channel pins.
void applyColor(pin_size_t rPin, pin_size_t gPin, pin_size_t bPin, uint8_t mask) {
  writeLed(rPin, mask & MASK_R);
  writeLed(gPin, mask & MASK_G);
  writeLed(bPin, mask & MASK_B);
}

// ---------- Color cycle -------------------------------------------------

void stepColorCycle(unsigned long now) {
  static unsigned long lastStepMs = 0;
  static uint8_t colorIndex = 0;
  static bool firstStep = true;

  if (!firstStep && (now - lastStepMs) < COLOR_STEP_MS) {
    return;
  }
  lastStepMs = now;
  firstStep = false;

  const uint8_t led3Mask = COLOR_SEQUENCE[colorIndex];
  const uint8_t led4Mask = led3Mask ^ MASK_ALL;  // complementary color

  applyColor(LED3_R, LED3_G, LED3_B, led3Mask);
  applyColor(LED4_R, LED4_G, LED4_B, led4Mask);

  Serial.print("LED3: ");
  Serial.print(colorName(led3Mask));
  Serial.print("  |  LED4: ");
  Serial.println(colorName(led4Mask));

  colorIndex = (colorIndex + 1) % NUM_COLORS;
}

// ---------- Heartbeat ---------------------------------------------------

// Double-blink: ON 0-100ms, OFF 100-200ms, ON 200-300ms, OFF until 2000ms.
void stepHeartbeat(unsigned long now) {
  const unsigned long phase = now % HEARTBEAT_PERIOD_MS;
  const bool on = (phase < HEARTBEAT_PULSE_MS) ||
                  (phase >= 2 * HEARTBEAT_PULSE_MS && phase < 3 * HEARTBEAT_PULSE_MS);
  writeLed(LED_BUILTIN, on);
}

// ---------- Arduino entry points ----------------------------------------

void setup() {
  Serial.begin(115200);

  const pin_size_t ledPins[] = {
    LED3_R, LED3_G, LED3_B, LED4_R, LED4_G, LED4_B, LED_BUILTIN
  };
  for (pin_size_t pin : ledPins) {
    pinMode(pin, OUTPUT);
    writeLed(pin, false);  // start with everything off
  }
}

void loop() {
  const unsigned long now = millis();
  stepColorCycle(now);
  stepHeartbeat(now);
}
