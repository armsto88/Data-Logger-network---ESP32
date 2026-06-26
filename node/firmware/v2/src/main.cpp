#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <Wire.h>
#include <RTClib.h>
#include <esp_wifi.h>
#include <esp_mac.h>
#include <esp_sleep.h>
#include <Preferences.h>
#include <nvs_flash.h>
#include <nvs.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include "esp_system.h"
#include "sensors.h"
#include "sensors/soil_moist_temp.h"
#include "storage/local_queue.h"
#include "storage/node_config_store.h"
#include "message_dispatch.h"
#include "node_event_queue.h"

#include "protocol.h"     // pins, ESPNOW_CHANNEL, protocol structs

#ifndef FW_BUILD
  #define FW_BUILD __DATE__ " " __TIME__
#endif

// -------------------- Node config --------------------
#ifndef NODE_ID
#define NODE_ID   "ENV_001"
#endif

#ifndef NODE_TYPE
#define NODE_TYPE "MULTI_ENV_V1"
#endif

#ifndef NODE_ID_AUTO_FROM_MAC
#define NODE_ID_AUTO_FROM_MAC 1
#endif

// Runtime node identity. By default it is generated from STA MAC so each board
// is unique without per-device firmware edits.
static char gNodeId[16] = NODE_ID;
#undef NODE_ID
#define NODE_ID gNodeId

static void initNodeIdentity() {
#if NODE_ID_AUTO_FROM_MAC
  uint8_t mac[6] = {0};
  if (esp_read_mac(mac, ESP_MAC_WIFI_STA) == ESP_OK) {
    snprintf(gNodeId, sizeof(gNodeId), "ENV_%02X%02X%02X", mac[3], mac[4], mac[5]);
  } else {
    strlcpy(gNodeId, "ENV_UNKNOWN", sizeof(gNodeId));
  }
#endif
}

// -------------------- I2C bus & mux --------------------
// Single I2C bus (I2C0) for RTC + PCA9548A + ADS1115
TwoWire WireRtc(0);

// Mux helper using config from protocol.h (MUX_ADDR, MUX_CHANNELS)
bool muxSelectChannel(uint8_t ch) {
  if (ch >= MUX_CHANNELS) return false;
  WireRtc.beginTransmission(MUX_ADDR);
  WireRtc.write(1 << ch);
  return (WireRtc.endTransmission() == 0);
}

// Hardware
RTC_DS3231 rtc;

#ifndef ENABLE_POWER_HOLD_CONTROL
#define ENABLE_POWER_HOLD_CONTROL 1
#endif

#ifndef PWR_HOLD_PIN
#define PWR_HOLD_PIN -1
#endif

#ifndef PWR_HOLD_ACTIVE_HIGH
#define PWR_HOLD_ACTIVE_HIGH 1
#endif

#ifndef PWR_HOLD_FALLBACK_SLEEP_DELAY_MS
#define PWR_HOLD_FALLBACK_SLEEP_DELAY_MS 1500UL
#endif

// Post-wake command window: keep ESP-NOW alive for N ms after HELLO to receive
// CONFIG_SNAPSHOT or other commands before the power cut. Set 0 to disable.
#ifndef POST_WAKE_WINDOW_MS
#define POST_WAKE_WINDOW_MS 1500
#endif

#ifndef NODE_EVENT_QUEUE_DEPTH
#define NODE_EVENT_QUEUE_DEPTH 12
#endif

#ifndef NODE_I2C_SCAN_ON_BOOT
#define NODE_I2C_SCAN_ON_BOOT 0
#endif

// Sync wake behavior (minute-resolution DS3231 Alarm2 windowing)
#ifndef SYNC_PRE_WAKE_SEC
#define SYNC_PRE_WAKE_SEC 0
#endif

#ifndef SYNC_LISTEN_WINDOW_MS
#define SYNC_LISTEN_WINDOW_MS 60000
#endif

#ifndef SYNC_MARKER_GRACE_SEC
#define SYNC_MARKER_GRACE_SEC 25UL
#endif

#ifndef SYNC_STALE_THRESHOLD_SEC
#define SYNC_STALE_THRESHOLD_SEC (24UL * 60UL * 60UL)
#endif

#ifndef SYNC_STALE_RECOVERY_WINDOW_MS
#define SYNC_STALE_RECOVERY_WINDOW_MS 12000UL
#endif

#if ENABLE_POWER_HOLD_CONTROL && (PWR_HOLD_PIN >= 0)
static const uint8_t kPwrHoldOnLevel  = PWR_HOLD_ACTIVE_HIGH ? HIGH : LOW;
static const uint8_t kPwrHoldOffLevel = PWR_HOLD_ACTIVE_HIGH ? LOW  : HIGH;
static bool gPowerCutRequested = false;
static uint32_t gPowerCutAtMs = 0;
#endif

enum class RecoveryReason : uint8_t {
  NONE = 0,
  RTC_ABSENT,
  RTC_LOST_POWER,
  ALARM_VERIFY_FAILED,
  CONFIG_PERSIST_FAILED,
  QUEUE_PERSIST_FAILED
};

static bool g_rtcReady = false;
static bool g_rtcPowerLost = false;
static RecoveryReason g_recoveryReason = RecoveryReason::NONE;
static bool g_alarmWakeVerified = false;
static bool g_eventApplyActive = false;
static uint32_t g_postWakeWindowUntilMs = 0;

static bool hasCriticalPendingWork();
static void serviceNodeEvents(uint32_t maxEvents = 8);
static bool bringupEspNow();
static bool ensureEspNowPeer(const uint8_t* mac);
static void initPowerHoldControl() {
#if ENABLE_POWER_HOLD_CONTROL && (PWR_HOLD_PIN >= 0)
  pinMode(PWR_HOLD_PIN, OUTPUT);
  digitalWrite(PWR_HOLD_PIN, kPwrHoldOnLevel);
  Serial.printf("[PWR_HOLD] enabled on pin=%d active=%s\n",
                PWR_HOLD_PIN,
                PWR_HOLD_ACTIVE_HIGH ? "HIGH" : "LOW");
#else
  Serial.println("[PWR_HOLD] disabled (set PWR_HOLD_PIN>=0 and ENABLE_POWER_HOLD_CONTROL=1 to use RTC power gating)");
#endif
}

static void assertPowerHold(const char* reason) {
#if ENABLE_POWER_HOLD_CONTROL && (PWR_HOLD_PIN >= 0)
  digitalWrite(PWR_HOLD_PIN, kPwrHoldOnLevel);
  Serial.printf("[PWR_HOLD] asserted (%s)\n", reason ? reason : "n/a");
#else
  (void)reason;
#endif
}

static void schedulePowerCut(const char* reason, uint32_t delayMs = 20) {
#if ENABLE_POWER_HOLD_CONTROL && (PWR_HOLD_PIN >= 0)
  gPowerCutRequested = true;
  gPowerCutAtMs = millis() + delayMs;
  Serial.printf("[PWR_HOLD] release scheduled in %lu ms (%s)\n",
                (unsigned long)delayMs,
                reason ? reason : "n/a");
#else
  (void)reason;
  (void)delayMs;
#endif
}

static void processPowerCut() {
#if ENABLE_POWER_HOLD_CONTROL && (PWR_HOLD_PIN >= 0)
  if (!gPowerCutRequested) return;

  const int32_t waitMs = (int32_t)(gPowerCutAtMs - millis());
  if (waitMs > 0) return;

  serviceNodeEvents(12);
  if (hasCriticalPendingWork()) {
    gPowerCutAtMs = millis() + 250UL;
    Serial.println("[PWR_HOLD] release deferred: critical node work still pending");
    return;
  }

  Serial.println("[PWR_HOLD] releasing hold now (expect power-off)");
  pinMode(PWR_HOLD_PIN, OUTPUT);
  digitalWrite(PWR_HOLD_PIN, kPwrHoldOffLevel);

  // Briefly sample the pin latch/readback for bench diagnostics.
  delay(2);
  Serial.printf("[PWR_HOLD] off-level written=%d readback=%d\n",
                (int)kPwrHoldOffLevel,
                (int)digitalRead(PWR_HOLD_PIN));

  // Prevent accidental float/leakback from re-enabling the gate path.
#if PWR_HOLD_ACTIVE_HIGH
  pinMode(PWR_HOLD_PIN, INPUT_PULLDOWN);
#else
  pinMode(PWR_HOLD_PIN, INPUT_PULLUP);
#endif

  delay(20);

  // If hardware hold-gate does not actually cut power, force deep sleep as a fallback.
  const uint32_t fallbackWaitStart = millis();
  while ((millis() - fallbackWaitStart) < (uint32_t)PWR_HOLD_FALLBACK_SLEEP_DELAY_MS) {
    delay(10);
  }

  Serial.println("[PWR_HOLD] still alive after hold release -> deep sleep fallback");
  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
#if defined(RTC_INT_PIN) && (RTC_INT_PIN >= 0)
  esp_err_t wakeCfg = esp_sleep_enable_ext0_wakeup((gpio_num_t)RTC_INT_PIN, 0);
  Serial.printf("[PWR_HOLD] deep sleep wake via RTC_INT pin=%d level=0: %s\n",
                RTC_INT_PIN,
                (wakeCfg == ESP_OK) ? "OK" : esp_err_to_name(wakeCfg));
#endif
  Serial.println("[PWR_HOLD] entering deep sleep now");
  esp_deep_sleep_start();

  while (true) {
    delay(1000);
  }
#endif
}

// ---- DS3231 helpers ----

static uint8_t readDS3231StatusReg() {
  WireRtc.beginTransmission(0x68);
  WireRtc.write(0x0F);                 // STATUS register
  WireRtc.endTransmission(false);
  WireRtc.requestFrom((uint8_t)0x68, (uint8_t)1);
  if (WireRtc.available()) return WireRtc.read();
  return 0xFF;
}

static bool readDS3231RegChecked(uint8_t reg, uint8_t& value) {
  WireRtc.beginTransmission(0x68);
  WireRtc.write(reg);
  if (WireRtc.endTransmission(false) != 0) return false;
  if (WireRtc.requestFrom((uint8_t)0x68, (uint8_t)1) != 1) return false;
  if (!WireRtc.available()) return false;
  value = WireRtc.read();
  return true;
}

static bool writeDS3231RegChecked(uint8_t reg, uint8_t value) {
  WireRtc.beginTransmission(0x68);
  WireRtc.write(reg);
  WireRtc.write(value);
  return WireRtc.endTransmission() == 0;
}

static bool readDS3231Block(uint8_t startReg, uint8_t* out, size_t len) {
  if (!out || len == 0) return false;
  WireRtc.beginTransmission(0x68);
  WireRtc.write(startReg);
  if (WireRtc.endTransmission(false) != 0) return false;
  if (WireRtc.requestFrom((uint8_t)0x68, (uint8_t)len) != (int)len) return false;
  for (size_t i = 0; i < len; ++i) {
    if (!WireRtc.available()) return false;
    out[i] = WireRtc.read();
  }
  return true;
}

static void writeDS3231StatusReg(uint8_t s) {
  WireRtc.beginTransmission(0x68);
  WireRtc.write(0x0F);
  WireRtc.write(s);
  WireRtc.endTransmission();
}

static inline void formatTime(const DateTime& dt, char* out, size_t n) {
  snprintf(out, n, "%04d-%02d-%02d %02d:%02d:%02d",
           dt.year(), dt.month(), dt.day(),
           dt.hour(), dt.minute(), dt.second());
}

static void initNVS()
{
  esp_err_t err = nvs_flash_init();
  if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    Serial.printf("[NVS] init err: %s → erasing NVS partition...\n", esp_err_to_name(err));
    ESP_ERROR_CHECK(nvs_flash_erase());
    err = nvs_flash_init();
  }
  if (err == ESP_OK) {
    Serial.println("[NVS] init OK");
  } else {
    Serial.printf("[NVS] init FAILED: %s\n", esp_err_to_name(err));
  }
}

// ---- DS3231 helpers ----

// Read DS3231 A1F (Alarm1 Flag). Returns 0 or 1, or 0xFF on I2C error.
static uint8_t readDS3231_A1F() {
  uint8_t s = readDS3231StatusReg();
  if (s != 0xFF) return (s & 0x01) ? 1 : 0;         // bit0 = A1F
  return 0xFF;
}

// Read DS3231 A2F (Alarm2 Flag). Returns 0 or 1, or 0xFF on I2C error.
static uint8_t readDS3231_A2F() {
  uint8_t s = readDS3231StatusReg();
  if (s != 0xFF) return (s & 0x02) ? 1 : 0;         // bit1 = A2F
  return 0xFF;
}

// Clear A1F (Alarm1 Flag) while preserving other status bits.
static void clearDS3231_A1F() {
  uint8_t s = readDS3231StatusReg();
  if (s == 0xFF) return;
  s &= (uint8_t)~0x01;
  writeDS3231StatusReg(s);
}

static void clearDS3231_A2F() {
  uint8_t s = readDS3231StatusReg();
  if (s == 0xFF) return;
  s &= (uint8_t)~0x02;
  writeDS3231StatusReg(s);
}

static void clearDS3231_AlarmFlags() {
  uint8_t s = readDS3231StatusReg();
  if (s == 0xFF) return;
  s &= (uint8_t)~0x03;
  writeDS3231StatusReg(s);
}

// Enable INTCN + A1IE (Alarm1 interrupt)
static void ds3231EnableAlarmInterrupt() {
  WireRtc.beginTransmission(0x68);
  WireRtc.write(0x0E);  // CTRL
  WireRtc.endTransmission(false);
  WireRtc.requestFrom((uint8_t)0x68, (uint8_t)1);
  uint8_t ctrl = WireRtc.available() ? WireRtc.read() : 0;

  ctrl |= 0b00000101;  // INTCN (bit2) + A1IE (bit0)

  WireRtc.beginTransmission(0x68);
  WireRtc.write(0x0E);
  WireRtc.write(ctrl);
  WireRtc.endTransmission();
}

