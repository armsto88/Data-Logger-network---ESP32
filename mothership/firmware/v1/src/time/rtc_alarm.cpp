#include "time/rtc_alarm.h"
#include "system/pins.h"
#include <Wire.h>

static RTC_DS3231 gRTC;
static bool gRTCInitialized = false;

// DS3231 I2C address
static constexpr uint8_t kRTCAddr = 0x68;

// --- Low-level register access ---

static uint8_t toBcd(uint8_t v) {
  return static_cast<uint8_t>(((v / 10) << 4) | (v % 10));
}

static uint8_t fromBcd(uint8_t v) {
  return static_cast<uint8_t>((v & 0x0F) + ((v >> 4) * 10));
}

static bool writeReg(uint8_t reg, uint8_t value) {
  Wire.beginTransmission(kRTCAddr);
  Wire.write(reg);
  Wire.write(value);
  return Wire.endTransmission() == 0;
}

static bool readReg(uint8_t reg, uint8_t& value) {
  Wire.beginTransmission(kRTCAddr);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  Wire.requestFrom(static_cast<uint8_t>(kRTCAddr), static_cast<uint8_t>(1));
  if (!Wire.available()) return false;
  value = Wire.read();
  return true;
}

// --- Alarm register programming ---

static bool writeAlarm1Exact(const DateTime& t) {
  Wire.beginTransmission(kRTCAddr);
  Wire.write(0x07);  // Alarm 1 seconds register
  // Match sec/min/hour, ignore day (A1M1..A1M3 = 0, A1M4 = 1, DY/DT = 0)
  // A1M4=1 means the alarm fires when seconds+minutes+hours match, on any day.
  Wire.write(toBcd(static_cast<uint8_t>(t.second())) & 0x7F);   // A1M1=0
  Wire.write(toBcd(static_cast<uint8_t>(t.minute())) & 0x7F);   // A1M2=0
  Wire.write(toBcd(static_cast<uint8_t>(t.hour())) & 0x3F);     // A1M3=0
  Wire.write(0x80 | toBcd(static_cast<uint8_t>(t.day())));      // A1M4=1 (ignore day)
  return Wire.endTransmission() == 0;
}

static bool readAlarm1Registers(uint8_t regs[4]) {
  // Read alarm 1 registers: 0x07 (seconds) through 0x0A (day/date)
  for (int i = 0; i < 4; i++) {
    if (!readReg(static_cast<uint8_t>(0x07 + i), regs[i])) {
      return false;
    }
  }
  return true;
}

// --- Public API ---

RtcInitStatus initRTC() {
  Wire.begin(PIN_SDA, PIN_SCL);
  Wire.setClock(100000);

  if (!gRTC.begin(&Wire)) {
    gRTCInitialized = false;
    Serial.println("[RTC] DS3231 not found at 0x68");
    return RTC_ABSENT;
  }

  gRTCInitialized = true;
  const bool lostPower = gRTC.lostPower();
  if (lostPower) {
    Serial.println("[RTC] DS3231 lost power — time is invalid, needs setting");
  }

  DateTime now = gRTC.now();
  Serial.printf("[RTC] DS3231 initialized, current time: %04d-%02d-%02d %02d:%02d:%02d\n",
                now.year(), now.month(), now.day(), now.hour(), now.minute(), now.second());
  if (lostPower || now.year() < 2024) {
    if (!lostPower) {
      Serial.printf("[RTC] DS3231 time is invalid: year %d is before 2024\n", now.year());
    }
    return RTC_PRESENT_TIME_INVALID;
  }
  return RTC_OK;
}

bool rtcTimeValid() {
  if (!gRTCInitialized) return false;
  if (gRTC.lostPower()) return false;
  DateTime now = gRTC.now();
  return now.year() >= 2024;
}

uint32_t getRTCTime() {
  if (!gRTCInitialized) return 0;
  DateTime now = gRTC.now();
  return now.unixtime();
}

void setRTCTime(uint32_t unixTime) {
  if (!gRTCInitialized) return;
  DateTime dt(static_cast<uint32_t>(unixTime));
  gRTC.adjust(dt);
  Serial.printf("[RTC] Time set to %04d-%02d-%02d %02d:%02d:%02d\n",
                dt.year(), dt.month(), dt.day(), dt.hour(), dt.minute(), dt.second());
}

