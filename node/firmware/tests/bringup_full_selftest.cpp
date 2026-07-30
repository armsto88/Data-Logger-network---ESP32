// bringup_full_selftest.cpp
//
// Combined, non-interactive node hardware self-test. Flashed by
// scripts/node_test_harness.sh, which parses the RESULT| lines below and only
// flashes production firmware (env:esp32wroom) if every required check passes.
//
// Unlike bringup_v3_main_systems.cpp (menu-driven, ultrasonic V3 board) this
// sketch runs once on boot, needs no operator keystrokes, and emits a
// machine-parseable verdict per subsystem:
//
//   RESULT|<NAME>|PASS|<details>
//   RESULT|<NAME>|FAIL|<details>
//   RESULT|<NAME>|WARN|<details>          informational, never blocks
//   RESULT|SUMMARY|<pass>/<pass+fail>|OVERALL:PASS      (or OVERALL:FAIL)
//
// Pins/addresses are inherited from env:esp32wroom build_flags (SDA=18,
// SCL=19, MUX=0x71, PWR_HOLD=23 active-high, BAT_ADC=35) so the self-test and
// the production build can never drift apart.
//
// The soil path deliberately reuses the PRODUCTION driver
// (src/drivers/ads1115_helper.cpp) and the production CWT TH-A conversion
// (src/sensors/sensors_soil_ads_calib.h) rather than a stand-in, so a pass
// here means the real code path works.
//
// Build/flash:  pio run -e esp32wroom-selftest -t upload --upload-port COMx
// Normal use:   scripts/node_test_harness.sh --port COMx

#include <Arduino.h>
#include <Wire.h>
#include <RTClib.h>
#include <Adafruit_SHT4x.h>
#include <Adafruit_AS7341.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <Preferences.h>
#include <nvs_flash.h>

#include "drivers/ads1115_helper.h"
#include "sensors/sensors_soil_ads_calib.h"

// ─────────────────────────────────────────────────────── config (build flags)

#ifndef RTC_SDA_PIN
#define RTC_SDA_PIN 18
#endif
#ifndef RTC_SCL_PIN
#define RTC_SCL_PIN 19
#endif
#ifndef MUX_ADDR
#define MUX_ADDR 0x71
#endif
#ifndef PWR_HOLD_PIN
#define PWR_HOLD_PIN 23
#endif
#ifndef PWR_HOLD_ACTIVE_HIGH
#define PWR_HOLD_ACTIVE_HIGH 1
#endif
#ifndef BAT_ADC_PIN
#define BAT_ADC_PIN 35
#endif
#ifndef BAT_ADC_SAMPLES
#define BAT_ADC_SAMPLES 16
#endif
#ifndef BAT_DIVIDER_SCALE
#define BAT_DIVIDER_SCALE 3.58f
#endif
#ifndef BAT_ADC_VREF
#define BAT_ADC_VREF 3.3f
#endif

// GPIO4 is the RX_EN_N net shared with the ultrasonic RX enable (see
// node/docs/NODE-PCB-OVERVIEW.md). It is NOT the DS3231 INT line — the RTC
// alarm drives the VSYS power-gate FET directly and never reaches a GPIO.
#ifndef REED_WIND_PIN
#define REED_WIND_PIN 4
#endif
#ifndef REED_WIND_DEBOUNCE_MS
#define REED_WIND_DEBOUNCE_MS 5
#endif
#ifndef SELFTEST_WIND_WINDOW_MS
#define SELFTEST_WIND_WINDOW_MS 10000
#endif

// Set by the harness (--skip-wind) for unattended runs where nobody is present
// to spin the anemometer cup.
#ifndef SELFTEST_SKIP_WIND
#define SELFTEST_SKIP_WIND 0
#endif

#ifndef SELFTEST_I2C_HZ
#define SELFTEST_I2C_HZ 100000
#endif

// Head start for the harness's serial monitor to attach after upload.
#ifndef SELFTEST_BOOT_SETTLE_MS
#define SELFTEST_BOOT_SETTLE_MS 6000
#endif

#ifndef RTC_I2C_ADDR
#define RTC_I2C_ADDR 0x68
#endif
#ifndef ADS_I2C_ADDR
#define ADS_I2C_ADDR 0x48
#endif
#define MUX_CH_SHT40  0
#define MUX_CH_AS7341 1
#define SHT40_I2C_ADDR  0x44
#define AS7341_I2C_ADDR 0x39

// DS3231 alarm round-trip budget. Alarm is armed +6 s out; poll a little
// longer to absorb the RTC's 1 s tick granularity.
#define SELFTEST_ALARM_INTERVAL_S 6
#define SELFTEST_ALARM_TIMEOUT_MS 12000

