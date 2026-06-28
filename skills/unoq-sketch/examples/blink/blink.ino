// UNO Q — blink the built-in LED.
// NOTE: LED_BUILTIN on this board is ACTIVE LOW (LOW = on).
//
// Build & upload (run from the Linux side of this board):
//   arduino-cli compile -b arduino:zephyr:unoq ./blink
//   arduino-cli upload  -b arduino:zephyr:unoq -p <board-wlan0-ip> ./blink

void setup() {
  pinMode(LED_BUILTIN, OUTPUT);
  Serial.begin(115200);
}

void loop() {
  digitalWrite(LED_BUILTIN, LOW);   // ON  (active low)
  Serial.println("on");
  delay(500);

  digitalWrite(LED_BUILTIN, HIGH);  // OFF
  Serial.println("off");
  delay(500);
}
