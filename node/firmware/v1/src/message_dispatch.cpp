#include "message_dispatch.h"

#include <string.h>

namespace {

constexpr size_t kMaxCommandField = 20;

bool hasNullWithin(const char* text, size_t width) {
  for (size_t i = 0; i < width; ++i) {
    if (text[i] == '\0') return true;
  }
  return false;
}

bool readCommand(const uint8_t* data, size_t len, char* out, size_t outLen) {
  if (!data || !out || outLen == 0 || len == 0) return false;

  const size_t scanLen = (len < kMaxCommandField) ? len : kMaxCommandField;
  size_t nulAt = scanLen;
  for (size_t i = 0; i < scanLen; ++i) {
    if (data[i] == '\0') {
      nulAt = i;
      break;
    }
  }
  if (nulAt == scanLen) return false;

  const size_t copyLen = (nulAt < (outLen - 1)) ? nulAt : (outLen - 1);
  memcpy(out, data, copyLen);
  out[copyLen] = '\0';
  return true;
}

template <typename T>
bool exactSize(size_t len) {
  return len == sizeof(T);
}

template <typename T>
const T* asPacket(const uint8_t* data, size_t len) {
  if (!data || len != sizeof(T)) return nullptr;
  return reinterpret_cast<const T*>(data);
}

bool targetMatches(const char* packetNodeId, size_t width, const char* nodeId) {
  if (!nodeId || !hasNullWithin(packetNodeId, width)) return false;
  return strncmp(packetNodeId, nodeId, width) == 0;
}

}  // namespace

const char* incomingMessageTypeName(IncomingMessageType type) {
  switch (type) {
    case IncomingMessageType::DISCOVER_RESPONSE: return "DISCOVER_RESPONSE";
    case IncomingMessageType::DISCOVERY_SCAN:    return "DISCOVERY_SCAN";
    case IncomingMessageType::PAIRING_RESPONSE:  return "PAIRING_RESPONSE";
    case IncomingMessageType::PAIR_NODE:         return "PAIR_NODE";
    case IncomingMessageType::DEPLOY_NODE:       return "DEPLOY_NODE";
    case IncomingMessageType::UNPAIR_NODE:       return "UNPAIR_NODE";
    case IncomingMessageType::SET_SCHEDULE:      return "SET_SCHEDULE";
    case IncomingMessageType::SET_SYNC_SCHED:    return "SET_SYNC_SCHED";
    case IncomingMessageType::SYNC_WINDOW_OPEN:  return "SYNC_WINDOW_OPEN";
    case IncomingMessageType::TIME_SYNC:         return "TIME_SYNC";
    case IncomingMessageType::CONFIG_SNAPSHOT:   return "CONFIG_SNAPSHOT";
    case IncomingMessageType::SNAPSHOT_ACK:      return "SNAPSHOT_ACK";
    case IncomingMessageType::INVALID:
    default:                                     return "INVALID";
  }
}

IncomingMessageType classifyIncomingMessage(const uint8_t* data, size_t len) {
  char command[kMaxCommandField + 1] = {0};
  if (!readCommand(data, len, command, sizeof(command))) {
    return IncomingMessageType::INVALID;
  }

  if (strcmp(command, "DISCOVER_RESPONSE") == 0) {
    return exactSize<discovery_response_t>(len)
        ? IncomingMessageType::DISCOVER_RESPONSE
        : IncomingMessageType::INVALID;
  }
  if (strcmp(command, "DISCOVERY_SCAN") == 0) {
    return exactSize<discovery_response_t>(len)
        ? IncomingMessageType::DISCOVERY_SCAN
        : IncomingMessageType::INVALID;
  }
  if (strcmp(command, "PAIRING_RESPONSE") == 0) {
    return exactSize<pairing_response_t>(len)
        ? IncomingMessageType::PAIRING_RESPONSE
        : IncomingMessageType::INVALID;
  }
  if (strcmp(command, "PAIR_NODE") == 0) {
    return exactSize<pairing_command_t>(len)
        ? IncomingMessageType::PAIR_NODE
        : IncomingMessageType::INVALID;
  }
  if (strcmp(command, "DEPLOY_NODE") == 0) {
    return exactSize<deployment_command_t>(len)
        ? IncomingMessageType::DEPLOY_NODE
        : IncomingMessageType::INVALID;
  }
  if (strcmp(command, "UNPAIR_NODE") == 0) {
    return exactSize<unpair_command_t>(len)
        ? IncomingMessageType::UNPAIR_NODE
        : IncomingMessageType::INVALID;
  }
  if (strcmp(command, "SET_SCHEDULE") == 0) {
    return exactSize<schedule_command_message_t>(len)
        ? IncomingMessageType::SET_SCHEDULE
        : IncomingMessageType::INVALID;
  }
  if (strcmp(command, "SET_SYNC_SCHED") == 0) {
    return exactSize<sync_schedule_command_message_t>(len)
        ? IncomingMessageType::SET_SYNC_SCHED
        : IncomingMessageType::INVALID;
  }
  if (strcmp(command, "SYNC_WINDOW_OPEN") == 0) {
    return exactSize<sync_schedule_command_message_t>(len)
        ? IncomingMessageType::SYNC_WINDOW_OPEN
        : IncomingMessageType::INVALID;
  }
  if (strcmp(command, "TIME_SYNC") == 0) {
    return exactSize<time_sync_response_t>(len)
        ? IncomingMessageType::TIME_SYNC
        : IncomingMessageType::INVALID;
  }
  if (strcmp(command, "CONFIG_SNAPSHOT") == 0) {
    return exactSize<config_snapshot_message_t>(len)
        ? IncomingMessageType::CONFIG_SNAPSHOT
        : IncomingMessageType::INVALID;
  }
  if (strcmp(command, "SNAPSHOT_ACK") == 0) {
    return exactSize<snapshot_ack_t>(len)
        ? IncomingMessageType::SNAPSHOT_ACK
        : IncomingMessageType::INVALID;
  }

  return IncomingMessageType::INVALID;
}

