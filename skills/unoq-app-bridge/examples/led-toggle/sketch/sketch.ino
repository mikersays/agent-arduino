// UNO Q App example — MCU side.
// Exposes "set_led_state" over the RouterBridge so Python (Linux) can drive the LED.
// LED_BUILTIN is ACTIVE LOW (LOW = on).

#include <Arduino_RouterBridge.h>

void setup() {
  pinMode(LED_BUILTIN, OUTPUT);
  Bridge.begin();
  Bridge.provide("set_led_state", set_led_state);
}

void loop() {
  // Nothing here — work is driven by RPC calls from Python.
}

// Called from Python via Bridge.call("set_led_state", <bool>).
// Runs on a Bridge worker thread (not loop()); this handler touches no shared
// state, so no mutex is needed here. Add one if you share variables with loop().
void set_led_state(bool state) {
  digitalWrite(LED_BUILTIN, state ? LOW : HIGH);
}
