#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <Wire.h>
#include <RTClib.h>
#include <esp_wifi.h>
#include <esp_mac.h>
#include <Preferences.h>
#include <nvs_flash.h>
#include <nvs.h>
#include "esp_system.h"
#include "sensors.h"
#include "sensors/soil_moist_temp.h"
#include "storage/local_queue.h"

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

// Post-wake command window: keep ESP-NOW alive for N ms after HELLO to receive
// CONFIG_SNAPSHOT or other commands before the power cut. Set 0 to disable.
#ifndef POST_WAKE_WINDOW_MS
#define POST_WAKE_WINDOW_MS 1500
#endif

// Sync wake behavior (minute-resolution DS3231 Alarm2 windowing)
#ifndef SYNC_PRE_WAKE_SEC
#define SYNC_PRE_WAKE_SEC 30
#endif

#ifndef SYNC_LISTEN_WINDOW_MS
#define SYNC_LISTEN_WINDOW_MS 60000
#endif

#if ENABLE_POWER_HOLD_CONTROL && (PWR_HOLD_PIN >= 0)
static const uint8_t kPwrHoldOnLevel  = PWR_HOLD_ACTIVE_HIGH ? HIGH : LOW;
static const uint8_t kPwrHoldOffLevel = PWR_HOLD_ACTIVE_HIGH ? LOW  : HIGH;
static bool gPowerCutRequested = false;
static uint32_t gPowerCutAtMs = 0;
#endif