// ─────────────────────────────────────────────────────────────── result plumbing

static uint16_t g_pass = 0;
static uint16_t g_fail = 0;
static uint16_t g_warn = 0;

static void report(const char *name, const char *verdict, const char *fmt, ...) {
  char detail[192];
  va_list args;
  va_start(args, fmt);
  vsnprintf(detail, sizeof(detail), fmt, args);
  va_end(args);

  if (strcmp(verdict, "PASS") == 0)      g_pass++;
  else if (strcmp(verdict, "FAIL") == 0) g_fail++;
  else                                   g_warn++;

  Serial.printf("RESULT|%s|%s|%s\n", name, verdict, detail);
  Serial.flush();
}

// ───────────────────────────────────────────────────────────────── I2C helpers

static bool i2cPresent(uint8_t addr) {
  Wire.beginTransmission(addr);
  return Wire.endTransmission() == 0;
}

static bool muxSelect(uint8_t ch) {
  Wire.beginTransmission(MUX_ADDR);
  Wire.write((uint8_t)(1U << ch));
  return Wire.endTransmission() == 0;
}

static void muxDisableAll() {
  Wire.beginTransmission(MUX_ADDR);
  Wire.write((uint8_t)0x00);
  Wire.endTransmission();
}

static bool rtcReadReg(uint8_t reg, uint8_t &value) {
  Wire.beginTransmission(RTC_I2C_ADDR);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((int)RTC_I2C_ADDR, 1) != 1) return false;
  if (!Wire.available()) return false;
  value = Wire.read();
  return true;
}

static bool rtcWriteReg(uint8_t reg, uint8_t value) {
  Wire.beginTransmission(RTC_I2C_ADDR);
  Wire.write(reg);
  Wire.write(value);
  return Wire.endTransmission() == 0;
}

// ────────────────────────────────────────────────────────── 1. I2C bus + speed

// Required devices: without these the board cannot do its job at all.
static bool g_muxOk = false;
static bool g_adsPresent = false;

static void checkI2cBus() {
  uint8_t found[16];
  uint8_t nFound = 0;

  for (uint8_t addr = 0x01; addr < 0x7F; addr++) {
    if (i2cPresent(addr)) {
      if (nFound < sizeof(found)) found[nFound] = addr;
      nFound++;
    }
  }

  char list[96];
  int off = 0;
  for (uint8_t i = 0; i < nFound && i < sizeof(found); i++) {
    off += snprintf(list + off, sizeof(list) - off, "%s0x%02X",
                    i ? " " : "", found[i]);
    if (off >= (int)sizeof(list) - 6) break;
  }
  if (nFound == 0) snprintf(list, sizeof(list), "none");

  const bool rtcSeen = i2cPresent(RTC_I2C_ADDR);
  g_adsPresent       = i2cPresent(ADS_I2C_ADDR);
  g_muxOk            = i2cPresent(MUX_ADDR);

  // Measure effective bus throughput: 100 address probes. At 100 kHz an
  // address-only transaction is ~9 bits + overhead, so a healthy bus lands in
  // the low hundreds of microseconds each. A pulled-up-wrong or
  // clock-stretched bus shows up here as a large per-probe time.
  const uint32_t t0 = micros();
  for (uint8_t i = 0; i < 100; i++) (void)i2cPresent(RTC_I2C_ADDR);
  const uint32_t perProbeUs = (micros() - t0) / 100;

  if (rtcSeen && g_adsPresent && g_muxOk) {
    report("I2C_BUS", "PASS",
           "sda=%d scl=%d clk=%dHz devices=[%s] probe=%luus/txn "
           "(rtc0x%02X ads0x%02X mux0x%02X all present)",
           RTC_SDA_PIN, RTC_SCL_PIN, SELFTEST_I2C_HZ, list,
           (unsigned long)perProbeUs, RTC_I2C_ADDR, ADS_I2C_ADDR, MUX_ADDR);
  } else {
    report("I2C_BUS", "FAIL",
           "sda=%d scl=%d clk=%dHz devices=[%s] missing:%s%s%s",
           RTC_SDA_PIN, RTC_SCL_PIN, SELFTEST_I2C_HZ, list,
           rtcSeen ? "" : " RTC@0x68",
           g_adsPresent ? "" : " ADS@0x48",
           g_muxOk ? "" : " MUX@0x71");
  }
}

// ───────────────────────────────────────────────── 2. RTC presence + plausible time

static RTC_DS3231 g_rtc;
static bool g_rtcOk = false;

