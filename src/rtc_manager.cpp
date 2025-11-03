#include "rtc_manager.h"
#include <Wire.h>
#include <DS3232RTC.h>
#include "config.h"

DS3232RTC rtc;

void setupRTC() {
    Wire.begin(RTC_SDA, RTC_SCL); // Start I2C on defined pins
    rtc.begin();

    Serial.println("RTC initialized. Time will only be set via user/web/NTP.");
}

String getTimestamp() {
    tmElements_t tm;
    rtc.read(tm); // Read the RTC current time
    char buf[24];
    snprintf(buf, sizeof(buf), "%04u-%02u-%02u %02u:%02u:%02u",
        tm.Year + 1970, tm.Month, tm.Day, tm.Hour, tm.Minute, tm.Second);
    return String(buf);
}