static void ds3231EnableAlarm2Interrupt() {
  WireRtc.beginTransmission(0x68);
  WireRtc.write(0x0E);  // CTRL
  WireRtc.endTransmission(false);
  WireRtc.requestFrom((uint8_t)0x68, (uint8_t)1);
  uint8_t ctrl = WireRtc.available() ? WireRtc.read() : 0;

  ctrl |= 0b00000110;  // INTCN (bit2) + A2IE (bit1)

  WireRtc.beginTransmission(0x68);
  WireRtc.write(0x0E);
  WireRtc.write(ctrl);
  WireRtc.endTransmission();
}

// Disable A1 interrupt while leaving INTCN set.
static void ds3231DisableAlarmInterrupt() {
  WireRtc.beginTransmission(0x68);
  WireRtc.write(0x0E);  // CTRL
  WireRtc.endTransmission(false);
  WireRtc.requestFrom((uint8_t)0x68, (uint8_t)1);
  uint8_t ctrl = WireRtc.available() ? WireRtc.read() : 0;

  ctrl &= (uint8_t)~0b00000001;  // clear A1IE
  ctrl |= 0b00000100;            // keep INTCN

  WireRtc.beginTransmission(0x68);
  WireRtc.write(0x0E);
  WireRtc.write(ctrl);
  WireRtc.endTransmission();
}

static void ds3231DisableAlarm2Interrupt() {
  WireRtc.beginTransmission(0x68);
  WireRtc.write(0x0E);  // CTRL
  WireRtc.endTransmission(false);
  WireRtc.requestFrom((uint8_t)0x68, (uint8_t)1);
  uint8_t ctrl = WireRtc.available() ? WireRtc.read() : 0;

  ctrl &= (uint8_t)~0b00000010;  // clear A2IE
  ctrl |= 0b00000100;            // keep INTCN

  WireRtc.beginTransmission(0x68);
  WireRtc.write(0x0E);
  WireRtc.write(ctrl);
  WireRtc.endTransmission();
}

// Low-level: write DS3231 Alarm1 registers
static bool ds3231WriteA1(uint8_t secReg, uint8_t minReg, uint8_t hourReg, uint8_t dayReg) {
  WireRtc.beginTransmission(0x68);
  WireRtc.write(0x07);                // A1 seconds register
  WireRtc.write(secReg);
  WireRtc.write(minReg);
  WireRtc.write(hourReg);
  WireRtc.write(dayReg);
  return WireRtc.endTransmission() == 0;
}

static bool ds3231WriteA2(uint8_t minReg, uint8_t hourReg, uint8_t dayReg) {
  WireRtc.beginTransmission(0x68);
  WireRtc.write(0x0B);                // A2 minutes register
  WireRtc.write(minReg);
  WireRtc.write(hourReg);
  WireRtc.write(dayReg);
  return WireRtc.endTransmission() == 0;
}

// Program “every minute” (seconds == 00, ignore others)
struct AlarmBytes {
  uint8_t a1[4];
  uint8_t a2[3];
};

struct AlarmArmResult {
  bool alarm1Written = false;
  bool alarm1Verified = false;
  bool alarm2Written = false;
  bool alarm2Verified = false;
  bool controlVerified = false;
  bool flagsCleared = false;
  uint8_t controlReg = 0;
  uint8_t statusReg = 0xFF;
};

static bool verifyAlarm1Bytes(const uint8_t expected[4]) {
  uint8_t actual[4] = {0};
  return readDS3231Block(0x07, actual, sizeof(actual)) &&
         memcmp(actual, expected, sizeof(actual)) == 0;
}

static bool verifyAlarm2Bytes(const uint8_t expected[3]) {
  uint8_t actual[3] = {0};
  return readDS3231Block(0x0B, actual, sizeof(actual)) &&
         memcmp(actual, expected, sizeof(actual)) == 0;
}

static bool enableAndVerifyAlarmInterrupts(uint8_t& controlRegOut) {
  uint8_t ctrl = 0;
  if (!readDS3231RegChecked(0x0E, ctrl)) return false;
  ctrl |= 0b00000111;  // INTCN + A2IE + A1IE
  if (!writeDS3231RegChecked(0x0E, ctrl)) return false;
  if (!readDS3231RegChecked(0x0E, controlRegOut)) return false;
  return (controlRegOut & 0b00000111) == 0b00000111;
}

static bool clearAndVerifyAlarmFlags(uint8_t& statusRegOut) {
  uint8_t status = 0;
  if (!readDS3231RegChecked(0x0F, status)) return false;
  status &= (uint8_t)~0x03;
  if (!writeDS3231RegChecked(0x0F, status)) return false;
  if (!readDS3231RegChecked(0x0F, statusRegOut)) return false;
  return (statusRegOut & 0x03) == 0;
}

static bool ds3231EveryMinute() {
  // A1M1=0 (match seconds), seconds=00
  uint8_t A1SEC  = 0b00000000;               // 00 seconds, A1M1=0
  // A1M2=1 (ignore minutes), A1M3=1 (ignore hours), A1M4=1 (ignore day/date), DY/DT=0
  uint8_t A1MIN  = 0b10000000;               // ignore minutes
  uint8_t A1HOUR = 0b10000000;               // ignore hours
  uint8_t A1DAY  = 0b10000000;               // ignore day/date

  return ds3231WriteA1(A1SEC, A1MIN, A1HOUR, A1DAY);
}

// Compute next trigger as now + interval minutes (relative scheduling), program A1.
static bool ds3231ArmNextInNMinutes(uint8_t intervalMin,
                                    DateTime* nextOut = nullptr,
                                    AlarmBytes* bytesOut = nullptr) {
  if (intervalMin == 0) intervalMin = 1;

  DateTime now = rtc.now();
  DateTime next = now + TimeSpan(0, 0, intervalMin, 0);

  const uint8_t secBCD  = uint8_t(((next.second()/10)<<4) | (next.second()%10)); // A1M1=0
  const uint8_t minBCD  = uint8_t(((next.minute()/10)<<4) | (next.minute()%10)); // A1M2=0
  const uint8_t hourBCD = uint8_t(((next.hour()/10)<<4)   | (next.hour()%10));   // A1M3=0 (24h)
  const uint8_t dayReg  = 0b10000000; // A1M4=1 ignore day/date

  char buf[24]; formatTime(next, buf, sizeof(buf));
  Serial.printf("[A1] Next alarm in %u min at %s\n", intervalMin, buf);

  if (nextOut) *nextOut = next;
  if (bytesOut) {
    bytesOut->a1[0] = secBCD;
    bytesOut->a1[1] = minBCD;
    bytesOut->a1[2] = hourBCD;
    bytesOut->a1[3] = dayReg;
  }
  return ds3231WriteA1(secBCD, minBCD, hourBCD, dayReg);
}

static bool ds3231ArmSyncWake(uint16_t syncIntervalMin,
                              uint32_t syncPhaseUnix,
                              uint32_t preWakeSec,
                              DateTime* wakeOut = nullptr,
                              AlarmBytes* bytesOut = nullptr) {
  uint32_t nowUnix = rtc.now().unixtime();
  uint32_t nextSyncUnix = 0;

  if (syncIntervalMin == 0) {
    // Daily mode: syncPhaseUnix carries the mothership's daily HH:MM anchor.
    DateTime now(nowUnix);
    DateTime phase((syncPhaseUnix > 0) ? syncPhaseUnix : nowUnix);
    DateTime next(now.year(), now.month(), now.day(), phase.hour(), phase.minute(), 0);
    if (now.unixtime() >= next.unixtime()) {
      DateTime tomorrow(now.unixtime() + 24UL * 60UL * 60UL);
      next = DateTime(tomorrow.year(), tomorrow.month(), tomorrow.day(), phase.hour(), phase.minute(), 0);
    }
    nextSyncUnix = next.unixtime();
  } else {
    uint32_t periodSec = (uint32_t)syncIntervalMin * 60UL;
    uint32_t phase = (syncPhaseUnix > 0) ? syncPhaseUnix : nowUnix;

    // Alarm2 is minute-resolution. Keep phase minute-aligned so each sync slot
    // lands exactly on minute boundaries (e.g. 20:12:00, 20:17:00, ...).
    if ((phase % 60UL) != 0UL) {
      uint32_t oldPhase = phase;
      phase -= (phase % 60UL);
      Serial.printf("[A2] phase minute-aligned: %lu -> %lu\n",
                    (unsigned long)oldPhase,
                    (unsigned long)phase);
    }

    if (phase > nowUnix) {
      // syncPhaseUnix is the actual next fleet sync slot sent by the mothership
      // at deploy time. Arm directly for it; subsequent syncs will roll forward
      // from the anchor received in periodic SET_SYNC_SCHED broadcasts.
      nextSyncUnix = phase;
    } else {
      uint32_t slot = (nowUnix - phase) / periodSec;
      nextSyncUnix = phase + (slot + 1UL) * periodSec;
    }

    // Safety: DS3231 A2 is minute-resolution — if the computed slot boundary
    // has already passed (e.g. data-wake ran a few seconds past :00), arming
    // for that minute would miss it and not fire until the same time next day.
    // Advance one period so the alarm always lands in the future.
    if (nextSyncUnix <= nowUnix) {
      nextSyncUnix += periodSec;
      Serial.printf("[A2] sync slot already past, advancing one period -> %lu\n",
                    (unsigned long)nextSyncUnix);
    }
  }
  uint32_t wakeUnixRaw = (nextSyncUnix > preWakeSec) ? (nextSyncUnix - preWakeSec) : nextSyncUnix;

  // DS3231 Alarm2 is minute-resolution (no seconds register). If we pass a
  // wake time with seconds, programming minute/hour directly effectively floors
  // to :00 and can wake up to 59s too early. Round up to next minute instead.
  // Alarm2 is minute-resolution only, so use a deterministic programmed minute.
  // This avoids logging impossible second-level wake values (e.g. xx:yy:49).
  uint32_t wakeUnix = wakeUnixRaw;
  if ((wakeUnix % 60UL) != 0UL) {
    wakeUnix += 60UL - (wakeUnix % 60UL);
  }
  if (wakeUnix <= nowUnix) {
    if (syncIntervalMin == 0) {
      wakeUnix += 24UL * 60UL * 60UL;
    } else {
      wakeUnix += (uint32_t)syncIntervalMin * 60UL;
    }
  }
  DateTime wakeRaw(wakeUnixRaw);

  DateTime wake(wakeUnix);
  const uint8_t minBCD  = uint8_t(((wake.minute()/10)<<4) | (wake.minute()%10));
  const uint8_t hourBCD = uint8_t(((wake.hour()/10)<<4)   | (wake.hour()%10));
  const uint8_t dayReg  = 0b10000000; // ignore day/date

  char wakeStr[24]; formatTime(wake, wakeStr, sizeof(wakeStr));
  char wakeRawStr[24]; formatTime(wakeRaw, wakeRawStr, sizeof(wakeRawStr));
  char syncStr[24]; formatTime(DateTime(nextSyncUnix), syncStr, sizeof(syncStr));
  if (syncIntervalMin == 0) {
    Serial.printf("[A2] Daily sync wake armed for %s (raw %s, target sync %s, pre=%lus, minute-resolution)\n",
                  wakeStr, wakeRawStr, syncStr, (unsigned long)preWakeSec);
  } else {
    Serial.printf("[A2] Sync wake armed for %s (raw %s, target sync %s, pre=%lus, minute-resolution)\n",
                  wakeStr, wakeRawStr, syncStr, (unsigned long)preWakeSec);
  }

  if (wakeOut) *wakeOut = wake;
  if (bytesOut) {
    bytesOut->a2[0] = minBCD;
    bytesOut->a2[1] = hourBCD;
    bytesOut->a2[2] = dayReg;
  }
  return ds3231WriteA2(minBCD, hourBCD, dayReg);
}

// -------------------- Node state --------------------
enum NodeState {
  STATE_UNPAIRED = 0,   // no mothership MAC known
  STATE_PAIRED   = 1,   // has mothership MAC, but not deployed
  STATE_DEPLOYED = 2    // has mothership MAC + deployed flag set
};

static NodeState nodeState = STATE_UNPAIRED;

// -------------------- Persistent config (NVS-backed) --------------------
// Under hard power-cut sleep, RTC RAM does not survive. NVS is the source of truth.
int       bootCount        = 0;
bool      rtcSynced        = false;
uint8_t   mothershipMAC[6] = {0};
uint8_t   g_intervalMin    = 1;
bool      deployedFlag     = false;
// New: unix time of last successful TIME_SYNC (also mirrored into NVS)
uint32_t  lastTimeSyncUnix = 0;
uint16_t  g_syncIntervalMin = 15;
uint32_t  g_syncPhaseUnix = 0;
uint32_t  g_lastSyncSlot = 0xFFFFFFFFUL;
uint16_t  g_appliedConfigVersion = 0;

static bool g_rescueModeActive = false;
static uint32_t g_lastRescueBeaconMs = 0;

// Set when UNPAIR_NODE is received during an active sync wake cycle.
// Causes finalizeWakeAndSleep to skip the power cut so the UNPAIRED idle
// loop can keep the radio on for 15 minutes before auto power-off.
static bool g_postUnpairHold = false;

#ifndef RESCUE_BOOT_THRESHOLD
#define RESCUE_BOOT_THRESHOLD 3
#endif

#ifndef RESCUE_BOOT_WINDOW_SEC
#define RESCUE_BOOT_WINDOW_SEC 20UL
#endif

#ifndef RESCUE_BEACON_INTERVAL_MS
#define RESCUE_BEACON_INTERVAL_MS 5000UL
#endif

