#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>

#include "protocol.h"

#ifndef ESPNOW_CHANNEL
#define ESPNOW_CHANNEL 1
#endif

#ifndef SYNC_INTERVAL_MIN
#define SYNC_INTERVAL_MIN 15
#endif

#ifndef WIFI_ON_WINDOW_MS
#define WIFI_ON_WINDOW_MS 12000
#endif

static const char* DEVICE_ID = "MOCK_MS";
static uint32_t g_lastWindowMs = 0;
static bool g_online = false;

static String macToStr(const uint8_t mac[6]) {
  char b[18];
  snprintf(b, sizeof(b), "%02X:%02X:%02X:%02X:%02X:%02X",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  return String(b);
}

static void onDataRecv(const uint8_t* mac, const uint8_t* incomingData, int len) {
  if (len != sizeof(sensor_data_message_t)) return;

  sensor_data_message_t msg{};
  memcpy(&msg, incomingData, sizeof(msg));

  Serial.printf("[MOCK] RX from %s node=%s id=%u type=%s label=%s value=%.4f ts=%lu qf=0x%04X\n",
                macToStr(mac).c_str(),
                msg.nodeId,
                (unsigned)msg.sensorId,
                msg.sensorType,
                msg.sensorLabel,
                msg.value,
                (unsigned long)msg.nodeTimestamp,
                (unsigned)msg.qualityFlags);
}

static bool bringOnline() {
  if (g_online) return true;

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  delay(80);

  if (esp_now_init() != ESP_OK) {
    Serial.println("[MOCK] esp_now_init failed");
    return false;
  }

  esp_now_register_recv_cb(onDataRecv);

  uint8_t bcast[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
  esp_now_peer_info_t pi{};
  memcpy(pi.peer_addr, bcast, 6);
  pi.channel = ESPNOW_CHANNEL;
  pi.ifidx = WIFI_IF_STA;
  pi.encrypt = false;
  esp_now_add_peer(&pi);

  esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);
  g_online = true;
  Serial.println("[MOCK] WiFi/ESP-NOW ON");
  return true;
}

static void shutdownRadio() {
  if (!g_online) return;
  esp_now_deinit();
  WiFi.mode(WIFI_OFF);
  g_online = false;
  Serial.println("[MOCK] WiFi/ESP-NOW OFF");
}

static void broadcastSyncSchedule() {
  sync_schedule_command_message_t cmd{};
  strcpy(cmd.command, "SET_SYNC_SCHED");
  cmd.syncIntervalMinutes = SYNC_INTERVAL_MIN;
  cmd.phaseUnix = (unsigned long)(millis() / 1000UL);
  strncpy(cmd.mothership_id, DEVICE_ID, sizeof(cmd.mothership_id) - 1);

  uint8_t bcast[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
  esp_err_t r = esp_now_send(bcast, (uint8_t*)&cmd, sizeof(cmd));

  Serial.printf("[MOCK] Broadcast SET_SYNC_SCHED interval=%u phase=%lu status=%s\n",
                (unsigned)SYNC_INTERVAL_MIN,
                (unsigned long)cmd.phaseUnix,
                (r == ESP_OK) ? "OK" : esp_err_to_name(r));
}

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("[MOCK] Mothership sync broadcaster start");

  if (bringOnline()) {
    broadcastSyncSchedule();
    delay(WIFI_ON_WINDOW_MS);
    shutdownRadio();
    g_lastWindowMs = millis();
  }
}

void loop() {
  const uint32_t periodMs = (uint32_t)SYNC_INTERVAL_MIN * 60UL * 1000UL;

  if (millis() - g_lastWindowMs >= periodMs) {
    if (bringOnline()) {
      broadcastSyncSchedule();
      uint32_t t0 = millis();
      while (millis() - t0 < WIFI_ON_WINDOW_MS) {
        delay(20);
      }
      shutdownRadio();
    }
    g_lastWindowMs = millis();
  }

  delay(100);
}