// A fresh node legitimately has no time on it: the mothership is the sole clock
// authority and sets the node at first sync. So an unset clock is NOT a board
// fault and must not block a production flash — otherwise every new board fails
// forever. What IS a fault is a DS3231 that will not answer, or one whose coin
// cell cannot hold time across the VSYS cut the node performs on every cycle.
//
// One run cannot tell "never set" from "cell is dead". So when the clock is
// unset we set it to a placeholder, clear the oscillator-stopped flag, and warn
// the operator to power-cycle and re-run: if the placeholder survives that, the
// cell is good and this check goes green.
static void checkRtcPresenceTime() {
  if (!g_rtc.begin(&Wire)) {
    report("RTC_PRESENCE_TIME", "FAIL", "DS3231 begin() failed at 0x%02X", RTC_I2C_ADDR);
    return;
  }
  g_rtcOk = true;

  const bool lostPower = g_rtc.lostPower();
  DateTime now = g_rtc.now();
  const uint16_t year = now.year();
  const bool plausible = (year >= 2024 && year <= 2035);

  if (plausible && !lostPower) {
    report("RTC_PRESENCE_TIME", "PASS",
           "%04u-%02u-%02u %02u:%02u:%02u temp=%.2fC lostPower=0 "
           "(oscillator held across power loss)",
           year, now.month(), now.day(), now.hour(), now.minute(), now.second(),
           g_rtc.getTemperature());
    return;
  }

  // Unset or oscillator-stopped: seed a placeholder so the next run can prove
  // retention. RTClib's adjust() also clears the OSF bit for us.
  const DateTime placeholder(F(__DATE__), F(__TIME__));
  g_rtc.adjust(placeholder);
  delay(10);

  const bool stillLost = g_rtc.lostPower();
  const DateTime after = g_rtc.now();

  if (stillLost || after.year() < 2024) {
    // Wrote the time and it did not stick — the chip answers but will not keep
    // time, so it can never fire the wake alarm in the field.
    report("RTC_PRESENCE_TIME", "FAIL",
           "clock will not hold a write: set %04u-%02u-%02u, read back "
           "%04u-%02u-%02u (lostPower=%d) - DS3231 or coin cell is faulty",
           placeholder.year(), placeholder.month(), placeholder.day(),
           after.year(), after.month(), after.day(), stillLost ? 1 : 0);
    return;
  }

  report("RTC_PRESENCE_TIME", "WARN",
         "clock was unset (was %04u-%02u-%02u lostPower=%d); seeded placeholder "
         "%04u-%02u-%02u %02u:%02u:%02u (LOCAL build time, NOT UTC - mothership "
         "sync must set real UTC). Power-cycle and re-run: if this survives, the "
         "coin cell is good",
         year, now.month(), now.day(), lostPower ? 1 : 0,
         after.year(), after.month(), after.day(),
         after.hour(), after.minute(), after.second());
}

// ────────────────────────────────────────────────────── 3. RTC alarm round-trip

static uint8_t toBcd(uint8_t v) { return (uint8_t)(((v / 10) << 4) | (v % 10)); }

static bool writeAlarm1Exact(const DateTime &t) {
  Wire.beginTransmission(RTC_I2C_ADDR);
  Wire.write(0x07);  // Alarm1 seconds register
  Wire.write(toBcd(t.second()) & 0x7F);
  Wire.write(toBcd(t.minute()) & 0x7F);
  Wire.write(toBcd(t.hour())   & 0x3F);
  Wire.write(toBcd(t.day())    & 0x3F);
  return Wire.endTransmission() == 0;
}

static bool clearAlarmFlags() {
  uint8_t status = 0;
  if (!rtcReadReg(0x0F, status)) return false;
  status &= (uint8_t)~0x03;  // clear A1F + A2F
  return rtcWriteReg(0x0F, status);
}

static bool alarm1Fired() {
  uint8_t status = 0;
  if (!rtcReadReg(0x0F, status)) return false;
  return (status & 0x01) != 0;
}

