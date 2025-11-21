#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <Wire.h>
#include <RTClib.h>
#include <esp_wifi.h>
#include <Preferences.h>
#include <nvs_flash.h>
#include <nvs.h>
#include "esp_system.h"

#include "protocol.h"     // pins, ESPNOW_CHANNEL, protocol structs

#ifndef FW_BUILD
  #define FW_BUILD __DATE__ " " __TIME__
#endif

// -------------------- Node config --------------------
#define NODE_ID   "TEMP_001"
#define NODE_TYPE "temperature"

// Hardware
RTC_DS3231 rtc;

// -------------------- DS3231 helpers --------------------
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
  Wire.beginTransmission(0x68);
  Wire.write(0x0F);                 // STATUS register
  Wire.endTransmission(false);
  Wire.requestFrom(0x68, 1);
  if (Wire.available()) {
    uint8_t s = Wire.read();
    return (s & 0x01) ? 1 : 0;      // bit0 = A1F
  }
  return 0xFF;
}

// Clear A1F (Alarm1 Flag) while preserving other status bits.
static void clearDS3231_A1F() {
  Wire.beginTransmission(0x68);
  Wire.write(0x0F);      // STATUS
  Wire.endTransmission(false);
  Wire.requestFrom(0x68, 1);
  uint8_t s = Wire.available() ? Wire.read() : 0;

  // Clear A1F (bit0) AND A2F (bit1) to be safe
  s &= ~0x03;

  Wire.beginTransmission(0x68);
  Wire.write(0x0F);
  Wire.write(s);
  Wire.endTransmission();
}

// Enable INTCN + A1IE (Alarm1 interrupt)
static void ds3231EnableAlarmInterrupt() {
  Wire.beginTransmission(0x68);
  Wire.write(0x0E);  // CTRL
  Wire.endTransmission(false);
  Wire.requestFrom(0x68, 1);
  uint8_t ctrl = Wire.available() ? Wire.read() : 0;

  ctrl |= 0b00000101;  // INTCN (bit2) + A1IE (bit0)

  Wire.beginTransmission(0x68);
  Wire.write(0x0E);
  Wire.write(ctrl);
  Wire.endTransmission();
}

// Low-level: write DS3231 Alarm1 registers
static bool ds3231WriteA1(uint8_t secReg, uint8_t minReg, uint8_t hourReg, uint8_t dayReg) {
  Wire.beginTransmission(0x68);
  Wire.write(0x07);                // A1 seconds register
  Wire.write(secReg);
  Wire.write(minReg);
  Wire.write(hourReg);
  Wire.write(dayReg);
  return Wire.endTransmission() == 0;
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

// Compute next trigger (aligned to interval; seconds=00), program A1.
static bool ds3231ArmNextInNMinutes(uint8_t intervalMin, DateTime* nextOut = nullptr) {
  DateTime now = rtc.now();

  // If not at :00, hop to the next minute :00
  if (now.second() != 0) {
    now = now + TimeSpan(0, 0, 1, 0);
    now = DateTime(now.year(), now.month(), now.day(),
                   now.hour(), now.minute(), 0);
  }

  // Minutes to add to hit the NEXT multiple of intervalMin
  uint8_t mod    = now.minute() % intervalMin;
  uint8_t addMin = (mod == 0) ? intervalMin : (intervalMin - mod);

  DateTime next = now + TimeSpan(0, 0, addMin, 0); // seconds already 0

  // Safety: if somehow next <= real RTC "now", push one minute
  DateTime chk = rtc.now();
  if (next <= chk) next = next + TimeSpan(0, 0, 1, 0);

  const uint8_t secBCD  = 0x00; // A1M1=0 (match seconds)
  const uint8_t minBCD  = uint8_t(((next.minute()/10)<<4) | (next.minute()%10)); // A1M2=0
  const uint8_t hourBCD = uint8_t(((next.hour()/10)<<4)   | (next.hour()%10));   // A1M3=0 (24h)
  const uint8_t dayReg  = 0b10000000; // A1M4=1 ignore day/date

  char buf[24]; formatTime(next, buf, sizeof(buf));
  Serial.print("[A1] Next alarm at "); Serial.println(buf);

  if (nextOut) *nextOut = next;
  return ds3231WriteA1(secBCD, minBCD, hourBCD, dayReg);
}


// -------------------- Node state --------------------
enum NodeState {
  STATE_UNPAIRED = 0,   // no mothership MAC known
  STATE_PAIRED   = 1,   // has mothership MAC, but not deployed
  STATE_DEPLOYED = 2    // has mothership MAC + deployed flag set
};

static NodeState nodeState = STATE_UNPAIRED;

// -------------------- Persistent config in RTC RAM --------------------
RTC_DATA_ATTR int       bootCount        = 0;
RTC_DATA_ATTR bool      rtcSynced        = false;
RTC_DATA_ATTR uint8_t   mothershipMAC[6] = {0};
RTC_DATA_ATTR uint8_t   g_intervalMin    = 1;
RTC_DATA_ATTR bool      deployedFlag     = false;
// New: unix time of last successful TIME_SYNC (also mirrored into NVS)
RTC_DATA_ATTR uint32_t  lastTimeSyncUnix = 0;

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
    p2.end();
    Serial.printf("🔍 NVS verify: state=%u deployed=%d rtc_synced=%d msmac='%s' lastSyncUnix=%lu\n",
                  st, dep, rs, ms.c_str(), (unsigned long)ls);
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
    memset(mothershipMAC, 0, sizeof(mothershipMAC));
    return;
  }

  uint8_t rawState   = p.getUChar("state", (uint8_t)STATE_UNPAIRED);
  rtcSynced          = p.getBool ("rtc_synced", false);
  deployedFlag       = p.getBool ("deployed",   false);
  g_intervalMin      = p.getUChar("interval",   1);
  lastTimeSyncUnix   = p.getULong("lastSync",   0);

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

  Serial.printf("💾 Node config loaded from NVS: state=%u, rtcSynced=%d, deployed=%d, interval=%u, msmac='%s'\n",
                (unsigned)nodeState, rtcSynced, deployedFlag, g_intervalMin, macHex.c_str());

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
static void sendSensorData();

