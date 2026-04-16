#include "rtc_manager.h"
#include <Wire.h>
#include <RTClib.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include "../system/config.h"

RTC_DS3231 rtc;  // Using only RTClib like your working sketch
static SemaphoreHandle_t gRtcMutex = nullptr;
static unsigned long gLastGoodRtcUnix = 0;
static unsigned long gLastGoodRtcMs = 0;

static bool rtcLock(TickType_t ticks = pdMS_TO_TICKS(200)) {
    if (gRtcMutex == nullptr) return false;
    return xSemaphoreTake(gRtcMutex, ticks) == pdTRUE;
}

static void rtcUnlock() {
    if (gRtcMutex) xSemaphoreGive(gRtcMutex);
}

void setupRTC() {
    Serial.println("Starting RTC setup...");
    Wire.begin(RTC_SDA, RTC_SCL);
    Serial.printf("I2C initialized on SDA:%d, SCL:%d\n", RTC_SDA, RTC_SCL);

    if (!gRtcMutex) {
        gRtcMutex = xSemaphoreCreateMutex();
    }
    if (!gRtcMutex) {
        Serial.println("❌ RTC mutex init failed");
        return;
    }
    
    if (!rtcLock()) {
        Serial.println("❌ RTC lock failed during setup");
        return;
    }

    if (!rtc.begin()) {
        rtcUnlock();
        Serial.println("❌ Couldn't find RTC");
        return;
    }
    
    if (rtc.lostPower()) {
        Serial.println("RTC lost power, setting to compile time");
        rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
    }
    
    DateTime now = rtc.now();
    gLastGoodRtcUnix = now.unixtime();
    gLastGoodRtcMs = millis();
    rtcUnlock();
    Serial.printf("✅ RTC initialized: %04d-%02d-%02d %02d:%02d:%02d\n",
                 now.year(), now.month(), now.day(), now.hour(), now.minute(), now.second());
}

static unsigned long estimatedUnixFromCache() {
    if (gLastGoodRtcUnix == 0) return 0;
    const unsigned long elapsedSec = (millis() - gLastGoodRtcMs) / 1000UL;
    return gLastGoodRtcUnix + elapsedSec;
}

void getRTCTimeString(char* buffer, size_t bufferSize) {
    if (!rtcLock()) {
        snprintf(buffer, bufferSize, "1970-01-01 00:00:00");
        return;
    }
    DateTime now = rtc.now();
    rtcUnlock();
    snprintf(buffer, bufferSize, "%04d-%02d-%02d %02d:%02d:%02d",
            now.year(), now.month(), now.day(), now.hour(), now.minute(), now.second());
}

unsigned long getRTCTimeUnix() {
    if (!rtcLock()) {
        return estimatedUnixFromCache();
    }
    DateTime now = rtc.now();
    const unsigned long unixNow = now.unixtime();
    if (unixNow > 946684800UL) { // 2000-01-01 sanity floor
        gLastGoodRtcUnix = unixNow;
        gLastGoodRtcMs = millis();
    }
    rtcUnlock();
    return (unixNow > 946684800UL) ? unixNow : estimatedUnixFromCache();
}

bool setRTCTime(int year, int month, int day, int hour, int minute, int second) {
    Serial.printf("Setting RTC time to: %04d-%02d-%02d %02d:%02d:%02d\n", 
                  year, month, day, hour, minute, second);
    
    if (!rtcLock()) {
        Serial.println("❌ RTC lock failed during setRTCTime");
        return false;
    }

    DateTime newTime(year, month, day, hour, minute, second);
    rtc.adjust(newTime);
    
    // Verify the time was set
    delay(500);
    DateTime verify = rtc.now();
    gLastGoodRtcUnix = verify.unixtime();
    gLastGoodRtcMs = millis();
    rtcUnlock();
    Serial.printf("✅ RTC verification: %04d-%02d-%02d %02d:%02d:%02d\n",
                 verify.year(), verify.month(), verify.day(), verify.hour(), verify.minute(), verify.second());
    
    return true;
}

void resetRTCToDefault() {
    Serial.println("Resetting RTC to default time...");
    DateTime defaultTime(2025, 1, 1, 12, 0, 0);
    if (!rtcLock()) {
        Serial.println("❌ RTC lock failed during reset");
        return;
    }
    rtc.adjust(defaultTime);
    gLastGoodRtcUnix = defaultTime.unixtime();
    gLastGoodRtcMs = millis();
    rtcUnlock();
    Serial.println("✅ RTC reset to: 2025-01-01 12:00:00");
}