static bool g_espNowReady = false;
static volatile uint32_t g_syncWindowMarkerMs = 0;
static volatile bool g_lastSendDone = false;
static volatile esp_now_send_status_t g_lastSendStatus = ESP_NOW_SEND_FAIL;
static volatile bool g_sendActive = false;
static volatile bool g_sendCallbackReceived = false;
static volatile esp_now_send_status_t g_sendDeliveryStatus = ESP_NOW_SEND_FAIL;
static volatile uint32_t g_sendGeneration = 0;
static uint8_t g_sendExpectedMac[6] = {0};
static bool g_waitingSnapshotAck = false;
static uint32_t g_expectedSnapshotAckSeq = 0;
static bool g_snapshotAckMatched = false;
static bool g_snapshotAckPersisted = false;
static volatile bool g_deployBootstrapPending = false;
static volatile bool g_rearmAlarmsPending    = false;  // set by callbacks; serviced from loop
static uint32_t      g_lastAlarmArmMs         = 0;      // millis() of last successful armDeploymentWakeAlarms

// --- Pending actions set by ESP-NOW callback, serviced from main loop ---
// The ESP-NOW receive callback runs on the Wi-Fi task and must not touch I2C
// (Wire/RTC) or NVS (Preferences) because those libraries are not thread-safe.
// Instead, the callback copies inbound data into these buffers and sets a flag;
// the main loop drains them from main-task context.
static volatile bool g_pendingTimeSync = false;
static time_sync_response_t g_pendingTimeSyncData;
static volatile bool g_pendingPairNode = false;
static uint8_t g_pendingPairNodeMac[6];
static volatile bool g_pendingPairingResponse = false;
static pairing_response_t g_pendingPairingResponseData;
static uint8_t g_pendingPairingResponseMac[6];
static volatile bool g_pendingUnpair = false;
static volatile bool g_pendingDeploy = false;
static deployment_command_t g_pendingDeployData;
static uint8_t g_pendingDeployMac[6];
static volatile bool g_pendingDeployAck = false;
static uint8_t g_pendingDeployAckMac[6];
static volatile bool g_pendingConfigSnapshot = false;
static config_snapshot_message_t g_pendingConfigSnapshotData;
static uint8_t g_pendingConfigSnapshotMac[6];
static volatile bool g_pendingPersistConfig = false;

static bool waitForSendDelivery(uint32_t timeoutMs) {
  const uint32_t start = millis();
  while ((millis() - start) < timeoutMs) {
    serviceNodeEvents(4);
    if (g_lastSendDone) {
      return g_lastSendStatus == ESP_NOW_SEND_SUCCESS;
    }
    delay(1);
  }
  return false;
}

struct SendResult {
  esp_err_t queueResult = ESP_FAIL;
  bool callbackReceived = false;
  esp_now_send_status_t deliveryStatus = ESP_NOW_SEND_FAIL;
};

static bool isBroadcastMac(const uint8_t* mac) {
  if (!mac) return false;
  for (int i = 0; i < 6; ++i) {
    if (mac[i] != 0xFF) return false;
  }
  return true;
}

static SendResult sendEspNowAndWait(const uint8_t* destination,
                                    const void* payload,
                                    size_t payloadLength,
                                    uint32_t timeoutMs) {
  SendResult result{};
  if (!destination || !payload || payloadLength == 0) {
    result.queueResult = ESP_ERR_INVALID_ARG;
    return result;
  }

  const uint32_t busyStart = millis();
  while (g_sendActive && (uint32_t)(millis() - busyStart) < timeoutMs) {
    serviceNodeEvents(4);
    delay(1);
  }
  if (g_sendActive) {
    result.queueResult = ESP_ERR_INVALID_STATE;
    return result;
  }

  if (!bringupEspNow()) {
    result.queueResult = ESP_ERR_ESPNOW_NOT_INIT;
    return result;
  }
  if (!ensureEspNowPeer(destination)) {
    result.queueResult = ESP_ERR_ESPNOW_NOT_FOUND;
    return result;
  }

  memcpy(g_sendExpectedMac, destination, sizeof(g_sendExpectedMac));
  g_sendCallbackReceived = false;
  g_sendDeliveryStatus = ESP_NOW_SEND_FAIL;
  g_lastSendDone = false;
  g_lastSendStatus = ESP_NOW_SEND_FAIL;
  ++g_sendGeneration;
  g_sendActive = true;

  esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);
  result.queueResult = esp_now_send(destination,
                                    reinterpret_cast<const uint8_t*>(payload),
                                    payloadLength);
  if (result.queueResult != ESP_OK) {
    g_sendActive = false;
    ++g_sendGeneration;
    return result;
  }

  const uint32_t start = millis();
  while ((uint32_t)(millis() - start) < timeoutMs) {
    serviceNodeEvents(4);
    if (g_sendCallbackReceived) {
      result.callbackReceived = true;
      result.deliveryStatus = g_sendDeliveryStatus;
      g_sendActive = false;
      return result;
    }
    delay(1);
  }

  g_sendActive = false;
  ++g_sendGeneration;
  result.callbackReceived = false;
  result.deliveryStatus = ESP_NOW_SEND_FAIL;
  return result;
}

// --- Derived state helpers ---
static bool hasMothershipMAC() {
  for (int i = 0; i < 6; ++i) {
    if (mothershipMAC[i] != 0) return true;
  }
  return false;
}

static NodeState currentNodeState() {
  if (!hasMothershipMAC()) return STATE_UNPAIRED;
  if (!deployedFlag)       return STATE_PAIRED;   // bound but not deployed
  return STATE_DEPLOYED;                          // bound + deployed
}

static void debugState(const char* where) {
  NodeState s = currentNodeState();
  Serial.print("[STATE] ");
  Serial.print(where);
  Serial.print(" hasMS=");
  Serial.print(hasMothershipMAC());
  Serial.print(" rtcSynced=");
  Serial.print(rtcSynced);
  Serial.print(" deployedFlag=");
  Serial.print(deployedFlag);
  Serial.print(" -> ");
  if (s == STATE_UNPAIRED)      Serial.println("UNPAIRED");
  else if (s == STATE_PAIRED)   Serial.println("PAIRED");
  else                          Serial.println("DEPLOYED");
}

// ---------- Persistent config (NVS) ----------
static bool persistNodeConfig(uint16_t candidateConfigVersion = 0,
                              bool useCandidateConfigVersion = false) {
  nodeState = currentNodeState();
  NodeConfigStoreRecord record{};
  memcpy(record.mothershipMac, mothershipMAC, sizeof(record.mothershipMac));
  record.state = static_cast<uint8_t>(nodeState);
  record.rtcSynced = rtcSynced;
  record.deployed = deployedFlag;
  record.rtcPowerLost = g_rtcPowerLost;
  record.recoveryReason = static_cast<uint8_t>(g_recoveryReason);
  record.wakeIntervalMin = g_intervalMin;
  record.syncIntervalMin = g_syncIntervalMin;
  record.syncPhaseUnix = g_syncPhaseUnix;
  record.lastTimeSyncUnix = lastTimeSyncUnix;
  record.lastSyncSlot = g_lastSyncSlot;
  record.appliedConfigVersion =
      useCandidateConfigVersion ? candidateConfigVersion : g_appliedConfigVersion;

  const bool ok = nodeConfigStoreSave(record);
  Serial.printf("[CFG] %s state=%u deployed=%d rtcSynced=%d cfgV=%u wake=%u sync=%u phase=%lu\n",
                ok ? "committed" : "commit FAILED",
                (unsigned)record.state, record.deployed ? 1 : 0,
                record.rtcSynced ? 1 : 0,
                (unsigned)record.appliedConfigVersion,
                (unsigned)record.wakeIntervalMin,
                (unsigned)record.syncIntervalMin,
                (unsigned long)record.syncPhaseUnix);
  return ok;
}

static void loadNodeConfig() {
  NodeConfigStoreRecord record{};
  NodeConfigLoadStatus loadStatus = NodeConfigLoadStatus::NoValidConfig;
  if (!nodeConfigStoreLoad(record, &loadStatus)) {
    Serial.println("[CFG] no valid config record; using defaults");
    nodeState        = STATE_UNPAIRED;
    rtcSynced        = false;
    deployedFlag     = false;
    g_intervalMin    = 1;
    lastTimeSyncUnix = 0;
    g_syncIntervalMin = 15;
    g_syncPhaseUnix = 0;
    g_lastSyncSlot = 0xFFFFFFFFUL;
    g_appliedConfigVersion = 0;
    memset(mothershipMAC, 0, sizeof(mothershipMAC));
    return;
  }

  memcpy(mothershipMAC, record.mothershipMac, sizeof(mothershipMAC));
  nodeState = record.state <= (uint8_t)STATE_DEPLOYED
      ? (NodeState)record.state
      : STATE_UNPAIRED;
  rtcSynced = record.rtcSynced;
  deployedFlag = record.deployed;
  g_rtcPowerLost = record.rtcPowerLost;
  g_recoveryReason = (RecoveryReason)record.recoveryReason;
  g_intervalMin = record.wakeIntervalMin > 0 ? record.wakeIntervalMin : 1;
  g_syncIntervalMin = record.syncIntervalMin;
  g_syncPhaseUnix = record.syncPhaseUnix;
  lastTimeSyncUnix = record.lastTimeSyncUnix;
  g_lastSyncSlot = record.lastSyncSlot;
  g_appliedConfigVersion = record.appliedConfigVersion;

  Serial.printf("[CFG] loaded status=%u state=%u rtcSynced=%d deployed=%d interval=%u syncMin=%u syncPhase=%lu syncSlot=%lu cfgV=%u\n",
                (unsigned)loadStatus,
                (unsigned)nodeState, rtcSynced, deployedFlag, g_intervalMin,
                (unsigned)g_syncIntervalMin, (unsigned long)g_syncPhaseUnix,
                (unsigned long)g_lastSyncSlot, (unsigned)g_appliedConfigVersion);

  if (lastTimeSyncUnix > 0) {
    DateTime ls(lastTimeSyncUnix);
    char buf[24]; formatTime(ls, buf, sizeof(buf));
    Serial.printf("   ↪ lastTimeSyncUnix=%lu (%s)\n",
                  (unsigned long)lastTimeSyncUnix, buf);
  } else {
    Serial.println("   ↪ lastTimeSyncUnix=0 (no previous TIME_SYNC recorded)");
  }
}

// -------------------- ESP-NOW --------------------
static void onDataSent(const uint8_t *mac_addr, esp_now_send_status_t status);
static void onDataReceived(const uint8_t *mac, const uint8_t *incomingData, int len);
static void sendTimeSyncRequest();
static void sendDiscoveryRequest();
static void sendPairingRequest();
static void sendNodeStatusUpdate(const char* reason = nullptr);
static void sendNodeHello();
static uint16_t getNodeConfigVersion();
static void setNodeConfigVersion(uint16_t v);
static bool updateRescueBootStreak(uint32_t nowUnix);
static void wipeNodeConfigToUnpaired();
static void enterRescueMode();
static bool bringupEspNow();
static bool ensureEspNowPeer(const uint8_t* mac);
static void shutdownEspNow();
static bool shouldSyncAt(uint32_t unixNow);
static void captureSensorsToQueue();
static void flushQueuedToMothership(uint32_t deadlineMs = 0);
static void runStaleSyncRecoveryIfNeeded();
static bool armDeploymentWakeAlarms(DateTime* nextDataOut = nullptr, DateTime* nextSyncOut = nullptr);
static void finalizeWakeAndSleep(const char* reason);

// Guard against stale g_espNowReady state by retrying once after a forced re-init.
static esp_err_t espnowSendWithRecover(const uint8_t* mac, const uint8_t* payload, size_t len) {
  if (!bringupEspNow()) return ESP_ERR_ESPNOW_NOT_INIT;

  if (!ensureEspNowPeer(mac)) return ESP_ERR_ESPNOW_NOT_FOUND;

  esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);
  esp_err_t res = esp_now_send(mac, payload, len);
  if (res != ESP_ERR_ESPNOW_NOT_INIT && res != ESP_ERR_ESPNOW_IF) return res;

  Serial.printf("⚠️ ESP-NOW send error (%s); forcing re-init and retrying once\n",
                esp_err_to_name(res));
  g_espNowReady = false;
  esp_now_deinit();
  WiFi.mode(WIFI_OFF);
  delay(40);

  if (!bringupEspNow()) return ESP_ERR_ESPNOW_NOT_INIT;
  if (!ensureEspNowPeer(mac)) return ESP_ERR_ESPNOW_NOT_FOUND;
  esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);
  return esp_now_send(mac, payload, len);
}

static bool ensureEspNowPeer(const uint8_t* mac) {
  if (!mac) return false;

  esp_now_peer_info_t pi{};
  memcpy(pi.peer_addr, mac, 6);
  pi.channel = ESPNOW_CHANNEL;
  pi.ifidx   = WIFI_IF_STA;
  pi.encrypt = false;

  esp_now_del_peer(mac);
  esp_err_t r = esp_now_add_peer(&pi);
  if (r != ESP_OK && r != ESP_ERR_ESPNOW_EXIST) {
    Serial.printf("⚠️ add peer failed for %02X:%02X:%02X:%02X:%02X:%02X: %s\n",
                  mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
                  esp_err_to_name(r));
    return false;
  }
  return true;
}

