// Mothership V1 bringup: Alarm 1 read-back verification and past-alarm guard.
// This test includes the production RTC alarm module so the dedicated
// build_src_filter compiles exactly the implementation used by main firmware.

#include <Arduino.h>

#include "../src/time/rtc_alarm.cpp"

namespace {

constexpr int kPwrHoldPin = 26;
constexpr int kIntervalMin = 1;
constexpr uint32_t kPeriodSec = kIntervalMin * 60UL;

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

}  // namespace

void setup() {
  // CRITICAL: assert PWR_HOLD before initializing any other subsystem.
  pinMode(kPwrHoldPin, OUTPUT);
  digitalWrite(kPwrHoldPin, HIGH);

  Serial.begin(115200);
  delay(400);
  Serial.println();
  Serial.println("=== Mothership V1 Alarm Verification Bring-up ===");

  const bool rtcReady = initRTC() != RTC_ABSENT;
  reportResult("RTC initialization", rtcReady);
  if (!rtcReady) {
    Serial.println("[FAIL] Cannot continue without the DS3231");
    return;
  }

  const uint32_t nowUnix = getRTCTime();
  const DateTime expected(nowUnix + kPeriodSec - 10UL);
  const bool armed = armNextSyncAlarmPhase(kIntervalMin, nowUnix);
  reportResult("Arm phase-aligned alarm one minute from anchor", armed);
  reportResult("Exact alarm read-back matches expected time",
               armed && verifyAlarmSet(expected));

  const uint8_t wrongMinute =
      toBcd(static_cast<uint8_t>((expected.minute() + 1) % 60));
  const bool corrupted = writeReg(0x08, wrongMinute);
  reportResult("Corrupt Alarm 1 minute register", corrupted);
  reportResult("Corrupted alarm is rejected",
               corrupted && !verifyAlarmSet(expected));

  const bool rearmed = armNextSyncAlarmPhase(kIntervalMin, nowUnix);
  reportResult("Re-arm alarm after corruption", rearmed);
  reportResult("Re-armed alarm verifies correctly",
               rearmed && verifyAlarmSet(expected));

  const uint32_t guardNowUnix = getRTCTime();
  const uint32_t nearFuturePhaseUnix = guardNowUnix + 5UL;
  const DateTime guardedExpected(nearFuturePhaseUnix - 10UL + kPeriodSec);
  const bool guardArmed =
      armNextSyncAlarmPhase(kIntervalMin, nearFuturePhaseUnix);
  reportResult("Arm alarm with near-future phase anchor", guardArmed);
  reportResult("Past-alarm guard advances one full period",
               guardArmed && verifyAlarmSet(guardedExpected));

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
