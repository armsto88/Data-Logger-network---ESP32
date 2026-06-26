#pragma once

#include <Arduino.h>
#include "message_dispatch.h"
#include "protocol.h"

enum class NodeEventType : uint8_t {
  DISCOVERY_RESPONSE = 0,
  DISCOVERY_SCAN,
  PAIRING_RESPONSE,
  PAIR_NODE,
  DEPLOY_NODE,
  UNPAIR_NODE,
  SET_SCHEDULE,
  SET_SYNC_SCHED,
  SYNC_WINDOW_OPEN,
  TIME_SYNC,
  CONFIG_SNAPSHOT,
  SNAPSHOT_ACK
};

struct NodeEvent {
  NodeEventType type;
  uint8_t senderMac[6];
  uint16_t payloadLength;
  uint32_t receivedMs;
  union {
    discovery_response_t discovery;
    pairing_response_t pairingResponse;
    pairing_command_t pairNode;
    deployment_command_t deploy;
    unpair_command_t unpair;
    schedule_command_message_t schedule;
    sync_schedule_command_message_t syncSchedule;
    time_sync_response_t timeSync;
    config_snapshot_message_t configSnapshot;
    snapshot_ack_t snapshotAck;
  } payload;
};

struct NodeEventCounters {
  volatile uint32_t callbackEventsReceived;
  volatile uint32_t callbackEventsDropped;
  volatile uint32_t callbackInvalidPackets;
};

bool initNodeEventQueue(size_t capacity = 12);
void resetNodeEventQueueForTest();
bool enqueueValidatedNodeEvent(const uint8_t senderMac[6],
                               IncomingMessageType type,
                               const uint8_t* data,
                               size_t len,
                               uint32_t receivedMs);
bool popNodeEvent(NodeEvent& out);
bool nodeEventsPending();
uint32_t nodeEventQueueDepth();
NodeEventCounters getNodeEventCounters();
void noteInvalidNodePacket();

