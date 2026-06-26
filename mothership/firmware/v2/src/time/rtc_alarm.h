#pragma once

#include <Arduino.h>
#include <RTClib.h>

// DS3231 RTC alarm programming for Mothership V1.
// Manages I2C communication with the DS3231 and provides
// alarm programming for scheduled sync wakes.

enum RtcInitStatus {
  RTC_OK,
  RTC_PRESENT_TIME_INVALID,
  RTC_ABSENT
};

RtcInitStatus initRTC();
bool rtcTimeValid();
uint32_t getRTCTime();
void setRTCTime(uint32_t unixTime);
bool armRescueAlarm(int intervalMin);
bool armNextSyncAlarm(int intervalMin);
bool armNextSyncAlarmPhase(int intervalMin, uint32_t phaseUnix);
bool armDailyAlarm(int hour, int minute);
bool clearAlarmFlag();
void disableAlarmInterrupt();
bool readAlarmFlag();
bool verifyAlarmSet();
bool verifyAlarmSet(const DateTime& expected);
