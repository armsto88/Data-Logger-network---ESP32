#include <Arduino.h>
#include <Wire.h>
#include <RTClib.h>

#ifndef I2C_SDA_PIN
#define I2C_SDA_PIN 18
#endif
#ifndef I2C_SCL_PIN
#define I2C_SCL_PIN 19
#endif
#ifndef RTC_ADDR
#define RTC_ADDR 0x68
#endif
#ifndef ALARM_INTERVAL_S
#define ALARM_INTERVAL_S 10
#endif
#ifndef ALARM_HOLD_MS
#define ALARM_HOLD_MS 5000
#endif
#ifndef FORCE_SET_RTC_ON_BOOT
#define FORCE_SET_RTC_ON_BOOT 1
#endif

namespace {

RTC_DS3231 rtc;

uint8_t toBcd(uint8_t v) {
  return static_cast<uint8_t>(((v / 10) << 4) | (v % 10));
}

bool writeReg(uint8_t reg, uint8_t value) {
  Wire.beginTransmission(RTC_ADDR);
  Wire.write(reg);
  Wire.write(value);
  return Wire.endTransmission() == 0;
}

bool readReg(uint8_t reg, uint8_t& value) {
  Wire.beginTransmission(RTC_ADDR);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) {
    return false;
  }

  Wire.requestFrom(static_cast<uint8_t>(RTC_ADDR), static_cast<uint8_t>(1));
  if (!Wire.available()) {
    return false;
  }

  value = Wire.read();
  return true;
}

bool writeAlarm1Exact(const DateTime& t) {
  Wire.beginTransmission(RTC_ADDR);
  Wire.write(0x07);  // Alarm1 seconds register
  // Match sec/min/hour/date (A1M1..A1M4 = 0, DY/DT = 0)
  Wire.write(toBcd(static_cast<uint8_t>(t.second())) & 0x7F);
  Wire.write(toBcd(static_cast<uint8_t>(t.minute())) & 0x7F);
  Wire.write(toBcd(static_cast<uint8_t>(t.hour())) & 0x3F);
  Wire.write(toBcd(static_cast<uint8_t>(t.day())) & 0x3F);
  return Wire.endTransmission() == 0;
}

bool clearAlarmFlags() {
  uint8_t status = 0;
  if (!readReg(0x0F, status)) {
    return false;
  }
  status &= static_cast<uint8_t>(~0x03);  // clear A1F and A2F
  return writeReg(0x0F, status);
}

bool enableAlarmInterrupt() {
  uint8_t ctrl = 0;
  if (!readReg(0x0E, ctrl)) {
    return false;
  }
  ctrl |= 0x04;  // INTCN=1
  ctrl |= 0x01;  // A1IE=1
  return writeReg(0x0E, ctrl);
}

bool alarmFlagSet() {
  uint8_t status = 0;
  if (!readReg(0x0F, status)) {
    return false;
  }
  return (status & 0x01) != 0;
}

void printTime(const char* prefix, const DateTime& t) {
  char buf[24];
  snprintf(buf,
           sizeof(buf),
           "%04d-%02d-%02d %02d:%02d:%02d",
           t.year(),
           t.month(),
           t.day(),
           t.hour(),
           t.minute(),
           t.second());
  Serial.print(prefix);
  Serial.println(buf);
}

bool armNextAlarm(const DateTime& fromNow) {
  DateTime base = fromNow;
  DateTime next = base + TimeSpan(0, 0, 0, ALARM_INTERVAL_S);
  if (!writeAlarm1Exact(next)) {
    return false;
  }
  printTime("[A1] armed for ", next);
  return true;
}

void syncRtcFromSystemTime() {
#if FORCE_SET_RTC_ON_BOOT
  DateTime buildTime(F(__DATE__), F(__TIME__));
  rtc.adjust(buildTime);
  printTime("[RTC] set to build time: ", buildTime);
#else
  DateTime now = rtc.now();
  printTime("[RTC] left unchanged: ", now);
#endif
}

void configureAlarmEngine() {
  if (!clearAlarmFlags()) {
    Serial.println("[RTC] failed to clear alarm flags");
  }
  if (!enableAlarmInterrupt()) {
    Serial.println("[RTC] failed to enable INTCN/A1IE");
  }

  DateTime now = rtc.now();
  printTime("[RTC] now: ", now);
  if (!armNextAlarm(now)) {
    Serial.println("[RTC] failed to arm Alarm1");
  }
}

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(400);
  Serial.println();
  Serial.println("=== DS3231 Alarm Bring-up (10 s) ===");

  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
  Wire.setClock(100000);

  if (!rtc.begin(&Wire)) {
    Serial.println("[RTC] DS3231 not found at 0x68");
    while (true) {
      delay(1000);
    }
  }

  Serial.printf("[CFG] I2C SDA=%d SCL=%d RTC=0x%02X INTERVAL=%ds\n",
                I2C_SDA_PIN,
                I2C_SCL_PIN,
                RTC_ADDR,
                ALARM_INTERVAL_S);
  Serial.printf("[CFG] Alarm hold before clear = %d ms\n", ALARM_HOLD_MS);

  syncRtcFromSystemTime();
  configureAlarmEngine();

  Serial.println("[INFO] Polling DS3231 A1F flag only (alarm line is external to gate path).");
}

void loop() {
  if (alarmFlagSet()) {
    DateTime firedAt = rtc.now();
    printTime("[A1] fired at ", firedAt);

    if (ALARM_HOLD_MS > 0) {
      Serial.printf("[A1] holding active for %d ms before clear\n", ALARM_HOLD_MS);
      delay(ALARM_HOLD_MS);
    }

    if (clearAlarmFlags()) {
      Serial.println("[A1] flag cleared");
    } else {
      Serial.println("[A1] clear failed");
    }

    DateTime now = rtc.now();
    if (!armNextAlarm(now)) {
      Serial.println("[A1] re-arm failed");
    }

    // Let line settle after flag clear before re-checking.
    delay(50);
  }

  delay(25);
}