// The DS3231 INT/SQW line drives the VSYS power-gate FET, not a GPIO (see
// src/main.cpp — RTC_INT_PIN is a vestigial software misnomer). So this
// verifies the alarm over I2C only: arm it, watch A1F assert, clear it.
// That is exactly the path the node relies on to schedule its own wake.
static void checkRtcAlarmRoundtrip() {
  if (!g_rtcOk) {
    report("RTC_ALARM_ROUNDTRIP", "FAIL", "skipped - RTC did not initialise");
    return;
  }

  if (!clearAlarmFlags()) {
    report("RTC_ALARM_ROUNDTRIP", "FAIL", "could not clear A1F/A2F (ctrl/status write failed)");
    return;
  }

  // INTCN=1 + A1IE=1 — same bits the production wake path sets.
  uint8_t ctrl = 0;
  if (!rtcReadReg(0x0E, ctrl)) {
    report("RTC_ALARM_ROUNDTRIP", "FAIL", "control register read failed");
    return;
  }
  ctrl |= 0x04 | 0x01;
  if (!rtcWriteReg(0x0E, ctrl)) {
    report("RTC_ALARM_ROUNDTRIP", "FAIL", "control register write failed (INTCN/A1IE)");
    return;
  }

  // Not const: RTClib declares DateTime::operator+ non-const.
  DateTime armedAt = g_rtc.now();
  const DateTime target = armedAt + TimeSpan(0, 0, 0, SELFTEST_ALARM_INTERVAL_S);
  if (!writeAlarm1Exact(target)) {
    report("RTC_ALARM_ROUNDTRIP", "FAIL", "Alarm1 register write failed");
    return;
  }

  Serial.printf("[INFO] Alarm1 armed for +%ds, polling A1F...\n", SELFTEST_ALARM_INTERVAL_S);

  const uint32_t start = millis();
  bool fired = false;
  while (millis() - start < SELFTEST_ALARM_TIMEOUT_MS) {
    if (alarm1Fired()) { fired = true; break; }
    delay(25);
  }
  const uint32_t elapsedMs = millis() - start;

  if (!fired) {
    report("RTC_ALARM_ROUNDTRIP", "FAIL",
           "A1F never asserted within %dms - node would never wake itself",
           SELFTEST_ALARM_TIMEOUT_MS);
    return;
  }

  const bool cleared = clearAlarmFlags();
  if (!cleared) {
    report("RTC_ALARM_ROUNDTRIP", "FAIL",
           "A1F asserted after %lums but could not be cleared - node would re-wake in a loop",
           (unsigned long)elapsedMs);
    return;
  }

  report("RTC_ALARM_ROUNDTRIP", "PASS",
         "A1F asserted after %lums (armed +%ds) and cleared cleanly",
         (unsigned long)elapsedMs, SELFTEST_ALARM_INTERVAL_S);
}

// ────────────────────────────────────────────────────────────── 4. PWR_HOLD GPIO

#if PWR_HOLD_ACTIVE_HIGH
#define PWR_HOLD_ASSERT   HIGH
#define PWR_HOLD_RELEASE  LOW
#else
#define PWR_HOLD_ASSERT   LOW
#define PWR_HOLD_RELEASE  HIGH
#endif

// Smoke test only: proves the GPIO drives both rails cleanly. It deliberately
// does NOT hold the release long enough to actually collapse VSYS — confirming
// the FET gate really cuts power needs a DMM on VSYS and is a separate manual
// step (see bringup_pwr_hold_gate.cpp).
static void checkPwrHoldGpio() {
  pinMode(PWR_HOLD_PIN, OUTPUT);

  digitalWrite(PWR_HOLD_PIN, PWR_HOLD_ASSERT);
  delay(20);
  const int asserted = digitalRead(PWR_HOLD_PIN);

  digitalWrite(PWR_HOLD_PIN, PWR_HOLD_RELEASE);
  delay(300);
  const int released = digitalRead(PWR_HOLD_PIN);

  // Re-latch immediately — if we got this far the board is still alive.
  digitalWrite(PWR_HOLD_PIN, PWR_HOLD_ASSERT);
  delay(20);
  const int relatched = digitalRead(PWR_HOLD_PIN);

  if (asserted == PWR_HOLD_ASSERT && released == PWR_HOLD_RELEASE &&
      relatched == PWR_HOLD_ASSERT) {
    report("PWR_HOLD_GPIO", "PASS",
           "gpio%d active_high=%d drives both rails (assert=%d release=%d relatch=%d); "
           "GPIO-level only - VSYS cut needs a DMM",
           PWR_HOLD_PIN, PWR_HOLD_ACTIVE_HIGH, asserted, released, relatched);
  } else {
    report("PWR_HOLD_GPIO", "FAIL",
           "gpio%d readback wrong (assert=%d want=%d, release=%d want=%d, relatch=%d)",
           PWR_HOLD_PIN, asserted, PWR_HOLD_ASSERT, released, PWR_HOLD_RELEASE, relatched);
  }
}

// ───────────────────────────────────────────────────────── 5. SHT40 air temp/RH

static Adafruit_SHT4x g_sht4;

