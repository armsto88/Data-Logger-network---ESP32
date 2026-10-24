#pragma once

#include <Arduino.h>
#include <RTClib.h>

// DS3231 RTC alarm programming for Mothership V1.
// Manages I2C communication with the DS3231 and provides
// alarm programming for scheduled sync wakes.

// Seconds before a sync slot that the hub wakes. Hub boot (storage init,
// upload-queue scan, purge) used to finish after the 10 s lead and eat the
// rendezvous; 45 s absorbs it in front of the slot (rendezvous design note,
// section 6.2 / Stage 0a). Clamped to half the sync period when armed.
#ifndef HUB_SYNC_PREWAKE_SEC
#define HUB_SYNC_PREWAKE_SEC 45
#endif

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
