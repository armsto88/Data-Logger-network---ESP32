#pragma once

#include <Arduino.h>
#include <vector>
#include "protocol.h"

// Config-mode ESP-NOW command layer for Mothership V1.
// Slim extract from production espnow_manager — provides command sends
// and a receive callback that updates the node registry.  No continuous
// loop, no stale inference, no snapshot queue.

bool initEspNowConfig(int channel);

// Commands / broadcasts (mothership -> nodes)
bool sendDiscoveryBroadcast();
bool pairNode(const String& nodeId);
bool deploySelectedNodes(const std::vector<String>& nodeIds);
bool sendUnpairToNode(const String& nodeId);
bool broadcastTimeSyncAll();
bool sendTimeSync(const uint8_t* mac, const char* nodeId);
bool broadcastWakeInterval(int intervalMinutes);
bool broadcastSyncSchedule(int syncIntervalMinutes, unsigned long phaseUnix);
bool broadcastSyncWindowOpen(unsigned long phaseUnix);
bool sendConfigSnapshot(const uint8_t* mac, const char* nodeId);

// NODE_HELLO handler — updates registry, pushes CONFIG_SNAPSHOT if needed.
void handleNodeHello(const uint8_t* senderMac, const node_hello_message_t& hello);

// Drain any pending work (no continuous loop).  Currently a no-op but
// kept for future queue-drain logic.
void espnowConfigPoll();