bool armRescueAlarm(int intervalMin) {
  if (!gRTCInitialized || intervalMin <= 0) return false;

  DateTime now = gRTC.now();
  DateTime rescueTime = now + TimeSpan(0, 0, intervalMin, 0);

  // Enable the Alarm 1 interrupt output before programming the fallback.
  uint8_t ctrl = 0;
  if (!readReg(0x0E, ctrl)) {
    Serial.println("[RTC] Failed to read control register for rescue alarm");
    return false;
  }
  ctrl |= 0x04;  // INTCN=1
  ctrl |= 0x01;  // A1IE=1
  if (!writeReg(0x0E, ctrl)) {
    Serial.println("[RTC] Failed to write control register for rescue alarm");
    return false;
  }

  if (!writeAlarm1Exact(rescueTime)) {
    Serial.println("[RTC] Failed to write rescue alarm registers");
    return false;
  }

  if (!verifyAlarmSet(rescueTime)) {
    Serial.println("[RTC] Rescue alarm read-back verification failed");
    return false;
  }

  if (!clearAlarmFlag()) {
    Serial.println("[RTC] Failed to clear alarm flag after rescue verification");
    return false;
  }

  Serial.printf("[RTC] Rescue alarm armed for %04d-%02d-%02d %02d:%02d:%02d (in %d min)\n",
                rescueTime.year(), rescueTime.month(), rescueTime.day(),
                rescueTime.hour(), rescueTime.minute(), rescueTime.second(), intervalMin);
  return true;
}

bool armNextSyncAlarm(int intervalMin) {
  if (!gRTCInitialized) return false;

  DateTime now = gRTC.now();
  uint32_t nowUnix = now.unixtime();
  uint32_t periodSec = static_cast<uint32_t>(intervalMin) * 60UL;
  // Wake 10 seconds before the sync time so the mothership is already
  // listening when the node wakes and sends its snapshot.
  DateTime next = now + TimeSpan(0, 0, intervalMin, 0) - TimeSpan(0, 0, 0, 10);
  uint32_t nextSyncUnix = next.unixtime();
  if (nextSyncUnix <= nowUnix + 5) {
    nextSyncUnix += periodSec;
    next = DateTime(nextSyncUnix);
  }

  // Enable the Alarm 1 interrupt output.
  uint8_t ctrl = 0;
  if (!readReg(0x0E, ctrl)) {
    Serial.println("[RTC] Failed to read control register");
    return false;
  }
  ctrl |= 0x04;  // INTCN=1
  ctrl |= 0x01;  // A1IE=1
  if (!writeReg(0x0E, ctrl)) {
    Serial.println("[RTC] Failed to write control register");
    return false;
  }
  Serial.printf("[RTC] Control register: 0x%02X (INTCN=1, A1IE=1)\n", ctrl);

  if (!writeAlarm1Exact(next)) {
    Serial.println("[RTC] Failed to write Alarm 1 registers");
    return false;
  }

  if (!verifyAlarmSet(next)) {
    Serial.println("[RTC] Alarm 1 read-back verification failed");
    return false;
  }

  if (!clearAlarmFlag()) {
    Serial.println("[RTC] Failed to clear alarm flag after verification");
    return false;
  }

  Serial.printf("[RTC] Alarm 1 armed for %04d-%02d-%02d %02d:%02d:%02d (in %d min)\n",
                next.year(), next.month(), next.day(), next.hour(), next.minute(), next.second(), intervalMin);
  return true;
}

