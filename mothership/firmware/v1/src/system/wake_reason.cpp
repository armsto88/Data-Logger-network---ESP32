#include "system/wake_reason.h"
#include "system/power.h"
#include "system/pins.h"
#include <Wire.h>
#include <RTClib.h>

static RTC_DS3231 gWakeRTC;
static bool gWakeRTCInitialized = false;

// DS3231 register access helpers (same pattern as node bringup)
static bool wakeReadReg(uint8_t reg, uint8_t& value) {
  Wire.beginTransmission(0x68);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  Wire.requestFrom(static_cast<uint8_t>(0x68), static_cast<uint8_t>(1));
  if (!Wire.available()) return false;
  value = Wire.read();
  return true;
}

static bool wakeReadAlarm1Flag(bool& alarmSet) {
  uint8_t status = 0;
  if (!wakeReadReg(0x0F, status)) return false;
  alarmSet = (status & 0x01) != 0;
  return true;
}

WakeSources detectWakeSources() {
  WakeSources sources{};
  sources.configRequested = readConfigWake();

  // Read the RTC even when config is requested so simultaneous wake sources
  // are not collapsed into an if/else decision.
  if (!gWakeRTCInitialized) {
    Wire.begin(PIN_SDA, PIN_SCL);
    Wire.setClock(100000);
    gWakeRTCInitialized = true;
  }
  sources.rtcStatusRead = wakeReadAlarm1Flag(sources.rtcAlarm);
  return sources;
}

WakeReason selectWakeReason(const WakeSources& sources) {
  if (sources.configRequested) return WAKE_CONFIG_BUTTON;
  if (sources.rtcAlarm) return WAKE_RTC_ALARM;
  return WAKE_USB_SERVICE;
}

WakeReason detectWakeReason() {
  return selectWakeReason(detectWakeSources());
}

const char* wakeReasonStr(WakeReason reason) {
  switch (reason) {
    case WAKE_RTC_ALARM:     return "RTC_ALARM";
    case WAKE_CONFIG_BUTTON: return "CONFIG_BUTTON";
    case WAKE_USB_SERVICE:   return "USB_SERVICE";
    case WAKE_UNKNOWN:        return "UNKNOWN";
    default:                  return "INVALID";
  }
}

void printWakeReason(WakeReason reason) {
  Serial.printf("[WAKE] Wake reason: %s\n", wakeReasonStr(reason));
  switch (reason) {
    case WAKE_RTC_ALARM:
      Serial.println("[WAKE] DS3231 Alarm 1 flag was set — scheduled sync wake");
      break;
    case WAKE_CONFIG_BUTTON:
      Serial.println("[WAKE] Config latch is set — button press wake");
      break;
    case WAKE_USB_SERVICE:
      Serial.println("[WAKE] No config or RTC alarm — USB service / debug wake");
      break;
    default:
      Serial.println("[WAKE] Could not determine wake source");
      break;
  }
}

void printWakeSources(const WakeSources& sources) {
  Serial.printf("[WAKE] Sources: config=%s rtc_alarm=%s rtc_read=%s\n",
                sources.configRequested ? "YES" : "no",
                sources.rtcAlarm ? "YES" : "no",
                sources.rtcStatusRead ? "ok" : "FAILED");
  if (sources.configRequested && sources.rtcAlarm) {
    Serial.println("[WAKE] Config and RTC are both active — config takes priority");
  }
}