static void onDataSent(const uint8_t *mac_addr, esp_now_send_status_t status) {
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
      rtcSynced        = true;
      deployedFlag     = true;
      lastTimeSyncUnix = rtc.now().unixtime();   // treat deploy time as fresh sync
      memcpy(mothershipMAC, mac, 6);
      persistNodeConfig();

      Serial.print("RTC synchronized to: ");
      Serial.println(rtc.now().timestamp());
      Serial.printf("⏰ lastTimeSyncUnix set to %lu at DEPLOY\n",
                    (unsigned long)lastTimeSyncUnix);
      Serial.println("✅ Node deployed; ready for alarm-driven sends");
      debugState("after DEPLOY");

      // Arm first RTC alarm based on current interval
      bool ok = false;
      DateTime next;
      if (g_intervalMin <= 1) {
        ok = ds3231EveryMinute();
        DateTime now = rtc.now();
        DateTime nextDisplay = (now.second() == 0)
            ? now + TimeSpan(0,0,1,0)
            : DateTime(now.year(),now.month(),now.day(),now.hour(),now.minute(),0)
              + TimeSpan(0,0,1,0);
        next = nextDisplay;
      } else {
        ok = ds3231ArmNextInNMinutes(g_intervalMin, &next);
      }
      ds3231EnableAlarmInterrupt();
      clearDS3231_A1F();  // ensure flag low until first alarm

      char nextStr[24]; formatTime(next, nextStr, sizeof(nextStr));
      Serial.printf("[DEPLOY] First alarm armed for %s (ok=%d, interval=%u min)\n",
                    nextStr, ok, g_intervalMin);

      // Optional: initial reading right after deploy
      Serial.println("📤 Initial post-deploy reading…");
      sendSensorData();
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
      persistNodeConfig();
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

      bool ok = false;
      DateTime next;
      if (g_intervalMin <= 1) {
        ok = ds3231EveryMinute();
        DateTime now = rtc.now();
        DateTime nextDisplay = (now.second() == 0)
            ? now + TimeSpan(0,0,1,0)
            : DateTime(now.year(),now.month(),now.day(),now.hour(),now.minute(),0)
              + TimeSpan(0,0,1,0);
        next = nextDisplay;
      } else {
        ok = ds3231ArmNextInNMinutes(g_intervalMin, &next);
      }

      ds3231EnableAlarmInterrupt();
      clearDS3231_A1F();   // flag low until first match

      char nowStr[24], nextStr[24];
      formatTime(rtc.now(), nowStr, sizeof(nowStr));
      formatTime(next,       nextStr, sizeof(nextStr));

      Serial.println("[SET_SCHEDULE] received");
      Serial.printf("   interval: %u -> %u minutes\n", oldInterval, g_intervalMin);
      Serial.printf("   now:  %s\n",  nowStr);
      Serial.printf("   next: %s\n",  nextStr);
      Serial.printf("   status: %s\n", ok ? "OK" : "FAIL");

      persistNodeConfig();
    }
    return;
  }

  // 7) Time sync response from mothership
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
}

