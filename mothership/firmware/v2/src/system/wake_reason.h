#pragma once

#include <Arduino.h>

// Wake reason detection for Mothership V1.
// Determines why the board powered on so the main loop can take
// the appropriate action (sync, config, or service mode).

enum WakeReason {
  WAKE_RTC_ALARM,      // DS3231 Alarm 1 fired
  WAKE_CONFIG_BUTTON,  // Config latch is set (button press)
  WAKE_USB_SERVICE,    // USB VBUS present (service/debug)
  WAKE_UNKNOWN         // Cannot determine wake source
};

struct WakeSources {
  bool configRequested;
  bool rtcAlarm;
  bool rtcStatusRead;
};

// Capture wake inputs independently. Both configRequested and rtcAlarm may be
// true; selectWakeReason() gives the config request priority in that case.
WakeSources detectWakeSources();
WakeReason  selectWakeReason(const WakeSources& sources);
WakeReason detectWakeReason();
const char* wakeReasonStr(WakeReason reason);
void printWakeSources(const WakeSources& sources);
void printWakeReason(WakeReason reason);
