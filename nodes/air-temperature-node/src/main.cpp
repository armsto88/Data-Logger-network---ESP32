#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_sleep.h>
#include <Wire.h>
#include <RTClib.h>
#include <esp_wifi.h>

#include "protocol.h"     // pins, ESPNOW_CHANNEL, protocol structs
#include <Preferences.h>

#ifndef FW_BUILD
  #define FW_BUILD __DATE__ " " __TIME__
#endif

// -------------------- Node config --------------------
#define NODE_ID   "TEMP_001"
#define NODE_TYPE "temperature"

// Hardware
RTC_DS3231 rtc;

static inline void formatTime(const DateTime& dt, char* out, size_t n) {
  snprintf(out, n, "%04d-%02d-%02d %02d:%02d:%02d",
           dt.year(), dt.month(), dt.day(),
           dt.hour(), dt.minute(), dt.second());
}

// ---- DS3231 INT/SQW monitoring (active-LOW, open-drain) ----
volatile bool g_alarmFallingISR = false;  // set by ISR when INT goes LOW

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
  // read STATUS
  Wire.beginTransmission(0x68);
  Wire.write(0x0F);
  Wire.endTransmission(false);
  Wire.requestFrom(0x68, 1);
  uint8_t s = Wire.available() ? Wire.read() : 0;

  // clear A1F (bit0)
  s &= ~0x01;

  // write STATUS back
  Wire.beginTransmission(0x68);
  Wire.write(0x0F);
  Wire.write(s);
  Wire.endTransmission();
}

// INT/SQW falling-edge ISR
void IRAM_ATTR rtcIntISR() {
  g_alarmFallingISR = true;
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

  // Enable interrupt on alarm
  // CTRL: INTCN=1 (bit2), A1IE=1 (bit0)
  Wire.beginTransmission(0x68);
  Wire.write(0x0E); // CTRL
  Wire.endTransmission(false);
  Wire.requestFrom(0x68, 1);
  uint8_t ctrl = Wire.available() ? Wire.read() : 0;
  ctrl |= 0b00000101; // INTCN|A1IE

  Wire.beginTransmission(0x68);
  Wire.write(0x0E);
  Wire.write(ctrl);
  Wire.endTransmission();

  // Clear A1F before arming
  clearDS3231_A1F();

  return ds3231WriteA1(A1SEC, A1MIN, A1HOUR, A1DAY);
}

// Compute next trigger (aligned to interval; seconds=00), program A1,
// and optionally return the computed "next" DateTime via *nextOut.
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

  // ---- Program A1: match sec=00, min, hour; ignore day/date ----
  const uint8_t secBCD  = 0x00; // A1M1=0
  const uint8_t minBCD  = uint8_t(((next.minute()/10)<<4) | (next.minute()%10)); // A1M2=0
  const uint8_t hourBCD = uint8_t(((next.hour()/10)<<4)   | (next.hour()%10));   // A1M3=0 (24h)
  const uint8_t dayReg  = 0b10000000; // A1M4=1 ignore day/date

  // Enable INTCN|A1IE (preserve other bits)
  Wire.beginTransmission(0x68); Wire.write(0x0E); Wire.endTransmission(false);
  Wire.requestFrom(0x68, 1);
  uint8_t ctrl = Wire.available() ? Wire.read() : 0;
  ctrl |= 0b00000101;
  Wire.beginTransmission(0x68); Wire.write(0x0E); Wire.write(ctrl); Wire.endTransmission();

  // Clear pending alarm
  clearDS3231_A1F();

  // Debug: print the next time
  char buf[24]; formatTime(next, buf, sizeof(buf));
  Serial.print("[A1] Next alarm at "); Serial.println(buf);

  if (nextOut) *nextOut = next;
  return ds3231WriteA1(secBCD, minBCD, hourBCD, dayReg);
}
// Node States (for logging / loop logic only – no separate stored state)
enum NodeState {
  STATE_UNPAIRED = 0,   // no mothership MAC known
  STATE_PAIRED   = 1,   // has mothership MAC, but not deployed
  STATE_DEPLOYED = 2    // has mothership MAC + RTC synced (deployed)
};