static bool bringupEspNow() {
  if (g_espNowReady) return true;

  WiFi.mode(WIFI_OFF);
  delay(20);
  if (!WiFi.mode(WIFI_STA)) {
    Serial.println("❌ WiFi STA mode set failed");
    return false;
  }
  WiFi.disconnect();
  delay(80);

  if (esp_now_init() != ESP_OK) {
    Serial.println("❌ ESP-NOW init failed");
    return false;
  }

  esp_now_register_send_cb(onDataSent);
  esp_now_register_recv_cb(onDataReceived);

  // Broadcast peer for fleet-level commands.
  {
    esp_now_peer_info_t pi{};
    static const uint8_t bcast[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
    memcpy(pi.peer_addr, bcast, 6);
    pi.channel = ESPNOW_CHANNEL;
    pi.ifidx   = WIFI_IF_STA;
    pi.encrypt = false;
    esp_err_t r = esp_now_add_peer(&pi);
    if (r != ESP_OK && r != ESP_ERR_ESPNOW_EXIST) {
      Serial.printf("⚠️ add broadcast peer failed: %s\n", esp_err_to_name(r));
    }
  }

  if (hasMothershipMAC()) {
    esp_now_peer_info_t pi{};
    memcpy(pi.peer_addr, mothershipMAC, 6);
    pi.channel = ESPNOW_CHANNEL;
    pi.ifidx   = WIFI_IF_STA;
    pi.encrypt = false;
    esp_now_del_peer(mothershipMAC);
    esp_err_t r = esp_now_add_peer(&pi);
    if (r != ESP_OK && r != ESP_ERR_ESPNOW_EXIST) {
      Serial.printf("⚠️ add mothership peer failed: %s\n", esp_err_to_name(r));
    }
  }

  esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);
  g_espNowReady = true;
  Serial.println("✅ ESP-NOW online");
  return true;
}

static void shutdownEspNow() {
  if (!g_espNowReady) return;
  if (g_sendActive) {
    Serial.println("[ESP-NOW] shutdown deferred: send active");
    return;
  }
  esp_now_deinit();
  WiFi.mode(WIFI_OFF);
  g_espNowReady = false;
  Serial.println("📴 WiFi/ESP-NOW off");
}

static bool shouldSyncAt(uint32_t unixNow) {
  if (!rtcSynced || g_syncIntervalMin == 0) return false;

  const uint32_t period = (uint32_t)g_syncIntervalMin * 60UL;
  uint32_t phase = g_syncPhaseUnix;
  if (phase == 0 || phase > unixNow) phase = 0;

  uint32_t slot = (unixNow - phase) / period;
  if (slot == g_lastSyncSlot) return false;

  g_lastSyncSlot = slot;
  persistNodeConfig();
  return true;
}

static uint32_t nextSyncSlotUnix(uint32_t nowUnix) {
  if (g_syncIntervalMin == 0) {
    DateTime now(nowUnix);
    DateTime phase((g_syncPhaseUnix > 0) ? g_syncPhaseUnix : nowUnix);
    DateTime target(now.year(), now.month(), now.day(), phase.hour(), phase.minute(), 0);
    return target.unixtime();
  }

  const uint32_t periodSec = (uint32_t)g_syncIntervalMin * 60UL;
  uint32_t phase = g_syncPhaseUnix;
  if (phase == 0) phase = nowUnix;
  if (phase > nowUnix) return phase;

  // For an A2-driven sync wake we want the current slot boundary, not the next one.
  const uint32_t slot = (nowUnix - phase) / periodSec;
  return phase + slot * periodSec;
}

// V2 key-value snapshot capture.
// Builds a node_snapshot_v2_t header + v2_reading_t[] array from the sensor
// registry and battery ADC. Does NOT enqueue yet (local_queue::enqueueV2() is
// Phase 2c). Logs the V2 data via Serial for verification.
static void captureSensorsToQueue() {
  v2_reading_t readings[MAX_READINGS_PER_SNAPSHOT];
  size_t count = 0;

  // Battery voltage — dedicated ADC read (same as V1 path).
#ifdef BAT_ADC_PIN
  {
    analogReadResolution(12);
    analogSetPinAttenuation(BAT_ADC_PIN, ADC_11db);
    uint32_t raw_sum = 0;
    for (int i = 0; i < BAT_ADC_SAMPLES; ++i) {
      raw_sum += analogRead(BAT_ADC_PIN);
      delayMicroseconds(500);
    }
    const uint16_t raw_avg = (uint16_t)(raw_sum / BAT_ADC_SAMPLES);
    const float pin_v      = (static_cast<float>(raw_avg) / 4095.0f) * 3.3f;
    const float bat_v      = pin_v * BAT_DIVIDER_SCALE;

    if (count < MAX_READINGS_PER_SNAPSHOT) {
      readings[count].sensorId = SENSOR_ID_BAT_V;
      readings[count].value    = bat_v;
      ++count;
    }
    Serial.printf("[BAT-V2] raw=%u pin_v=%.3fV bat_v=%.3fV\n", raw_avg, pin_v, bat_v);
  }
#endif

  // Build readings from the sensor registry (skips SENSOR_ID_UNKNOWN).
  size_t sensorReadings = buildReadingsArray(&readings[count],
                                             MAX_READINGS_PER_SNAPSHOT - count);
  count += sensorReadings;

  // Assemble V2 header.
  node_snapshot_v2_t snap2{};
  strncpy(snap2.command, "NODE_SNAPSHOT2", sizeof(snap2.command) - 1);
  strncpy(snap2.nodeId,  NODE_ID,         sizeof(snap2.nodeId)  - 1);
  snap2.nodeTimestamp   = rtc.now().unixtime();
  snap2.seqNum          = local_queue::nextSeq();
  snap2.sensorCount     = (uint16_t)count;
  snap2.qualityFlags    = 0;
  snap2.configVersion   = 0; // filled by flush from NVS in Phase 2c
  snap2.protocolVersion = NODE_PROTOCOL_VERSION;
  snap2.reserved        = 0;

  // Log V2 data for verification.
  Serial.printf("🧾 V2 snapshot seq=%lu sensorCount=%u (header=%uB + body=%uB = %uB)\n",
                (unsigned long)snap2.seqNum,
                (unsigned)count,
                (unsigned)sizeof(node_snapshot_v2_t),
                (unsigned)(count * sizeof(v2_reading_t)),
                (unsigned)(sizeof(node_snapshot_v2_t) + count * sizeof(v2_reading_t)));
  for (size_t i = 0; i < count; ++i) {
    Serial.printf("   [%02u] id=%-5u value=%.4f\n",
                  (unsigned)i,
                  (unsigned)readings[i].sensorId,
                  readings[i].value);
  }

  // Enqueue the V2 snapshot into the local queue (Phase 2c).
  if (local_queue::enqueueV2(snap2, readings, count)) {
    Serial.printf("🧾 V2 snapshot enqueued (seq=%lu sensorCount=%u)\n",
                  (unsigned long)snap2.seqNum, (unsigned)count);
  } else {
    Serial.println("❌ V2 snapshot enqueue failed");
  }
}

static void flushQueuedToMothership(uint32_t deadlineMs) {
  if (!hasMothershipMAC()) {
    Serial.println("⚠️ flush skipped: no mothership MAC");
    return;
  }

  if (!bringupEspNow()) {
    Serial.println("❌ flush skipped: ESP-NOW bringup failed");
    return;
  }

  size_t sent = 0;
  while (local_queue::count() > 0) {
    if (deadlineMs != 0) {
      int32_t msLeft = (int32_t)(deadlineMs - millis());
      if (msLeft <= 0) {
        Serial.println("⏱️ flush deadline reached; keeping remaining queue for next sync window");
        break;
      }
      if (msLeft < 250) {
        Serial.printf("⏱️ flush stopping with %ldms left; preserving queue\n", (long)msLeft);
        break;
      }
    }

    // Peek the oldest V2 record as raw wire bytes.
    // Static keeps ~250B off the stack; single-threaded flush path.
    static uint8_t snapBuf[2 + sizeof(node_snapshot_v2_t) +
                           MAX_READINGS_PER_SNAPSHOT * sizeof(v2_reading_t)];
    size_t snapLen = 0;
    if (!local_queue::peekV2(snapBuf, sizeof(snapBuf), snapLen)) break;

    // Parse the V2 header from the buffer for ACK tracking and config tagging.
    node_snapshot_v2_t* snap2 = reinterpret_cast<node_snapshot_v2_t*>(snapBuf);
    const uint32_t seqNum = snap2->seqNum;

    // Tag with current config version at send time so mothership can track it.
    snap2->configVersion = (uint16_t)getNodeConfigVersion();

    g_waitingSnapshotAck = false;
    g_snapshotAckMatched = false;
    g_snapshotAckPersisted = false;
    g_expectedSnapshotAckSeq = seqNum;

    SendResult send = sendEspNowAndWait(mothershipMAC, snapBuf, snapLen, 250);
    if (send.queueResult != ESP_OK) {
      Serial.printf("❌ queue flush send failed at seq=%lu: %s\n",
                    (unsigned long)seqNum,
                    esp_err_to_name(send.queueResult));
      break;
    }

    if (!send.callbackReceived || send.deliveryStatus != ESP_NOW_SEND_SUCCESS) {
      Serial.printf("❌ queue flush delivery not confirmed at seq=%lu (status=%d)\n",
                    (unsigned long)seqNum,
                    (int)send.deliveryStatus);
      break;
    }

#if NODE_REQUIRE_DURABLE_SNAPSHOT_ACK
    g_waitingSnapshotAck = true;
    const uint32_t ackStart = millis();
    while ((uint32_t)(millis() - ackStart) < 750UL) {
      serviceNodeEvents(8);
      if (g_snapshotAckMatched) break;
      delay(5);
    }
    g_waitingSnapshotAck = false;
    if (!g_snapshotAckMatched || !g_snapshotAckPersisted) {
      Serial.printf("[ACK] durable SNAPSHOT_ACK missing for seq=%lu; retaining queue head\n",
                    (unsigned long)seqNum);
      break;
    }
#else
    Serial.println("[ACK] legacy mode: popping after verified ESP-NOW link delivery");
#endif

    if (!local_queue::pop()) {
      Serial.println("❌ queue pop failed after send");
      break;
    }
    sent++;
    delay(5);
  }

  Serial.printf("📤 queue flush done: sent=%u pending=%u\n",
                (unsigned)sent,
                (unsigned)local_queue::count());

  if (currentNodeState() == STATE_DEPLOYED) {
    shutdownEspNow();
  }
}

static void onDataSent(const uint8_t *mac_addr, esp_now_send_status_t status) {
  if (!mac_addr) return;
  if (!g_sendActive) return;
  if (memcmp(mac_addr, g_sendExpectedMac, 6) != 0) {
    return;
  }
  g_sendDeliveryStatus = status;
  g_sendCallbackReceived = true;
  g_lastSendStatus = status;
  g_lastSendDone = true;
}
static void onDataReceived(const uint8_t *mac, const uint8_t *incomingData, int len) {
  if (!mac || !incomingData || len <= 0) {
    noteInvalidNodePacket();
    return;
  }

  const IncomingMessageType type =
      classifyIncomingMessage(incomingData, static_cast<size_t>(len));
  if (type == IncomingMessageType::INVALID ||
      !incomingMessageTextFieldsTerminated(type, incomingData, static_cast<size_t>(len)) ||
      !incomingMessageHasValidTarget(type, incomingData, static_cast<size_t>(len), NODE_ID)) {
    noteInvalidNodePacket();
    return;
  }

  const bool operational =
      type == IncomingMessageType::DEPLOY_NODE ||
      type == IncomingMessageType::UNPAIR_NODE ||
      type == IncomingMessageType::SET_SCHEDULE ||
      type == IncomingMessageType::SET_SYNC_SCHED ||
      type == IncomingMessageType::SYNC_WINDOW_OPEN ||
      type == IncomingMessageType::TIME_SYNC ||
      type == IncomingMessageType::CONFIG_SNAPSHOT ||
      type == IncomingMessageType::SNAPSHOT_ACK;

  if (operational && hasMothershipMAC() && memcmp(mac, mothershipMAC, 6) != 0) {
    noteInvalidNodePacket();
    return;
  }

  if (operational && !hasMothershipMAC() &&
      type != IncomingMessageType::DEPLOY_NODE) {
    noteInvalidNodePacket();
    return;
  }

  enqueueValidatedNodeEvent(mac, type, incomingData,
                            static_cast<size_t>(len), millis());
  return;
}

static bool validWakeInterval(uint32_t minutes) {
  switch (minutes) {
    case 1: case 5: case 10: case 20: case 30: case 60:
      return true;
    default:
      return false;
  }
}

static bool plausibleUnix(uint32_t unixTime) {
  return unixTime >= 1704067200UL && unixTime <= 2145916800UL;  // 2024-01-01 .. 2038-01-01
}

static bool validDateTimeParts(uint32_t year, uint32_t month, uint32_t day,
                               uint32_t hour, uint32_t minute, uint32_t second) {
  if (year < 2024 || year > 2037) return false;
  if (month < 1 || month > 12) return false;
  if (day < 1 || day > 31) return false;
  if (hour > 23 || minute > 59 || second > 59) return false;
  return true;
}

static bool validSyncScheduleValue(uint32_t syncIntervalMin, uint32_t phaseUnix) {
  if (syncIntervalMin == 0) {
    return plausibleUnix(phaseUnix);
  }
  if (syncIntervalMin > 24UL * 60UL) return false;
  if (phaseUnix != 0 && !plausibleUnix(phaseUnix)) return false;
  return true;
}

static bool hasCriticalPendingWork() {
  if (g_eventApplyActive || nodeEventsPending()) return true;
  if (g_pendingTimeSync || g_pendingPairNode || g_pendingPairingResponse ||
      g_pendingUnpair || g_pendingDeploy || g_pendingDeployAck ||
      g_pendingConfigSnapshot || g_pendingPersistConfig ||
      g_deployBootstrapPending || g_rearmAlarmsPending) {
    return true;
  }
  if (g_sendActive || g_waitingSnapshotAck) return true;
  if ((int32_t)(g_postWakeWindowUntilMs - millis()) > 0) return true;
  if (currentNodeState() == STATE_DEPLOYED && rtcSynced && !g_alarmWakeVerified) return true;
  return false;
}

