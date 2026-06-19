// Mothership V1 bringup: Config-button + RTC alarm wake test
// End-to-end power-cycle test for the config-wake / RTC-alarm wake path.
//
// Flow:
//   1. Assert PWR_HOLD immediately (keep board alive).
//   2. Init I2C + DS3231 RTC.
//   3. Set RTC time to current build time.
//   4. Arm RTC Alarm 1 for 2 minutes from now (A1M4=1, A1IE=1, INTCN=1).
//   5. Print alarm time + control register.
//   6. Wait 5 seconds so serial output can be read.
//   7. Release PWR_HOLD — board powers off.
//   8. On next wake (RTC alarm at 2 min OR config button), print:
//        - Wake reason (config latch state, alarm flag state)
//        - GPIO32 (CONFIG_WAKE) state
//        - DS3231 status register
//        - DS3231 control register
//        - Current RTC time
//   9. Stay on for 30 seconds so output can be read, then release PWR_HOLD again.

#include <Arduino.h>
#include <Wire.h>
#include <RTClib.h>

#ifndef PIN_PWR_HOLD
#define PIN_PWR_HOLD 26
#endif
#ifndef PIN_CONFIG_WAKE
#define PIN_CONFIG_WAKE 32
#endif
#ifndef PIN_CONFIG_CLEAR
#define PIN_CONFIG_CLEAR 25
#endif
#ifndef PIN_CFG_LED
#define PIN_CFG_LED 27
#endif
#ifndef PIN_SDA
#define PIN_SDA 21
#endif
#ifndef PIN_SCL
#define PIN_SCL 22
#endif
#ifndef RTC_ADDR
#define RTC_ADDR 0x68
#endif
#ifndef ALARM_MINUTES_FROM_NOW
#define ALARM_MINUTES_FROM_NOW 2
#endif
#ifndef PRE_SHUTDOWN_HOLD_MS
#define PRE_SHUTDOWN_HOLD_MS 5000
#endif
#ifndef POST_WAKE_HOLD_MS
#define POST_WAKE_HOLD_MS 30000
#endif

namespace {

RTC_DS3231 rtc;
bool rtcOk = false;

uint8_t toBcd(uint8_t v) {
  return static_cast<uint8_t>(((v / 10) << 4) | (v % 10));
}

bool readReg(uint8_t reg, uint8_t& value) {
  Wire.beginTransmission(RTC_ADDR);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  Wire.requestFrom(static_cast<uint8_t>(RTC_ADDR), static_cast<uint8_t>(1));
  if (!Wire.available()) return false;
  value = Wire.read();
  return true;
}

bool writeReg(uint8_t reg, uint8_t value) {
  Wire.beginTransmission(RTC_ADDR);
  Wire.write(reg);
  Wire.write(value);
  return Wire.endTransmission() == 0;
}

bool clearAlarmFlags() {
  uint8_t status = 0;
  if (!readReg(0x0F, status)) return false;
  status &= static_cast<uint8_t>(~0x03);  // clear A1F + A2F
  return writeReg(0x0F, status);
}

bool alarm1FlagSet() {
  uint8_t status = 0;
  if (!readReg(0x0F, status)) return false;
  return (status & 0x01) != 0;
}

// Arm Alarm 1 to fire at an exact sec/min/hour, matching on those three
// fields but ignoring day-of-month (A1M4=1). This is the standard
// "alarm once per day at HH:MM:SS" mode used by the V1 firmware.
bool armAlarm1At(const DateTime& t) {
  Wire.beginTransmission(RTC_ADDR);
  Wire.write(0x07);  // Alarm 1 seconds register
  Wire.write(toBcd(static_cast<uint8_t>(t.second())) & 0x7F);  // A1M1=0 (match)
  Wire.write(toBcd(static_cast<uint8_t>(t.minute())) & 0x7F);  // A1M2=0 (match)
  Wire.write(toBcd(static_cast<uint8_t>(t.hour()))   & 0x3F);  // A1M3=0 (match)
  Wire.write(0x80 | toBcd(static_cast<uint8_t>(t.day())));      // A1M4=1 (ignore day)
  if (Wire.endTransmission() != 0) return false;

  // Control register 0x0E: INTCN=1 (INT/SQW = interrupt), A1IE=1 (Alarm1 enable)
  uint8_t ctrl = 0;
  if (!readReg(0x0E, ctrl)) return false;
  ctrl |= 0x05;  // INTCN=1, A1IE=1
  return writeReg(0x0E, ctrl);
}

void printTime(const char* prefix, const DateTime& t) {
  char buf[24];
  snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d",
           t.year(), t.month(), t.day(), t.hour(), t.minute(), t.second());
  Serial.print(prefix);
  Serial.println(buf);
}