static void checkAirTempRh() {
  if (!g_muxOk) {
    report("AIR_TEMP_RH", "FAIL", "skipped - I2C mux 0x%02X absent", MUX_ADDR);
    return;
  }
  if (!muxSelect(MUX_CH_SHT40)) {
    report("AIR_TEMP_RH", "FAIL", "mux channel %d select failed", MUX_CH_SHT40);
    return;
  }
  delay(5);

  if (!g_sht4.begin(&Wire)) {
    report("AIR_TEMP_RH", "FAIL",
           "SHT40 not found at 0x%02X on mux ch%d", SHT40_I2C_ADDR, MUX_CH_SHT40);
    muxDisableAll();
    return;
  }
  g_sht4.setPrecision(SHT4X_HIGH_PRECISION);
  g_sht4.setHeater(SHT4X_NO_HEATER);

  sensors_event_t humidity, temp;
  const bool ok = g_sht4.getEvent(&humidity, &temp);
  muxDisableAll();

  if (!ok) {
    report("AIR_TEMP_RH", "FAIL", "SHT40 getEvent() failed on mux ch%d", MUX_CH_SHT40);
    return;
  }

  const float t  = temp.temperature;
  const float rh = humidity.relative_humidity;

  if (isnan(t) || isnan(rh) || t < -40.0f || t > 85.0f || rh < 0.0f || rh > 100.0f) {
    report("AIR_TEMP_RH", "FAIL",
           "reading out of range: air_temp=%.2fC air_rh=%.2f%% (valid -40..85C, 0..100%%)",
           t, rh);
    return;
  }

  report("AIR_TEMP_RH", "PASS", "air_temp=%.2fC air_rh=%.2f%% (SHT40 mux ch%d)",
         t, rh, MUX_CH_SHT40);
}

// ──────────────────────────────────────────────────────────── 6. AS7341 spectral

static Adafruit_AS7341 g_as7341;

static void checkSpectral() {
  if (!g_muxOk) {
    report("SPECTRAL", "FAIL", "skipped - I2C mux 0x%02X absent", MUX_ADDR);
    return;
  }
  if (!muxSelect(MUX_CH_AS7341)) {
    report("SPECTRAL", "FAIL", "mux channel %d select failed", MUX_CH_AS7341);
    return;
  }
  delay(5);

  if (!g_as7341.begin(AS7341_I2CADDR_DEFAULT, &Wire)) {
    report("SPECTRAL", "FAIL",
           "AS7341 begin() failed at 0x%02X on mux ch%d", AS7341_I2C_ADDR, MUX_CH_AS7341);
    muxDisableAll();
    return;
  }

  // Same acquisition config as bringup_sht40_as7343_mux.cpp / the production
  // backend's starting point.
  g_as7341.powerEnable(true);
  if (!g_as7341.setATIME(29) || !g_as7341.setASTEP(599) ||
      !g_as7341.setGain(AS7341_GAIN_4X) ||
      !g_as7341.enableSpectralMeasurement(true)) {
    report("SPECTRAL", "FAIL", "AS7341 acquisition config failed (ATIME/ASTEP/GAIN)");
    muxDisableAll();
    return;
  }

  const bool ok = g_as7341.readAllChannels();
  uint16_t f[8] = {0};
  uint16_t clear = 0, nir = 0;
  if (ok) {
    f[0] = g_as7341.getChannel(AS7341_CHANNEL_415nm_F1);
    f[1] = g_as7341.getChannel(AS7341_CHANNEL_445nm_F2);
    f[2] = g_as7341.getChannel(AS7341_CHANNEL_480nm_F3);
    f[3] = g_as7341.getChannel(AS7341_CHANNEL_515nm_F4);
    f[4] = g_as7341.getChannel(AS7341_CHANNEL_555nm_F5);
    f[5] = g_as7341.getChannel(AS7341_CHANNEL_590nm_F6);
    f[6] = g_as7341.getChannel(AS7341_CHANNEL_630nm_F7);
    f[7] = g_as7341.getChannel(AS7341_CHANNEL_680nm_F8);
    clear = g_as7341.getChannel(AS7341_CHANNEL_CLEAR);
    nir   = g_as7341.getChannel(AS7341_CHANNEL_NIR);
  }
  muxDisableAll();

  if (!ok) {
    report("SPECTRAL", "FAIL", "AS7341 readAllChannels() failed");
    return;
  }

  uint32_t sum = 0;
  for (uint8_t i = 0; i < 8; i++) sum += f[i];

  // All-zero across every band means the sensor answered on I2C but is not
  // actually converting (the failure mode chased in the AS7341 metadata bug).
  if (sum == 0 && clear == 0 && nir == 0) {
    report("SPECTRAL", "FAIL",
           "all channels read 0 - sensor present but not converting (cover/uncover to retest)");
    return;
  }

  report("SPECTRAL", "PASS",
         "F1-F8=[%u %u %u %u %u %u %u %u] clear=%u nir=%u sum=%lu (gain=4X mux ch%d)",
         f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7], clear, nir,
         (unsigned long)sum, MUX_CH_AS7341);
}

// ──────────────────────────────────────────────────────────── 7. Reed wind (GPIO4)

