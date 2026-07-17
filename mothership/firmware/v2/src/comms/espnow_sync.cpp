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
// CONFIG_ACK queue — small; nodes ACK an applied/UNPAIRED NODE_CONFIG.
static QueueHandle_t gAckQueue = nullptr;
static constexpr int kAckQueueDepth = 8;
static QueueHandle_t gHelloQueue = nullptr;
static QueueHandle_t gCapsQueue = nullptr;
static QueueHandle_t gDoneQueue = nullptr;
static QueueHandle_t gReleaseAckQueue = nullptr;
static constexpr int kControlQueueDepth = 32;

static volatile bool gControlSendActive = false;
static volatile bool gControlSendComplete = false;
static volatile esp_now_send_status_t gControlSendStatus = ESP_NOW_SEND_FAIL;
static uint8_t gControlExpectedMac[6] = {0};

// Broadcast address for ESP-NOW
static constexpr uint8_t kBroadcastAddr[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

static void onEspNowSend(const uint8_t* mac, esp_now_send_status_t status) {
  if (!gControlSendActive || !mac || memcmp(mac, gControlExpectedMac, 6) != 0) return;
  gControlSendStatus = status;
  gControlSendComplete = true;
}

static bool ensureSyncPeer(const uint8_t* mac) {
  if (!mac) return false;
  esp_now_peer_info_t peer{};
  memcpy(peer.peer_addr, mac, 6);
  peer.channel = static_cast<uint8_t>(gChannel);
  peer.ifidx = WIFI_IF_STA;
  peer.encrypt = false;
  esp_err_t added = esp_now_add_peer(&peer);
  return added == ESP_OK || added == ESP_ERR_ESPNOW_EXIST;
}

static bool sendControlPacket(const uint8_t* mac, const void* packet, size_t len) {
  if (!mac || !packet || len == 0 || !ensureSyncPeer(mac)) return false;
  for (uint8_t attempt = 1; attempt <= 3; ++attempt) {
    memcpy(gControlExpectedMac, mac, 6);
    gControlSendComplete = false;
    gControlSendStatus = ESP_NOW_SEND_FAIL;
    gControlSendActive = true;
    esp_err_t queued = esp_now_send(mac, reinterpret_cast<const uint8_t*>(packet), len);
    if (queued == ESP_OK) {
      const uint32_t started = millis();
      while (!gControlSendComplete && (uint32_t)(millis() - started) < 350UL) {
        delay(1);
      }
    }
    const bool delivered = queued == ESP_OK && gControlSendComplete &&
                           gControlSendStatus == ESP_NOW_SEND_SUCCESS;
    gControlSendActive = false;
    if (delivered) return true;
    delay(25UL * attempt);
  }
  return false;
}

// Internal receive handler (ESP-IDF 4.4 API: mac_addr, data, len)
static void onEspNowRecv(const uint8_t* mac_addr, const uint8_t* data, int len) {
  if (gHelloQueue && mac_addr && data &&
      len == static_cast<int>(sizeof(node_hello_message_t)) &&
      strncmp(reinterpret_cast<const char*>(data), "NODE_HELLO", 10) == 0) {
    SyncHelloSlot slot{};
    memcpy(slot.mac, mac_addr, sizeof(slot.mac));
    memcpy(&slot.hello, data, sizeof(slot.hello));
    xQueueSendToBack(gHelloQueue, &slot, 0);
    return;
  }

  // FW_CAPS — additive node firmware/OTA identity, sent after NODE_HELLO.
  // Exact-length + tag match; older nodes never send it. Enqueue for the main
  // loop to fold into the registry (never touch the registry from the callback).
  if (gCapsQueue && mac_addr && data &&
      len == static_cast<int>(sizeof(fw_caps_message_t)) &&
      strncmp(reinterpret_cast<const char*>(data), "FW_CAPS", 7) == 0) {
    SyncCapsSlot slot{};
    memcpy(slot.mac, mac_addr, sizeof(slot.mac));
    memcpy(&slot.caps, data, sizeof(slot.caps));
    xQueueSendToBack(gCapsQueue, &slot, 0);
    return;
  }

  if (gDoneQueue && mac_addr && data &&
      len == static_cast<int>(sizeof(dump_done_message_t)) &&
      strncmp(reinterpret_cast<const char*>(data), "DUMP_DONE", 10) == 0) {
    SyncDoneSlot slot{};
    memcpy(slot.mac, mac_addr, sizeof(slot.mac));
    memcpy(&slot.done, data, sizeof(slot.done));
    xQueueSendToBack(gDoneQueue, &slot, 0);
    return;
  }

  if (gReleaseAckQueue && mac_addr && data &&
      len == static_cast<int>(sizeof(sync_release_ack_message_t)) &&
      strncmp(reinterpret_cast<const char*>(data), "RELEASE_ACK", 12) == 0) {
    SyncReleaseAckSlot slot{};
    memcpy(slot.mac, mac_addr, sizeof(slot.mac));
    memcpy(&slot.ack, data, sizeof(slot.ack));
    xQueueSendToBack(gReleaseAckQueue, &slot, 0);
    return;
  }

  // CONFIG_ACK — a node confirming it applied (or is unpairing on) a NODE_CONFIG.
  // Enqueue for handleSyncWake to reconcile. Same drop-oldest policy as snapshots.
  if (gAckQueue && mac_addr && data &&
      len == static_cast<int>(sizeof(config_apply_ack_message_t)) &&
      strncmp(reinterpret_cast<const char*>(data), "CONFIG_ACK", 10) == 0) {
    config_apply_ack_message_t ack{};
    memcpy(&ack, data, sizeof(ack));
    if (xQueueSendToBack(gAckQueue, &ack, 0) != pdTRUE) {
      config_apply_ack_message_t discard{};
      xQueueReceive(gAckQueue, &discard, 0);
      xQueueSendToBack(gAckQueue, &ack, 0);
    }
    return;
  }

  if (gSnapQueue && mac_addr && data) {
    // V2 snapshot (NODE_SNAPSHOT2) — variable length. Store the fully decoded
    // snapshot directly (no V1 downgrade) so extended metadata (Clear/NIR/
    // gain/integration/saturated) survives into processSnapshot().
    if (isV2Snapshot(data, len)) {
      EspNowSnapSlot slot{};
      memcpy(slot.mac, mac_addr, sizeof(slot.mac));
      if (decodeV2(data, len, slot.snap)) {
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
        decodeV1(*snap, slot.snap);

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
  if (esp_now_register_send_cb(onEspNowSend) != ESP_OK) {
    Serial.println("[ESP-NOW] Register send callback failed");
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

  const bool delivered = sendControlPacket(kBroadcastAddr, &msg, sizeof(msg));
  if (delivered) {
    Serial.println("[ESP-NOW] SYNC_WINDOW_OPEN broadcast sent");
    gSyncWindowOpen = true;
  } else {
    Serial.println("[ESP-NOW] SYNC_WINDOW_OPEN broadcast failed");
  }
}

bool broadcastSyncSessionOpen(const sync_session_open_message_t& open) {
  const bool ok = sendControlPacket(kBroadcastAddr, &open, sizeof(open));
  Serial.printf("[ESP-NOW] SYNC_SESSION id=%lu join=%ums window=%us -> %s\n",
                (unsigned long)open.sessionId, (unsigned)open.joinWindowMs,
                (unsigned)open.sessionWindowSec, ok ? "OK" : "FAIL");
  return ok;
}

bool sendDumpGrant(const uint8_t* mac, const dump_grant_message_t& grant) {
  return sendControlPacket(mac, &grant, sizeof(grant));
}

bool sendSyncRelease(const uint8_t* mac, const sync_release_message_t& release) {
  return sendControlPacket(mac, &release, sizeof(release));
}

bool sendSnapshotAckNow(const uint8_t* mac, const snapshot_ack_t& ack) {
  return sendControlPacket(mac, &ack, sizeof(ack));
}

void broadcastSyncScheduleNow(int syncIntervalMinutes, uint32_t phaseUnix) {
  // Hand the new schedule to the fleet while they are awake in this window.
  // Mirrors broadcastSyncWindowOpen() — reuses the broadcast peer added in
  // initEspNowSyncOnly(). Nodes apply this via the SET_SYNC_SCHED event and
  // re-arm A2 to phaseUnix + syncIntervalMinutes.
  sync_schedule_command_message_t msg = {};
  strncpy(msg.command, "SET_SYNC_SCHED", sizeof(msg.command) - 1);
  strncpy(msg.mothership_id, "M001", sizeof(msg.mothership_id) - 1);
  msg.syncIntervalMinutes = (unsigned long)syncIntervalMinutes;
  msg.phaseUnix = phaseUnix;

  const bool result = sendControlPacket(kBroadcastAddr, &msg, sizeof(msg));
  Serial.printf("[ESP-NOW] SET_SYNC_SCHED broadcast syncMin=%d phase=%lu -> %s\n",
                syncIntervalMinutes, (unsigned long)phaseUnix,
                result ? "OK" : "FAIL");
}

void broadcastWakeIntervalNow(int intervalMinutes) {
  // SET_SYNC_SCHED carries only the sync schedule; the recording/wake interval
  // is a separate command (SET_SCHEDULE). Broadcast it over the same broadcast
  // peer so deployed nodes update their A1 (data) alarm. Nodes ignore it when
  // the interval is unchanged, so broadcasting every window is idempotent.
  schedule_command_message_t cmd = {};
  strncpy(cmd.command, "SET_SCHEDULE", sizeof(cmd.command) - 1);
  strncpy(cmd.mothership_id, "M001", sizeof(cmd.mothership_id) - 1);
  cmd.intervalMinutes = intervalMinutes;

  const bool result = sendControlPacket(kBroadcastAddr, &cmd, sizeof(cmd));
  Serial.printf("[ESP-NOW] SET_SCHEDULE broadcast interval=%d min -> %s\n",
                intervalMinutes, result ? "OK" : "FAIL");
}

void broadcastNodeConfigNow(const node_config_message_t& cfg) {
  // Broadcast the target node's desired state over the broadcast peer. Only the
  // addressed node (cfg.nodeId) acts on it; others validate the target and drop.
  const bool result = sendControlPacket(kBroadcastAddr, &cfg, sizeof(cfg));
  Serial.printf("[ESP-NOW] NODE_CONFIG -> %.15s v%u target=%u wake=%u syncMin=%u -> %s\n",
                cfg.nodeId, (unsigned)cfg.configVersion, (unsigned)cfg.targetState,
                (unsigned)cfg.wakeIntervalMin, (unsigned)cfg.syncIntervalMin,
                result ? "OK" : "FAIL");
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

  // CONFIG_ACK queue — created alongside the snapshot queue for the sync window.
  if (gAckQueue) {
    vQueueDelete(gAckQueue);
    gAckQueue = nullptr;
  }
  gAckQueue = xQueueCreate(kAckQueueDepth, sizeof(config_apply_ack_message_t));
  if (!gAckQueue) {
    Serial.println("[ESP-NOW] CONFIG_ACK queue allocation failed");
  }

  if (gHelloQueue) vQueueDelete(gHelloQueue);
  if (gCapsQueue) vQueueDelete(gCapsQueue);
  if (gDoneQueue) vQueueDelete(gDoneQueue);
  if (gReleaseAckQueue) vQueueDelete(gReleaseAckQueue);
  gHelloQueue = xQueueCreate(kControlQueueDepth, sizeof(SyncHelloSlot));
  gCapsQueue = xQueueCreate(kControlQueueDepth, sizeof(SyncCapsSlot));
  gDoneQueue = xQueueCreate(kControlQueueDepth, sizeof(SyncDoneSlot));
  gReleaseAckQueue = xQueueCreate(kControlQueueDepth, sizeof(SyncReleaseAckSlot));
  if (!gHelloQueue || !gCapsQueue || !gDoneQueue || !gReleaseAckQueue) {
    Serial.println("[ESP-NOW] coordinated sync queue allocation failed");
  }
}

int drainSyncHellos(SyncHelloSlot* out, int maxItems) {
  if (!gHelloQueue || !out || maxItems <= 0) return 0;
  int count = 0;
  while (count < maxItems && xQueueReceive(gHelloQueue, &out[count], 0) == pdTRUE) ++count;
  return count;
}

int drainSyncCaps(SyncCapsSlot* out, int maxItems) {
  if (!gCapsQueue || !out || maxItems <= 0) return 0;
  int count = 0;
  while (count < maxItems && xQueueReceive(gCapsQueue, &out[count], 0) == pdTRUE) ++count;
  return count;
}

int drainDumpDone(SyncDoneSlot* out, int maxItems) {
  if (!gDoneQueue || !out || maxItems <= 0) return 0;
  int count = 0;
  while (count < maxItems && xQueueReceive(gDoneQueue, &out[count], 0) == pdTRUE) ++count;
  return count;
}

int drainReleaseAcks(SyncReleaseAckSlot* out, int maxItems) {
  if (!gReleaseAckQueue || !out || maxItems <= 0) return 0;
  int count = 0;
  while (count < maxItems && xQueueReceive(gReleaseAckQueue, &out[count], 0) == pdTRUE) ++count;
  return count;
}

int drainConfigAcks(config_apply_ack_message_t* out, int maxAcks) {
  if (!gAckQueue || !out || maxAcks <= 0) return 0;
  int drained = 0;
  while (drained < maxAcks &&
         xQueueReceive(gAckQueue, &out[drained], 0) == pdTRUE) {
    ++drained;
  }
  return drained;
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
  if (gAckQueue) {
    vQueueDelete(gAckQueue);
    gAckQueue = nullptr;
  }
  if (gHelloQueue) {
    vQueueDelete(gHelloQueue);
    gHelloQueue = nullptr;
  }
  if (gCapsQueue) {
    vQueueDelete(gCapsQueue);
    gCapsQueue = nullptr;
  }
  if (gDoneQueue) {
    vQueueDelete(gDoneQueue);
    gDoneQueue = nullptr;
  }
  if (gReleaseAckQueue) {
    vQueueDelete(gReleaseAckQueue);
    gReleaseAckQueue = nullptr;
  }
  gRecvCallback = nullptr;
  gSyncWindowOpen = false;
  Serial.printf("[ESP-NOW] Sync deinitialized (dropped=%lu)\n",
                static_cast<unsigned long>(gSnapDropCount));
}
