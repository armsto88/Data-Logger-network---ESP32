// Mothership V1 bringup: boot rescue alarm and normal schedule replacement.
// Includes the production RTC alarm module so this dedicated environment
// exercises the same implementation as the main firmware.

#include <Arduino.h>

#include "../src/time/rtc_alarm.cpp"

namespace {

constexpr int kPwrHoldPin = 26;
constexpr int kRescueIntervalMin = 1;
constexpr int kSyncIntervalMin = 5;

int gPassed = 0;
int gFailed = 0;

void reportResult(const char* name, bool passed) {
  Serial.printf("[%s] %s\n", passed ? "PASS" : "FAIL", name);
  if (passed) {
    ++gPassed;
  } else {
    ++gFailed;
  }
}

void printTime(const char* prefix, const DateTime& time) {
  Serial.printf("%s%04d-%02d-%02d %02d:%02d:%02d\n",
                prefix, time.year(), time.month(), time.day(),
                time.hour(), time.minute(), time.second());
}

}  // namespace

void setup() {
  // CRITICAL: assert PWR_HOLD before initializing any other subsystem.
  pinMode(kPwrHoldPin, OUTPUT);
  digitalWrite(kPwrHoldPin, HIGH);

  const uint32_t bootStartMs = millis();
  Serial.begin(115200);
  delay(400);
  Serial.println();
  Serial.println("=== Mothership V1 Rescue Alarm Bring-up ===");

  const bool rtcReady = initRTC() != RTC_ABSENT;
  reportResult("RTC initialization", rtcReady);
  if (!rtcReady) {
    Serial.println("[FAIL] Cannot continue without the DS3231");
    return;
  }

  const uint32_t rescueBaseUnix = getRTCTime();
  const DateTime rescueExpected(rescueBaseUnix + kRescueIntervalMin * 60UL);
  const bool rescueArmed = armRescueAlarm(kRescueIntervalMin);
  reportResult("Arm one-minute rescue alarm", rescueArmed);
  reportResult("Rescue alarm verifies at expected time",
               rescueArmed && verifyAlarmSet(rescueExpected));
  reportResult("Rescue alarm armed within two seconds",
               rescueArmed && millis() - bootStartMs <= 2000UL);
  const int32_t rescueRemainingSec =
      static_cast<int32_t>(rescueExpected.unixtime() - getRTCTime());
  reportResult("Rescue alarm is approximately one minute ahead",
               rescueArmed && rescueRemainingSec >= 58 && rescueRemainingSec <= 60);
  printTime("[TEST] Rescue alarm time: ", rescueExpected);
  Serial.printf("[TEST] Rescue alarm remaining: %ld seconds\n",
                static_cast<long>(rescueRemainingSec));

  Serial.println("[TEST] Waiting 5 seconds before normal schedule replacement...");
  delay(5000);

  const uint32_t phaseUnix = rescueBaseUnix;
  const DateTime scheduleExpected(
      phaseUnix + static_cast<uint32_t>(kSyncIntervalMin) * 60UL - 10UL);
  const bool scheduleArmed = armNextSyncAlarmPhase(kSyncIntervalMin, phaseUnix);
  reportResult("Replace rescue alarm with phase-aligned schedule", scheduleArmed);
  reportResult("Replacement schedule verifies correctly",
               scheduleArmed && verifyAlarmSet(scheduleExpected));
  printTime("[TEST] Replacement alarm time: ", scheduleExpected);

  Serial.printf("=== RESULT: %s (%d passed, %d failed) ===\n",
                gFailed == 0 ? "PASS" : "FAIL", gPassed, gFailed);
}

void loop() {
  static uint32_t lastHeartbeatMs = 0;
  if (millis() - lastHeartbeatMs >= 5000) {
    lastHeartbeatMs = millis();
    Serial.printf("[IDLE] heartbeat - result remains %s\n",
                  gFailed == 0 ? "PASS" : "FAIL");
  }
  delay(50);
}