static void initPowerHoldControl() {
#if ENABLE_POWER_HOLD_CONTROL && (PWR_HOLD_PIN >= 0)
  pinMode(PWR_HOLD_PIN, OUTPUT);
  digitalWrite(PWR_HOLD_PIN, kPwrHoldOffLevel);
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
static bool ds3231ArmNextInNMinutes(uint8_t intervalMin, DateTime* nextOut = nullptr) {
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
  return ds3231WriteA1(secBCD, minBCD, hourBCD, dayReg);
}

static bool ds3231ArmSyncWake(uint16_t syncIntervalMin,
                              uint32_t syncPhaseUnix,
                              uint32_t preWakeSec,
                              DateTime* wakeOut = nullptr) {
  if (syncIntervalMin == 0) return false;

  uint32_t nowUnix = rtc.now().unixtime();
  uint32_t periodSec = (uint32_t)syncIntervalMin * 60UL;
  uint32_t phase = syncPhaseUnix;
  if (phase == 0 || phase > nowUnix) phase = nowUnix;

  uint32_t slot = (nowUnix - phase) / periodSec;
  uint32_t nextSyncUnix = phase + (slot + 1UL) * periodSec;
  uint32_t wakeUnix = (nextSyncUnix > preWakeSec) ? (nextSyncUnix - preWakeSec) : nextSyncUnix;

  DateTime wake(wakeUnix);
  const uint8_t minBCD  = uint8_t(((wake.minute()/10)<<4) | (wake.minute()%10));
  const uint8_t hourBCD = uint8_t(((wake.hour()/10)<<4)   | (wake.hour()%10));
  const uint8_t dayReg  = 0b10000000; // ignore day/date

  char wakeStr[24]; formatTime(wake, wakeStr, sizeof(wakeStr));
  char syncStr[24]; formatTime(DateTime(nextSyncUnix), syncStr, sizeof(syncStr));
  Serial.printf("[A2] Sync wake armed for %s (target sync %s, pre=%lus)\n",
                wakeStr, syncStr, (unsigned long)preWakeSec);

  if (wakeOut) *wakeOut = wake;
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

static bool g_espNowReady = false;
static volatile uint32_t g_syncWindowMarkerMs = 0;
static volatile bool g_lastSendDone = false;
static volatile esp_now_send_status_t g_lastSendStatus = ESP_NOW_SEND_FAIL;

static bool waitForSendDelivery(uint32_t timeoutMs) {
  const uint32_t start = millis();
  while ((millis() - start) < timeoutMs) {
    if (g_lastSendDone) {
      return g_lastSendStatus == ESP_NOW_SEND_SUCCESS;
    }
    delay(1);
  }
  return false;
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
static void persistNodeConfig() {
  Preferences p;

  if (!p.begin("node_cfg", false /* readWrite */)) {
    Serial.println("⚠️ persistNodeConfig: begin() failed");
    return;
  }

  nodeState = currentNodeState();

  p.putUChar("state",      (uint8_t)nodeState);
  p.putBool ("rtc_synced", rtcSynced);
  p.putBool ("deployed",   deployedFlag);
  p.putUChar("interval",   g_intervalMin);
  p.putULong("lastSync",   lastTimeSyncUnix);
  p.putUShort("syncMin",    g_syncIntervalMin);
  p.putULong("syncPhase",   g_syncPhaseUnix);
  p.putULong("syncSlot",    g_lastSyncSlot);

  // store mothership MAC as 12-char hex string
  char macHex[13];
  snprintf(macHex, sizeof(macHex),
           "%02X%02X%02X%02X%02X%02X",
           mothershipMAC[0], mothershipMAC[1], mothershipMAC[2],
           mothershipMAC[3], mothershipMAC[4], mothershipMAC[5]);
  p.putString("msmac", macHex);

  p.end();
  Serial.println("💾 Node config persisted to NVS");

  // --- immediate re-read sanity check ---
  Preferences p2;
  if (p2.begin("node_cfg", true /* readOnly */)) {
    uint8_t  st = p2.getUChar("state", 255);
    String   ms = p2.getString("msmac", "");
    bool     dep= p2.getBool("deployed", false);
    bool     rs = p2.getBool("rtc_synced", false);
    uint32_t ls = p2.getULong("lastSync", 0);
    uint16_t sm = p2.getUShort("syncMin", 15);
    uint32_t sp = p2.getULong("syncPhase", 0);
    uint32_t ss = p2.getULong("syncSlot", 0xFFFFFFFFUL);
    p2.end();
    Serial.printf("🔍 NVS verify: state=%u deployed=%d rtc_synced=%d msmac='%s' lastSyncUnix=%lu syncMin=%u syncPhase=%lu syncSlot=%lu\n",
            st, dep, rs, ms.c_str(), (unsigned long)ls, (unsigned)sm,
            (unsigned long)sp, (unsigned long)ss);
  } else {
    Serial.println("⚠️ NVS verify: begin() failed");
  }
}

static void loadNodeConfig() {
  Preferences p;

  if (!p.begin("node_cfg", true /* readOnly */)) {
    Serial.println("⚠️ loadNodeConfig: begin() failed (read-only)");
    nodeState        = STATE_UNPAIRED;
    rtcSynced        = false;
    deployedFlag     = false;
    g_intervalMin    = 1;
    lastTimeSyncUnix = 0;
    g_syncIntervalMin = 15;
    g_syncPhaseUnix = 0;
    g_lastSyncSlot = 0xFFFFFFFFUL;
    memset(mothershipMAC, 0, sizeof(mothershipMAC));
    return;
  }

  uint8_t rawState   = p.getUChar("state", (uint8_t)STATE_UNPAIRED);
  rtcSynced          = p.getBool ("rtc_synced", false);
  deployedFlag       = p.getBool ("deployed",   false);
  g_intervalMin      = p.getUChar("interval",   1);
  lastTimeSyncUnix   = p.getULong("lastSync",   0);
  g_syncIntervalMin  = p.getUShort("syncMin",   15);
  g_syncPhaseUnix    = p.getULong("syncPhase",  0);
  g_lastSyncSlot     = p.getULong("syncSlot",   0xFFFFFFFFUL);

  String macHex = p.getString("msmac", "");
  p.end();

  if (rawState <= (uint8_t)STATE_DEPLOYED) nodeState = (NodeState)rawState;
  else                                     nodeState = STATE_UNPAIRED;

  if (macHex.length() == 12) {
    for (int i = 0; i < 6; ++i) {
      String b = macHex.substring(i*2, i*2+2);
      mothershipMAC[i] = (uint8_t) strtoul(b.c_str(), nullptr, 16);
    }
  } else {
    memset(mothershipMAC, 0, sizeof(mothershipMAC));
  }

  Serial.printf("💾 Node config loaded from NVS: state=%u, rtcSynced=%d, deployed=%d, interval=%u, syncMin=%u, syncPhase=%lu, syncSlot=%lu, msmac='%s'\n",
                (unsigned)nodeState, rtcSynced, deployedFlag, g_intervalMin,
                (unsigned)g_syncIntervalMin, (unsigned long)g_syncPhaseUnix,
                (unsigned long)g_lastSyncSlot, macHex.c_str());

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
static void sendNodeHello();
static uint16_t getNodeConfigVersion();
static void setNodeConfigVersion(uint16_t v);
static bool bringupEspNow();
static void shutdownEspNow();
static bool shouldSyncAt(uint32_t unixNow);
static void captureSensorsToQueue();
static void flushQueuedToMothership(uint32_t deadlineMs = 0);
static bool armDeploymentWakeAlarms(DateTime* nextDataOut = nullptr, DateTime* nextSyncOut = nullptr);
static void finalizeWakeAndSleep(const char* reason);

// Guard against stale g_espNowReady state by retrying once after a forced re-init.
static esp_err_t espnowSendWithRecover(const uint8_t* mac, const uint8_t* payload, size_t len) {
  if (!bringupEspNow()) return ESP_ERR_ESPNOW_NOT_INIT;

  esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);
  esp_err_t res = esp_now_send(mac, payload, len);
  if (res != ESP_ERR_ESPNOW_NOT_INIT) return res;

  Serial.println("⚠️ ESP-NOW reported NOT_INIT during send; forcing re-init and retrying once");
  g_espNowReady = false;
  esp_now_deinit();
  WiFi.mode(WIFI_OFF);
  delay(20);

  if (!bringupEspNow()) return ESP_ERR_ESPNOW_NOT_INIT;
  esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);
  return esp_now_send(mac, payload, len);
}

static bool bringupEspNow() {
  if (g_espNowReady) return true;

  WiFi.mode(WIFI_STA);
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
    esp_now_add_peer(&pi);
  }

  if (hasMothershipMAC()) {
    esp_now_peer_info_t pi{};
    memcpy(pi.peer_addr, mothershipMAC, 6);
    pi.channel = ESPNOW_CHANNEL;
    pi.ifidx   = WIFI_IF_STA;
    pi.encrypt = false;
    esp_now_del_peer(mothershipMAC);
    esp_now_add_peer(&pi);
  }

  esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);
  g_espNowReady = true;
  Serial.println("✅ ESP-NOW online");
  return true;
}

