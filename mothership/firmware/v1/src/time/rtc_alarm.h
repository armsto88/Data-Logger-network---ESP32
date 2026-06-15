#pragma once

#include <Arduino.h>
#include <RTClib.h>

// DS3231 RTC alarm programming for Mothership V1.
// Manages I2C communication with the DS3231 and provides
// alarm programming for scheduled sync wakes.

bool initRTC();
uint32_t getRTCTime();
void setRTCTime(uint32_t unixTime);
bool armNextSyncAlarm(int intervalMin);
bool armDailyAlarm(int hour, int minute);
bool clearAlarmFlag();
bool readAlarmFlag();
bool verifyAlarmSet();