void printReg(const char* label, uint8_t reg) {
  uint8_t v = 0;
  if (readReg(reg, v)) {
    Serial.printf("%s reg 0x%02X = 0x%02X (bin ", label, reg, v);
    for (int i = 7; i >= 0; i--) Serial.write((v >> i) & 1 ? '1' : '0');
    Serial.println(")");
  } else {
    Serial.printf("%s reg 0x%02X read FAILED\n", label, reg);
  }
}

}  // namespace

void setup() {
  // 1. CRITICAL: assert PWR_HOLD immediately so the board stays awake.
  pinMode(PIN_PWR_HOLD, OUTPUT);
  digitalWrite(PIN_PWR_HOLD, HIGH);

  // Config latch pins
  pinMode(PIN_CONFIG_WAKE, INPUT);
  pinMode(PIN_CONFIG_CLEAR, OUTPUT);
  digitalWrite(PIN_CONFIG_CLEAR, LOW);

  // Status LED
  pinMode(PIN_CFG_LED, OUTPUT);
  digitalWrite(PIN_CFG_LED, LOW);

  Serial.begin(115200);
  delay(800);
  Serial.println();
  Serial.println("=== Mothership V1 Config-Wake + RTC Alarm Test ===");

  // 2. Init I2C + DS3231 RTC
  Wire.begin(PIN_SDA, PIN_SCL);
  Wire.setClock(100000);

  if (rtc.begin(&Wire)) {
    rtcOk = true;
    Serial.println("[RTC] DS3231 found at 0x68");
  } else {
    rtcOk = false;
    Serial.println("[RTC] DS3231 NOT found — aborting (holding PWR_HOLD).");
    while (true) { delay(1000); }
  }

  // Detect whether this boot is a fresh arm (first run) or a wake.
  // Heuristic: if the alarm flag is already set OR the config latch is LOW,
  // treat this as a wake; otherwise treat as the initial arm pass.
  bool configLatched = (digitalRead(PIN_CONFIG_WAKE) == LOW);
  bool alarmFired    = alarm1FlagSet();

  Serial.printf("[BOOT] CONFIG_WAKE=%s  A1F=%s\n",
                configLatched ? "LOW(latched)" : "HIGH(clear)",
                alarmFired ? "SET" : "clear");

  if (!configLatched && !alarmFired) {
    // ---- Initial arm pass ----
    Serial.println("[PASS 1] Initial arm — setting RTC time and arming Alarm 1.");

    // 3. Set RTC time to current build time
    DateTime buildTime(F(__DATE__), F(__TIME__));
    rtc.adjust(buildTime);
    printTime("[RTC] set to build time: ", buildTime);

    // Clear any stale alarm flags before arming
    if (!clearAlarmFlags()) {
      Serial.println("[RTC] WARNING: failed to clear alarm flags");
    }

    // 4. Arm RTC Alarm 1 for ALARM_MINUTES_FROM_NOW minutes from now
    DateTime now = rtc.now();
    printTime("[RTC] now: ", now);
    DateTime base = now;  // non-const copy for operator+
    DateTime alarmAt = base + TimeSpan(0, 0, ALARM_MINUTES_FROM_NOW, 0);

    if (!armAlarm1At(alarmAt)) {
      Serial.println("[RTC] FAILED to arm Alarm 1 — holding PWR_HOLD.");
      while (true) { delay(1000); }
    }

    // 5. Print alarm time and control register
    printTime("[A1] armed for ", alarmAt);
    printReg("[A1] control", 0x0E);
    printReg("[A1] status ", 0x0F);
    Serial.printf("[A1] alarm in %d minute(s). Board will power off, then wake via RTC or config button.\n",
                   ALARM_MINUTES_FROM_NOW);

    // 6. Wait 5 seconds so serial output can be read
    Serial.printf("[INFO] Holding for %d ms so you can read this...\n", PRE_SHUTDOWN_HOLD_MS);
    delay(PRE_SHUTDOWN_HOLD_MS);

    // 7. Release PWR_HOLD — board powers off
    Serial.println("[INFO] Releasing PWR_HOLD — board should power off now.");
    Serial.println("[INFO] If you can read this, PWR_HOLD release did not cut power.");
    digitalWrite(PIN_CFG_LED, LOW);
    digitalWrite(PIN_PWR_HOLD, LOW);
    // Should never reach here
    while (true) { delay(1000); }
  }

  // ---- Wake pass (RTC alarm or config button) ----
  Serial.println("[PASS 2] Wake detected — reporting wake reason and registers.");

  // 8. Print wake diagnostics
  Serial.println("----- WAKE DIAGNOSTICS -----");
  Serial.printf("Wake reason: %s\n",
                configLatched ? "CONFIG_BUTTON (latch LOW)"
                               : (alarmFired ? "RTC_ALARM (A1F set)"
                                             : "UNKNOWN / USB_SERVICE"));
  Serial.printf("  CONFIG_WAKE (GPIO%d) = %s\n",
                PIN_CONFIG_WAKE,
                configLatched ? "LOW (latch set)" : "HIGH (not set)");
  Serial.printf("  DS3231 A1F flag      = %s\n", alarmFired ? "SET" : "clear");
  printReg("  DS3231 status  ", 0x0F);
  printReg("  DS3231 control ", 0x0E);
  if (rtcOk) {
    DateTime now = rtc.now();
    printTime("  Current RTC time    ", now);
  } else {
    Serial.println("  Current RTC time    : RTC unavailable");
  }
  Serial.println("-----------------------------");

  // Blink LED to indicate we're alive after wake
  for (int i = 0; i < 3; i++) {
    digitalWrite(PIN_CFG_LED, HIGH); delay(150);
    digitalWrite(PIN_CFG_LED, LOW);  delay(150);
  }

  // 9. Stay on for 30 seconds so output can be read, then release PWR_HOLD again
  Serial.printf("[INFO] Staying on for %d ms so you can read the diagnostics above.\n",
                POST_WAKE_HOLD_MS);
  Serial.println("[INFO] After this hold, PWR_HOLD will be released again.");
}

void loop() {
  static unsigned long wakeStart = millis();
  unsigned long elapsed = millis() - wakeStart;
  unsigned long remaining = (POST_WAKE_HOLD_MS > elapsed) ? (POST_WAKE_HOLD_MS - elapsed) : 0;

  if (remaining > 0 && (remaining % 5000 == 0) && remaining != POST_WAKE_HOLD_MS) {
    Serial.printf("[INFO] Power-down in %lu s\n", remaining / 1000);
  }

  if (elapsed >= POST_WAKE_HOLD_MS) {
    Serial.println("[INFO] Releasing PWR_HOLD — board will power off again.");
    Serial.println("[INFO] Re-arm by pressing config button or waiting for next RTC alarm.");
    digitalWrite(PIN_CFG_LED, LOW);
    digitalWrite(PIN_PWR_HOLD, LOW);
    while (true) { delay(1000); }
  }

  delay(100);
}