// -------------------- Persistent config in RTC RAM --------------------
RTC_DATA_ATTR int       bootCount     = 0;
RTC_DATA_ATTR bool      rtcSynced     = false;     // true once DEPLOY_NODE or TIME_SYNC sets RTC
RTC_DATA_ATTR uint8_t   mothershipMAC[6] = {0};    // 0 = not bound to any mothership yet
RTC_DATA_ATTR uint8_t   g_intervalMin = 1;         // default 1 minute
RTC_DATA_ATTR NodeState nodeState = STATE_UNPAIRED;


// --- Derived state helpers (simpler model) ---
static bool hasMothershipMAC() {
  for (int i = 0; i < 6; ++i) {
    if (mothershipMAC[i] != 0) return true;
  }
  return false;
}


static NodeState currentNodeState() {
  if (!hasMothershipMAC()) return STATE_UNPAIRED;
  if (!rtcSynced)          return STATE_PAIRED;    // bound but not deployed
  return STATE_DEPLOYED;                           // bound + deployed
}

// ---------- Persistent config (NVS) ----------
static void persistNodeConfig() {
  Preferences p;
  if (!p.begin("node_cfg", /*readWrite=*/true)) {
    Serial.println("⚠️ persistNodeConfig: begin() failed");
    return;
  }

  p.putString("fw", FW_BUILD);
  p.putBool("rtc_synced", rtcSynced);
  p.putUChar("interval", g_intervalMin);

  // store mothership MAC as 12-char hex string
  char macHex[13];
  snprintf(macHex, sizeof(macHex),
           "%02X%02X%02X%02X%02X%02X",
           mothershipMAC[0], mothershipMAC[1], mothershipMAC[2],
           mothershipMAC[3], mothershipMAC[4], mothershipMAC[5]);
  p.putString("msmac", macHex);

  p.end();
  Serial.println("💾 Node config persisted to NVS");
}