// ==================== Actions ======================
static void sendTimeSyncRequest() {
  static const uint8_t broadcastAddress[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};

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

  pairing_request_t m{};
  strcpy(m.command, "PAIRING_REQUEST");
  strcpy(m.nodeId,  NODE_ID);

  esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);
  esp_err_t res = esp_now_send(broadcastAddress, (uint8_t*)&m, sizeof(m));
  Serial.println(res == ESP_OK ? "📋 Pairing status request sent" : "❌ Pairing request failed");
}

static void sendSensorData() {
  NodeState s = currentNodeState();

  // Timestamp for logs
  char nowStr[24];
  formatTime(rtc.now(), nowStr, sizeof(nowStr));

  Serial.printf("📤 sendSensorData() @ %s | state=%d rtcSynced=%d hasMS=%d\n",
                nowStr, (int)s, rtcSynced, hasMothershipMAC());

  if (s != STATE_DEPLOYED || !rtcSynced || !hasMothershipMAC()) {
    Serial.println("⚠️ Not DEPLOYED / RTC unsynced / no mothership — skipping data send");
    return;
  }

  // Dummy reading
  float temperature = 20.0 + (random(0, 200) / 10.0f);

  sensor_data_message_t msg{};
  strcpy(msg.nodeId,     NODE_ID);
  strcpy(msg.sensorType, NODE_TYPE);
  msg.value         = temperature;
  msg.nodeTimestamp = rtc.now().unixtime();

  esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);
  esp_err_t res = esp_now_send(mothershipMAC, (uint8_t*)&msg, sizeof(msg));

  if (res == ESP_OK) {
    Serial.print("📊 Sensor packet sent → temp = ");
    Serial.print(temperature);
    Serial.println(" °C");

    Serial.printf("   → sent to mothership MAC %02X:%02X:%02X:%02X:%02X:%02X\n",
                  mothershipMAC[0], mothershipMAC[1], mothershipMAC[2],
                  mothershipMAC[3], mothershipMAC[4], mothershipMAC[5]);
    Serial.println("   (Mothership should now log this row to CSV)");
  } else {
    Serial.print("❌ Failed to send data, esp_err = ");
    Serial.println(esp_err_to_name(res));
  }
}


// -------------------- Per-alarm handler --------------------
static void handleRtcAlarmEvent() {
  DateTime fired = rtc.now();
  char firedStr[24];
  formatTime(fired, firedStr, sizeof(firedStr));
  uint8_t a1fBefore = readDS3231_A1F();

  Serial.println("⚡ DS3231 alarm detected → "
                 "simulating FET ON / LED ON / node power APPLIED");
  Serial.printf("⏰ RTC alarm context @ %s  | A1F(before)=%u\n",
                firedStr, a1fBefore);

  // 1) Use this alarm as the trigger to send data (while "power is ON")
  NodeState s = currentNodeState();
  if (s == STATE_DEPLOYED && rtcSynced && hasMothershipMAC()) {
    Serial.println("📤 Alarm → sending sensor data (node is 'powered' in simulation)");
    sendSensorData();
  } else {
    Serial.println("⚠️ Alarm but node not ready (not DEPLOYED / RTC unsynced / no mothership)");
  }

  // 2) Re-arm the next alarm based on g_intervalMin
  bool ok = false;
  DateTime next;
  if (g_intervalMin <= 1) {
    ok = ds3231EveryMinute();
    DateTime now = rtc.now();
    DateTime nextDisplay = (now.second() == 0)
        ? now + TimeSpan(0,0,1,0)
        : DateTime(now.year(),now.month(),now.day(),now.hour(),now.minute(),0)
          + TimeSpan(0,0,1,0);
    next = nextDisplay;
  } else {
    ok = ds3231ArmNextInNMinutes(g_intervalMin, &next);
  }
  ds3231EnableAlarmInterrupt();

  char nextStr[24];
  formatTime(next, nextStr, sizeof(nextStr));
  Serial.printf("   🔁 Next alarm armed at %s (%s)\n",
                nextStr, ok ? "OK" : "FAIL");

  // 3) NOW clear A1F so INT/SQW goes HIGH again
  clearDS3231_A1F();
  delay(5);

  uint8_t a1fAfter = readDS3231_A1F();
  Serial.printf("🔚 DS3231 A1F cleared → A1F(after)=%u\n", a1fAfter);
  Serial.println("   → Simulated behaviour: FET OFF / LED OFF / node power CUT");
}



