#include "node_event_queue.h"

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <string.h>

namespace {

QueueHandle_t g_queue = nullptr;
NodeEventCounters g_counters{};

NodeEventType toNodeEventType(IncomingMessageType type) {
  switch (type) {
    case IncomingMessageType::DISCOVER_RESPONSE: return NodeEventType::DISCOVERY_RESPONSE;
    case IncomingMessageType::DISCOVERY_SCAN:    return NodeEventType::DISCOVERY_SCAN;
    case IncomingMessageType::PAIRING_RESPONSE:  return NodeEventType::PAIRING_RESPONSE;
    case IncomingMessageType::PAIR_NODE:         return NodeEventType::PAIR_NODE;
    case IncomingMessageType::DEPLOY_NODE:       return NodeEventType::DEPLOY_NODE;
    case IncomingMessageType::UNPAIR_NODE:       return NodeEventType::UNPAIR_NODE;
    case IncomingMessageType::SET_SCHEDULE:      return NodeEventType::SET_SCHEDULE;
    case IncomingMessageType::SET_SYNC_SCHED:    return NodeEventType::SET_SYNC_SCHED;
    case IncomingMessageType::SYNC_WINDOW_OPEN:  return NodeEventType::SYNC_WINDOW_OPEN;
    case IncomingMessageType::TIME_SYNC:         return NodeEventType::TIME_SYNC;
    case IncomingMessageType::CONFIG_SNAPSHOT:   return NodeEventType::CONFIG_SNAPSHOT;
    case IncomingMessageType::SNAPSHOT_ACK:      return NodeEventType::SNAPSHOT_ACK;
    case IncomingMessageType::INVALID:
    default:                                     return NodeEventType::DISCOVERY_RESPONSE;
  }
}

template <typename T>
void copyPacket(T& dst, const uint8_t* data) {
  memcpy(&dst, data, sizeof(T));
}

void forceTermination(NodeEvent& ev) {
  switch (ev.type) {
    case NodeEventType::DISCOVERY_RESPONSE:
    case NodeEventType::DISCOVERY_SCAN:
      ev.payload.discovery.command[sizeof(ev.payload.discovery.command) - 1] = '\0';
      ev.payload.discovery.mothership_id[sizeof(ev.payload.discovery.mothership_id) - 1] = '\0';
      break;
    case NodeEventType::PAIRING_RESPONSE:
      ev.payload.pairingResponse.command[sizeof(ev.payload.pairingResponse.command) - 1] = '\0';
      ev.payload.pairingResponse.nodeId[sizeof(ev.payload.pairingResponse.nodeId) - 1] = '\0';
      ev.payload.pairingResponse.mothership_id[sizeof(ev.payload.pairingResponse.mothership_id) - 1] = '\0';
      break;
    case NodeEventType::PAIR_NODE:
      ev.payload.pairNode.command[sizeof(ev.payload.pairNode.command) - 1] = '\0';
      ev.payload.pairNode.nodeId[sizeof(ev.payload.pairNode.nodeId) - 1] = '\0';
      ev.payload.pairNode.mothership_id[sizeof(ev.payload.pairNode.mothership_id) - 1] = '\0';
      break;
    case NodeEventType::DEPLOY_NODE:
      ev.payload.deploy.command[sizeof(ev.payload.deploy.command) - 1] = '\0';
      ev.payload.deploy.nodeId[sizeof(ev.payload.deploy.nodeId) - 1] = '\0';
      ev.payload.deploy.mothership_id[sizeof(ev.payload.deploy.mothership_id) - 1] = '\0';
      break;
    case NodeEventType::UNPAIR_NODE:
      ev.payload.unpair.command[sizeof(ev.payload.unpair.command) - 1] = '\0';
      ev.payload.unpair.nodeId[sizeof(ev.payload.unpair.nodeId) - 1] = '\0';
      ev.payload.unpair.mothership_id[sizeof(ev.payload.unpair.mothership_id) - 1] = '\0';
      break;
    case NodeEventType::SET_SCHEDULE:
      ev.payload.schedule.command[sizeof(ev.payload.schedule.command) - 1] = '\0';
      ev.payload.schedule.mothership_id[sizeof(ev.payload.schedule.mothership_id) - 1] = '\0';
      break;
    case NodeEventType::SET_SYNC_SCHED:
    case NodeEventType::SYNC_WINDOW_OPEN:
      ev.payload.syncSchedule.command[sizeof(ev.payload.syncSchedule.command) - 1] = '\0';
      ev.payload.syncSchedule.mothership_id[sizeof(ev.payload.syncSchedule.mothership_id) - 1] = '\0';
      break;
    case NodeEventType::TIME_SYNC:
      ev.payload.timeSync.command[sizeof(ev.payload.timeSync.command) - 1] = '\0';
      ev.payload.timeSync.mothership_id[sizeof(ev.payload.timeSync.mothership_id) - 1] = '\0';
      break;
    case NodeEventType::CONFIG_SNAPSHOT:
      ev.payload.configSnapshot.command[sizeof(ev.payload.configSnapshot.command) - 1] = '\0';
      ev.payload.configSnapshot.mothership_id[sizeof(ev.payload.configSnapshot.mothership_id) - 1] = '\0';
      break;
    case NodeEventType::SNAPSHOT_ACK:
      ev.payload.snapshotAck.command[sizeof(ev.payload.snapshotAck.command) - 1] = '\0';
      ev.payload.snapshotAck.nodeId[sizeof(ev.payload.snapshotAck.nodeId) - 1] = '\0';
      break;
  }
}

bool copyPayload(NodeEvent& ev, IncomingMessageType type, const uint8_t* data, size_t len) {
  ev.type = toNodeEventType(type);
  ev.payloadLength = static_cast<uint16_t>(len);

  switch (type) {
    case IncomingMessageType::DISCOVER_RESPONSE:
    case IncomingMessageType::DISCOVERY_SCAN:
      if (len != sizeof(discovery_response_t)) return false;
      copyPacket(ev.payload.discovery, data);
      break;
    case IncomingMessageType::PAIRING_RESPONSE:
      if (len != sizeof(pairing_response_t)) return false;
      copyPacket(ev.payload.pairingResponse, data);
      break;
    case IncomingMessageType::PAIR_NODE:
      if (len != sizeof(pairing_command_t)) return false;
      copyPacket(ev.payload.pairNode, data);
      break;
    case IncomingMessageType::DEPLOY_NODE:
      if (len != sizeof(deployment_command_t)) return false;
      copyPacket(ev.payload.deploy, data);
      break;
    case IncomingMessageType::UNPAIR_NODE:
      if (len != sizeof(unpair_command_t)) return false;
      copyPacket(ev.payload.unpair, data);
      break;
    case IncomingMessageType::SET_SCHEDULE:
      if (len != sizeof(schedule_command_message_t)) return false;
      copyPacket(ev.payload.schedule, data);
      break;
    case IncomingMessageType::SET_SYNC_SCHED:
    case IncomingMessageType::SYNC_WINDOW_OPEN:
      if (len != sizeof(sync_schedule_command_message_t)) return false;
      copyPacket(ev.payload.syncSchedule, data);
      break;
    case IncomingMessageType::TIME_SYNC:
      if (len != sizeof(time_sync_response_t)) return false;
      copyPacket(ev.payload.timeSync, data);
      break;
    case IncomingMessageType::CONFIG_SNAPSHOT:
      if (len != sizeof(config_snapshot_message_t)) return false;
      copyPacket(ev.payload.configSnapshot, data);
      break;
    case IncomingMessageType::SNAPSHOT_ACK:
      if (len != sizeof(snapshot_ack_t)) return false;
      copyPacket(ev.payload.snapshotAck, data);
      break;
    case IncomingMessageType::INVALID:
    default:
      return false;
  }

  forceTermination(ev);
  return true;
}

}  // namespace

