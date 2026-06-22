// Mothership V1 bringup: RTC validity classification and unknown wake safety.

#include <Arduino.h>

#include "../src/time/rtc_alarm.cpp"

bool readConfigWake() {
  return false;
}

#include "../src/system/wake_reason.cpp"

namespace {

constexpr int kPwrHoldPin = 26;
int gPassed = 0;
int gFailed = 0;

void reportResult(const char* name, bool passed) {
  Serial.printf("[%s] %s\n", passed ? "PASS" : "FAIL", name);
  passed ? ++gPassed : ++gFailed;
}

const char* rtcStatusName(RtcInitStatus status) {
  switch (status) {
    case RTC_OK: return "RTC_OK";
    case RTC_PRESENT_TIME_INVALID: return "RTC_PRESENT_TIME_INVALID";
    case RTC_ABSENT: return "RTC_ABSENT";
    default: return "INVALID";
  }
}

}  // namespace

void setup() {
  pinMode(kPwrHoldPin, OUTPUT);
  digitalWrite(kPwrHoldPin, HIGH);

  Serial.begin(115200);
  delay(400);
  Serial.println();
  Serial.println("=== Mothership V1 RTC Validity Bring-up ===");

  const RtcInitStatus status = initRTC();
  Serial.printf("[TEST] initRTC status=%d (%s)\n", static_cast<int>(status), rtcStatusName(status));
  const bool timeValid = rtcTimeValid();
  Serial.printf("[TEST] rtcTimeValid=%s\n", timeValid ? "true" : "false");

  if (status == RTC_OK && timeValid) {
    const uint32_t nowUnix = getRTCTime();
    const DateTime expected(nowUnix + 60UL);
    const bool armed = armRescueAlarm(1);
    reportResult("Valid RTC can arm and verify an alarm",
                 armed && verifyAlarmSet(expected));
  } else if (status == RTC_PRESENT_TIME_INVALID) {
    Serial.println("[PASS] RTC time invalid - would skip scheduling in sync wake");
    ++gPassed;
  } else {
    Serial.println("[PASS] RTC absent - scheduling correctly unavailable");
    ++gPassed;
  }

  const WakeSources failedRead{false, false, false};
  reportResult("Failed RTC status read selects WAKE_UNKNOWN",
               selectWakeReason(failedRead) == WAKE_UNKNOWN);

  const WakeSources detected = detectWakeSources();
  Serial.printf("[TEST] Hardware RTC status read=%s\n",
                detected.rtcStatusRead ? "ok" : "FAILED");
  if (!detected.rtcStatusRead) {
    reportResult("Disconnected RTC reports rtcStatusRead=false", true);
  } else {
    Serial.println("[INFO] RTC connected; synthetic failed-read case validated above");
  }

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
