#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_sleep.h>
#include <Wire.h>
#include <RTClib.h>
#include <esp_wifi.h>

#include "protocol.h"     // one source of truth for all message structs + pins + channel
#include "rtc_manager.h"  // setDS3231WakeInterval(...)


// -------------------- Node config --------------------
#define NODE_ID   "TEMP_001"
#define NODE_TYPE "temperature"

// Node States (node-internal)
enum NodeState {
  STATE_UNPAIRED = 0,
  STATE_PAIRED   = 1,
  STATE_DEPLOYED = 2
};

// Persist across deep sleep
RTC_DATA_ATTR NodeState nodeState = STATE_UNPAIRED;
RTC_DATA_ATTR int       bootCount = 0;
RTC_DATA_ATTR bool      rtcSynced = false;
RTC_DATA_ATTR uint8_t   mothershipMAC[6] = {0x30, 0xED, 0xA0, 0xAA, 0x67, 0x84}; // seed; will update after discovery

// Hardware
RTC_DS3231 rtc;

// ESP-NOW
esp_now_peer_info_t gPeerInfo; // not strictly required as a global, but fine
bool waitingForTimeSync = false;


// -------------------- Prototypes --------------------
static void onDataSent(const uint8_t *mac_addr, esp_now_send_status_t status);
static void onDataReceived(const uint8_t *mac, const uint8_t *incomingData, int len);
static void sendDiscoveryRequest();
static void sendPairingRequest();
static void sendSensorData();


// ==================== Callbacks =====================
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
    }
    else if (strcmp(resp.command, "DISCOVERY_SCAN") == 0) {
      // Mothership is scanning; if we’re unpaired, announce ourselves
      if (nodeState == STATE_UNPAIRED) {
        Serial.println("🔍 Responding to discovery scan…");
        sendDiscoveryRequest();
      }
    }
    return;
  }

  // 2) Pairing response
  if (len == sizeof(pairing_response_t)) {
    pairing_response_t pr;
    memcpy(&pr, incomingData, sizeof(pr));

    if (strcmp(pr.command, "PAIRING_RESPONSE") == 0 && strcmp(pr.nodeId, NODE_ID) == 0) {
      if (pr.isPaired) {
        Serial.println("📋 Pairing confirmed!");
        nodeState = STATE_PAIRED;
        memcpy(mothershipMAC, mac, 6);
      } else {
        Serial.println("📋 Still unpaired; continuing discovery…");
      }
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
      rtcSynced  = true;
      nodeState  = STATE_DEPLOYED;
      memcpy(mothershipMAC, mac, 6);

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
      memset(mothershipMAC, 0, sizeof(mothershipMAC));
      nodeState = STATE_UNPAIRED;
      sendDiscoveryRequest();
    }
    return;
  }

  // 5) Schedule / interval command (set DS3231 Alarm)
  if (len == sizeof(schedule_command_message_t)) {
    schedule_command_message_t cmd;
    memcpy(&cmd, incomingData, sizeof(cmd));
    if (strcmp(cmd.command, "SET_SCHEDULE") == 0) {
      String dbg;
      bool ok = setDS3231WakeInterval((uint8_t)cmd.intervalMinutes, dbg, rtc);
      Serial.println("[SET_SCHEDULE] received");
      Serial.print(dbg);  // shows current time, next alarm, registers, flags
      Serial.printf("[SET_SCHEDULE] status: %s\n", ok ? "OK" : "FAIL");
    }
    return;
  }
}


// ==================== Actions ======================
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
  if (nodeState != STATE_DEPLOYED) {
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
  bootCount++;

  Serial.println("====================================");
  Serial.print("🌡️ Air Temperature Node: "); Serial.println(NODE_ID);
  Serial.print("Boot #"); Serial.println(bootCount);
  Serial.print("MAC: "); Serial.println(WiFi.macAddress());
  Serial.print("State: ");
  if (nodeState == STATE_UNPAIRED) Serial.println("UNPAIRED");
  else if (nodeState == STATE_PAIRED) Serial.println("PAIRED");
  else Serial.println("DEPLOYED");
  Serial.println("====================================");

  // I2C + RTC
  Wire.begin(RTC_SDA_PIN, RTC_SCL_PIN);  // pins from protocol.h
  if (!rtc.begin()) {
    Serial.println("❌ RTC not found!");
  } else {
    Serial.println("✅ RTC initialized");
    if (rtcSynced) {
      Serial.print("RTC Time: ");
      Serial.println(rtc.now().timestamp());
    } else {
      Serial.println("RTC not synchronized yet");
    }
  }

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

  // Preloaded mothership peer (optional — will be overwritten on discovery anyway)
  {
    esp_now_peer_info_t pi{};
    memcpy(pi.peer_addr, mothershipMAC, 6);
    pi.channel = ESPNOW_CHANNEL;
    pi.ifidx   = WIFI_IF_STA;
    pi.encrypt = false;
    esp_now_del_peer(mothershipMAC);
    if (esp_now_add_peer(&pi) == ESP_OK) {
      Serial.print("✅ Preloaded mothership peer: ");
      for (int i = 0; i < 6; ++i) { if (i) Serial.print(":"); Serial.printf("%02X", mothershipMAC[i]); }
      Serial.println();
    } else {
      Serial.println("❌ Failed to add mothership peer");
    }
  }
}

void loop() {
  static unsigned long lastAction = 0;
  unsigned long nowMs = millis();

  if (nodeState == STATE_UNPAIRED) {
    if (nowMs - lastAction > 10000UL) {
      Serial.println("🔍 Searching for motherships…");
      sendDiscoveryRequest();
      delay(1000);
      Serial.println("📞 Polling pairing status…");
      sendPairingRequest();
      lastAction = nowMs;
    }
  } else if (nodeState == STATE_PAIRED) {
    if (nowMs - lastAction > 5000UL) {
      Serial.println("🟡 Paired, waiting for DEPLOY command…");
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