static void loadNodeConfig() {
  Preferences p;
  if (!p.begin("node_cfg", /*readWrite=*/true)) {
    Serial.println("⚠️ loadNodeConfig: begin() failed");
    return;
  }

  String prevFw = p.getString("fw", "");
  if (prevFw != String(FW_BUILD)) {
    // New firmware → reset soft state
    Serial.println("🧽 New firmware detected — resetting node binding + RTC sync");
    rtcSynced = false;
    g_intervalMin = 1;
    memset(mothershipMAC, 0, sizeof(mothershipMAC));
    p.putString("fw", FW_BUILD);
    p.putBool("rtc_synced", rtcSynced);
    p.putUChar("interval", g_intervalMin);
    p.putString("msmac", "");
    p.end();
    return;
  }

  rtcSynced     = p.getBool("rtc_synced", false);
  g_intervalMin = p.getUChar("interval", g_intervalMin);

  String macHex = p.getString("msmac", "");
  if (macHex.length() == 12) {
    for (int i = 0; i < 6; ++i) {
      String b = macHex.substring(i*2, i*2+2);
      mothershipMAC[i] = (uint8_t) strtoul(b.c_str(), nullptr, 16);
    }
  }

  p.end();

  NodeState st = currentNodeState();
  Serial.print("💾 Node config loaded from NVS: state=");
  if (st == STATE_UNPAIRED) Serial.print("UNPAIRED");
  else if (st == STATE_PAIRED) Serial.print("PAIRED");
  else Serial.print("DEPLOYED");
  Serial.print(", interval="); Serial.println(g_intervalMin);
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

  // 1) Discovery response
  if (len == sizeof(discovery_response_t)) {
    discovery_response_t resp;
    memcpy(&resp, incomingData, sizeof(resp));

    if (strcmp(resp.command, "DISCOVER_RESPONSE") == 0) {
      Serial.print("📡 Discovered by: ");
      Serial.println(resp.mothership_id);

      // Remember mothership MAC
      memcpy(mothershipMAC, mac, 6);

      // Ensure we have the mothership as a peer
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
    }
    else if (strcmp(resp.command, "DISCOVERY_SCAN") == 0) {
      // Always respond so UI can see us even if states drift
      Serial.println("🔍 Responding to discovery scan…");
      sendDiscoveryRequest();
    }
    return;
  }

  // 2) Pairing response (poll-based pairing status)
  if (len == sizeof(pairing_response_t)) {
    pairing_response_t pr;
    memcpy(&pr, incomingData, sizeof(pr));

    if (strcmp(pr.command, "PAIRING_RESPONSE") == 0 && strcmp(pr.nodeId, NODE_ID) == 0) {
      if (pr.isPaired) {
        Serial.println("📋 Pairing confirmed via PAIRING_RESPONSE");
        memcpy(mothershipMAC, mac, 6);
        persistNodeConfig();
      } else {
        Serial.println("📋 Still unpaired; continuing discovery…");
      }
    }
    return;
  }

if (len == sizeof(pairing_command_t)) {
  pairing_command_t pc;
  memcpy(&pc, incomingData, sizeof(pc));

  if (strcmp(pc.command, "PAIR_NODE") == 0 && strcmp(pc.nodeId, NODE_ID) == 0) {
    Serial.println("📋 Direct PAIR_NODE command received");

    // We’re still bound to this mothership
    memcpy(mothershipMAC, mac, 6);

    // ⇩⇩ KEY LINES ⇩⇩
    // Going back to "paired but NOT deployed":
    rtcSynced = false;           // deployment is no longer valid
    nodeState = STATE_PAIRED;    // if you still use nodeState in your loop

    // Persist updated config/state
    persistNodeConfig();
    Serial.println("💾 Node state persisted after PAIR_NODE (rtcSynced=false, STATE_PAIRED)");
  }
  return;
}



  // 3) Deployment command (RTC time set)
  if (len == sizeof(deployment_command_t)) {
    deployment_command_t dc;
    memcpy(&dc, incomingData, sizeof(dc));

    if (strcmp(dc.command, "DEPLOY_NODE") == 0 && strcmp(dc.nodeId, NODE_ID) == 0) {
      Serial.println("🚀 Deployment command received");

      rtc.adjust(DateTime(dc.year, dc.month, dc.day, dc.hour, dc.minute, dc.second));
      rtcSynced = true;
      memcpy(mothershipMAC, mac, 6);
      persistNodeConfig();      // ensure we remember RTC synced + MAC

      Serial.print("RTC synchronized to: ");
      Serial.println(rtc.now().timestamp());
      Serial.println("✅ Node deployed; will start transmitting data");
    }
    return;
  }

  // 4) Remote unpair
if (len == sizeof(unpair_command_t)) {
  unpair_command_t uc;
  memcpy(&uc, incomingData, sizeof(uc));

  if (strcmp(uc.command, "UNPAIR_NODE") == 0) {
    Serial.println("🗑️ UNPAIR received");

    memset(mothershipMAC, 0, sizeof(mothershipMAC)); // forget who we belong to
    rtcSynced = false;                                // definitely not deployed
    nodeState = STATE_UNPAIRED;                       // for any code using nodeState

    persistNodeConfig();
    Serial.println("💾 Node config persisted after UNPAIR");
    sendDiscoveryRequest();                           // announce ourselves again
  }
  return;
}



  // 5) Schedule / interval command (set DS3231 Alarm)
  if (len == sizeof(schedule_command_message_t)) {
    schedule_command_message_t cmd;
    memcpy(&cmd, incomingData, sizeof(cmd));

    if (strcmp(cmd.command, "SET_SCHEDULE") == 0) {
      uint8_t oldInterval = g_intervalMin;
      g_intervalMin = (uint8_t)cmd.intervalMinutes;

      String dbg;
      bool ok;
      DateTime next;
      if (g_intervalMin <= 1) {
        ok = ds3231EveryMinute();
        dbg += F("Mode: every minute (sec==00, ignore min/hour/day)\n");
        // Predict next (:00) for display only
        DateTime now = rtc.now();
        DateTime nextDisplay = (now.second() == 0)
            ? now + TimeSpan(0,0,1,0)  // if exactly at :00, show next minute
            : DateTime(now.year(),now.month(),now.day(),now.hour(),now.minute(),0) + TimeSpan(0,0,1,0);
        next = nextDisplay;
      } else {
        ok = ds3231ArmNextInNMinutes(g_intervalMin, &next);
        dbg += F("Mode: N-minute interval with re-arm on each alarm\n");
      }

      char nowStr[24], nextStr[24];
      formatTime(rtc.now(), nowStr, sizeof(nowStr));
      formatTime(next,        nextStr, sizeof(nextStr));

      Serial.println("[SET_SCHEDULE] received");
      Serial.printf("   interval: %u -> %u minutes\n", oldInterval, g_intervalMin);
      Serial.printf("   now:  %s\n",  nowStr);
      Serial.printf("   next: %s\n",  nextStr);
      Serial.print(dbg);
      Serial.printf("   status: %s\n", ok ? "OK" : "FAIL");

      persistNodeConfig();
    }
    return;
  }

  // 6) Time sync response from mothership
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

      rtc.adjust(dt);
      rtcSynced = true;
      persistNodeConfig();

      char buf[24];
      formatTime(dt, buf, sizeof(buf));
      Serial.print("⏰ TIME_SYNC received, RTC set to ");
      Serial.println(buf);
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
  NodeState s = currentNodeState();   // uses hasMothershipMAC() + rtcSynced

  if (s != STATE_DEPLOYED) {
    Serial.println("⚠️ Not deployed — skipping data");
    return;
  }

  // dummy reading
  float temperature = 20.0 + (random(0, 200) / 10.0f);

  sensor_data_message_t msg{};
  strcpy(msg.nodeId,     NODE_ID);
  strcpy(msg.sensorType, NODE_TYPE);
  msg.value         = temperature;
  msg.nodeTimestamp = rtc.now().unixtime();

  esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);
  esp_err_t res = esp_now_send(mothershipMAC, (uint8_t*)&msg, sizeof(msg));
  if (res == ESP_OK) {
    Serial.print("📊 Sent temp = ");
    Serial.print(temperature);
    Serial.println(" °C");
  } else {
    Serial.println("❌ Failed to send data");
  }
}