bool initNodeEventQueue(size_t capacity) {
  if (g_queue) return true;
  const UBaseType_t depth = static_cast<UBaseType_t>(capacity > 0 ? capacity : 12);
  g_queue = xQueueCreate(depth, sizeof(NodeEvent));
  if (!g_queue) return false;
  g_counters = {};
  return true;
}

void resetNodeEventQueueForTest() {
  if (g_queue) {
    vQueueDelete(g_queue);
    g_queue = nullptr;
  }
  g_counters = {};
}

bool enqueueValidatedNodeEvent(const uint8_t senderMac[6],
                               IncomingMessageType type,
                               const uint8_t* data,
                               size_t len,
                               uint32_t receivedMs) {
  if (!g_queue && !initNodeEventQueue()) {
    ++g_counters.callbackEventsDropped;
    return false;
  }
  if (!senderMac || !data || type == IncomingMessageType::INVALID) {
    ++g_counters.callbackInvalidPackets;
    return false;
  }

  NodeEvent ev{};
  memcpy(ev.senderMac, senderMac, sizeof(ev.senderMac));
  ev.receivedMs = receivedMs;
  if (!copyPayload(ev, type, data, len)) {
    ++g_counters.callbackInvalidPackets;
    return false;
  }

  if (xQueueSendToBack(g_queue, &ev, 0) != pdTRUE) {
    ++g_counters.callbackEventsDropped;
    return false;
  }

  ++g_counters.callbackEventsReceived;
  return true;
}

bool popNodeEvent(NodeEvent& out) {
  if (!g_queue) return false;
  return xQueueReceive(g_queue, &out, 0) == pdTRUE;
}

bool nodeEventsPending() {
  return g_queue && uxQueueMessagesWaiting(g_queue) > 0;
}

uint32_t nodeEventQueueDepth() {
  return g_queue ? static_cast<uint32_t>(uxQueueMessagesWaiting(g_queue)) : 0;
}

NodeEventCounters getNodeEventCounters() {
  return g_counters;
}

void noteInvalidNodePacket() {
  ++g_counters.callbackInvalidPackets;
}
