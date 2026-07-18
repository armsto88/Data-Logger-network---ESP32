#pragma once

#include <Arduino.h>
#include <esp_now.h>
#include "protocol.h"
#include "storage/flash_logger.h"  // DecodedSnapshot

// ESP-NOW sync-only module for Mothership V1.
// Provides receive-only ESP-NOW on a fixed channel without WiFi AP.
// Used during RTC-alarm sync windows to collect node data.

// Callback type for received ESP-NOW data
typedef void (*EspNowRecvCallback)(const uint8_t* mac, const uint8_t* data, int len);

// Holds the fully-decoded snapshot (V1 or V2, any sensor mix) so nothing is
// lost between the ESP-NOW receive callback and processSnapshot(). Previously
// this held a node_snapshot_t (V1-only fixed fields), which silently dropped
// the AS7341 extended metadata (Clear/NIR/gain/integration/saturated) — those
// fields have no slot in node_snapshot_t, so every sync-window-delivered
// reading lost them even though the wire packet carried them correctly.
struct EspNowSnapSlot {
  uint8_t mac[6];
  DecodedSnapshot snap;
};

struct SyncHelloSlot {
  uint8_t mac[6];
  node_hello_message_t hello;
};

struct SyncDoneSlot {
  uint8_t mac[6];
  dump_done_message_t done;
};

struct SyncReleaseAckSlot {
  uint8_t mac[6];
  sync_release_ack_message_t ack;
};

struct SyncCapsSlot {
  uint8_t mac[6];
  fw_caps_message_t caps;
};

// An unpaired node emits NODE_STATUS while it stays awake for recovery. The
// main sync loop validates its stable MAC + nodeId against the registry before
// sending a recovery DEPLOY_NODE.
struct SyncStatusSlot {
  uint8_t mac[6];
  node_status_message_t status;
};

struct SyncDeployAckSlot {
  uint8_t mac[6];
  deployment_ack_message_t ack;
};

bool initEspNowSyncOnly(int channel);
void broadcastSyncWindowOpen();
bool broadcastSyncSessionOpen(const sync_session_open_message_t& open);
bool sendDumpGrant(const uint8_t* mac, const dump_grant_message_t& grant);
bool sendSyncRelease(const uint8_t* mac, const sync_release_message_t& release);
bool sendSnapshotAckNow(const uint8_t* mac, const snapshot_ack_t& ack);
bool sendDeploymentNow(const uint8_t* mac, const deployment_command_t& deploy);
// Announce a new sync schedule (SET_SYNC_SCHED) to the fleet over the
// broadcast peer during a sync window. Used to hand a changed schedule to
// sleeping nodes at the moment they are awake on the OLD schedule.
void broadcastSyncScheduleNow(int syncIntervalMinutes, uint32_t phaseUnix);

// Announce the recording/wake interval (SET_SCHEDULE) to the fleet over the
// broadcast peer during a sync window. SET_SYNC_SCHED does NOT carry the wake
// interval, so this must be broadcast separately for a recording-interval
// change to reach deployed nodes. Nodes apply it only when it differs, so it
// is safe to broadcast every window.
void broadcastWakeIntervalNow(int intervalMinutes);

// Unified declarative NODE_CONFIG broadcast (server -> node). Carries the
// target node's desired state (schedule + targetState + monotonic version).
// Broadcast every sync window per node; nodes apply only a strictly newer
// version and ACK via CONFIG_ACK. Supersedes SET_SCHEDULE + SET_SYNC_SCHED +
// UNPAIR_NODE for deployed nodes.
void broadcastNodeConfigNow(const node_config_message_t& cfg);

void registerReceiveCallback(EspNowRecvCallback cb);
void espnowSyncLoop();
void initSnapQueue(int depth);
int drainSnapQueue(EspNowSnapSlot* outSlots, int maxSlots);
uint32_t getSnapDropCount();
int drainSyncHellos(SyncHelloSlot* out, int maxItems);
// FW_CAPS collection — nodes report firmware/OTA identity after NODE_HELLO. The
// receive callback enqueues them; handleSyncWake drains and updates the registry.
int drainSyncCaps(SyncCapsSlot* out, int maxItems);
int drainSyncStatuses(SyncStatusSlot* out, int maxItems);
int drainDeployAcks(SyncDeployAckSlot* out, int maxItems);
int drainDumpDone(SyncDoneSlot* out, int maxItems);
int drainReleaseAcks(SyncReleaseAckSlot* out, int maxItems);

// CONFIG_ACK collection — nodes ACK an applied/UNPAIRED NODE_CONFIG during the
// sync window. The receive callback enqueues them; handleSyncWake drains and
// reconciles (converge deployed nodes, confirm+remove unpaired nodes).
int drainConfigAcks(config_apply_ack_message_t* out, int maxAcks);

void deinitEspNowSync();
