/*
  i2c-scanner.ino — I2C bus scanner for the Arduino UNO Q (STM32U585 MCU side)

  What it does:
    Every 5 seconds, scans the I2C bus for devices at addresses 0x08..0x77.
    Prints a formatted table of found devices to Serial (hex addresses), with
    a best-effort annotation for well-known Modulino / QWIIC device addresses.
    After the table it prints a summary line (device count + scan duration in
    milliseconds). If the bus is empty it prints "no devices found".

    As a headless indicator, LED_BUILTIN blinks once per device found after
    each scan (LED_BUILTIN is ACTIVE-LOW on this board: LOW = ON).

  Wiring:
    No external hardware required (works with an empty bus). To see results,
    connect any I2C device (e.g. a Modulino or QWIIC breakout) to the QWIIC
    connector / SDA+SCL pins.

  Build / deploy:
    Compile: arduino-cli compile -b arduino:zephyr:unoq ./i2c-scanner
    Upload:  arduino-cli upload -b arduino:zephyr:unoq -p <board-ip> ./i2c-scanner
*/

#include <Wire.h>

// ---------- Configuration ----------
const uint8_t  I2C_FIRST_ADDR   = 0x08;   // below 0x08 are reserved addresses
const uint8_t  I2C_LAST_ADDR    = 0x77;   // above 0x77 are reserved addresses
const uint32_t SCAN_INTERVAL_MS = 5000;   // time between scans
const uint32_t SERIAL_BAUD      = 115200;

// LED_BUILTIN polarity: on the UNO Q the LED is active-low (LOW = ON).
const bool     LED_ACTIVE_LOW   = true;
const uint32_t BLINK_ON_MS      = 120;    // per-device blink on-time
const uint32_t BLINK_OFF_MS     = 180;    // gap between blinks

// ---------- Known-device annotations (best effort, not exhaustive) ----------
struct KnownDevice {
  uint8_t     addr;
  const char* name;
};

const KnownDevice KNOWN_DEVICES[] = {
  { 0x0F, "possibly Zio/Qwiic keypad" },
  { 0x10, "possibly VEML6075 UV (Qwiic)" },
  { 0x19, "possibly LIS3DH accel" },
  { 0x1C, "possibly Modulino Movement (LIS2DUXS12) / magnetometer" },
  { 0x29, "possibly VL53L0X/VL53L1X ToF (Modulino Distance / Qwiic)" },
  { 0x36, "possibly Modulino Buttons/Buzzer (ATtiny) or seesaw" },
  { 0x3C, "possibly SSD1306 OLED" },
  { 0x3D, "possibly SSD1306 OLED (alt)" },
  { 0x44, "possibly SHT4x temp/humidity" },
  { 0x48, "possibly ADS1x15 ADC / TMP102" },
  { 0x50, "possibly EEPROM (24Cxx)" },
  { 0x52, "possibly APDS-9960 gesture" },
  { 0x5A, "possibly MPR121 touch / CCS811" },
  { 0x60, "possibly ATECC608 / MCP4725 DAC / Modulino Thermo" },
  { 0x62, "possibly SCD4x CO2" },
  { 0x68, "possibly RTC (DS3231/PCF8523) or IMU (MPU6050)" },
  { 0x6A, "possibly LSM6DSx IMU" },
  { 0x6B, "possibly LSM6DSx IMU (alt)" },
  { 0x70, "possibly I2C mux (TCA9548A) / HT16K33" },
  { 0x76, "possibly BME280/BMP280 (Modulino Pressure)" },
  { 0x77, "possibly BME280/BMP280 (alt)" },
};
const size_t KNOWN_DEVICE_COUNT = sizeof(KNOWN_DEVICES) / sizeof(KNOWN_DEVICES[0]);

// Note column width; must fit the longest name above so the table stays aligned.
const int NOTE_COL_WIDTH = 56;

// ---------- Scan state ----------
const uint8_t MAX_ADDRESSES = I2C_LAST_ADDR - I2C_FIRST_ADDR + 1;  // 112

uint8_t  foundAddrs[MAX_ADDRESSES];
uint8_t  foundCount     = 0;
uint32_t lastScanMs     = 0;
uint32_t scanNumber     = 0;
bool     firstScanDone  = false;

// ---------- Blink state machine (non-blocking) ----------
uint8_t  blinksRemaining = 0;
bool     ledIsOn         = false;
uint32_t ledPhaseStartMs = 0;