// ==================== Setup/Loop ====================
void setup() {
  Serial.begin(115200);
  delay(2000);

  initNVS();
  loadNodeConfig();
  bootCount++;

  Serial.println("====================================");
  Serial.print("🌡️ Air Temperature Node: "); Serial.println(NODE_ID);
  Serial.print("Boot #"); Serial.println(bootCount);
  Serial.print("MAC: "); Serial.println(WiFi.macAddress());
  debugState("after setup load");
  Serial.println("====================================");

  // I2C + RTC
  Wire.begin(RTC_SDA_PIN, RTC_SCL_PIN);  // pins from protocol.h
  if (!rtc.begin()) {
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

    // Normalise A1F at boot so we don't start on a stale alarm
    uint8_t a1fInit = readDS3231_A1F();
    if (a1fInit == 0xFF) {
      Serial.println("⚠️ readDS3231_A1F() at boot returned 0xFF (I2C error?)");
    } else if (a1fInit == 1) {
      Serial.println("⚠️ A1F was already set at boot → clearing so next alarm edge is visible");
      clearDS3231_A1F();
      uint8_t a1fPost = readDS3231_A1F();
      Serial.printf("   ↪ A1F(after clear)=%u\n", a1fPost);
    } else {
      Serial.println("[RTC] A1F=0 at boot (idle)");
    }
  }

  // No RTC_INT_PIN or ISR — INT only drives FET + LED in hardware.

  // WiFi / ESPNOW
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  delay(1000);

  if (esp_now_init() != ESP_OK) {
    Serial.println("❌ ESP-NOW init failed");
    return;
  }
  Serial.println("✅ ESP-NOW initialized");

  esp_now_register_send_cb(onDataSent);
  esp_now_register_recv_cb(onDataReceived);

  // Broadcast peer for discovery
  {
    esp_now_peer_info_t pi{};
    static const uint8_t broadcastAddress[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
    memcpy(pi.peer_addr, broadcastAddress, 6);
    pi.channel = ESPNOW_CHANNEL;
    pi.ifidx   = WIFI_IF_STA;
    pi.encrypt = false;
    esp_now_add_peer(&pi);
  }

  // Preloaded mothership peer (only if we already know MAC from NVS)
  if (hasMothershipMAC()) {
    esp_now_peer_info_t pi{};
    memcpy(pi.peer_addr, mothershipMAC, 6);
    pi.channel = ESPNOW_CHANNEL;
    pi.ifidx   = WIFI_IF_STA;
    pi.encrypt = false;

    esp_now_del_peer(mothershipMAC);
    if (esp_now_add_peer(&pi) == ESP_OK) {
      Serial.print("✅ Preloaded mothership peer: ");
      for (int i = 0; i < 6; ++i) {
        if (i) Serial.print(":");
        Serial.printf("%02X", mothershipMAC[i]);
      }
      Serial.println();
    } else {
      Serial.println("❌ Failed to add mothership peer");
    }
  }

  persistNodeConfig();
  Serial.println("🔁 Setup persisted baseline node config");
  debugState("end of setup");
}

void loop() {
  static unsigned long lastAction      = 0;
  static unsigned long lastTimeSyncReq = 0;
  unsigned long nowMs = millis();

  NodeState st = currentNodeState();

  // If we are bound but RTC isn't synced, occasionally ask for time.
  if (hasMothershipMAC() && !rtcSynced) {
    if (nowMs - lastTimeSyncReq > 30000UL) {
      Serial.println("⏰ Bound but RTC unsynced → requesting initial TIME_SYNC");
      sendTimeSyncRequest();
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
        lastTimeSyncReq = nowMs;
      }
    }
  }

  // --- RTC alarm handling (simple A1F poll, like old version) ---
  {
    uint8_t a1 = readDS3231_A1F();
    if (a1 == 0xFF) {
      static unsigned long lastErr = 0;
      if (nowMs - lastErr > 5000UL) {
        Serial.println("⚠️ readDS3231_A1F() error (0xFF)");
        lastErr = nowMs;
      }
    } else if (a1 == 1) {
      Serial.println("⚡ A1F=1 → handleRtcAlarmEvent()");
      handleRtcAlarmEvent();
      // handleRtcAlarmEvent() will clear A1F, so next poll should see 0
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
