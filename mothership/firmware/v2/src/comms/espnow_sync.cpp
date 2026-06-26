#include "comms/espnow_sync.h"
#include "storage/flash_logger.h"  // DecodedSnapshot, decodeV2, decodedToV1
#include "system/pins.h"
#include <WiFi.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

// Protocol header for sync window messages
#include "protocol.h"

static EspNowRecvCallback gRecvCallback = nullptr;
static int gChannel = ESPNOW_CHANNEL;
static volatile bool gSyncWindowOpen = false;
static QueueHandle_t gSnapQueue = nullptr;
static int gSnapQueueDepth = 8;
static volatile uint32_t gSnapDropCount = 0;

// Broadcast address for ESP-NOW
static constexpr uint8_t kBroadcastAddr[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

// Internal receive handler (ESP-IDF 4.4 API: mac_addr, data, len)
static void onEspNowRecv(const uint8_t* mac_addr, const uint8_t* data, int len) {
  if (gSnapQueue && mac_addr && data) {
    // V2 snapshot (NODE_SNAPSHOT2) — variable length.
    if (isV2Snapshot(data, len)) {
      DecodedSnapshot decoded;
      if (decodeV2(data, len, decoded)) {
        EspNowSnapSlot slot{};
        memcpy(slot.mac, mac_addr, sizeof(slot.mac));
        decodedToV1(decoded, slot.snap);

        if (xQueueSendToBack(gSnapQueue, &slot, 0) != pdTRUE) {
          EspNowSnapSlot discarded{};
          xQueueReceive(gSnapQueue, &discarded, 0);
          xQueueSendToBack(gSnapQueue, &slot, 0);
          ++gSnapDropCount;
        }
        return;
      }
    }

    // V1 snapshot (NODE_SNAPSHOT) — fixed 124 bytes.
    if (len >= static_cast<int>(sizeof(node_snapshot_t))) {
      const node_snapshot_t* snap = reinterpret_cast<const node_snapshot_t*>(data);
      if (strncmp(snap->command, "NODE_SNAPSHOT", 15) == 0) {
        EspNowSnapSlot slot{};
        memcpy(slot.mac, mac_addr, sizeof(slot.mac));
        memcpy(&slot.snap, snap, sizeof(slot.snap));

        if (xQueueSendToBack(gSnapQueue, &slot, 0) != pdTRUE) {
          // Keep the newest field reading: discard one oldest slot and retry.
          EspNowSnapSlot discarded{};
          xQueueReceive(gSnapQueue, &discarded, 0);
          xQueueSendToBack(gSnapQueue, &slot, 0);
          ++gSnapDropCount;
        }
        return;
      }
    }
  }

  // Backward compatibility for bring-up users that register a callback and
  // do not enable the production snapshot queue.
  if (!gSnapQueue && gRecvCallback) {
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

void initSnapQueue(int depth) {
  if (gSnapQueue) {
    vQueueDelete(gSnapQueue);
    gSnapQueue = nullptr;
  }
  gSnapQueueDepth = depth > 0 ? depth : 8;
  gSnapDropCount = 0;
  gSnapQueue = xQueueCreate(static_cast<UBaseType_t>(gSnapQueueDepth),
                            sizeof(EspNowSnapSlot));
  if (!gSnapQueue) {
    Serial.println("[ESP-NOW] Snapshot queue allocation failed");
  } else {
    Serial.printf("[ESP-NOW] Snapshot queue ready (depth=%d)\n", gSnapQueueDepth);
  }
}

int drainSnapQueue(EspNowSnapSlot* outSlots, int maxSlots) {
  if (!gSnapQueue || !outSlots || maxSlots <= 0) return 0;

  int drained = 0;
  while (drained < maxSlots &&
         xQueueReceive(gSnapQueue, &outSlots[drained], 0) == pdTRUE) {
    ++drained;
  }
  return drained;
}

uint32_t getSnapDropCount() {
  return gSnapDropCount;
}

void deinitEspNowSync() {
  esp_now_unregister_recv_cb();
  esp_now_deinit();
  if (gSnapQueue) {
    vQueueDelete(gSnapQueue);
    gSnapQueue = nullptr;
  }
  gRecvCallback = nullptr;
  gSyncWindowOpen = false;
  Serial.printf("[ESP-NOW] Sync deinitialized (dropped=%lu)\n",
                static_cast<unsigned long>(gSnapDropCount));
}
