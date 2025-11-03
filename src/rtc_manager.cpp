#include "rtc_manager.h"
#include <Wire.h>
#include <RTClib.h>
#include "config.h"

RTC_DS3231 rtc;  // Using only RTClib like your working sketch

void setupRTC() {
    Serial.println("Starting RTC setup...");
    Wire.begin(RTC_SDA, RTC_SCL);
    Serial.printf("I2C initialized on SDA:%d, SCL:%d\n", RTC_SDA, RTC_SCL);
    
    if (!rtc.begin()) {
        Serial.println("❌ Couldn't find RTC");
        return;
    }
    
    if (rtc.lostPower()) {
        Serial.println("RTC lost power, setting to compile time");
        rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
    }
    
    DateTime now = rtc.now();
    Serial.printf("✅ RTC initialized: %04d-%02d-%02d %02d:%02d:%02d\n",
                 now.year(), now.month(), now.day(), now.hour(), now.minute(), now.second());
}

void getRTCTimeString(char* buffer, size_t bufferSize) {
    DateTime now = rtc.now();
    snprintf(buffer, bufferSize, "%04d-%02d-%02d %02d:%02d:%02d",
            now.year(), now.month(), now.day(), now.hour(), now.minute(), now.second());
}

bool setRTCTime(int year, int month, int day, int hour, int minute, int second) {
    Serial.printf("Setting RTC time to: %04d-%02d-%02d %02d:%02d:%02d\n", 
                  year, month, day, hour, minute, second);
    
    DateTime newTime(year, month, day, hour, minute, second);
    rtc.adjust(newTime);
    
    // Verify the time was set
    delay(500);
    DateTime verify = rtc.now();
    Serial.printf("✅ RTC verification: %04d-%02d-%02d %02d:%02d:%02d\n",
                 verify.year(), verify.month(), verify.day(), verify.hour(), verify.minute(), verify.second());
    
    return true;
}

void resetRTCToDefault() {
    Serial.println("Resetting RTC to default time...");
    DateTime defaultTime(2025, 1, 1, 12, 0, 0);
    rtc.adjust(defaultTime);
    Serial.println("✅ RTC reset to: 2025-01-01 12:00:00");
}