static void shutdownEspNow() {
  if (!g_espNowReady) return;
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

static void captureSensorsToQueue() {
  if (g_numSensors == 0) {
    Serial.println("⚠️ No sensors configured (g_numSensors == 0)");
    return;
  }

  uint32_t nowUnix = rtc.now().unixtime();
  for (size_t i = 0; i < g_numSensors; ++i) {
    float value = 0.0f;
    if (!readSensor(i, value)) {
      Serial.printf("⚠️ Sensor read failed for slot %u\n", (unsigned)i);
      continue;
    }

    const char* labelStr = g_sensors[i].label ? g_sensors[i].label : "UNKNOWN";
    const char* typeStr  = g_sensors[i].sensorType ? g_sensors[i].sensorType : "UNKNOWN";
    uint16_t sensorId = g_sensors[i].sensorId;
    if (local_queue::enqueue(nowUnix, sensorId, typeStr, labelStr, value, 0)) {
      Serial.printf("🧾 queued seq=%lu %s=%.4f (pending=%u)\n",
                    (unsigned long)(local_queue::nextSeq() - 1),
                    labelStr,
                    value,
                    (unsigned)local_queue::count());
    } else {
      Serial.printf("❌ queue append failed for %s\n", labelStr);
    }
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

    local_queue::QueuedSample rec{};
    if (!local_queue::peek(rec)) break;

    sensor_data_message_t msg{};
    strcpy(msg.nodeId, NODE_ID);
    strncpy(msg.sensorType, rec.sensorType, sizeof(msg.sensorType) - 1);
    msg.sensorType[sizeof(msg.sensorType) - 1] = '\0';
    strncpy(msg.sensorLabel, rec.sensorLabel, sizeof(msg.sensorLabel) - 1);
    msg.sensorLabel[sizeof(msg.sensorLabel) - 1] = '\0';
    msg.sensorId = rec.sensorId;
    msg.value = rec.value;
    msg.nodeTimestamp = rec.sampleUnix;
    msg.qualityFlags = rec.qualityFlags;

    g_lastSendDone = false;
    g_lastSendStatus = ESP_NOW_SEND_FAIL;

    esp_err_t res = espnowSendWithRecover(mothershipMAC, (uint8_t*)&msg, sizeof(msg));
    if (res != ESP_OK) {
      Serial.printf("❌ queue flush send failed at seq=%lu: %s\n",
                    (unsigned long)rec.sampleSeq,
                    esp_err_to_name(res));
      break;
    }

    if (!waitForSendDelivery(200)) {
      Serial.printf("❌ queue flush delivery not confirmed at seq=%lu (status=%d)\n",
                    (unsigned long)rec.sampleSeq,
                    (int)g_lastSendStatus);
      break;
    }

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
  (void)mac_addr;
  g_lastSendStatus = status;
  g_lastSendDone = true;
  Serial.print("Send Status: ");
  Serial.println(status == ESP_NOW_SEND_SUCCESS ? "Success" : "Fail");
}

static void onDataReceived(const uint8_t *mac, const uint8_t *incomingData, int len) {
  Serial.print("📨 ESP-NOW message received, len=");
  Serial.println(len);

  // 1) Discovery-type messages
  if (len == sizeof(discovery_response_t)) {
    discovery_response_t resp;
    memcpy(&resp, incomingData, sizeof(resp));

    if (strcmp(resp.command, "DISCOVER_RESPONSE") == 0) {
      Serial.print("📡 Discovered by: ");
      Serial.println(resp.mothership_id);

      memcpy(mothershipMAC, mac, 6);

      esp_now_peer_info_t pi{};
      memcpy(pi.peer_addr, mac, 6);
      pi.channel = ESPNOW_CHANNEL;
      pi.ifidx   = WIFI_IF_STA;
      pi.encrypt = false;

      esp_now_del_peer(mac);
      if (esp_now_add_peer(&pi) == ESP_OK) {
        Serial.println("✅ Mothership added as peer");
      } else {
        Serial.println("❌ Failed to add mothership as peer");
      }

      persistNodeConfig();
      debugState("after DISCOVER_RESPONSE");
    }
    else if (strcmp(resp.command, "DISCOVERY_SCAN") == 0) {
      Serial.println("🔍 Responding to discovery scan…");
      sendDiscoveryRequest();
    }
    return;
  }

  // 2) Pairing response
  if (len == sizeof(pairing_response_t)) {
    pairing_response_t pr;
    memcpy(&pr, incomingData, sizeof(pr));

    if (strcmp(pr.command, "PAIRING_RESPONSE") == 0 && strcmp(pr.nodeId, NODE_ID) == 0) {
      if (pr.isPaired) {
        Serial.println("📋 Pairing confirmed via PAIRING_RESPONSE");
        memcpy(mothershipMAC, mac, 6);
        rtcSynced        = false;
        deployedFlag     = false;
        lastTimeSyncUnix = 0;
        ds3231DisableAlarmInterrupt();
        ds3231DisableAlarm2Interrupt();
        clearDS3231_AlarmFlags();
        local_queue::clear();
        Serial.println("🧹 Cleared local queue after PAIRING_RESPONSE");
        persistNodeConfig();
        debugState("after PAIRING_RESPONSE");
      } else {
        Serial.println("📋 Still unpaired; continuing discovery…");
      }
    }
    return;
  }

  // 3) Direct PAIR_NODE command
  if (len == sizeof(pairing_command_t)) {
    pairing_command_t pc;
    memcpy(&pc, incomingData, sizeof(pc));

    if (strcmp(pc.command, "PAIR_NODE") == 0 && strcmp(pc.nodeId, NODE_ID) == 0) {
      Serial.println("📋 Direct PAIR_NODE command received");

      memcpy(mothershipMAC, mac, 6);

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

      debugState("after PAIR_NODE");
    }
    return;
  }

  // 4) Deployment command (RTC time set + first alarm arm)
  if (len == sizeof(deployment_command_t)) {
    deployment_command_t dc;
    memcpy(&dc, incomingData, sizeof(dc));

    if (strcmp(dc.command, "DEPLOY_NODE") == 0 && strcmp(dc.nodeId, NODE_ID) == 0) {
      Serial.println("🚀 Deployment command received");

      rtc.adjust(DateTime(dc.year, dc.month, dc.day, dc.hour, dc.minute, dc.second));
      if (dc.wakeIntervalMin > 0) {
        g_intervalMin = dc.wakeIntervalMin;
      }
      if (dc.syncIntervalMin > 0) {
        g_syncIntervalMin = dc.syncIntervalMin;
      }
      if (dc.syncPhaseUnix > 0) {
        g_syncPhaseUnix = dc.syncPhaseUnix;
      }
      if (dc.configVersion > 0) {
        setNodeConfigVersion(dc.configVersion);
      }
      rtcSynced        = true;
      deployedFlag     = true;
      lastTimeSyncUnix = rtc.now().unixtime();   // treat deploy time as fresh sync
      if (g_syncPhaseUnix == 0) g_syncPhaseUnix = lastTimeSyncUnix;
      g_lastSyncSlot = 0xFFFFFFFFUL;
      memcpy(mothershipMAC, mac, 6);
      persistNodeConfig();

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
      debugState("after DEPLOY");

      // Send explicit deployment ACK so mothership can mark DEPLOYED immediately.
      deployment_ack_message_t dack{};
      strcpy(dack.command, "DEPLOY_ACK");
      strncpy(dack.nodeId, NODE_ID, sizeof(dack.nodeId) - 1);
      dack.deployed = 1;
      dack.rtcUnix  = rtc.now().unixtime();
      esp_err_t dackRes = espnowSendWithRecover(mac, (uint8_t*)&dack, sizeof(dack));
      Serial.printf("📨 DEPLOY_ACK sent: %s\n",
            dackRes == ESP_OK ? "OK" : esp_err_to_name(dackRes));

      // Immediate first cycle on deploy, then schedule next wake from cycle end.
      Serial.println("🧾 DEPLOY immediate cycle start");
      captureSensorsToQueue();
      Serial.printf("🧾 DEPLOY immediate sample done; pending queue=%u\n", (unsigned)local_queue::count());

      finalizeWakeAndSleep("deploy immediate cycle complete");
    }
    return;
  }

  // 5) Remote unpair
  if (len == sizeof(unpair_command_t)) {
    unpair_command_t uc;
    memcpy(&uc, incomingData, sizeof(uc));

    if (strcmp(uc.command, "UNPAIR_NODE") == 0) {
      Serial.println("🗑️ UNPAIR received");

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
      Serial.println("💾 Node config persisted after UNPAIR");

      debugState("after UNPAIR");
    }
    return;
  }

  // 6) Schedule / interval command (set DS3231 Alarm)
  if (len == sizeof(schedule_command_message_t)) {
    schedule_command_message_t cmd;
    memcpy(&cmd, incomingData, sizeof(cmd));

    if (strcmp(cmd.command, "SET_SCHEDULE") == 0) {
      uint8_t oldInterval = g_intervalMin;
      g_intervalMin = (uint8_t)cmd.intervalMinutes;

      bool ok = true;
      DateTime nextData;
      DateTime nextSync;
      char nowStr[24], nextStr[24], syncStr[24];
      formatTime(rtc.now(), nowStr, sizeof(nowStr));

      // Intention: paired/unpaired nodes store interval but remain idle.
      if (currentNodeState() == STATE_DEPLOYED && rtcSynced) {
        ok = armDeploymentWakeAlarms(&nextData, &nextSync);
        formatTime(nextData, nextStr, sizeof(nextStr));
        formatTime(nextSync, syncStr, sizeof(syncStr));
      } else {
        ds3231DisableAlarmInterrupt();
        ds3231DisableAlarm2Interrupt();
        clearDS3231_AlarmFlags();
        snprintf(nextStr, sizeof(nextStr), "(idle: not deployed)");
        snprintf(syncStr, sizeof(syncStr), "(idle: not deployed)");
      }

      Serial.println("[SET_SCHEDULE] received");
      Serial.printf("   interval: %u -> %u minutes\n", oldInterval, g_intervalMin);
      Serial.printf("   now:  %s\n",  nowStr);
      Serial.printf("   nextData: %s\n",  nextStr);
      Serial.printf("   nextSync: %s\n",  syncStr);
      Serial.printf("   status: %s\n", ok ? "OK" : "FAIL");

      persistNodeConfig();
    }
    return;
  }

  // 7) Sync schedule command (fleet-wide broadcast from mothership)
  if (len == sizeof(sync_schedule_command_message_t)) {
    sync_schedule_command_message_t sc{};
    memcpy(&sc, incomingData, sizeof(sc));

    if (strcmp(sc.command, "SET_SYNC_SCHED") == 0) {
      uint16_t oldMin = g_syncIntervalMin;
      g_syncIntervalMin = (uint16_t)sc.syncIntervalMinutes;
      g_syncPhaseUnix = (uint32_t)sc.phaseUnix;
      g_lastSyncSlot = 0xFFFFFFFFUL;

      Serial.printf("[SET_SYNC_SCHED] sync interval: %u -> %u minutes, phaseUnix=%lu\n",
                    (unsigned)oldMin,
                    (unsigned)g_syncIntervalMin,
                    (unsigned long)g_syncPhaseUnix);

      if (currentNodeState() == STATE_DEPLOYED && rtcSynced) {
        DateTime nextData;
        DateTime nextSync;
        const bool rearmOk = armDeploymentWakeAlarms(&nextData, &nextSync);
        char dataStr[24]; formatTime(nextData, dataStr, sizeof(dataStr));
        char syncStr[24]; formatTime(nextSync, syncStr, sizeof(syncStr));
        Serial.printf("[SET_SYNC_SCHED] immediate re-arm data=%s sync=%s (%s)\n",
                      dataStr, syncStr, rearmOk ? "OK" : "PARTIAL");
      }

      persistNodeConfig();
    } else if (strcmp(sc.command, "SYNC_WINDOW_OPEN") == 0) {
      g_syncWindowMarkerMs = millis();
      Serial.printf("[SYNC_WINDOW_OPEN] marker received phaseUnix=%lu\n",
                    (unsigned long)sc.phaseUnix);
    }
    return;
  }

  // 8) Time sync response from mothership
  if (len == sizeof(time_sync_response_t)) {
    time_sync_response_t resp;
    memcpy(&resp, incomingData, sizeof(resp));

    if (strcmp(resp.command, "TIME_SYNC") == 0) {
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
      lastTimeSyncUnix = dt.unixtime();

      persistNodeConfig();

      char buf[24]; formatTime(dt, buf, sizeof(buf));
      Serial.print("⏰ TIME_SYNC received, RTC set to ");
      Serial.println(buf);

      if (prevSync > 0) {
        DateTime prev(prevSync);
        char prevStr[24]; formatTime(prev, prevStr, sizeof(prevStr));
        Serial.printf("   ↪ Previous sync: %lu (%s)\n",
                      (unsigned long)prevSync, prevStr);
      }
      Serial.printf("   ↪ New lastTimeSyncUnix: %lu (%s)\n",
                    (unsigned long)lastTimeSyncUnix, buf);

      debugState("after TIME_SYNC");

      // NOTE: we do NOT touch alarms here.
    }
    return;
  }

  // 9) CONFIG_SNAPSHOT from mothership (pull handshake response)
  if (len == sizeof(config_snapshot_message_t)) {
    config_snapshot_message_t snap{};
    memcpy(&snap, incomingData, sizeof(snap));

    if (strcmp(snap.command, "CONFIG_SNAPSHOT") == 0) {
      uint16_t currentVer = getNodeConfigVersion();
      Serial.printf("📦 CONFIG_SNAPSHOT received: v%u (current v%u) wakeMin=%u syncMin=%u\n",
                    snap.configVersion, currentVer,
                    snap.wakeIntervalMin, snap.syncIntervalMin);

      config_apply_ack_message_t ack{};
      strcpy(ack.command, "CONFIG_ACK");
      strncpy(ack.nodeId, NODE_ID, sizeof(ack.nodeId) - 1);
      ack.appliedVersion = snap.configVersion;
      ack.ok = 0;

      if (snap.configVersion > currentVer) {
        // Apply: wake interval
        if (snap.wakeIntervalMin > 0 && snap.wakeIntervalMin != g_intervalMin) {
          g_intervalMin = snap.wakeIntervalMin;
          // Re-arm already happens at end of alarm cycle; just update state
          Serial.printf("   ↪ wake interval updated: %u min\n", g_intervalMin);
        }
        // Apply: sync schedule
        if (snap.syncIntervalMin > 0) {
          g_syncIntervalMin = snap.syncIntervalMin;
          g_syncPhaseUnix   = snap.syncPhaseUnix;
          g_lastSyncSlot    = 0xFFFFFFFFUL;
          Serial.printf("   ↪ sync schedule updated: %u min, phase=%lu\n",
                        g_syncIntervalMin, (unsigned long)g_syncPhaseUnix);
        }
        setNodeConfigVersion(snap.configVersion);
        persistNodeConfig();
        ack.ok = 1;
        Serial.printf("   ↪ Config v%u applied OK\n", snap.configVersion);
      } else {
        Serial.printf("   ↪ Config v%u not newer than current v%u; ignored\n",
                      snap.configVersion, currentVer);
        ack.ok = 1; // still ACK so mothership knows we're current
      }

      // Send ACK back to mothership
      esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);
      esp_now_send(mac, (uint8_t*)&ack, sizeof(ack));
      Serial.printf("   ↪ CONFIG_ACK sent (v%u ok=%d)\n", ack.appliedVersion, ack.ok);
    }
    return;
  }
}

// ==================== Actions ======================
static void sendTimeSyncRequest() {
  static const uint8_t broadcastAddress[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};

  if (!bringupEspNow()) return;

  time_sync_request_t req{};
  strcpy(req.nodeId, NODE_ID);
  strcpy(req.command, "REQUEST_TIME");
  req.requestTime = millis();

  esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);
  esp_err_t res = esp_now_send(broadcastAddress, (uint8_t*)&req, sizeof(req));

  if (res == ESP_OK) {
    Serial.println("⏰ Time sync request sent");
  } else {
    Serial.print("❌ Time sync request failed: ");
    Serial.println(esp_err_to_name(res));
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

  esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);
  esp_err_t res = esp_now_send(broadcastAddress, (uint8_t*)&m, sizeof(m));
  Serial.println(res == ESP_OK ? "📡 Discovery request sent" : "❌ Discovery request failed");
}

