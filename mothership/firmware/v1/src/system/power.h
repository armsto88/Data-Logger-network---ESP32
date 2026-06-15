#pragma once

#include <Arduino.h>

// Power gating and control module for Mothership V1.
// Manages PWR_HOLD, config latch, battery ADC, and status LED.

void powerInit();
void assertPwrHold();
void releasePwrHold();
bool readConfigWake();
void clearConfigLatch();
float readBatteryVoltage();
void setLed(bool on);
void toggleLed();
bool isPwrHoldAsserted();