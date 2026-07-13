#pragma once

#include <Arduino.h>
#include "protocol.h"

enum class IncomingMessageType : uint8_t {
  INVALID = 0,
  DISCOVER_RESPONSE,
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
  SNAPSHOT_ACK,
  NODE_CONFIG,
  SYNC_SESSION,
  DUMP_GRANT,
  SYNC_RELEASE
};

const char* incomingMessageTypeName(IncomingMessageType type);

// Command-first classifier for inbound mothership -> node packets.
// It never reads beyond len, rejects unterminated command fields, and only
// returns a concrete type when len exactly matches that command's wire struct.
IncomingMessageType classifyIncomingMessage(const uint8_t* data, size_t len);

bool incomingMessageHasValidTarget(IncomingMessageType type,
                                   const uint8_t* data,
                                   size_t len,
                                   const char* nodeId);

bool incomingMessageTextFieldsTerminated(IncomingMessageType type,
                                         const uint8_t* data,
                                         size_t len);