static void sendPairingRequest() {
  static const uint8_t broadcastAddress[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};

  if (!bringupEspNow()) return;

  pairing_request_t m{};
  strcpy(m.command, "PAIRING_REQUEST");
  strcpy(m.nodeId,  NODE_ID);

  esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);
  esp_err_t res = esp_now_send(broadcastAddress, (uint8_t*)&m, sizeof(m));
  Serial.println(res == ESP_OK ? "📋 Pairing status request sent" : "❌ Pairing request failed");
}

// Pull-handshake: current config version persisted in NVS
static uint16_t getNodeConfigVersion() {
  Preferences p;
  if (!p.begin("node_cfg", true)) return 0;
  uint16_t v = p.getUShort("cfgVer", 0);
  p.end();
  return v;
}

static void setNodeConfigVersion(uint16_t v) {
  Preferences p;
  if (!p.begin("node_cfg", false)) return;
  p.putUShort("cfgVer", v);
  p.end();
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
  esp_err_t resDirect = espnowSendWithRecover(mothershipMAC, (uint8_t*)&hello, sizeof(hello));
  esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);
  esp_err_t resBcast = esp_now_send(broadcastAddress, (uint8_t*)&hello, sizeof(hello));

  Serial.printf("👋 NODE_HELLO sent: cfgV=%u wakeMin=%u qDepth=%u : direct=%s bcast=%s\n",
                hello.configVersion, hello.wakeIntervalMin, hello.queueDepth,
                resDirect == ESP_OK ? "OK" : esp_err_to_name(resDirect),
                resBcast == ESP_OK ? "OK" : esp_err_to_name(resBcast));
}