bool armNextSyncAlarmPhase(int intervalMin, uint32_t phaseUnix) {
  if (!gRTCInitialized) return false;

  DateTime now = gRTC.now();
  uint32_t nowUnix = now.unixtime();
  uint32_t nextSyncUnix = 0;
  uint32_t periodSec = static_cast<uint32_t>(intervalMin) * 60UL;

  if (phaseUnix > 0 && intervalMin > 0) {
    // Compute next sync time aligned to phase anchor
    if (nowUnix < phaseUnix) {
      nextSyncUnix = phaseUnix;
    } else {
      uint32_t elapsed = nowUnix - phaseUnix;
      uint32_t slots = elapsed / periodSec;
      nextSyncUnix = phaseUnix + (slots + 1) * periodSec;
    }
  } else {
    // No phase anchor — fall back to now + interval
    nextSyncUnix = nowUnix + (uint32_t)intervalMin * 60UL;
  }

  // Pre-wake 10 seconds before sync time
  if (nextSyncUnix > 10) nextSyncUnix -= 10;
  if (nextSyncUnix <= nowUnix + 5) {
    nextSyncUnix += periodSec;
  }

  DateTime next(nextSyncUnix);

  // Enable the Alarm 1 interrupt output.
  uint8_t ctrl = 0;
  if (!readReg(0x0E, ctrl)) {
    Serial.println("[RTC] Failed to read control register");
    return false;
  }
  ctrl |= 0x04;  // INTCN=1
  ctrl |= 0x01;  // A1IE=1
  if (!writeReg(0x0E, ctrl)) {
    Serial.println("[RTC] Failed to write control register");
    return false;
  }
  Serial.printf("[RTC] Control register: 0x%02X (INTCN=1, A1IE=1)\n", ctrl);

  if (!writeAlarm1Exact(next)) {
    Serial.println("[RTC] Failed to write Alarm 1 registers");
    return false;
  }

  if (!verifyAlarmSet(next)) {
    Serial.println("[RTC] Alarm 1 read-back verification failed");
    return false;
  }

  if (!clearAlarmFlag()) {
    Serial.println("[RTC] Failed to clear alarm flag after verification");
    return false;
  }

  Serial.printf("[RTC] Alarm 1 armed for %04d-%02d-%02d %02d:%02d:%02d (phase-aligned, in %d sec)\n",
                next.year(), next.month(), next.day(), next.hour(), next.minute(), next.second(),
                (int)(nextSyncUnix - nowUnix));
  return true;
}

bool armDailyAlarm(int hour, int minute) {
  if (!gRTCInitialized) return false;

  DateTime now = gRTC.now();
  DateTime alarmTime(now.year(), now.month(), now.day(), hour, minute, 0);

  // If the alarm time has already passed today, schedule for tomorrow
  if (alarmTime.unixtime() <= now.unixtime()) {
    alarmTime = alarmTime + TimeSpan(1, 0, 0, 0);  // +1 day
  }

  // Enable alarm interrupt: INTCN=1, A1IE=1
  uint8_t ctrl = 0;
  if (!readReg(0x0E, ctrl)) return false;
  ctrl |= 0x04;  // INTCN=1
  ctrl |= 0x01;  // A1IE=1
  if (!writeReg(0x0E, ctrl)) return false;

  if (!writeAlarm1Exact(alarmTime)) {
    Serial.println("[RTC] Failed to write daily alarm registers");
    return false;
  }

  if (!verifyAlarmSet(alarmTime)) {
    Serial.println("[RTC] Daily alarm read-back verification failed");
    return false;
  }

  if (!clearAlarmFlag()) {
    Serial.println("[RTC] Failed to clear alarm flag after verification");
    return false;
  }

  Serial.printf("[RTC] Daily alarm armed for %02d:%02d\n", hour, minute);
  return true;
}

bool clearAlarmFlag() {
  uint8_t status = 0;
  if (!readReg(0x0F, status)) return false;
  status &= static_cast<uint8_t>(~0x03);  // Clear A1F and A2F
  bool ok = writeReg(0x0F, status);
  if (ok) {
    Serial.println("[RTC] Alarm flags cleared");
  }
  return ok;
}

bool readAlarmFlag() {
  uint8_t status = 0;
  if (!readReg(0x0F, status)) return false;
  return (status & 0x01) != 0;
}

