#pragma once

#include <Arduino.h>
#include <vector>
#include "protocol.h"  // ESPNOW_CHANNEL

// Node registry + NVS persistence for Mothership V1 config mode.
// Slim extract from production espnow_manager — no runtime inference,
// no continuous loop, no snapshot queue.  Provides the data model the
// config server needs to display and manage nodes.

// -----------------------------------------------------------------------------
// Registry model (mirrors production espnow_manager.h)
// -----------------------------------------------------------------------------

enum NodeState : uint8_t {
  UNPAIRED = 0,
  PAIRED   = 1,
  DEPLOYED = 2
};

enum NodePendingState : uint8_t {
  PENDING_NONE = 0,
  PENDING_TO_UNPAIRED = 1,
  PENDING_TO_PAIRED = 2,
  PENDING_TO_DEPLOYED = 3,
};

struct NodeInfo {
  uint8_t   mac[6];
  String    nodeId;
  String    nodeType;
  uint32_t  lastSeen;
  bool      isActive;
  NodeState state;
  uint8_t   channel;
  String    userId;
  String    name;

  uint32_t  lastTimeSyncMs;
  uint8_t   wakeIntervalMin;
  uint8_t   lastReportedQueueDepth;
  float     lastReportedBatV;       // NAN = unknown
  float     latitude;               // NAN = not set
  float     longitude;              // NAN = not set
  uint8_t   inferredWakeIntervalMin;
  uint32_t  lastNodeTimestamp;
  uint16_t  configVersionApplied;
  uint32_t  lastConfigPushMs;
  bool      deployPending;
  bool      stateChangePending;
  NodePendingState pendingTargetState;
  uint32_t  pendingSinceMs;
  uint32_t  pendingLastAttemptMs;
  uint32_t  lastStateAppliedMs;
  NodePendingState lastAppliedTargetState;
  uint32_t  deployedSinceUnix;    // unix timestamp when state became DEPLOYED (0 = not deployed)
  bool      recordingPaused;      // standby: deployed but recording paused (UI status)
  // Configured "expected" sensors + fault tracking (SNAP_PRESENT_* capability bits).
  uint16_t  expectedSensorMask;   // sensors the operator marked installed (no VALID bit)
  uint16_t  lastSensorPresent;    // sensorPresent from the most recent snapshot
  uint16_t  sensorMissPrev;       // expected & ~present from the previous snapshot (debounce)
  uint16_t  sensorFaultMask;      // configured sensors absent >=2 consecutive snapshots
  // Runtime-inference fields kept for struct compatibility; zeroed in config mode.
  bool      syncStale;
  uint8_t   staleMissCount;
  uint32_t  lastStaleAssistMs;
  // Firmware / OTA capability reported by the node via FW_CAPS (RAM only; empty
  // strings / 0 = not yet reported). Refreshed each sync window a node is heard.
  String    fwVersion;          // node firmware semver, e.g. "0.1.0"
  String    fwBuildId;          // node git build id
  String    hwRevision;         // node hardware target, e.g. "node-v3"
  uint8_t   otaProtocolVersion; // 0 = unknown
  uint32_t  otaMaxImageSize;    // inactive-slot capacity (bytes); 0 = unknown
  bool      rollbackCapable;
  bool      hasFirmwareCaps;    // true once a FW_CAPS has been received
  // OTA A/B slot state from FW_CAPS v2 (empty otaRunningSlot = not reported by
  // this node's firmware, e.g. an older v1 FW_CAPS).
  String    otaRunningSlot;     // "app0" / "app1"
  uint8_t   otaRunningState;    // FwOtaState of the running slot
  uint8_t   otaOtherState;      // FwOtaState of the inactive slot
  String    otaOtherVersion;    // inactive slot's app-desc version
  bool      hasSlotInfo;        // true once a FW_CAPS v2 (slot fields) arrived
};

struct NodeDesiredConfig {
  uint16_t configVersion;
  uint8_t  wakeIntervalMin;
  uint16_t syncIntervalMin;
  uint32_t syncPhaseUnix;
  uint8_t  targetState;     // 0=UNPAIRED, 2=DEPLOYED/ACTIVE, 3=STANDBY
  uint16_t sensorMask;      // configured sensors: SNAP_PRESENT_* bits + NODE_SENSOR_MASK_VALID
                            // (0 = unset/auto; the node then auto-detects everything)
};

// Update a registered node's cached expected-sensor mask (RAM only) so snapshot
// fault detection doesn't touch NVS in the hot path. Pass the raw capability
// bits (NODE_SENSOR_MASK_VALID stripped). No-op if the node isn't registered.
void setNodeExpectedSensorMask(const char* nodeId, uint16_t capabilityBits);

// Fold a node's FW_CAPS report into the registry (RAM only). No-op if the node
// isn't registered. Called from handleSyncWake after draining the caps queue.
void setNodeFirmwareCaps(const fw_caps_message_t& caps);

// -----------------------------------------------------------------------------
// Registry queries
// -----------------------------------------------------------------------------

extern std::vector<NodeInfo> registeredNodes;

std::vector<NodeInfo> getRegisteredNodes();
std::vector<NodeInfo> getUnpairedNodes();
std::vector<NodeInfo> getPairedNodes();
NodeState getNodeState(const char* nodeId);
String    getMothershipsMAC();

// Build the status.nodes[] JSON array for the Supabase upload payload.
// nowUnix: mothership RTC time (unix seconds), used to convert each node's
// millis-based lastSeen into an absolute lastSeenUnix.  Returns "[]" when the
// registry is empty.  Field names/casing match the backend spec exactly.
String buildNodesStatusJson(uint32_t nowUnix);

// -----------------------------------------------------------------------------
// Persistence (NVS)
// -----------------------------------------------------------------------------

void savePairedNodes();
void loadPairedNodes();

NodeDesiredConfig getDesiredConfig(const char* nodeId);
bool setDesiredConfig(const char* nodeId, const NodeDesiredConfig& cfg);

// -----------------------------------------------------------------------------
// Node meta helpers (numeric ID + Name + Notes in NVS)
// -----------------------------------------------------------------------------

String getNodeUserId(const String& nodeId);
void   setNodeUserId(const String& nodeId, String userId);
String getNodeName(const String& nodeId);
void   setNodeName(const String& nodeId, String name);
String getNodeNotes(const String& nodeId);
void   setNodeNotes(const String& nodeId, String notes);

String getCsvNodeId(const String& nodeId);
String getCsvNodeName(const String& nodeId);

// -----------------------------------------------------------------------------
// Registry mutation (used by espnow_config.cpp)
// -----------------------------------------------------------------------------

void registerNode(const uint8_t* mac,
                  const char* nodeId,
                  const char* nodeType,
                  NodeState state);

bool unpairNode(const String& nodeId);