static void serviceNodeEvents(uint32_t maxEvents) {
  if (g_eventApplyActive) return;
  g_eventApplyActive = true;

  uint32_t serviced = 0;
  NodeEvent ev{};
  while (serviced < maxEvents && popNodeEvent(ev)) {
    serviced++;

    switch (ev.type) {
      case NodeEventType::DISCOVERY_RESPONSE:
        Serial.printf("[EVENT] DISCOVER_RESPONSE from %02X:%02X:%02X:%02X:%02X:%02X ignored for binding; explicit PAIR required\n",
                      ev.senderMac[0], ev.senderMac[1], ev.senderMac[2],
                      ev.senderMac[3], ev.senderMac[4], ev.senderMac[5]);
        break;

      case NodeEventType::DISCOVERY_SCAN:
        Serial.println("[EVENT] DISCOVERY_SCAN -> sending discovery request");
        sendDiscoveryRequest();
        break;

      case NodeEventType::PAIRING_RESPONSE:
        if (ev.payload.pairingResponse.isPaired) {
          memcpy(&g_pendingPairingResponseData, &ev.payload.pairingResponse,
                 sizeof(g_pendingPairingResponseData));
          memcpy(g_pendingPairingResponseMac, ev.senderMac, 6);
          g_pendingPairingResponse = true;
        }
        break;

      case NodeEventType::PAIR_NODE:
        memcpy(g_pendingPairNodeMac, ev.senderMac, 6);
        g_pendingPairNode = true;
        break;

      case NodeEventType::DEPLOY_NODE:
        if (!validDateTimeParts(ev.payload.deploy.year, ev.payload.deploy.month,
                                ev.payload.deploy.day, ev.payload.deploy.hour,
                                ev.payload.deploy.minute, ev.payload.deploy.second) ||
            !validWakeInterval(ev.payload.deploy.wakeIntervalMin) ||
            !validSyncScheduleValue(ev.payload.deploy.syncIntervalMin,
                                    ev.payload.deploy.syncPhaseUnix)) {
          Serial.println("[EVENT] DEPLOY_NODE rejected: invalid time/schedule");
          break;
        }
        memcpy(&g_pendingDeployData, &ev.payload.deploy, sizeof(g_pendingDeployData));
        memcpy(g_pendingDeployMac, ev.senderMac, 6);
        memcpy(g_pendingDeployAckMac, ev.senderMac, 6);
        g_pendingDeploy = true;
        g_pendingDeployAck = true;
        break;

      case NodeEventType::UNPAIR_NODE:
        g_pendingUnpair = true;
        break;

      case NodeEventType::SET_SCHEDULE: {
        const int requested = ev.payload.schedule.intervalMinutes;
        if (requested < 0 || !validWakeInterval((uint32_t)requested)) {
          Serial.printf("[EVENT] SET_SCHEDULE rejected: interval=%d\n", requested);
          break;
        }
        const uint8_t newInterval = (uint8_t)requested;
        if (newInterval != g_intervalMin) {
          Serial.printf("[EVENT] SET_SCHEDULE wake interval %u -> %u\n",
                        (unsigned)g_intervalMin, (unsigned)newInterval);
          g_intervalMin = newInterval;
          g_pendingPersistConfig = true;
          if (currentNodeState() == STATE_DEPLOYED && rtcSynced && !g_deployBootstrapPending) {
            g_rearmAlarmsPending = true;
          }
        }
        break;
      }

      case NodeEventType::SET_SYNC_SCHED: {
        const uint32_t syncMin = (uint32_t)ev.payload.syncSchedule.syncIntervalMinutes;
        const uint32_t phase = (uint32_t)ev.payload.syncSchedule.phaseUnix;
        if (!validSyncScheduleValue(syncMin, phase)) {
          Serial.printf("[EVENT] SET_SYNC_SCHED rejected: syncMin=%lu phase=%lu\n",
                        (unsigned long)syncMin, (unsigned long)phase);
          break;
        }
        const bool changed = (g_syncIntervalMin != (uint16_t)syncMin) ||
                             (g_syncPhaseUnix != phase);
        if (changed) {
          Serial.printf("[EVENT] SET_SYNC_SCHED syncMin %u -> %lu phase %lu -> %lu\n",
                        (unsigned)g_syncIntervalMin,
                        (unsigned long)syncMin,
                        (unsigned long)g_syncPhaseUnix,
                        (unsigned long)phase);
          g_syncIntervalMin = (uint16_t)syncMin;
          g_syncPhaseUnix = phase;
          g_lastSyncSlot = 0xFFFFFFFFUL;
          g_pendingPersistConfig = true;
          if (currentNodeState() == STATE_DEPLOYED && rtcSynced && !g_deployBootstrapPending) {
            g_rearmAlarmsPending = true;
          }
        }
        break;
      }

      case NodeEventType::SYNC_WINDOW_OPEN:
        g_syncWindowMarkerMs = ev.receivedMs ? ev.receivedMs : millis();
        Serial.printf("[EVENT] SYNC_WINDOW_OPEN marker phaseUnix=%lu\n",
                      (unsigned long)ev.payload.syncSchedule.phaseUnix);
        break;

      case NodeEventType::TIME_SYNC:
        if (!validDateTimeParts(ev.payload.timeSync.year, ev.payload.timeSync.month,
                                ev.payload.timeSync.day, ev.payload.timeSync.hour,
                                ev.payload.timeSync.minute, ev.payload.timeSync.second)) {
          Serial.println("[EVENT] TIME_SYNC rejected: invalid date/time");
          break;
        }
        memcpy(&g_pendingTimeSyncData, &ev.payload.timeSync, sizeof(g_pendingTimeSyncData));
        g_pendingTimeSync = true;
        break;

      case NodeEventType::CONFIG_SNAPSHOT:
        if (!validWakeInterval(ev.payload.configSnapshot.wakeIntervalMin) ||
            !validSyncScheduleValue(ev.payload.configSnapshot.syncIntervalMin,
                                    ev.payload.configSnapshot.syncPhaseUnix)) {
          Serial.println("[EVENT] CONFIG_SNAPSHOT rejected: invalid schedule");
          break;
        }
        memcpy(&g_pendingConfigSnapshotData, &ev.payload.configSnapshot,
               sizeof(g_pendingConfigSnapshotData));
        memcpy(g_pendingConfigSnapshotMac, ev.senderMac, 6);
        g_pendingConfigSnapshot = true;
        break;

      case NodeEventType::SNAPSHOT_ACK:
        if (g_waitingSnapshotAck &&
            ev.payload.snapshotAck.seqNum == g_expectedSnapshotAckSeq &&
            ev.payload.snapshotAck.persisted == 1 &&
            ev.payload.snapshotAck.protocolVersion == NODE_PROTOCOL_VERSION) {
          g_snapshotAckMatched = true;
          g_snapshotAckPersisted = true;
          Serial.printf("[ACK] durable SNAPSHOT_ACK matched seq=%lu\n",
                        (unsigned long)g_expectedSnapshotAckSeq);
        } else {
          Serial.printf("[ACK] stale/mismatched SNAPSHOT_ACK ignored seq=%lu expected=%lu\n",
                        (unsigned long)ev.payload.snapshotAck.seqNum,
                        (unsigned long)g_expectedSnapshotAckSeq);
        }
        break;
    }
  }

  g_eventApplyActive = false;
}

// ==================== Actions ======================
static void sendTimeSyncRequest() {
  static const uint8_t broadcastAddress[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};

  if (!bringupEspNow()) return;

  time_sync_request_t req{};
  strcpy(req.nodeId, NODE_ID);
  strcpy(req.command, "REQUEST_TIME");
  req.requestTime = millis();

  SendResult send = sendEspNowAndWait(broadcastAddress, &req, sizeof(req), 250);

  if (send.queueResult == ESP_OK) {
    Serial.println("⏰ Time sync request sent");
  } else {
    Serial.print("❌ Time sync request failed: ");
    Serial.println(esp_err_to_name(send.queueResult));
  }
}

static void runStaleSyncRecoveryIfNeeded() {
  if (!rtcSynced || !hasMothershipMAC()) return;

  const uint32_t nowUnix = rtc.now().unixtime();
  if (lastTimeSyncUnix == 0 || nowUnix <= lastTimeSyncUnix) return;

  const uint32_t ageSec = nowUnix - lastTimeSyncUnix;
  if (ageSec < (uint32_t)SYNC_STALE_THRESHOLD_SEC) return;

  Serial.printf("⚠️ Sync stale (age=%lus) -> data-wake recovery: HELLO + REQUEST_TIME\n",
                (unsigned long)ageSec);

  g_syncWindowMarkerMs = 0;
  if (!bringupEspNow()) {
    Serial.println("⚠️ Stale-sync recovery skipped: ESP-NOW bringup failed");
    return;
  }

  const uint32_t syncBefore = lastTimeSyncUnix;
  sendNodeHello();
  sendTimeSyncRequest();

  const uint32_t windowStart = millis();
  const uint32_t deadline = windowStart + (uint32_t)SYNC_STALE_RECOVERY_WINDOW_MS;
  while ((int32_t)(deadline - millis()) > 0) {
    serviceNodeEvents(8);
    if (lastTimeSyncUnix > syncBefore) break;
    if (g_syncWindowMarkerMs != 0) break;
    delay(50);
  }

  const bool timeRecovered = (lastTimeSyncUnix > syncBefore);
  const bool markerSeen = (g_syncWindowMarkerMs != 0);

  if (markerSeen) {
    uint32_t flushDeadline = deadline;
    if ((uint32_t)SYNC_STALE_RECOVERY_WINDOW_MS > 1000UL) {
      flushDeadline -= 1000UL;
    }
    Serial.println("📶 Stale-sync recovery saw marker -> flushing queue");
    flushQueuedToMothership(flushDeadline);
  }

  if (timeRecovered) {
    DateTime ls(lastTimeSyncUnix);
    char lsStr[24];
    formatTime(ls, lsStr, sizeof(lsStr));
    Serial.printf("✅ Stale-sync recovery updated TIME_SYNC -> %s\n", lsStr);
  } else if (!markerSeen) {
    Serial.println("⚠️ Stale-sync recovery window ended with no marker/time response");
  }
}

static void sendDiscoveryRequest() {
  static const uint8_t broadcastAddress[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};

  if (!bringupEspNow()) return;

  discovery_message_t m{};
  strcpy(m.nodeId,   NODE_ID);
  strcpy(m.nodeType, NODE_TYPE);
  strcpy(m.command,  "DISCOVER_REQUEST");
  m.timestamp = millis();

  SendResult send = sendEspNowAndWait(broadcastAddress, &m, sizeof(m), 250);
  Serial.println(send.queueResult == ESP_OK ? "📡 Discovery request sent" : "❌ Discovery request failed");
}

static void sendPairingRequest() {
  static const uint8_t broadcastAddress[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};

  if (!bringupEspNow()) return;

  pairing_request_t m{};
  strcpy(m.command, "PAIRING_REQUEST");
  strcpy(m.nodeId,  NODE_ID);

  SendResult send = sendEspNowAndWait(broadcastAddress, &m, sizeof(m), 250);
  Serial.println(send.queueResult == ESP_OK ? "📋 Pairing status request sent" : "❌ Pairing request failed");
}

static void sendNodeStatusUpdate(const char* reason) {
  static const uint8_t broadcastAddress[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};

  if (!bringupEspNow()) return;

  node_status_message_t st{};
  strcpy(st.command, "NODE_STATUS");
  strncpy(st.nodeId, NODE_ID, sizeof(st.nodeId) - 1);
  st.state = (uint8_t)currentNodeState();
  st.rtcSynced = rtcSynced ? 1 : 0;
  st.deployed = deployedFlag ? 1 : 0;
  st.rescueMode = g_rescueModeActive ? 1 : 0;
  st.rtcUnix = g_rtcReady ? rtc.now().unixtime() : 0;

  esp_err_t bcastRes = sendEspNowAndWait(broadcastAddress, &st, sizeof(st), 250).queueResult;

  esp_err_t directRes = ESP_ERR_INVALID_STATE;
  if (hasMothershipMAC()) {
    directRes = sendEspNowAndWait(mothershipMAC, &st, sizeof(st), 250).queueResult;
  }

  Serial.printf("📣 NODE_STATUS sent (%s): state=%u rtcSynced=%u deployed=%u rescue=%u rtcUnix=%lu direct=%s bcast=%s\n",
                reason ? reason : "periodic",
                (unsigned)st.state,
                (unsigned)st.rtcSynced,
                (unsigned)st.deployed,
                (unsigned)st.rescueMode,
                (unsigned long)st.rtcUnix,
                (directRes == ESP_OK) ? "OK" : esp_err_to_name(directRes),
                (bcastRes == ESP_OK) ? "OK" : esp_err_to_name(bcastRes));
}

static bool updateRescueBootStreak(uint32_t nowUnix) {
  Preferences p;
  if (!p.begin("rescue_ctl", false)) {
    Serial.println("⚠️ Rescue streak: NVS open failed");
    return false;
  }

  const uint32_t lastBootUnix = p.getULong("last_boot", 0);
  uint8_t streak = p.getUChar("streak", 0);

  if (lastBootUnix > 0 && nowUnix >= lastBootUnix &&
      (nowUnix - lastBootUnix) <= (uint32_t)RESCUE_BOOT_WINDOW_SEC) {
    if (streak < 255) streak++;
  } else {
    streak = 1;
  }

  const bool trigger = (streak >= (uint8_t)RESCUE_BOOT_THRESHOLD);

  p.putULong("last_boot", nowUnix);
  p.putUChar("streak", trigger ? 0 : streak);
  p.end();

  Serial.printf("[RESCUE] boot streak=%u/%u (window=%lus now=%lu last=%lu)%s\n",
                (unsigned)streak,
                (unsigned)RESCUE_BOOT_THRESHOLD,
                (unsigned long)RESCUE_BOOT_WINDOW_SEC,
                (unsigned long)nowUnix,
                (unsigned long)lastBootUnix,
                trigger ? " -> TRIGGER" : "");
  return trigger;
}

