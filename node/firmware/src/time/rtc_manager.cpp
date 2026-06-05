#include "rtc_manager.h"
#include <Wire.h>

static const uint8_t DS3231_ADDR = 0x68;
static inline uint8_t toBCD(uint8_t v){ return (uint8_t)(((v/10)<<4) | (v%10)); }

static uint8_t ds3231Read(uint8_t reg){
  Wire.beginTransmission(DS3231_ADDR); Wire.write(reg);
  Wire.endTransmission(false);
  Wire.requestFrom((int)DS3231_ADDR, 1);
  return Wire.read();
}
static void ds3231Write(uint8_t reg, uint8_t v){
  Wire.beginTransmission(DS3231_ADDR); Wire.write(reg); Wire.write(v); Wire.endTransmission();
}

static void computeNextBoundary(const DateTime& now, uint8_t intervalMin, uint8_t &H, uint8_t &M, uint8_t &S){
  int total = now.hour()*60 + now.minute() + (now.second()>0 ? 1 : 0);
  int snapped = ((total + intervalMin - 1) / intervalMin) * intervalMin; // ceil
  snapped %= (24*60);
  H = snapped/60; M = snapped%60; S = 0;
}

bool setDS3231WakeInterval(int intervalMinutes, String &debugOut, RTC_DS3231 &rtcRef){
  switch (intervalMinutes){ case 1: case 5: case 10: case 20: case 30: case 60: break; default: intervalMinutes = 5; }

  DateTime now = rtcRef.now();
  uint8_t nh=0,nm=0,ns=0; computeNextBoundary(now, (uint8_t)intervalMinutes, nh,nm,ns);

  // Alarm1 regs 0x07..0x0A:
  // A1M1=0, A1M2=0, A1M3=0 (match ss, mm, hh)
  // A1M4=1 (bit7), DY/DT=0 (bit6) => daily HH:MM:SS (ignore date/day)
  ds3231Write(0x07, toBCD(ns));                  // sec
  ds3231Write(0x08, toBCD(nm));                  // min
  ds3231Write(0x09, toBCD(nh));                  // hour (24h)
  ds3231Write(0x0A, (1<<7) | (0<<6) | toBCD(1)); // A1M4=1, DY/DT=0, date=1

  // Control: INTCN=1 (bit2), A1IE=1 (bit0)
  uint8_t ctrl = ds3231Read(0x0E);
  ctrl |= (1<<2); // INTCN
  ctrl |= (1<<0); // A1IE
  ds3231Write(0x0E, ctrl);

  // Clear A1F
  uint8_t stat = ds3231Read(0x0F);
  stat &= ~(1<<0);
  ds3231Write(0x0F, stat);

  char buf[220];
  snprintf(buf, sizeof(buf),
    "Current RTC: %04d-%02d-%02d %02d:%02d:%02d\n"
    "Interval: %d min\nNext alarm (daily): %02u:%02u:%02u\n"
    "Alarm1 regs: %02X %02X %02X %02X\nCTRL: %02X (INTCN=1,A1IE=1)\nSTAT: %02X (A1F cleared)\n",
    now.year(), now.month(), now.day(), now.hour(), now.minute(), now.second(),
    intervalMinutes, nh,nm,ns,
    toBCD(ns), toBCD(nm), toBCD(nh), (1<<7)|(0<<6)|toBCD(1),
    ctrl, stat);
  debugOut += buf;
  return true;
}