// ==================== Setup/Loop ====================
void setup() {
  Serial.begin(115200);
  delay(2000); // CDC on C3 needs a moment

  // Load persisted config (rtcSynced / interval / mothership MAC / fw)
  loadNodeConfig();
  bootCount++;

  Serial.println("====================================");
  Serial.print("🌡️ Air Temperature Node: "); Serial.println(NODE_ID);
  Serial.print("Boot #"); Serial.println(bootCount);
  Serial.print("MAC: "); Serial.println(WiFi.macAddress());
  Serial.print("State: ");
  NodeState st = currentNodeState();
  if (st == STATE_UNPAIRED)      Serial.println("UNPAIRED");
  else if (st == STATE_PAIRED)   Serial.println("PAIRED");
  else                           Serial.println("DEPLOYED");
  Serial.println("====================================");

  // I2C + RTC
  Wire.begin(RTC_SDA_PIN, RTC_SCL_PIN);  // pins from protocol.h
  if (!rtc.begin()) {
    Serial.println("❌ RTC not found!");
  } else {
    Serial.println("✅ RTC initialized");

    if (rtc.lostPower()) {
      Serial.println("⚠️ RTC lost power since last run");
      rtcSynced = false;         // whatever was saved is now stale
      persistNodeConfig();       // update NVS
    } else if (rtcSynced) {
      Serial.print("RTC Time: ");
      Serial.println(rtc.now().timestamp());
    } else {
      Serial.println("RTC not synchronized yet");
    }
  }

  // INT/SQW -> GPIO input with pull-up (INT is open-drain, idle HIGH, active LOW)
  pinMode(RTC_INT_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(RTC_INT_PIN), rtcIntISR, FALLING);

  // Optional boot diagnostics
  uint8_t a1f0 = readDS3231_A1F();
  Serial.printf("[RTC_INT] boot: pin=%d level=%d A1F=%u\n",
                RTC_INT_PIN, digitalRead(RTC_INT_PIN), a1f0);

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
}

