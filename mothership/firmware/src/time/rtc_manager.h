#pragma once
#include <cstddef>  // For size_t

void setupRTC();
void getRTCTimeString(char* buffer, size_t bufferSize);
unsigned long getRTCTimeUnix();
bool setRTCTime(int year, int month, int day, int hour, int minute, int second);
void resetRTCToDefault();