static volatile uint32_t g_windEdges = 0;
static volatile unsigned long g_windLastEdgeMs = 0;

static void IRAM_ATTR onWindFalling() {
  const unsigned long now = millis();
  if (now - g_windLastEdgeMs >= REED_WIND_DEBOUNCE_MS) {
    g_windLastEdgeMs = now;
    g_windEdges++;
  }
}

// Firmware cannot spin the cup, so this one is operator-gated by design: no
// rotation means no edges, and a silent PASS there would ship a node whose
// anemometer is dead.
static void checkWindReed() {
#if SELFTEST_SKIP_WIND
  report("WIND_REED", "WARN",
         "skipped (--skip-wind) - anemometer on gpio%d NOT verified", REED_WIND_PIN);
  return;
#else
  pinMode(REED_WIND_PIN, INPUT_PULLUP);
  noInterrupts();
  g_windEdges = 0;
  g_windLastEdgeMs = 0;
  interrupts();

  attachInterrupt(digitalPinToInterrupt(REED_WIND_PIN), onWindFalling, FALLING);

  Serial.println();
  Serial.println("========================================================");
  Serial.printf ("  >>> SPIN THE ANEMOMETER CUP NOW  (%d seconds) <<<\n",
                 SELFTEST_WIND_WINDOW_MS / 1000);
  Serial.println("========================================================");
  Serial.flush();

  const uint32_t start = millis();
  uint32_t lastTick = 0;
  while (millis() - start < SELFTEST_WIND_WINDOW_MS) {
    const uint32_t elapsed = millis() - start;
    if (elapsed / 1000 != lastTick) {
      lastTick = elapsed / 1000;
      noInterrupts();
      const uint32_t c = g_windEdges;
      interrupts();
      Serial.printf("[WIND] t=%lus edges=%lu\n",
                    (unsigned long)lastTick, (unsigned long)c);
      Serial.flush();
    }
    delay(20);
  }

  detachInterrupt(digitalPinToInterrupt(REED_WIND_PIN));

  noInterrupts();
  const uint32_t edges = g_windEdges;
  interrupts();

  const float windowS = SELFTEST_WIND_WINDOW_MS / 1000.0f;
  const float hz      = edges / windowS;
  const float mps     = hz * 0.6667f;  // WH-SP-WS01: 1 Hz = 2.4 km/h

  if (edges == 0) {
    report("WIND_REED", "FAIL",
           "0 edges in %.0fs on gpio%d - cup not spun, reed dead, or wiring open",
           windowS, REED_WIND_PIN);
    return;
  }

  report("WIND_REED", "PASS",
         "%lu edges in %.0fs on gpio%d = %.2fHz ~ %.2fm/s",
         (unsigned long)edges, windowS, REED_WIND_PIN, hz, mps);
#endif
}

// ────────────────────────────────────────────── 8. Soil 1/2 + volt1/volt2 (ADS1115)

static ADS1115 g_ads(Wire, ADS_I2C_ADDR);