// ---------- Helpers ----------
void ledWrite(bool on) {
  digitalWrite(LED_BUILTIN, (on == !LED_ACTIVE_LOW) ? HIGH : LOW);
}

const char* lookupKnownName(uint8_t addr) {
  for (size_t i = 0; i < KNOWN_DEVICE_COUNT; i++) {
    if (KNOWN_DEVICES[i].addr == addr) {
      return KNOWN_DEVICES[i].name;
    }
  }
  return "";
}

void printHexAddr(uint8_t addr) {
  Serial.print("0x");
  if (addr < 0x10) {
    Serial.print('0');
  }
  Serial.print(addr, HEX);
}

// Print a table border row sized to the note column: +------+--...--+
void printTableBorder() {
  Serial.print("  +------+");
  for (int i = 0; i < NOTE_COL_WIDTH + 2; i++) {
    Serial.print('-');
  }
  Serial.println('+');
}

// Perform one full bus scan; fills foundAddrs/foundCount, returns duration in ms.
uint32_t scanBus() {
  foundCount = 0;
  const uint32_t start = millis();
  for (uint8_t addr = I2C_FIRST_ADDR; addr <= I2C_LAST_ADDR; addr++) {
    Wire.beginTransmission(addr);
    const uint8_t err = Wire.endTransmission();
    if (err == 0 && foundCount < MAX_ADDRESSES) {
      foundAddrs[foundCount++] = addr;
    }
  }
  return millis() - start;
}

void printReport(uint32_t durationMs) {
  Serial.println();
  Serial.print("I2C scan #");
  Serial.print(scanNumber);
  Serial.print(" (");
  printHexAddr(I2C_FIRST_ADDR);
  Serial.print("..");
  printHexAddr(I2C_LAST_ADDR);
  Serial.println(")");

  if (foundCount == 0) {
    Serial.println("  no devices found");
  } else {
    printTableBorder();
    Serial.print("  | addr | note");
    for (int pad = NOTE_COL_WIDTH - 4; pad > 0; pad--) {
      Serial.print(' ');
    }
    Serial.println(" |");
    printTableBorder();
    for (uint8_t i = 0; i < foundCount; i++) {
      Serial.print("  | ");
      printHexAddr(foundAddrs[i]);
      Serial.print(" | ");
      const char* name = lookupKnownName(foundAddrs[i]);
      Serial.print(name);
      // Pad the note column to a fixed width of NOTE_COL_WIDTH characters.
      for (int pad = NOTE_COL_WIDTH - (int)strlen(name); pad > 0; pad--) {
        Serial.print(' ');
      }
      Serial.println(" |");
    }
    printTableBorder();
  }

  Serial.print("  summary: ");
  Serial.print(foundCount);
  Serial.print(foundCount == 1 ? " device" : " devices");
  Serial.print(" found in ");
  Serial.print(durationMs);
  Serial.println(" ms");
}

// Advance the per-device blink state machine without blocking.
void updateBlinker(uint32_t now) {
  if (blinksRemaining == 0 && !ledIsOn) {
    return;
  }
  if (ledIsOn) {
    if (now - ledPhaseStartMs >= BLINK_ON_MS) {
      ledWrite(false);
      ledIsOn = false;
      ledPhaseStartMs = now;
    }
  } else {
    if (blinksRemaining > 0 && now - ledPhaseStartMs >= BLINK_OFF_MS) {
      ledWrite(true);
      ledIsOn = true;
      blinksRemaining--;
      ledPhaseStartMs = now;
    }
  }
}

// ---------- Arduino entry points ----------
void setup() {
  pinMode(LED_BUILTIN, OUTPUT);
  ledWrite(false);

  Serial.begin(SERIAL_BAUD);

  Wire.begin();

  Serial.println("UNO Q I2C scanner: scanning 0x08..0x77 every 5 s");
}

void loop() {
  const uint32_t now = millis();

  if (!firstScanDone || now - lastScanMs >= SCAN_INTERVAL_MS) {
    lastScanMs = now;
    firstScanDone = true;
    scanNumber++;

    const uint32_t duration = scanBus();
    printReport(duration);

    // Queue one blink per found device (headless indicator).
    blinksRemaining = foundCount;
    ledPhaseStartMs = now;
  }

  updateBlinker(millis());
}