static void wipeNodeConfigToUnpaired() {
  nodeState = STATE_UNPAIRED;
  rtcSynced = false;
  deployedFlag = false;
  g_intervalMin = 1;
  g_syncIntervalMin = 15;
  g_syncPhaseUnix = 0;
  g_lastSyncSlot = 0xFFFFFFFFUL;
  lastTimeSyncUnix = 0;
  memset(mothershipMAC, 0, sizeof(mothershipMAC));

  ds3231DisableAlarmInterrupt();
  ds3231DisableAlarm2Interrupt();
  clearDS3231_AlarmFlags();

  setNodeConfigVersion(0);
  persistNodeConfig();
  debugState("after RESCUE wipe");
}

static void enterRescueMode() {
  g_rescueModeActive = true;
  g_lastRescueBeaconMs = 0;

  Serial.println("==================================================");
  Serial.println("RESCUE MODE ACTIVE");
  Serial.println("- Trigger: rapid 3-boot gesture");
  Serial.println("- Action: node config wiped to UNPAIRED");
  Serial.println("- Behavior: stay awake with radio listening");
  Serial.println("==================================================");

  wipeNodeConfigToUnpaired();
  if (!bringupEspNow()) {
    Serial.println("⚠️ RESCUE: ESP-NOW bringup failed; retrying in loop");
  }
  sendNodeStatusUpdate("rescue-entry");
  sendDiscoveryRequest();
}

// Pull-handshake: current config version persisted in NVS
static uint16_t getNodeConfigVersion() {
  return g_appliedConfigVersion;
}

static void setNodeConfigVersion(uint16_t v) {
  g_appliedConfigVersion = v;
}

// Send NODE_HELLO to mothership at top of each wake cycle (before data flush)
static void sendNodeHello() {
  if (!hasMothershipMAC()) return;
  if (!bringupEspNow()) return;
  static const uint8_t broadcastAddress[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};

  node_hello_message_t hello{};
  strcpy(hello.command, "NODE_HELLO");
  strncpy(hello.nodeId,   NODE_ID,   sizeof(hello.nodeId)   - 1);
  strncpy(hello.nodeType, NODE_TYPE, sizeof(hello.nodeType) - 1);
  hello.configVersion   = getNodeConfigVersion();
  hello.wakeIntervalMin = g_intervalMin;
  hello.queueDepth      = (uint8_t)min((int)local_queue::count(), 255);
  hello.rtcUnix         = rtc.now().unixtime();

  // Send direct first, then broadcast fallback to improve contact probability.
  esp_err_t resDirect = sendEspNowAndWait(mothershipMAC, &hello, sizeof(hello), 250).queueResult;
  esp_err_t resBcast = sendEspNowAndWait(broadcastAddress, &hello, sizeof(hello), 250).queueResult;

  Serial.printf("👋 NODE_HELLO sent: cfgV=%u wakeMin=%u qDepth=%u : direct=%s bcast=%s\n",
                hello.configVersion, hello.wakeIntervalMin, hello.queueDepth,
                resDirect == ESP_OK ? "OK" : esp_err_to_name(resDirect),
                resBcast == ESP_OK ? "OK" : esp_err_to_name(resBcast));
  if ((uint32_t)POST_WAKE_WINDOW_MS > 0) {
    g_postWakeWindowUntilMs = millis() + (uint32_t)POST_WAKE_WINDOW_MS;
  }
}
static bool armDeploymentWakeAlarms(DateTime* nextDataOut, DateTime* nextSyncOut) {
  Serial.printf("[ARM] syncMode=%s syncMin=%u phase=%lu wakeMin=%u\n",
                (g_syncIntervalMin == 0) ? "daily" : "interval",
                (unsigned)g_syncIntervalMin,
                (unsigned long)g_syncPhaseUnix,
                (unsigned)g_intervalMin);
  g_alarmWakeVerified = false;
  if (!g_rtcReady || !rtcSynced) {
    Serial.println("[ARM] refused: RTC not ready/synced");
    return false;
  }

  AlarmBytes expected{};
  AlarmArmResult result{};
  result.alarm1Written = ds3231ArmNextInNMinutes(g_intervalMin, nextDataOut, &expected);
  result.alarm1Verified = result.alarm1Written && verifyAlarm1Bytes(expected.a1);

  result.alarm2Written = ds3231ArmSyncWake(g_syncIntervalMin, g_syncPhaseUnix,
                                           SYNC_PRE_WAKE_SEC, nextSyncOut,
                                           &expected);
  result.alarm2Verified = result.alarm2Written && verifyAlarm2Bytes(expected.a2);

  if (result.alarm1Verified && result.alarm2Verified) {
    result.controlVerified = enableAndVerifyAlarmInterrupts(result.controlReg);
  }
  if (result.controlVerified) {
    result.flagsCleared = clearAndVerifyAlarmFlags(result.statusReg);
  }

  const bool okBoth = result.alarm1Verified && result.alarm2Verified &&
                      result.controlVerified && result.flagsCleared;

  Serial.printf("[ARM] verify a1(w=%d v=%d) a2(w=%d v=%d) ctrl=%d(0x%02X) flags=%d(0x%02X)\n",
                result.alarm1Written ? 1 : 0,
                result.alarm1Verified ? 1 : 0,
                result.alarm2Written ? 1 : 0,
                result.alarm2Verified ? 1 : 0,
                result.controlVerified ? 1 : 0,
                result.controlReg,
                result.flagsCleared ? 1 : 0,
                result.statusReg);

  if (okBoth) {
    g_lastAlarmArmMs = millis();
    g_alarmWakeVerified = true;
  } else {
    g_recoveryReason = RecoveryReason::ALARM_VERIFY_FAILED;
  }
  return okBoth;
}

static void finalizeWakeAndSleep(const char* reason) {
  Serial.printf("⚙️ [FINALIZE] entry: %s\n", reason ? reason : "wake cycle complete");

  serviceNodeEvents(12);

  if (currentNodeState() == STATE_DEPLOYED && rtcSynced && hasMothershipMAC()) {
    DateTime nextData{};
    DateTime nextSync{};
    bool okBoth = false;

    // Retry alarm arm up to 3 times – I2C can fail transiently.
    for (int attempt = 1; attempt <= 3 && !okBoth; attempt++) {
      okBoth = armDeploymentWakeAlarms(&nextData, &nextSync);
      if (!okBoth && attempt < 3) {
        Serial.printf("⚠️ [FINALIZE] Alarm arm failed (attempt %d/3) – retrying\n", attempt);
        delay(10);
      }
    }

    char dataStr[24]; formatTime(nextData, dataStr, sizeof(dataStr));
    char syncStr[24]; formatTime(nextSync, syncStr, sizeof(syncStr));
    Serial.printf("🔁 [FINALIZE] Alarms armed: data=%s sync=%s (%s)\n",
                  dataStr, syncStr, okBoth ? "OK" : "PARTIAL");
    if (!okBoth) {
      g_recoveryReason = RecoveryReason::ALARM_VERIFY_FAILED;
      Serial.println("[FINALIZE] Alarm verification failed after retries; retaining PWR_HOLD");
      sendNodeStatusUpdate("alarm-fault");
      return;
    }
  } else {
    // Node is no longer deployed (e.g. UNPAIR received mid-cycle).
    // Ensure alarms are disabled and flags cleared so we don't wake again.
    ds3231DisableAlarmInterrupt();
    ds3231DisableAlarm2Interrupt();
    clearDS3231_AlarmFlags();
    Serial.println("🔁 [FINALIZE] Not deployed – alarms disabled, no re-arm");
  }

  if (g_postUnpairHold) {
    // UNPAIR was received during this sync wake. Keep PWR_HOLD asserted AND radio on;
    // the UNPAIRED idle loop will listen for re-pair/commands for 15 minutes then power off.
    g_postUnpairHold = false;
    Serial.println("🔁 [FINALIZE] Post-unpair hold – radio stays on, deferring power cut to idle loop (15 min)");
    return;
  }

  while ((int32_t)(g_postWakeWindowUntilMs - millis()) > 0) {
    serviceNodeEvents(12);
    delay(20);
  }
  serviceNodeEvents(12);

  shutdownEspNow();

  schedulePowerCut(reason ? reason : "wake cycle complete");
  Serial.printf("💤 [FINALIZE] Power cut scheduled – reason: %s\n",
                reason ? reason : "wake cycle complete");
}



// -------------------- Per-alarm handler --------------------
static void handleRtcWakeEvents(bool dataWake, bool syncWake) {
  DateTime fired = rtc.now();
  char firedStr[24];
  formatTime(fired, firedStr, sizeof(firedStr));
  Serial.printf("⚡ RTC wake @ %s  dataWake=%d syncWake=%d\n",
                firedStr, dataWake ? 1 : 0, syncWake ? 1 : 0);

  if (!(currentNodeState() == STATE_DEPLOYED && rtcSynced && hasMothershipMAC())) {
    Serial.println("⚠️ Wake but node not ready (not DEPLOYED / RTC unsynced / no mothership)");
    clearDS3231_AlarmFlags();
    return;
  }

  if (dataWake) {
    clearDS3231_A1F();
    Serial.println("🧾 Data wake: sample locally (radio remains off)");
    captureSensorsToQueue();
    Serial.printf("🧾 Data wake complete; pending queue=%u\n", (unsigned)local_queue::count());
    runStaleSyncRecoveryIfNeeded();
  }

  if (syncWake) {
    clearDS3231_A2F();
    Serial.println("📶 Sync wake: enabling radio + listening for mothership sync burst");
    g_syncWindowMarkerMs = 0;
    if (bringupEspNow()) {
      sendNodeHello();
      const uint32_t nowUnix = rtc.now().unixtime();
      const uint32_t targetSyncUnix = nextSyncSlotUnix(nowUnix);
      const uint32_t baseListenSec = ((uint32_t)SYNC_LISTEN_WINDOW_MS + 999UL) / 1000UL;
      uint32_t listenUntilUnix = nowUnix + baseListenSec;
      if (targetSyncUnix > 0) {
        const uint32_t targetPlusGraceUnix = targetSyncUnix + (uint32_t)SYNC_MARKER_GRACE_SEC;
        if (listenUntilUnix < targetPlusGraceUnix) {
          listenUntilUnix = targetPlusGraceUnix;
        }
      }
      const uint32_t hardCapUnix = nowUnix + baseListenSec + (uint32_t)SYNC_MARKER_GRACE_SEC;
      if (listenUntilUnix > hardCapUnix) {
        Serial.printf("[SYNC] listen window capped: target=%lu cap=%lu\n",
                      (unsigned long)listenUntilUnix,
                      (unsigned long)hardCapUnix);
        listenUntilUnix = hardCapUnix;
      }

      char targetStr[24];
      if (targetSyncUnix > 0) formatTime(DateTime(targetSyncUnix), targetStr, sizeof(targetStr));
      else snprintf(targetStr, sizeof(targetStr), "n/a");
      char listenStr[24];
      formatTime(DateTime(listenUntilUnix), listenStr, sizeof(listenStr));
      Serial.printf("📶 Sync listen window until %s (target=%s, grace=%lus)\n",
                    listenStr,
                    targetStr,
                    (unsigned long)SYNC_MARKER_GRACE_SEC);

      const uint32_t windowStart = millis();
      while (rtc.now().unixtime() < listenUntilUnix) {
        serviceNodeEvents(8);
        if (g_syncWindowMarkerMs != 0) break;
        delay(50);
      }

      if (g_syncWindowMarkerMs != 0) {
        const uint32_t markerMs = g_syncWindowMarkerMs;
        const uint32_t markerDelay = (markerMs >= windowStart) ? (markerMs - windowStart) : 0;
        uint32_t flushDeadline = windowStart + (uint32_t)SYNC_LISTEN_WINDOW_MS;
        if ((uint32_t)SYNC_LISTEN_WINDOW_MS > 2000UL) {
          flushDeadline -= 2000UL;
        }
        Serial.printf("📶 Sync marker seen after %lums -> flushing queue\n",
                      (unsigned long)markerDelay);
        flushQueuedToMothership(flushDeadline);
      } else {
        Serial.println("⚠️ Sync marker not seen in listen window; flush skipped this cycle");
      }
    }
  }

  finalizeWakeAndSleep("wake cycle complete + next alarms armed");
}

// -------------------- I2C / Mux / ADS1115 Self-test --------------------

// Common ADS1115 default address
#define ADS1115_ADDR 0x48

static void testI2CBusesMuxAndADS() {
  Serial.println("====== I2C / Mux / ADS1115 Self-Test (single bus) ======");

  // 1) Probe DS3231 on bus
  WireRtc.beginTransmission(0x68);
  uint8_t errRtc = WireRtc.endTransmission();
  Serial.printf("RTC probe @0x68 on WireRtc -> %s (err=%u)\n",
                (errRtc == 0) ? "OK" : "FAIL", errRtc);

  // 2) Root bus scan (should show RTC, MUX, ADS1115, etc.)
  Serial.println("\nI2C scan on WireRtc:");
  uint8_t count = 0;
  for (uint8_t addr = 1; addr < 127; ++addr) {
    WireRtc.beginTransmission(addr);
    uint8_t err = WireRtc.endTransmission();
    if (err == 0) {
      Serial.printf("  Found device at 0x%02X\n", addr);
      count++;
    }
  }
  Serial.printf("  Done. Found %u device(s).\n", count);

  // 3) Probe mux (optional, if installed)
  WireRtc.beginTransmission(MUX_ADDR);
  uint8_t errMux = WireRtc.endTransmission();
  Serial.printf("MUX probe @0x%02X on WireRtc -> %s (err=%u)\n",
                MUX_ADDR,
                (errMux == 0) ? "OK" : "FAIL",
                errMux);

  // 4) Probe ADS1115 directly on root bus (NO muxSelectChannel)
  WireRtc.beginTransmission(ADS1115_ADDR);
  uint8_t errAds = WireRtc.endTransmission();
  Serial.printf("ADS1115 probe @0x%02X on WireRtc -> %s (err=%u)\n",
                ADS1115_ADDR,
                (errAds == 0) ? "OK" : "FAIL",
                errAds);

  Serial.println("====== I2C / Mux / ADS1115 Self-Test done ======");
}


