#include "rtc_manager.h"
#include <Wire.h>
#include <DS3232RTC.h>
#include "config.h"

DS3232RTC rtc;

void getRTCTimeString(char* buffer, size_t bufferSize) {
    tmElements_t tm;
    rtc.read(tm); // Read the RTC current time
    snprintf(buffer, bufferSize, "%04u-%02u-%02u %02u:%02u:%02u",
        tm.Year + 1970, tm.Month, tm.Day, tm.Hour, tm.Minute, tm.Second);
}

void setupRTC() {
    Wire.begin(RTC_SDA, RTC_SCL); // Start I2C on defined pins
    rtc.begin();

    Serial.println("RTC initialized. Time will only be set via user/web/NTP.");
}