bool incomingMessageHasValidTarget(IncomingMessageType type,
                                   const uint8_t* data,
                                   size_t len,
                                   const char* nodeId) {
  if (!nodeId || !data) return false;

  switch (type) {
    case IncomingMessageType::PAIRING_RESPONSE: {
      const auto* p = asPacket<pairing_response_t>(data, len);
      return p && targetMatches(p->nodeId, sizeof(p->nodeId), nodeId);
    }
    case IncomingMessageType::PAIR_NODE: {
      const auto* p = asPacket<pairing_command_t>(data, len);
      return p && targetMatches(p->nodeId, sizeof(p->nodeId), nodeId);
    }
    case IncomingMessageType::DEPLOY_NODE: {
      const auto* p = asPacket<deployment_command_t>(data, len);
      return p && targetMatches(p->nodeId, sizeof(p->nodeId), nodeId);
    }
    case IncomingMessageType::SNAPSHOT_ACK: {
      const auto* p = asPacket<snapshot_ack_t>(data, len);
      return p && targetMatches(p->nodeId, sizeof(p->nodeId), nodeId);
    }
    case IncomingMessageType::DISCOVER_RESPONSE:
    case IncomingMessageType::DISCOVERY_SCAN:
      return true;
    case IncomingMessageType::UNPAIR_NODE: {
      const auto* p = asPacket<unpair_command_t>(data, len);
      return p && targetMatches(p->nodeId, sizeof(p->nodeId), nodeId);
    }
    case IncomingMessageType::SET_SCHEDULE:
    case IncomingMessageType::SET_SYNC_SCHED:
    case IncomingMessageType::SYNC_WINDOW_OPEN:
    case IncomingMessageType::TIME_SYNC:
    case IncomingMessageType::CONFIG_SNAPSHOT:
      return true;
    case IncomingMessageType::INVALID:
    default:
      return false;
  }
}

bool incomingMessageTextFieldsTerminated(IncomingMessageType type,
                                         const uint8_t* data,
                                         size_t len) {
  if (!data) return false;

  switch (type) {
    case IncomingMessageType::DISCOVER_RESPONSE:
    case IncomingMessageType::DISCOVERY_SCAN: {
      const auto* p = asPacket<discovery_response_t>(data, len);
      return p && hasNullWithin(p->command, sizeof(p->command)) &&
             hasNullWithin(p->mothership_id, sizeof(p->mothership_id));
    }
    case IncomingMessageType::PAIRING_RESPONSE: {
      const auto* p = asPacket<pairing_response_t>(data, len);
      return p && hasNullWithin(p->command, sizeof(p->command)) &&
             hasNullWithin(p->nodeId, sizeof(p->nodeId)) &&
             hasNullWithin(p->mothership_id, sizeof(p->mothership_id));
    }
    case IncomingMessageType::PAIR_NODE: {
      const auto* p = asPacket<pairing_command_t>(data, len);
      return p && hasNullWithin(p->command, sizeof(p->command)) &&
             hasNullWithin(p->nodeId, sizeof(p->nodeId)) &&
             hasNullWithin(p->mothership_id, sizeof(p->mothership_id));
    }
    case IncomingMessageType::DEPLOY_NODE: {
      const auto* p = asPacket<deployment_command_t>(data, len);
      return p && hasNullWithin(p->command, sizeof(p->command)) &&
             hasNullWithin(p->nodeId, sizeof(p->nodeId)) &&
             hasNullWithin(p->mothership_id, sizeof(p->mothership_id));
    }
    case IncomingMessageType::UNPAIR_NODE: {
      const auto* p = asPacket<unpair_command_t>(data, len);
      return p && hasNullWithin(p->command, sizeof(p->command)) &&
             hasNullWithin(p->nodeId, sizeof(p->nodeId)) &&
             hasNullWithin(p->mothership_id, sizeof(p->mothership_id));
    }
    case IncomingMessageType::SET_SCHEDULE: {
      const auto* p = asPacket<schedule_command_message_t>(data, len);
      return p && hasNullWithin(p->command, sizeof(p->command)) &&
             hasNullWithin(p->mothership_id, sizeof(p->mothership_id));
    }
    case IncomingMessageType::SET_SYNC_SCHED:
    case IncomingMessageType::SYNC_WINDOW_OPEN: {
      const auto* p = asPacket<sync_schedule_command_message_t>(data, len);
      return p && hasNullWithin(p->command, sizeof(p->command)) &&
             hasNullWithin(p->mothership_id, sizeof(p->mothership_id));
    }
    case IncomingMessageType::TIME_SYNC: {
      const auto* p = asPacket<time_sync_response_t>(data, len);
      return p && hasNullWithin(p->command, sizeof(p->command)) &&
             hasNullWithin(p->mothership_id, sizeof(p->mothership_id));
    }
    case IncomingMessageType::CONFIG_SNAPSHOT: {
      const auto* p = asPacket<config_snapshot_message_t>(data, len);
      return p && hasNullWithin(p->command, sizeof(p->command)) &&
             hasNullWithin(p->mothership_id, sizeof(p->mothership_id));
    }
    case IncomingMessageType::SNAPSHOT_ACK: {
      const auto* p = asPacket<snapshot_ack_t>(data, len);
      return p && hasNullWithin(p->command, sizeof(p->command)) &&
             hasNullWithin(p->nodeId, sizeof(p->nodeId));
    }
    case IncomingMessageType::INVALID:
    default:
      return false;
  }
}