// ==================== Setup/Loop ====================
void setup() {
  initPowerHoldControl();
  assertPowerHold("boot");

  Serial.begin(115200);
  delay(2000);
  bool rtcReady = false;
  if (!initNodeEventQueue(NODE_EVENT_QUEUE_DEPTH)) {
    Serial.println("[EVENT] node event queue allocation failed");
  }

  initNodeIdentity();

  initNVS();
  loadNodeConfig();
  bootCount++;

  Serial.println("====================================");
  Serial.print("🌡️ Air Temperature Node: "); Serial.println(NODE_ID);
  Serial.print("Boot #"); Serial.println(bootCount);
  Serial.print("MAC: "); Serial.println(WiFi.macAddress());
  debugState("after setup load");
  Serial.println("====================================");
#if NODE_REQUIRE_DURABLE_SNAPSHOT_ACK
  Serial.println("[ACK] Durable SNAPSHOT_ACK mode ENABLED - requires matching mothership support");
#else
  Serial.println("[ACK] Legacy snapshot mode - queue pops after verified ESP-NOW link delivery");
#endif

  // Single I2C bus for RTC + MUX + ADS1115
  WireRtc.begin(RTC_SDA_PIN, RTC_SCL_PIN);
  Serial.printf("✅ WireRtc started on SDA=%d SCL=%d\n", RTC_SDA_PIN, RTC_SCL_PIN);

  // Make RTClib use this bus
  if (!rtc.begin(&WireRtc)) {
    Serial.println("❌ RTC not found!");
    g_rtcReady = false;
    g_recoveryReason = RecoveryReason::RTC_ABSENT;
  } else {
    rtcReady = true;
    g_rtcReady = true;
    Serial.println("✅ RTC initialized");

    if (rtc.lostPower()) {
      Serial.println("⚠️ RTC lost power since last run");
      rtcSynced        = false;
      g_rtcPowerLost = true;
      g_recoveryReason = RecoveryReason::RTC_LOST_POWER;
      lastTimeSyncUnix = 0;
      ds3231DisableAlarmInterrupt();
      ds3231DisableAlarm2Interrupt();
      clearDS3231_AlarmFlags();
      persistNodeConfig();
    } else if (rtcSynced) {
      Serial.print("RTC Time: ");
      Serial.println(rtc.now().timestamp());
    } else {
      Serial.println("RTC not synchronized yet");
    }

    // Read alarm flags before any boot-time re-arm/clear. If an alarm is pending,
    // preserve it for loop-time handling so wake reason is not masked.
    uint8_t statusInit = readDS3231StatusReg();
    const bool bootAlarmPending = (statusInit != 0xFF) && ((statusInit & 0x03) != 0);
    if (statusInit == 0xFF) {
      Serial.println("⚠️ DS3231 status read failed at boot (I2C error?)");
    } else if (bootAlarmPending) {
      Serial.printf("[BOOT] Alarm pending (A1F=%u A2F=%u) -> preserving for wake handler\n",
                    (statusInit & 0x01) ? 1 : 0,
                    (statusInit & 0x02) ? 1 : 0);
    } else {
      Serial.println("[RTC] A1F=0 A2F=0 at boot (idle)");
    }

    // Only re-arm alarms on boot when no wake alarm is pending.
    if (rtcSynced && deployedFlag && g_intervalMin > 0 && !bootAlarmPending) {
      DateTime nextData;
      DateTime nextSync;
      bool ok = armDeploymentWakeAlarms(&nextData, &nextSync);

      char nextDataStr[24];
      char nextSyncStr[24];
      formatTime(nextData, nextDataStr, sizeof(nextDataStr));
      formatTime(nextSync, nextSyncStr, sizeof(nextSyncStr));
      Serial.printf("[BOOT] Re-armed RTC wake alarms based on stored interval=%u -> data=%s sync=%s (ok=%d)\n",
                    g_intervalMin, nextDataStr, nextSyncStr, ok);
    }
  }

  if (!local_queue::begin()) {
    Serial.println("⚠️ Local queue init failed; logging/sync will be degraded");
  }

  if (rtcReady) {
    const uint32_t bootUnix = rtc.now().unixtime();
    if (updateRescueBootStreak(bootUnix)) {
      enterRescueMode();
    }
  }

  if (!bringupEspNow()) {
    Serial.println("⚠️ ESP-NOW bringup failed in setup");
  }

  // Initialise all sensors (SHT41, PAR, soil, wind stub, AUX stub via sensors.cpp)
  if (!initSensors()) {
    Serial.println("⚠️ Sensor init failed (continuing, but reads may fail)");
  }


  // I2C sanity check: RTC + mux + ADS1115 (bring-up/debug only by default)
#if NODE_I2C_SCAN_ON_BOOT
  testI2CBusesMuxAndADS();
#endif

  // In deployed mode, keep WiFi/ESP-NOW disabled except sync windows.
  if (currentNodeState() == STATE_DEPLOYED) {
    shutdownEspNow();
  }

  Serial.println("🔁 Setup complete");
  debugState("end of setup");
}