static bool armDeploymentWakeAlarms(DateTime* nextDataOut, DateTime* nextSyncOut) {
  bool okData = ds3231ArmNextInNMinutes(g_intervalMin, nextDataOut);
  bool okSync = ds3231ArmSyncWake(g_syncIntervalMin, g_syncPhaseUnix, SYNC_PRE_WAKE_SEC, nextSyncOut);

  ds3231EnableAlarmInterrupt();
  ds3231EnableAlarm2Interrupt();
  clearDS3231_AlarmFlags();

  return okData && okSync;
}

static void finalizeWakeAndSleep(const char* reason) {
  DateTime nextData;
  DateTime nextSync;
  bool okBoth = armDeploymentWakeAlarms(&nextData, &nextSync);

  char dataStr[24]; formatTime(nextData, dataStr, sizeof(dataStr));
  char syncStr[24]; formatTime(nextSync, syncStr, sizeof(syncStr));
  Serial.printf("🔁 Re-armed alarms data=%s sync=%s (%s)\n",
                dataStr, syncStr, okBoth ? "OK" : "PARTIAL");

  shutdownEspNow();
  schedulePowerCut(reason ? reason : "wake cycle complete");
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
  }

  if (syncWake) {
    clearDS3231_A2F();
    Serial.println("📶 Sync wake: enabling radio + listening for mothership sync burst");
    g_syncWindowMarkerMs = 0;
    if (bringupEspNow()) {
      sendNodeHello();
      const uint32_t windowStart = millis();
      while ((millis() - windowStart) < (uint32_t)SYNC_LISTEN_WINDOW_MS) {
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
  Serial.begin(115200);
  delay(2000);

  initNodeIdentity();

  initPowerHoldControl();
  assertPowerHold("boot");

  initNVS();
  loadNodeConfig();
  bootCount++;

  Serial.println("====================================");
  Serial.print("🌡️ Air Temperature Node: "); Serial.println(NODE_ID);
  Serial.print("Boot #"); Serial.println(bootCount);
  Serial.print("MAC: "); Serial.println(WiFi.macAddress());
  debugState("after setup load");
  Serial.println("====================================");

  // Single I2C bus for RTC + MUX + ADS1115
  WireRtc.begin(RTC_SDA_PIN, RTC_SCL_PIN);
  Serial.printf("✅ WireRtc started on SDA=%d SCL=%d\n", RTC_SDA_PIN, RTC_SCL_PIN);

  // Make RTClib use this bus
  if (!rtc.begin(&WireRtc)) {
    Serial.println("❌ RTC not found!");
  } else {
    Serial.println("✅ RTC initialized");

    if (rtc.lostPower()) {
      Serial.println("⚠️ RTC lost power since last run");
      rtcSynced        = false;
      deployedFlag     = false;
      lastTimeSyncUnix = 0;
      persistNodeConfig();
    } else if (rtcSynced) {
      Serial.print("RTC Time: ");
      Serial.println(rtc.now().timestamp());
    } else {
      Serial.println("RTC not synchronized yet");
    }

    // After rtc.begin(...) block, once we know rtcSynced/deployedFlag/g_intervalMin
    if (rtcSynced && deployedFlag && g_intervalMin > 0) {
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


    // Normalise alarm flags at boot so we don't start on stale edges.
    uint8_t statusInit = readDS3231StatusReg();
    if (statusInit == 0xFF) {
      Serial.println("⚠️ DS3231 status read failed at boot (I2C error?)");
    } else if ((statusInit & 0x03) != 0) {
      Serial.printf("⚠️ Alarm flags set at boot (A1F=%u A2F=%u) → clearing\n",
                    (statusInit & 0x01) ? 1 : 0,
                    (statusInit & 0x02) ? 1 : 0);
      clearDS3231_AlarmFlags();
    } else {
      Serial.println("[RTC] A1F=0 A2F=0 at boot (idle)");
    }
  }

  if (!local_queue::begin()) {
    Serial.println("⚠️ Local queue init failed; logging/sync will be degraded");
  }

  if (!bringupEspNow()) {
    Serial.println("⚠️ ESP-NOW bringup failed in setup");
  }

  // Initialise all sensors (SHT41, PAR, soil, wind stub, AUX stub via sensors.cpp)
  if (!initSensors()) {
    Serial.println("⚠️ Sensor init failed (continuing, but reads may fail)");
  }


  // I2C sanity check: RTC + mux + ADS1115 (optional to keep / update)
  testI2CBusesMuxAndADS();

  persistNodeConfig();

  // In deployed mode, keep WiFi/ESP-NOW disabled except sync windows.
  if (currentNodeState() == STATE_DEPLOYED) {
    shutdownEspNow();
  }

  Serial.println("🔁 Setup persisted baseline node config");
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

  processPowerCut();

  loopCounter++;

  // Very explicit heartbeat every 5 seconds
  if (nowMs - lastBeat > 5000UL) {
    lastBeat = nowMs;
    Serial.printf("💓 loop heartbeat #%lu, millis=%lu, state=%d, rtcSynced=%d, deployed=%d\n",
                  (unsigned long)loopCounter, nowMs, (int)st, rtcSynced, deployedFlag);
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
    }
  }
}


  // Simple state machine logs (for bench)
  if (st == STATE_UNPAIRED) {
    if (nowMs - lastAction > 15000UL) {
      debugState("loop");
      Serial.println("🟡 Unpaired – idle, waiting for discovery scan…");
      lastAction = nowMs;
    }
  } else if (st == STATE_PAIRED) {
    if (nowMs - lastAction > 5000UL) {
      debugState("loop");
      Serial.println("🟡 Bound, waiting for DEPLOY command…");
      lastAction = nowMs;
    }
  } else { // STATE_DEPLOYED
    if (nowMs - lastAction > 20000UL) {
      debugState("loop");
      Serial.println("🟢 Deployed — work happens on each DS3231 alarm.");
      lastAction = nowMs;
    }
  }

  delay(100);
}
