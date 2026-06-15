// Mothership V1 bringup: Full wake-reason detection
// This is the core boot logic for the V1 firmware.
// Reads CONFIG_WAKE_PIN (GPIO32) and DS3231 alarm flag to determine
// why the board woke up, then takes appropriate action.
// After 10 s of serial output, releases PWR_HOLD to power down.

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
#ifndef CONFIG_CLEAR_PULSE_MS
#define CONFIG_CLEAR_PULSE_MS 20
#endif
#ifndef SHUTDOWN_DELAY_MS
#define SHUTDOWN_DELAY_MS 10000
#endif

enum WakeReason {
  WAKE_RTC_ALARM,
  WAKE_CONFIG_BUTTON,
  WAKE_USB_SERVICE,
  WAKE_UNKNOWN
};

namespace {

RTC_DS3231 rtc;
bool rtcOk = false;

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
  status &= static_cast<uint8_t>(~0x03);
  return writeReg(0x0F, status);
}

bool alarm1FlagSet() {
  uint8_t status = 0;
  if (!readReg(0x0F, status)) return false;
  return (status & 0x01) != 0;
}

const char* wakeReasonStr(WakeReason r) {
  switch (r) {
    case WAKE_RTC_ALARM:    return "RTC_ALARM";
    case WAKE_CONFIG_BUTTON: return "CONFIG_BUTTON";
    case WAKE_USB_SERVICE:   return "USB_SERVICE";
    case WAKE_UNKNOWN:       return "UNKNOWN";
    default:                 return "INVALID";
  }
}

}  // namespace

WakeReason detectWakeReason() {
  // 1. Check config latch (active LOW)
  if (digitalRead(PIN_CONFIG_WAKE) == LOW) {
    return WAKE_CONFIG_BUTTON;
  }

  // 2. Check DS3231 alarm flag
  if (rtcOk && alarm1FlagSet()) {
    return WAKE_RTC_ALARM;
  }

  // 3. Check USB VBUS (if available — USB connected means service mode)
  // On ESP32, USB VBUS detect is not a standard GPIO; for now assume
  // if neither config nor RTC, it's USB service wake.
  // In production, check USB_VBUS pin or use esp_sleep_get_wakeup_cause().
  return WAKE_USB_SERVICE;
}

void clearConfigLatch() {
  digitalWrite(PIN_CONFIG_CLEAR, HIGH);
  delay(CONFIG_CLEAR_PULSE_MS);
  digitalWrite(PIN_CONFIG_CLEAR, LOW);
  delay(5);
}

void setup() {
  // CRITICAL: assert PWR_HOLD immediately
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
  Serial.println("=== Mothership V1 Wake-Reason Detection Bring-up ===");

  // Init I2C for RTC
  Wire.begin(PIN_SDA, PIN_SCL);
  Wire.setClock(100000);

  if (rtc.begin(&Wire)) {
    rtcOk = true;
    Serial.println("[RTC] DS3231 found");
  } else {
    rtcOk = false;
    Serial.println("[RTC] DS3231 NOT found — alarm detection disabled");
  }

  // Detect wake reason
  WakeReason reason = detectWakeReason();
  Serial.printf("Wake reason: %s\n", wakeReasonStr(reason));

  // Take action based on wake reason
  switch (reason) {
    case WAKE_CONFIG_BUTTON:
      Serial.println(">>> Config button wake — clearing latch");
      clearConfigLatch();
      Serial.printf("After clear: CONFIG_WAKE = %s\n",
                     digitalRead(PIN_CONFIG_WAKE) == LOW ? "LOW (FAILED)" : "HIGH (OK)");
      // Blink LED fast to indicate config mode
      for (int i = 0; i < 10; i++) {
        digitalWrite(PIN_CFG_LED, HIGH); delay(100);
        digitalWrite(PIN_CFG_LED, LOW);  delay(100);
      }
      break;

    case WAKE_RTC_ALARM:
      Serial.println(">>> RTC alarm wake — clearing alarm flag");
      if (rtcOk) {
        clearAlarmFlags();
        Serial.println("[RTC] Alarm flag cleared");
      }
      // Blink LED slow to indicate sync mode
      for (int i = 0; i < 5; i++) {
        digitalWrite(PIN_CFG_LED, HIGH); delay(500);
        digitalWrite(PIN_CFG_LED, LOW);  delay(500);
      }
      break;

    case WAKE_USB_SERVICE:
      Serial.println(">>> USB service wake — staying on");
      // Solid LED for service mode
      digitalWrite(PIN_CFG_LED, HIGH);
      break;

    default:
      Serial.println(">>> Unknown wake — defaulting to sync behavior");
      break;
  }

  Serial.printf("Shutting down in %d ms (releasing PWR_HOLD)...\n", SHUTDOWN_DELAY_MS);
  Serial.println("Press reset to re-test. Connect USB to keep board alive.");
}

void loop() {
  // Countdown to power-down
  static unsigned long startMs = millis();
  unsigned long elapsed = millis() - startMs;
  unsigned long remaining = (SHUTDOWN_DELAY_MS > elapsed) ? (SHUTDOWN_DELAY_MS - elapsed) : 0;

  if (remaining > 0 && (remaining % 1000 == 0)) {
    Serial.printf("Power-down in %lu s\n", remaining / 1000);
  }

  if (elapsed >= SHUTDOWN_DELAY_MS) {
    Serial.println("Releasing PWR_HOLD — board will power off.");
    Serial.println("If you can read this, PWR_HOLD release did not cut power.");
    digitalWrite(PIN_CFG_LED, LOW);
    digitalWrite(PIN_PWR_HOLD, LOW);
    // Should not reach here
    while (true) { delay(1000); }
  }

  delay(100);
}