void loop() {
  static unsigned long lastAction      = 0;
  static unsigned long lastTimeSyncReq = 0;
  static unsigned long lastBeat        = 0;   // heartbeat
  static unsigned long lastA1Check     = 0;   // when we last checked A1F
  static unsigned long loopCounter     = 0;   // just to see loop advancing

  unsigned long nowMs = millis();
  NodeState st = currentNodeState();

  serviceNodeEvents(12);
  processPowerCut();
  serviceNodeEvents(12);

  // --- Service pending ESP-NOW callback actions (main task context) ---
  // These handlers were deferred from onDataReceived() to avoid I2C/NVS
  // races with the Wi-Fi task.  Each block clears its flag before doing work
  // so a new callback during processing will set it again for the next pass.

  if (g_pendingTimeSync) {
    g_pendingTimeSync = false;
    time_sync_response_t& resp = g_pendingTimeSyncData;

    DateTime dt(
      (int)resp.year,
      (int)resp.month,
      (int)resp.day,
      (int)resp.hour,
      (int)resp.minute,
      (int)resp.second
    );

    uint32_t prevSync = lastTimeSyncUnix;
    rtc.adjust(dt);
    rtcSynced        = true;
    g_rtcPowerLost   = false;
    if (g_recoveryReason == RecoveryReason::RTC_LOST_POWER) {
      g_recoveryReason = RecoveryReason::NONE;
    }
    lastTimeSyncUnix = dt.unixtime();

    persistNodeConfig();

    char buf[24]; formatTime(dt, buf, sizeof(buf));
    Serial.print("⏰ [LOOP] TIME_SYNC applied, RTC set to ");
    Serial.println(buf);

    if (prevSync > 0) {
      DateTime prev(prevSync);
      char prevStr[24]; formatTime(prev, prevStr, sizeof(prevStr));
      Serial.printf("   ↪ Previous sync: %lu (%s)\n",
                    (unsigned long)prevSync, prevStr);
    }
    Serial.printf("   ↪ New lastTimeSyncUnix: %lu (%s)\n",
                  (unsigned long)lastTimeSyncUnix, buf);

    if (currentNodeState() == STATE_DEPLOYED && rtcSynced) {
      if (!g_deployBootstrapPending) {
        g_rearmAlarmsPending = true;
      }
      Serial.println("   ↪ TIME_SYNC re-arm queued for loop context");
    }

    debugState("after TIME_SYNC (loop)");
  }

  if (g_pendingPairNode) {
    g_pendingPairNode = false;

    Serial.println("📋 [LOOP] Applying PAIR_NODE");
    memcpy(mothershipMAC, g_pendingPairNodeMac, 6);

    g_postUnpairHold = false;
    rtcSynced        = false;
    deployedFlag     = false;
    lastTimeSyncUnix = 0;
    ds3231DisableAlarmInterrupt();
    ds3231DisableAlarm2Interrupt();
    clearDS3231_AlarmFlags();
    local_queue::clear();
    Serial.println("🧹 Cleared local queue after PAIR_NODE");
    persistNodeConfig();
    Serial.println("💾 Node state persisted after PAIR_NODE (rtcSynced=false, deployed=false)");
    if (g_rescueModeActive) {
      g_rescueModeActive = false;
      Serial.println("✅ RESCUE: PAIR_NODE received, exiting rescue mode");
    }

    debugState("after PAIR_NODE (loop)");
  }

  if (g_pendingPairingResponse) {
    g_pendingPairingResponse = false;
    pairing_response_t& pr = g_pendingPairingResponseData;

    if (pr.isPaired) {
      Serial.println("📋 [LOOP] Applying PAIRING_RESPONSE");
      memcpy(mothershipMAC, g_pendingPairingResponseMac, 6);
      g_postUnpairHold = false;
      rtcSynced        = false;
      deployedFlag     = false;
      lastTimeSyncUnix = 0;
      ds3231DisableAlarmInterrupt();
      ds3231DisableAlarm2Interrupt();
      clearDS3231_AlarmFlags();
      local_queue::clear();
      Serial.println("🧹 Cleared local queue after PAIRING_RESPONSE");
      persistNodeConfig();
      if (g_rescueModeActive) {
        g_rescueModeActive = false;
        Serial.println("✅ RESCUE: pairing response received, exiting rescue mode");
      }
      debugState("after PAIRING_RESPONSE (loop)");
    }
  }

  if (g_pendingUnpair) {
    g_pendingUnpair = false;

    Serial.println("🗑️ [LOOP] Applying UNPAIR");
    memset(mothershipMAC, 0, sizeof(mothershipMAC));
    rtcSynced        = false;
    deployedFlag     = false;
    lastTimeSyncUnix = 0;
    g_lastSyncSlot   = 0xFFFFFFFFUL;
    ds3231DisableAlarmInterrupt();
    ds3231DisableAlarm2Interrupt();
    clearDS3231_AlarmFlags();
    persistNodeConfig();
    local_queue::clear();
    g_postUnpairHold = true;
    Serial.println("💾 Node config persisted after UNPAIR");
    Serial.println("📡 Post-unpair: radio hold requested (15 min idle before power-off)");

    debugState("after UNPAIR (loop)");
  }

  if (g_pendingDeploy) {
    g_pendingDeploy = false;
    deployment_command_t& dc = g_pendingDeployData;

    Serial.println("🚀 [LOOP] Applying DEPLOY_NODE");

    const bool wasAlreadyDeployed = (currentNodeState() == STATE_DEPLOYED) && deployedFlag && rtcSynced;
    DateTime deployTime(dc.year, dc.month, dc.day, dc.hour, dc.minute, dc.second);
    uint32_t deployUnix = deployTime.unixtime();

    if (!wasAlreadyDeployed) {
      rtc.adjust(deployTime);
    }
    if (dc.wakeIntervalMin > 0) {
      g_intervalMin = dc.wakeIntervalMin;
    }
    if (dc.syncIntervalMin > 0) {
      g_syncIntervalMin = dc.syncIntervalMin;
    }
    if (dc.syncPhaseUnix > 0) {
      if (!wasAlreadyDeployed || g_syncPhaseUnix == 0 || dc.syncPhaseUnix >= g_syncPhaseUnix) {
        g_syncPhaseUnix = dc.syncPhaseUnix;
      } else {
        Serial.printf("↩️ Duplicate DEPLOY carried stale phase=%lu (current=%lu) -> ignored\n",
                      (unsigned long)dc.syncPhaseUnix,
                      (unsigned long)g_syncPhaseUnix);
      }
    }
    uint16_t candidateConfigVersion = getNodeConfigVersion();
    if (dc.configVersion > 0) {
      if (!wasAlreadyDeployed || dc.configVersion >= candidateConfigVersion) {
        candidateConfigVersion = dc.configVersion;
      }
    }
    g_postUnpairHold = false;
    rtcSynced        = true;
    deployedFlag     = true;
    if (!wasAlreadyDeployed) {
      lastTimeSyncUnix = rtc.now().unixtime();   // treat first deploy time as fresh sync
    } else if (deployUnix > lastTimeSyncUnix) {
      lastTimeSyncUnix = deployUnix;
    }
    if (g_syncPhaseUnix == 0) g_syncPhaseUnix = lastTimeSyncUnix;
    g_lastSyncSlot = 0xFFFFFFFFUL;
    memcpy(mothershipMAC, g_pendingDeployMac, 6);
    const bool deployConfigPersisted = persistNodeConfig(candidateConfigVersion, true);
    if (deployConfigPersisted) {
      setNodeConfigVersion(candidateConfigVersion);
    } else {
      Serial.println("⚠️ DEPLOY config persist failed; config version not advanced");
    }

    Serial.print("RTC synchronized to: ");
    Serial.println(rtc.now().timestamp());
    Serial.printf("⏰ lastTimeSyncUnix set to %lu at DEPLOY\n",
                  (unsigned long)lastTimeSyncUnix);
    Serial.printf("⚙️ DEPLOY config applied: cfgV=%u wakeMin=%u syncMin=%u phase=%lu\n",
          (unsigned)getNodeConfigVersion(),
          (unsigned)g_intervalMin,
          (unsigned)g_syncIntervalMin,
          (unsigned long)g_syncPhaseUnix);
    Serial.println("✅ Node deployed; ready for alarm-driven sends");
    debugState("after DEPLOY (loop)");

    if (!wasAlreadyDeployed) {
      // Defer immediate bootstrap to loop context for deterministic RTC/I2C handling.
      g_deployBootstrapPending = true;
      Serial.println("🧾 DEPLOY bootstrap queued for loop context");
    } else {
      // Duplicate deploy: refresh config but do NOT arm alarms from callback context.
      // Queueing here avoids I2C races with the loop's alarm flag polling.
      g_rearmAlarmsPending = true;
      Serial.println("↩️ Duplicate DEPLOY while already active: config updated, re-arm queued for loop");
    }
    if (g_rescueModeActive) {
      g_rescueModeActive = false;
      Serial.println("✅ RESCUE: DEPLOY received, exiting rescue mode");
    }
  }

  if (g_pendingDeployAck) {
    g_pendingDeployAck = false;

    // Send explicit deployment ACK so mothership can mark DEPLOYED immediately.
    deployment_ack_message_t dack{};
    strcpy(dack.command, "DEPLOY_ACK");
    strncpy(dack.nodeId, NODE_ID, sizeof(dack.nodeId) - 1);
    dack.deployed = 1;
    dack.rtcUnix  = rtc.now().unixtime();
    esp_err_t dackRes = sendEspNowAndWait(g_pendingDeployAckMac, &dack, sizeof(dack), 250).queueResult;
    Serial.printf("📨 [LOOP] DEPLOY_ACK sent: %s\n",
          dackRes == ESP_OK ? "OK" : esp_err_to_name(dackRes));
  }

  if (g_pendingConfigSnapshot) {
    g_pendingConfigSnapshot = false;
    config_snapshot_message_t& snap = g_pendingConfigSnapshotData;

    uint16_t currentVer = getNodeConfigVersion();
    Serial.printf("📦 [LOOP] CONFIG_SNAPSHOT applying: v%u (current v%u) wakeMin=%u syncMin=%u\n",
                  snap.configVersion, currentVer,
                  snap.wakeIntervalMin, snap.syncIntervalMin);

    config_apply_ack_message_t ack{};
    strcpy(ack.command, "CONFIG_ACK");
    strncpy(ack.nodeId, NODE_ID, sizeof(ack.nodeId) - 1);
    ack.appliedVersion = snap.configVersion;
    ack.ok = 0;

    if (snap.configVersion > currentVer) {
      const uint8_t oldWake = g_intervalMin;
      const uint16_t oldSyncMin = g_syncIntervalMin;
      const uint32_t oldPhase = g_syncPhaseUnix;
      const uint32_t oldSlot = g_lastSyncSlot;
      const uint16_t oldVersion = g_appliedConfigVersion;
      bool scheduleChanged = false;
      // Apply: wake interval
      if (snap.wakeIntervalMin > 0 && snap.wakeIntervalMin != g_intervalMin) {
        g_intervalMin = snap.wakeIntervalMin;
        scheduleChanged = true;
        Serial.printf("   ↪ wake interval updated: %u min\n", g_intervalMin);
      }
      // Apply: sync schedule
      if (snap.syncIntervalMin != g_syncIntervalMin || snap.syncPhaseUnix != g_syncPhaseUnix) {
        g_syncIntervalMin = snap.syncIntervalMin;
        g_syncPhaseUnix   = snap.syncPhaseUnix;
        g_lastSyncSlot    = 0xFFFFFFFFUL;
        scheduleChanged = true;
        Serial.printf("   ↪ sync schedule updated: %u min, phase=%lu\n",
                      g_syncIntervalMin, (unsigned long)g_syncPhaseUnix);
      }
      const bool configPersisted = persistNodeConfig(snap.configVersion, true);
      if (configPersisted) {
        setNodeConfigVersion(snap.configVersion);
      }
      if (scheduleChanged && configPersisted &&
          currentNodeState() == STATE_DEPLOYED && rtcSynced) {
        g_rearmAlarmsPending = true;
      }
      ack.ok = configPersisted ? 1 : 0;
      if (!configPersisted) {
        g_intervalMin = oldWake;
        g_syncIntervalMin = oldSyncMin;
        g_syncPhaseUnix = oldPhase;
        g_lastSyncSlot = oldSlot;
        g_appliedConfigVersion = oldVersion;
        ack.appliedVersion = oldVersion;
      }
      Serial.printf("   ↪ Config v%u %s\n", snap.configVersion,
                    configPersisted ? "applied OK" : "persist failed");
    } else {
      Serial.printf("   ↪ Config v%u not newer than current v%u; ignored\n",
                    snap.configVersion, currentVer);
      ack.ok = 1; // still ACK so mothership knows we're current
    }

    // Send ACK back to mothership from main task context.
    esp_err_t ackRes = sendEspNowAndWait(g_pendingConfigSnapshotMac, &ack, sizeof(ack), 250).queueResult;
    Serial.printf("   ↪ CONFIG_ACK sent (v%u ok=%d): %s\n",
                  ack.appliedVersion, ack.ok,
                  ackRes == ESP_OK ? "OK" : esp_err_to_name(ackRes));
  }

  if (g_pendingPersistConfig) {
    g_pendingPersistConfig = false;
    Serial.println("💾 [LOOP] Servicing deferred persistNodeConfig");
    persistNodeConfig();
  }

  if (g_deployBootstrapPending && currentNodeState() == STATE_DEPLOYED && rtcSynced) {
    g_deployBootstrapPending = false;
    Serial.println("🚦 [DEPLOY S1] Bootstrap start (loop context)");

    Serial.println("🚦 [DEPLOY S2] Capturing sensors");
    captureSensorsToQueue();
    Serial.printf("🚦 [DEPLOY S2] Capture done; pending queue=%u\n", (unsigned)local_queue::count());

    // S3: Best-effort immediate upload – do not wait for SYNC_WINDOW_OPEN.
    Serial.println("🚦 [DEPLOY S3] Attempting immediate HELLO + flush");
    if (bringupEspNow()) {
      sendNodeHello();
      flushQueuedToMothership(0);
      Serial.printf("🚦 [DEPLOY S3] Flush done; pending queue=%u\n", (unsigned)local_queue::count());
    } else {
      Serial.println("⚠️ [DEPLOY S3] ESP-NOW bringup failed – flush skipped, data in queue for sync wake");
    }

    // S4: Finalize unconditionally – RF failure in S3 must not block alarms + power cut.
    Serial.println("🚦 [DEPLOY S4] Calling finalizeWakeAndSleep");
    finalizeWakeAndSleep("deploy-bootstrap");
    return;
  }

  // Service alarm re-arm requests queued by callbacks (avoids I2C races with alarm polling).
  if (g_rearmAlarmsPending && !g_deployBootstrapPending &&
      currentNodeState() == STATE_DEPLOYED && rtcSynced) {
    g_rearmAlarmsPending = false;
    Serial.println("🔁 [LOOP] Servicing queued alarm re-arm");
    DateTime reArmData{}, reArmSync{};
    bool reArmOk = armDeploymentWakeAlarms(&reArmData, &reArmSync);
    if (reArmOk) {
      char dsStr[24], ssStr[24];
      formatTime(reArmData, dsStr, sizeof(dsStr));
      formatTime(reArmSync, ssStr, sizeof(ssStr));
      Serial.printf("🔁 [LOOP] Re-arm OK: data=%s sync=%s\n", dsStr, ssStr);
    } else {
      Serial.println("⚠️ [LOOP] Re-arm PARTIAL – will retry next contact");
      g_rearmAlarmsPending = true;  // retry
    }
  }

  if (g_rescueModeActive) {
    if (currentNodeState() != STATE_UNPAIRED) {
      g_rescueModeActive = false;
      Serial.println("✅ RESCUE: node no longer UNPAIRED, returning to normal loop");
    } else {
      if (!g_espNowReady) {
        bringupEspNow();
      }
      if ((nowMs - g_lastRescueBeaconMs) >= (uint32_t)RESCUE_BEACON_INTERVAL_MS) {
        g_lastRescueBeaconMs = nowMs;
        Serial.println("🛟 RESCUE beacon: broadcasting discovery request");
        sendNodeStatusUpdate("rescue-beacon");
        sendDiscoveryRequest();
      }
      delay(100);
      return;
    }
  }

  loopCounter++;

  // Very explicit heartbeat every 5 seconds
  if (nowMs - lastBeat > 5000UL) {
    lastBeat = nowMs;
    Serial.printf("💓 loop heartbeat #%lu, millis=%lu, state=%d, rtcSynced=%d, deployed=%d\n",
                  (unsigned long)loopCounter, nowMs, (int)st, rtcSynced, deployedFlag);
  }

  if (!g_rtcReady) {
    if (!g_espNowReady) {
      bringupEspNow();
    }
    if (nowMs - lastTimeSyncReq > 30000UL) {
      sendNodeStatusUpdate("rtc-absent");
      lastTimeSyncReq = nowMs;
    }
    delay(100);
    return;
  }

  // If we are bound but RTC isn't synced, occasionally ask for time.
  if (hasMothershipMAC() && !rtcSynced) {
    if (nowMs - lastTimeSyncReq > 30000UL) {
      Serial.println("⏰ Bound but RTC unsynced → requesting initial TIME_SYNC");
      sendTimeSyncRequest();
      if (currentNodeState() == STATE_DEPLOYED) shutdownEspNow();
      lastTimeSyncReq = nowMs;
    }
  }

  // If we are bound *and* RTC is synced, check if 24h have passed since last sync
  if (hasMothershipMAC() && rtcSynced && lastTimeSyncUnix > 0) {
    uint32_t nowUnix = rtc.now().unixtime();
    const uint32_t SYNC_PERIOD = 24UL * 3600UL;   // 24 hours in seconds

    if (nowUnix > lastTimeSyncUnix &&
        (nowUnix - lastTimeSyncUnix) > SYNC_PERIOD) {

      if (nowMs - lastTimeSyncReq > 30000UL) {  // don't spam, max 1 req / 30s
        uint32_t delta = nowUnix - lastTimeSyncUnix;
        Serial.printf("⏰ >24h since last TIME_SYNC (Δ=%lu s) → requesting periodic TIME_SYNC\n",
                      (unsigned long)delta);
        sendTimeSyncRequest();
        if (currentNodeState() == STATE_DEPLOYED) shutdownEspNow();
        lastTimeSyncReq = nowMs;
      }
    }
  }

// --- RTC alarm handling: check A1F/A2F once per second ---
if (nowMs - lastA1Check > 1000UL) {
  lastA1Check = nowMs;

  DateTime nowRtc = rtc.now();
  char tbuf[24];
  formatTime(nowRtc, tbuf, sizeof(tbuf));

  uint8_t status = readDS3231StatusReg();

  if (status == 0xFF) {
    Serial.printf("[RTC] %s A1F/A2F=ERR(0xFF)\n", tbuf);
  } else {
    const bool a1 = (status & 0x01) != 0;
    const bool a2 = (status & 0x02) != 0;
    Serial.printf("[RTC] %s A1F=%u A2F=%u\n", tbuf, a1 ? 1 : 0, a2 ? 1 : 0);

    if (a1 || a2) {
      if (currentNodeState() == STATE_DEPLOYED) {
        Serial.println("⚡ Alarm wake detected → handleRtcWakeEvents()");
        handleRtcWakeEvents(a1, a2);
      } else {
        // Ignore stale/deferred alarms outside deployed mode.
        clearDS3231_AlarmFlags();
        ds3231DisableAlarmInterrupt();
        ds3231DisableAlarm2Interrupt();
        Serial.println("⏸️ A1F/A2F cleared/ignored (node not deployed)");
      }
    } else if (currentNodeState() == STATE_DEPLOYED && rtcSynced &&
               !g_deployBootstrapPending && !g_rearmAlarmsPending &&
               g_lastAlarmArmMs != 0) {
      // Watchdog: if alarms were armed but haven't fired past the expected window, force re-arm.
      // Grace = interval + 2 minutes to tolerate clock skew and boot time.
      const uint32_t graceMs = ((uint32_t)g_intervalMin * 60UL + 120UL) * 1000UL;
      const uint32_t ageMs = (uint32_t)(millis() - g_lastAlarmArmMs);
      if ((int32_t)(ageMs - graceMs) > 0) {
        Serial.printf("⚠️ [WATCHDOG] Alarm overdue: armed %lus ago, interval=%umin – re-arming\n",
                      (unsigned long)(ageMs / 1000UL),
                      (unsigned)g_intervalMin);
        g_rearmAlarmsPending = true;
      }
    }
  }
}


  // Simple state machine logs (for bench)
  // For UNPAIRED and PAIRED: track idle entry time and shut down after 15 minutes.
  static uint32_t g_idleEntryMs = 0;
  static NodeState g_lastIdleState = STATE_DEPLOYED; // non-idle sentinel
  if (st == STATE_UNPAIRED || st == STATE_PAIRED) {
    // Reset idle timer whenever we transition into this state.
    if (g_lastIdleState == STATE_DEPLOYED) {
      g_idleEntryMs = nowMs;
    }
    g_lastIdleState = st;

    if (st == STATE_UNPAIRED) {
      // Keep ESP-NOW alive so the mothership can re-pair or issue commands.
      if (!g_espNowReady) {
        bringupEspNow();
      }
      if (nowMs - lastAction > 15000UL) {
        debugState("loop");
        Serial.println("🟡 Unpaired – radio on, listening for mothership…");
        lastAction = nowMs;
      }
    } else {
      if (nowMs - lastAction > 5000UL) {
        debugState("loop");
        Serial.println("🟡 Bound, waiting for DEPLOY command…");
        lastAction = nowMs;
      }
    }

    // Shut down after 15 minutes in UNPAIRED or PAIRED to save power.
    const uint32_t kIdleTimeoutMs = 15UL * 60UL * 1000UL;
    if (nowMs - g_idleEntryMs >= kIdleTimeoutMs) {
      Serial.printf("⏰ Idle timeout (%s) – powering off\n",
                    st == STATE_UNPAIRED ? "unpaired" : "paired");
      shutdownEspNow();
      schedulePowerCut(st == STATE_UNPAIRED ? "unpaired idle timeout" : "paired idle timeout");
    }
  } else { // STATE_DEPLOYED
    g_lastIdleState = STATE_DEPLOYED; // reset idle tracker
    if (nowMs - lastAction > 20000UL) {
      debugState("loop");
      Serial.println("🟢 Deployed — work happens on each DS3231 alarm.");
      lastAction = nowMs;
    }
  }

  delay(100);
}
