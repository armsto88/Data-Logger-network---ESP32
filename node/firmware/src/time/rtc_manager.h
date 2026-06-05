#pragma once
#include <RTClib.h>

// Programs DS3231 Alarm1 to the next N-minute boundary (daily HH:MM:SS),
// sets INTCN=1 and A1IE=1, clears A1F, and writes details into debugOut.
bool setDS3231WakeInterval(int intervalMinutes, String &debugOut, RTC_DS3231 &rtc);