static void checkSoilAndVolts() {
  if (!g_adsPresent) {
    report("SOIL_ADS", "FAIL", "ADS1115 absent at 0x%02X - soil + volt1/volt2 unavailable",
           ADS_I2C_ADDR);
    return;
  }
  if (!g_ads.begin()) {
    report("SOIL_ADS", "FAIL", "ADS1115 begin() failed at 0x%02X", ADS_I2C_ADDR);
    return;
  }

  // Production channel map (src/sensors/soil_moist_temp.cpp):
  //   A0 = SOIL1 temp, A1 = SOIL1 moisture (volt1),
  //   A2 = SOIL2 moisture (volt2), A3 = SOIL2 temp.
  int16_t raw[4];
  float   mv[4];
  bool    ok = true;
  for (uint8_t ch = 0; ch < 4; ch++) {
    if (!g_ads.readChannelMv(ch, raw[ch], mv[ch])) {
      ok = false;
      report("SOIL_ADS", "FAIL", "ADS1115 read failed on channel A%u", ch);
      return;
    }
  }
  (void)ok;

  const float v0 = (mv[0] / 1000.0f) * SOIL_ADC_INPUT_TO_SENSOR_VOLT_GAIN;
  const float v1 = (mv[1] / 1000.0f) * SOIL_ADC_INPUT_TO_SENSOR_VOLT_GAIN;
  const float v2 = (mv[2] / 1000.0f) * SOIL_ADC_INPUT_TO_SENSOR_VOLT_GAIN;
  const float v3 = (mv[3] / 1000.0f) * SOIL_ADC_INPUT_TO_SENSOR_VOLT_GAIN;

  const float soil1TempC = cwt_tha_temp_c_from_sensor_volts(v0);
  const float soil2TempC = cwt_tha_temp_c_from_sensor_volts(v3);

  report("SOIL_ADS", "PASS",
         "all 4 channels readable @0x%02X (A0=%.0fmV A1=%.0fmV A2=%.0fmV A3=%.0fmV)",
         ADS_I2C_ADDR, mv[0], mv[1], mv[2], mv[3]);

  report("SOIL1_TEMP", "PASS", "%.2fC (A0 sensorV=%.4f)", soil1TempC, v0);
  report("SOIL2_TEMP", "PASS", "%.2fC (A3 sensorV=%.4f)", soil2TempC, v3);

  // volt1/volt2 are the raw moisture-probe output volts the node ships in the
  // SOIL*_VWC channels — backend does the calibration. A disconnected probe is
  // a WARN, not a FAIL: it says nothing about the board itself.
  if (v1 < 0.05f) {
    report("VOLT1_SOIL1_MOIST", "WARN",
           "%.4fV on A1 near zero - SOIL1 probe unpowered or disconnected", v1);
  } else if (v1 > 5.25f) {
    report("VOLT1_SOIL1_MOIST", "WARN",
           "%.4fV on A1 above the 5V probe range - check gain/divider", v1);
  } else {
    report("VOLT1_SOIL1_MOIST", "PASS", "%.4fV on A1 (SOIL1 moisture output)", v1);
  }

  if (v2 < 0.05f) {
    report("VOLT2_SOIL2_MOIST", "WARN",
           "%.4fV on A2 near zero - SOIL2 probe unpowered or disconnected", v2);
  } else if (v2 > 5.25f) {
    report("VOLT2_SOIL2_MOIST", "WARN",
           "%.4fV on A2 above the 5V probe range - check gain/divider", v2);
  } else {
    report("VOLT2_SOIL2_MOIST", "PASS", "%.4fV on A2 (SOIL2 moisture output)", v2);
  }
}

// ─────────────────────────────────────────────────────────── 9. Battery ADC (GPIO35)

static void checkBatteryAdc() {
  analogReadResolution(12);
  analogSetPinAttenuation(BAT_ADC_PIN, ADC_11db);

  uint32_t sum = 0;
  uint16_t minRaw = 0xFFFF, maxRaw = 0;
  for (int i = 0; i < BAT_ADC_SAMPLES; i++) {
    const uint16_t r = (uint16_t)analogRead(BAT_ADC_PIN);
    sum += r;
    if (r < minRaw) minRaw = r;
    if (r > maxRaw) maxRaw = r;
    delay(2);
  }
  const uint16_t rawAvg = (uint16_t)(sum / BAT_ADC_SAMPLES);
  const float pinV = (rawAvg / 4095.0f) * BAT_ADC_VREF;
  const float batV = pinV * BAT_DIVIDER_SCALE;

  // A rail-pinned reading means the divider or the pin is broken, not just an
  // absent battery — that is a real board fault.
  if (rawAvg == 0) {
    report("BATTERY_ADC", "FAIL",
           "gpio%d reads 0 counts - divider open or ADC pin dead", BAT_ADC_PIN);
    return;
  }
  if (rawAvg >= 4090) {
    report("BATTERY_ADC", "FAIL",
           "gpio%d pinned at full scale (%u counts) - divider shorted or overvoltage",
           BAT_ADC_PIN, rawAvg);
    return;
  }

  // Bench runs are commonly USB-powered with no cell attached, so an
  // out-of-range voltage is informational only.
  if (batV < 2.5f || batV > 4.3f) {
    report("BATTERY_ADC", "WARN",
           "bat=%.3fV outside 2.5-4.3V (raw=%u pin=%.3fV scale=%.2f) - expected on bench power",
           batV, rawAvg, pinV, BAT_DIVIDER_SCALE);
    return;
  }

  report("BATTERY_ADC", "PASS",
         "bat=%.3fV (raw=%u spread=%u-%u pin=%.3fV scale=%.2f gpio%d)",
         batV, rawAvg, minRaw, maxRaw, pinV, BAT_DIVIDER_SCALE, BAT_ADC_PIN);
}

// ───────────────────────────────────────────────────────────────── 10. AUX1/AUX2

static void checkAux() {
  // src/sensors/sensors_aux_i2c.cpp is a stub backend today (count() == 0
  // unless AUX_I2C_ENABLE_STUB_SLOTS). Nothing to probe, so never block on it.
  report("AUX1", "WARN", "no backend wired (sensors_aux_i2c.cpp is a stub) - not verified");
  report("AUX2", "WARN", "no backend wired (sensors_aux_i2c.cpp is a stub) - not verified");
}

// ────────────────────────────────────────────────────────────── 11. ESP-NOW radio