void loop() {
  static unsigned long lastAction = 0;
  static unsigned long lastTimeSyncReq = 0;
  unsigned long nowMs = millis();

  NodeState st = currentNodeState();

  // If we are bound but RTC isn't synced (e.g. RTC lost power),
  // occasionally ask the mothership for time.
  if (st == STATE_PAIRED && !rtcSynced && hasMothershipMAC()) {
    if (nowMs - lastTimeSyncReq > 30000UL) { // every 30s max
      Serial.println("⏰ Bound but RTC unsynced → requesting TIME_SYNC");
      sendTimeSyncRequest();
      lastTimeSyncReq = nowMs;
    }
  }

  // Handle RTC alarm interrupt line toggling LOW
  if (g_alarmFallingISR) {
    g_alarmFallingISR = false;

    // Timestamp when we noticed the interrupt
    DateTime fired = rtc.now();
    char firedStr[24]; formatTime(fired, firedStr, sizeof(firedStr));
    uint8_t a1fBefore = readDS3231_A1F();

    Serial.printf("⏰ INT fell @ %s  | A1F(before)=%u\n", firedStr, a1fBefore);

    clearDS3231_A1F();  // release INT line
    delay(2);

    if (g_intervalMin > 1) {
      DateTime next;
      bool ok = ds3231ArmNextInNMinutes(g_intervalMin, &next);
      char nextStr[24]; formatTime(next, nextStr, sizeof(nextStr));
      Serial.printf("   re-armed +%u min → next %s (%s)\n",
                    g_intervalMin, nextStr, ok ? "OK" : "FAIL");
    } else {
      // Every-minute mode—next will be the next :00
      DateTime now = rtc.now();
      DateTime nxt = (now.second() == 0)
          ? now + TimeSpan(0,0,1,0)
          : DateTime(now.year(),now.month(),now.day(),now.hour(),now.minute(),0) + TimeSpan(0,0,1,0);
      char nextStr[24]; formatTime(nxt, nextStr, sizeof(nextStr));
      Serial.printf("   1-min mode re-armed → next %s\n", nextStr);
    }
  }

  if (st == STATE_UNPAIRED) {
    if (nowMs - lastAction > 10000UL) {
      Serial.println("🔍 Searching for motherships…");
      sendDiscoveryRequest();
      delay(1000);
      Serial.println("📞 Polling pairing status…");
      sendPairingRequest();
      lastAction = nowMs;
    }
  } else if (st == STATE_PAIRED) {
    if (nowMs - lastAction > 5000UL) {
      Serial.println("🟡 Bound, waiting for DEPLOY command…");
      lastAction = nowMs;
    }
  } else { // STATE_DEPLOYED
    if (nowMs - lastAction > 30000UL) { // demo fixed 30s
      Serial.println("🟢 Deployed — sending sensor data…");
      sendSensorData();
      lastAction = nowMs;

      // In production you’d deep sleep and use RTC INT pin wake.
      // esp_deep_sleep(intervalSeconds * 1000000ULL);
    }
  }

  delay(100);
}
