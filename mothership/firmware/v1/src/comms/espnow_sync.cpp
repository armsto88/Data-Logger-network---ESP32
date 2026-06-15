#include "comms/espnow_sync.h"
#include "system/pins.h"
#include <WiFi.h>
#include <esp_wifi.h>

// Protocol header for sync window messages
#include "protocol.h"

static EspNowRecvCallback gRecvCallback = nullptr;
static int gChannel = ESPNOW_CHANNEL;
static volatile bool gSyncWindowOpen = false;

// Broadcast address for ESP-NOW
static constexpr uint8_t kBroadcastAddr[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

// Internal receive handler (ESP-IDF 4.4 API: mac_addr, data, len)
static void onEspNowRecv(const uint8_t* mac_addr, const uint8_t* data, int len) {
  if (gRecvCallback) {
    gRecvCallback(mac_addr, data, len);
  }
}

bool initEspNowSyncOnly(int channel) {
  gChannel = channel;

  // WiFi STA mode only (no AP)
  WiFi.mode(WIFI_STA);
  esp_wifi_set_channel(static_cast<uint8_t>(gChannel), WIFI_SECOND_CHAN_NONE);
  Serial.printf("[ESP-NOW] STA mode, channel %d\n", gChannel);

  // Init ESP-NOW
  if (esp_now_init() != ESP_OK) {
    Serial.println("[ESP-NOW] Init failed");
    return false;
  }
  Serial.println("[ESP-NOW] Init OK");

  // Register receive callback
  if (esp_now_register_recv_cb(onEspNowRecv) != ESP_OK) {
    Serial.println("[ESP-NOW] Register recv callback failed");
    return false;
  }

  // Add broadcast peer so we can send sync window announcements
  esp_now_peer_info_t peerInfo = {};
  memcpy(peerInfo.peer_addr, kBroadcastAddr, 6);
  peerInfo.channel = static_cast<uint8_t>(gChannel);
  peerInfo.ifidx = WIFI_IF_STA;
  if (esp_now_add_peer(&peerInfo) != ESP_OK) {
    Serial.println("[ESP-NOW] Add broadcast peer failed (may already exist)");
  }

  Serial.println("[ESP-NOW] Sync-only mode ready");
  return true;
}

void broadcastSyncWindowOpen() {
  // Send a SYNC_WINDOW_OPEN broadcast so nodes know the mothership is listening
  sync_schedule_command_message_t msg = {};
  strncpy(msg.command, "SYNC_WINDOW_OPEN", sizeof(msg.command) - 1);
  strncpy(msg.mothership_id, "M001", sizeof(msg.mothership_id) - 1);
  msg.syncIntervalMinutes = 0;
  msg.phaseUnix = 0;

  esp_err_t result = esp_now_send(kBroadcastAddr, reinterpret_cast<const uint8_t*>(&msg), sizeof(msg));
  if (result == ESP_OK) {
    Serial.println("[ESP-NOW] SYNC_WINDOW_OPEN broadcast sent");
    gSyncWindowOpen = true;
  } else {
    Serial.printf("[ESP-NOW] SYNC_WINDOW_OPEN broadcast failed: %d\n", result);
  }
}

void registerReceiveCallback(EspNowRecvCallback cb) {
  gRecvCallback = cb;
}

void espnowSyncLoop() {
  // Currently a no-op — all receive handling is via the ESP-NOW callback.
  // This function exists as a placeholder for future queue-drain or
  // periodic sync window management logic.
  // The main loop should call this periodically during the sync window.
}