static void checkEspNowRadio() {
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(false, false);
  delay(50);

  const String mac = WiFi.macAddress();

  const esp_err_t err = esp_now_init();
  if (err != ESP_OK) {
    report("ESPNOW_RADIO", "FAIL", "esp_now_init() failed err=0x%X mac=%s",
           (unsigned)err, mac.c_str());
    return;
  }

  uint8_t primaryCh = 0;
  wifi_second_chan_t secondCh;
  esp_wifi_get_channel(&primaryCh, &secondCh);

  esp_now_deinit();
  report("ESPNOW_RADIO", "PASS", "mac=%s channel=%u esp_now_init OK",
         mac.c_str(), primaryCh);
}

// ───────────────────────────────────────────────────────────── 12. NVS round-trip

static void checkNvsRoundtrip() {
  esp_err_t err = nvs_flash_init();
  if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    // Report rather than erase: wiping NVS here would destroy a real node's
    // pairing/config, which is the opposite of what a pre-flash check should do.
    report("NVS_ROUNDTRIP", "FAIL",
           "nvs_flash_init() needs an erase (err=0x%X) - partition full or version-mismatched",
           (unsigned)err);
    return;
  }
  if (err != ESP_OK) {
    report("NVS_ROUNDTRIP", "FAIL", "nvs_flash_init() failed err=0x%X", (unsigned)err);
    return;
  }

  // Dedicated scratch namespace — never touches production keys.
  Preferences prefs;
  if (!prefs.begin("selftest", false)) {
    report("NVS_ROUNDTRIP", "FAIL", "Preferences.begin(\"selftest\") failed");
    return;
  }

  const uint32_t token = (uint32_t)millis() ^ 0xA5A5F00DUL;
  prefs.putULong("tok", token);
  const uint32_t readBack = prefs.getULong("tok", 0);
  prefs.remove("tok");
  prefs.end();

  if (readBack != token) {
    report("NVS_ROUNDTRIP", "FAIL", "wrote 0x%08lX read 0x%08lX - flash not retaining writes",
           (unsigned long)token, (unsigned long)readBack);
    return;
  }

  report("NVS_ROUNDTRIP", "PASS", "write/read/erase of 0x%08lX in scratch namespace OK",
         (unsigned long)token);
}

// ──────────────────────────────────────────────────────────────────── setup/loop

void setup() {
  // Latch power before anything else — on a deployed board the FET gate is the
  // only thing keeping VSYS up once the RTC alarm pulse passes.
  pinMode(PWR_HOLD_PIN, OUTPUT);
  digitalWrite(PWR_HOLD_PIN, PWR_HOLD_ASSERT);

  Serial.begin(115200);

  // The harness only opens the serial monitor after `pio run -t upload`
  // returns, which lands a couple of seconds after the board has already
  // reset and started running this code. Wait it out before emitting the
  // first RESULT line — otherwise the early checks scroll past uncaptured and
  // the harness (correctly) rejects the run as a truncated capture.
  delay(SELFTEST_BOOT_SETTLE_MS);

  Serial.println();
  Serial.println("=== NODE FULL SELF-TEST ===");
  Serial.printf("fw_role=%s hw_target=%s\n", FW_ROLE, FW_HW_TARGET);
  Serial.printf("build=%s %s\n", __DATE__, __TIME__);
  Serial.printf("pins: sda=%d scl=%d pwr_hold=%d bat_adc=%d reed_wind=%d\n",
                RTC_SDA_PIN, RTC_SCL_PIN, PWR_HOLD_PIN, BAT_ADC_PIN, REED_WIND_PIN);
  Serial.println("---- checks ----");
  Serial.flush();

  Wire.begin(RTC_SDA_PIN, RTC_SCL_PIN);
  Wire.setClock(SELFTEST_I2C_HZ);
  muxDisableAll();

  checkI2cBus();
  checkRtcPresenceTime();
  checkRtcAlarmRoundtrip();
  checkPwrHoldGpio();
  checkAirTempRh();
  checkSpectral();
  checkWindReed();
  checkSoilAndVolts();
  checkBatteryAdc();
  checkAux();
  checkEspNowRadio();
  checkNvsRoundtrip();

  const uint16_t total = g_pass + g_fail;
  Serial.println("---- summary ----");
  Serial.printf("RESULT|SUMMARY|%u/%u|OVERALL:%s\n",
                g_pass, total, g_fail == 0 ? "PASS" : "FAIL");
  Serial.printf("(warnings: %u - informational, do not block)\n", g_warn);
  Serial.println("=== SELF-TEST COMPLETE ===");
  Serial.flush();
}

void loop() {
  // Self-test is one-shot. Idle here so the board stays powered and the
  // harness can finish draining the serial log.
  delay(1000);
}
