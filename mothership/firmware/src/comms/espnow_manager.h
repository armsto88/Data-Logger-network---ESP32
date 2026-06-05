// include/espnow_manager.h
#pragma once
#include <Arduino.h>
#include <vector>
#include "protocol.h"   // all wire-level message structs & shared constants

// -----------------------------------------------------------------------------
// Known peers (declare here, define once in a .cpp — e.g., src/espnow_manager_globals.cpp)
// -----------------------------------------------------------------------------
extern const uint8_t KNOWN_SENSOR_NODES[][6];
extern const int NUM_KNOWN_SENSORS;

// Optional: standard channel the mothership uses for pairing/commands
#ifndef ESPNOW_PAIRING_CHANNEL
#define ESPNOW_PAIRING_CHANNEL 1
#endif

// -----------------------------------------------------------------------------
// Registry model
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

    // NEW: millis() at last TIME_SYNC sent from mothership to this node (0 = never)
    uint32_t  lastTimeSyncMs;
    // Wake interval last sent to this node via SET_SCHEDULE (0 = not yet scheduled)
    uint8_t   wakeIntervalMin;
    // Last queue depth reported by NODE_HELLO at wake start.
    uint8_t   lastReportedQueueDepth;
    // Last battery voltage seen in a NODE_SNAPSHOT from this node (NAN = unknown).
    float     lastReportedBatV;
    // Wake interval inferred from nodeTimestamp cadence in sensor data (0 = unknown)
    uint8_t   inferredWakeIntervalMin;
    // Last seen nodeTimestamp used for cadence inference (unix seconds)
    uint32_t  lastNodeTimestamp;
    // Last config version ACKed by this node (0 = never applied)
    uint16_t  configVersionApplied;
    // Last time mothership pushed CONFIG_SNAPSHOT to this node (ms, for throttling)
    uint32_t  lastConfigPushMs;
    // True after DEPLOY_NODE command is queued, cleared when runtime evidence confirms deploy
    bool      deployPending;
    // True when mothership has queued a state change that must be applied on node wake.
    bool      stateChangePending;
    // Target state the node should eventually adopt.
    NodePendingState pendingTargetState;
    // millis() when pending state was queued.
    uint32_t  pendingSinceMs;
    // millis() of the most recent resend attempt for pending state command.
    uint32_t  pendingLastAttemptMs;
    // millis() when a pending state transition was confirmed/applied.
    uint32_t  lastStateAppliedMs;
    // Last applied target that was confirmed.
    NodePendingState lastAppliedTargetState;
    // Derived stale state (mothership-side inference only)
    bool      syncStale;
    uint8_t   staleMissCount;
    uint32_t  lastStaleAssistMs;
};

// Desired configuration for each node, stored in mothership NVS.
// Bumping configVersion causes mothership to push a CONFIG_SNAPSHOT on next HELLO.
struct NodeDesiredConfig {
    uint16_t configVersion;
    uint8_t  wakeIntervalMin;
    uint16_t syncIntervalMin;
    uint32_t syncPhaseUnix;
};

// Get/set the desired config for a node by its firmware nodeId.
NodeDesiredConfig getDesiredConfig(const char* nodeId);
void setDesiredConfig(const char* nodeId, const NodeDesiredConfig& cfg);

typedef void (*SensorDataEventCallback)(const sensor_data_message_t& sample, const uint8_t mac[6]);


// -----------------------------------------------------------------------------
// Lifecycle
// -----------------------------------------------------------------------------
void setupESPNOW();
void espnow_loop();

// Handle a NODE_HELLO received from a node (called from OnDataRecv).
// Sends CONFIG_SNAPSHOT back if mothership has a newer config version.
void handleNodeHello(const uint8_t* senderMac, const node_hello_message_t& hello);

// -----------------------------------------------------------------------------
// Commands / broadcasts (mothership -> nodes)
// -----------------------------------------------------------------------------

bool broadcastTimeSyncAll();
bool broadcastTimeSyncIfDue(bool force = false);

/**
 * Send a time sync packet to a specific node (uses RTC time from mothership).
 */
bool sendTimeSync(const uint8_t* mac, const char* nodeId);

/**
 * Broadcast a discovery “scan” so unpaired nodes respond.
 */
bool sendDiscoveryBroadcast();

/**
 * Broadcast a wake interval (in minutes) to all PAIRED/DEPLOYED nodes.
 * Nodes should program their DS3231 alarm accordingly.
 */
bool broadcastWakeInterval(int intervalMinutes);

/**
 * Broadcast the fleet sync schedule so deployed nodes know when to enable WiFi
 * and upload queued samples.
 */
bool broadcastSyncSchedule(int syncIntervalMinutes, unsigned long phaseUnix);
bool broadcastSyncWindowOpen(unsigned long phaseUnix);

/**
 * Set node to PAIRED and notify it.
 */
bool pairNode(const String& nodeId);

/**
 * Send deployment command (with current RTC time) to selected nodes.
 */
bool deploySelectedNodes(const std::vector<String>& nodeIds);

/**
 * Locally unpair a node (registry + peer removal + persist).
 */
bool unpairNode(const String& nodeId);

/**
 * Send an UNPAIR command to a node (best-effort).
 */
bool sendUnpairToNode(const String& nodeId);

// -----------------------------------------------------------------------------
// Queries
// -----------------------------------------------------------------------------
std::vector<NodeInfo> getRegisteredNodes();
std::vector<NodeInfo> getUnpairedNodes();
std::vector<NodeInfo> getPairedNodes();
NodeState getNodeState(const char* nodeId);
String    getMothershipsMAC();
void      printRegisteredNodes();

// -----------------------------------------------------------------------------
// Persistence (NVS)
// -----------------------------------------------------------------------------
void savePairedNodes();
void loadPairedNodes();

void setSensorDataEventCallback(SensorDataEventCallback cb);