void disableAlarmInterrupt() {
  uint8_t ctrl = 0;
  if (!readReg(0x0E, ctrl)) {
    Serial.println("[RTC] Failed to read control register for disable");
    return;
  }
  ctrl &= static_cast<uint8_t>(~0x01);  // Clear A1IE (disable Alarm 1 interrupt)
  ctrl |= 0x04;                          // Keep INTCN=1 (interrupt output mode)
  if (!writeReg(0x0E, ctrl)) {
    Serial.println("[RTC] Failed to write control register for disable");
    return;
  }
  Serial.println("[RTC] Alarm interrupt disabled (INT pin released)");
}

bool verifyAlarmSet() {
  // Read back alarm 1 registers and verify they are not all 0xFF (unset)
  uint8_t regs[4];
  if (!readAlarm1Registers(regs)) {
    Serial.println("[RTC] Failed to read alarm registers for verification");
    return false;
  }

  // Check that at least one register has a valid BCD value (not 0xFF)
  bool anyValid = false;
  for (int i = 0; i < 4; i++) {
    if ((regs[i] & 0x7F) != 0x7F) {  // Mask out A1Mx bit
      anyValid = true;
    }
  }

  // Also verify INTCN and A1IE are set in control register
  uint8_t ctrl = 0;
  if (!readReg(0x0E, ctrl)) {
    Serial.println("[RTC] Failed to read control register for verification");
    return false;
  }

  bool intcnSet = (ctrl & 0x04) != 0;
  bool a1ieSet = (ctrl & 0x01) != 0;

  if (!intcnSet || !a1ieSet) {
    Serial.printf("[RTC] Verification: INTCN=%d A1IE=%d (both must be 1)\n",
                  intcnSet, a1ieSet);
    return false;
  }

  if (!anyValid) {
    Serial.println("[RTC] Verification: alarm registers appear unset");
    return false;
  }

  Serial.println("[RTC] Alarm verification passed");
  return true;
}

bool verifyAlarmSet(const DateTime& expected) {
  uint8_t regs[4];
  if (!readAlarm1Registers(regs)) {
    Serial.println("[RTC] Failed to read alarm registers for verification");
    return false;
  }

  const uint8_t alarmSecond = fromBcd(regs[0] & 0x7F);
  const uint8_t alarmMinute = fromBcd(regs[1] & 0x7F);
  const uint8_t alarmHour = fromBcd(regs[2] & 0x3F);
  const uint8_t alarmDay = fromBcd(regs[3] & 0x3F);

  const bool maskBitsMatch =
      (regs[0] & 0x80) == 0 &&  // A1M1=0
      (regs[1] & 0x80) == 0 &&  // A1M2=0
      (regs[2] & 0x80) == 0 &&  // A1M3=0
      (regs[3] & 0x80) != 0;    // A1M4=1
  const bool hourMode24 = (regs[2] & 0x40) == 0;
  const bool decodedTimeMatches =
      alarmSecond == expected.second() &&
      alarmMinute == expected.minute() &&
      alarmHour == expected.hour();
  const bool bcdTimeMatches =
      (regs[0] & 0x7F) == (toBcd(static_cast<uint8_t>(expected.second())) & 0x7F) &&
      (regs[1] & 0x7F) == (toBcd(static_cast<uint8_t>(expected.minute())) & 0x7F) &&
      (regs[2] & 0x3F) == (toBcd(static_cast<uint8_t>(expected.hour())) & 0x3F);

  uint8_t ctrl = 0;
  if (!readReg(0x0E, ctrl)) {
    Serial.println("[RTC] Failed to read control register for verification");
    return false;
  }

  const bool intcnSet = (ctrl & 0x04) != 0;
  const bool a1ieSet = (ctrl & 0x01) != 0;
  if (!maskBitsMatch || !hourMode24 || !decodedTimeMatches || !bcdTimeMatches ||
      !intcnSet || !a1ieSet) {
    Serial.printf(
        "[RTC] Verification failed: alarm=%02u:%02u:%02u day=%u "
        "expected=%02u:%02u:%02u masks=%d mode24=%d INTCN=%d A1IE=%d\n",
        alarmHour, alarmMinute, alarmSecond, alarmDay,
        expected.hour(), expected.minute(), expected.second(),
        maskBitsMatch, hourMode24, intcnSet, a1ieSet);
    return false;
  }

  Serial.println("[RTC] Alarm time verification passed");
  return true;
}
