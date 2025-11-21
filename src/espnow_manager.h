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
};


// -----------------------------------------------------------------------------
// Lifecycle
// -----------------------------------------------------------------------------
void setupESPNOW();
void espnow_loop();

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
