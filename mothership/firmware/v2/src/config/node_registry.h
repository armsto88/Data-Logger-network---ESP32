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
  // Runtime-inference fields kept for struct compatibility; zeroed in config mode.
  bool      syncStale;
  uint8_t   staleMissCount;
  uint32_t  lastStaleAssistMs;
};

struct NodeDesiredConfig {
  uint16_t configVersion;
  uint8_t  wakeIntervalMin;
  uint16_t syncIntervalMin;
  uint32_t syncPhaseUnix;
};

// -----------------------------------------------------------------------------
// Registry queries
// -----------------------------------------------------------------------------

extern std::vector<NodeInfo> registeredNodes;

std::vector<NodeInfo> getRegisteredNodes();
std::vector<NodeInfo> getUnpairedNodes();
std::vector<NodeInfo> getPairedNodes();
NodeState getNodeState(const char* nodeId);
String    getMothershipsMAC();

// -----------------------------------------------------------------------------
// Persistence (NVS)
// -----------------------------------------------------------------------------

void savePairedNodes();
void loadPairedNodes();

NodeDesiredConfig getDesiredConfig(const char* nodeId);
void setDesiredConfig(const char* nodeId, const NodeDesiredConfig& cfg);

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