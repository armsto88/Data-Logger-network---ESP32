#include "control/node_config_control.h"

#include "config/node_registry.h"
#include "protocol.h"

#include <string.h>

namespace {

NodeInfo* findRegisteredNode(const char* nodeId) {
  if (!nodeId || !nodeId[0]) return nullptr;
  for (auto& node : registeredNodes) {
    if (strncmp(node.nodeId.c_str(), nodeId, CMD_NODEID_LEN) == 0) return &node;
  }
  return nullptr;
}

bool desiredFieldsMatch(const NodeDesiredConfig& cfg,
                        const DispatchNodeConfig& desired,
                        const NodeConfigApplyOptions& options) {
  if (cfg.wakeIntervalMin != desired.wakeIntervalMin ||
      cfg.targetState != desired.targetState ||
      cfg.sensorMask != static_cast<uint16_t>(desired.sensorMask)) return false;
  if (options.overrideSyncSchedule &&
      (cfg.syncIntervalMin != options.syncIntervalMin ||
       cfg.syncPhaseUnix != options.syncPhaseUnix)) return false;
  return true;
}

void setPendingState(NodeInfo& node, uint8_t targetState) {
  node.stateChangePending = true;
  node.pendingSinceMs = millis();
  node.pendingLastAttemptMs = 0;
  if (targetState == 0) {
    node.pendingTargetState = PENDING_TO_UNPAIRED;
  } else {
    // STANDBY remains deployed; desiredTargetState in status carries the exact
    // 2/3 distinction while this legacy pending enum retains DEPLOYED.
    node.pendingTargetState = PENDING_TO_DEPLOYED;
  }
}

CommandResult rejectCommand(const Command& command) {
  return dispatcherReject(command.cmdId, command.source, OUT_INVALID);
}

}  // namespace

bool controlResolveBackendNodeConfig(const Command& requested,
                                     Command& resolved,
                                     CmdOutcome& rejection) {
  rejection = OUT_INVALID;
  resolved = requested;
  const uint8_t fields = requested.configFields == 0
      ? CFG_FIELDS_ALL : requested.configFields;
  NodeInfo* node = findRegisteredNode(requested.payload.nodeId);
  if (requested.type != CMD_SET_NODE_CONFIG || !node ||
      (fields & CFG_FIELD_TARGET_STATE) == 0 ||
      requested.payload.targetState < 2 || requested.payload.targetState > 3)
    return false;

  const NodeDesiredConfig existing =
      getDesiredConfig(requested.payload.nodeId);
  if (existing.configVersion == UINT16_MAX) return false;

  if ((fields & CFG_FIELD_WAKE_INTERVAL) == 0) {
    if (existing.wakeIntervalMin >= 1 && existing.wakeIntervalMin <= 240) {
      resolved.payload.wakeIntervalMin = existing.wakeIntervalMin;
    } else if (node->wakeIntervalMin >= 1 && node->wakeIntervalMin <= 240) {
      resolved.payload.wakeIntervalMin = node->wakeIntervalMin;
    } else {
      return false;
    }
  }
  if (resolved.payload.wakeIntervalMin < 1 ||
      resolved.payload.wakeIntervalMin > 240) return false;

  if ((fields & CFG_FIELD_SENSOR_MASK) == 0)
    resolved.payload.sensorMask = existing.sensorMask;
  if (resolved.payload.sensorMask > UINT16_MAX) return false;

  resolved.configFields = CFG_FIELDS_ALL;
  return true;
}

NodeConfigApplyResult controlApplyNodeConfig(
    const Command& command, const NodeConfigApplyOptions& options) {
  NodeConfigApplyResult out{};

  NodeInfo* node = findRegisteredNode(command.payload.nodeId);
  const bool allowedTarget = command.payload.targetState == 2 ||
                             command.payload.targetState == 3 ||
                             (options.allowUnpair && command.payload.targetState == 0);
  const bool valid = command.type == CMD_SET_NODE_CONFIG && node &&
                     command.payload.wakeIntervalMin >= 1 &&
                     command.payload.wakeIntervalMin <= 240 &&
                     command.payload.sensorMask <= UINT16_MAX && allowedTarget;

  NodeDesiredConfig existing{};
  if (node) existing = getDesiredConfig(command.payload.nodeId);
  // The existing node protocol compares versions strictly. Refuse a mutation
  // rather than wrapping 65535 to 1 and creating a desired state the node can
  // never consider newer.
  if (!valid || existing.configVersion == UINT16_MAX) {
    out.command = rejectCommand(command);
    out.durable = dispatcherEnsureDurable();
    // A durable terminal INVALID result is fully handled even though there is
    // intentionally no registry mutation. This permits the backend sequence to
    // advance instead of redelivering an impossible target forever.
    out.registryApplied = out.durable;
    return out;
  }

  out.command = dispatcherSubmit(command);
  out.durable = dispatcherEnsureDurable();
  if (!out.durable) return out;

  CommandResult stored{};
  if (!dispatcherResultFor(command.cmdId, &stored)) return out;

  // Rejected, superseded and already-converged commands have a complete durable
  // result but must not rewrite the current desired configuration.
  if (stored.outcome != OUT_ACCEPTED) {
    out.registryApplied = true;
    return out;
  }

  uint32_t dispatchRevision = 0;
  uint16_t boundWireVersion = 0;
  if (!dispatcherCurrentCommandForNode(command.payload.nodeId, command.cmdId,
                                       &dispatchRevision, &boundWireVersion)) {
    // This command was superseded between deliveries. Its durable result is the
    // acknowledgement; only the newest command may own NodeDesiredConfig.
    out.registryApplied = true;
    return out;
  }

  const DispatchNodeConfig* desired =
      dispatcherNodeConfig(command.payload.nodeId);
  if (!desired) return out;

  NodeDesiredConfig next = getDesiredConfig(command.payload.nodeId);
  const bool fieldsAlreadyApplied = desiredFieldsMatch(next, *desired, options);

  if (boundWireVersion > 0) {
    // Normal replay after cursor loss: restore the already assigned version
    // exactly, never increment it again.
    next.configVersion = boundWireVersion;
  } else if (out.command.outcome == OUT_REPLAY && fieldsAlreadyApplied &&
             next.configVersion > 0) {
    // Reset happened after registry persistence but before dispatcher binding.
    // Reuse the observed durable version and repair the binding.
    boundWireVersion = next.configVersion;
  } else {
    next.configVersion = next.configVersion == 0
        ? 1 : static_cast<uint16_t>(next.configVersion + 1U);
    boundWireVersion = next.configVersion;
  }

  next.wakeIntervalMin = desired->wakeIntervalMin;
  next.targetState = desired->targetState;
  next.sensorMask = static_cast<uint16_t>(desired->sensorMask);
  if (options.overrideSyncSchedule) {
    next.syncIntervalMin = options.syncIntervalMin;
    next.syncPhaseUnix = options.syncPhaseUnix;
  }

  if (!setDesiredConfig(command.payload.nodeId, next)) return out;
  if (!dispatcherBindNodeConfigVersion(command.payload.nodeId, dispatchRevision,
                                       boundWireVersion)) return out;

  setPendingState(*node, next.targetState);
  setNodeExpectedSensorMask(
      command.payload.nodeId,
      (next.sensorMask & NODE_SENSOR_MASK_VALID)
          ? static_cast<uint16_t>(next.sensorMask & ~NODE_SENSOR_MASK_VALID)
          : 0);
  savePairedNodes();

  out.wireConfigVersion = boundWireVersion;
  out.registryApplied = true;
  out.durable = dispatcherEnsureDurable();
  return out;
}

NodeConfigApplyResult controlApplyLocalNodeConfig(
    const char* nodeId, uint8_t wakeIntervalMin, uint8_t targetState,
    uint16_t sensorMask, const NodeConfigApplyOptions& options) {
  static uint8_t localSequence = 0;
  Command command{};
  snprintf(command.cmdId, CMD_ID_LEN, "L%08lX%08lX%02X",
           static_cast<unsigned long>(millis()),
           static_cast<unsigned long>(dispatcherRevision() + 1U),
           static_cast<unsigned>(localSequence++));
  command.type = CMD_SET_NODE_CONFIG;
  command.source = SRC_LOCAL_UI;
  command.expectedRevision = dispatcherRevision();
  strlcpy(command.payload.nodeId, nodeId ? nodeId : "", CMD_NODEID_LEN);
  command.payload.wakeIntervalMin = wakeIntervalMin;
  command.payload.targetState = targetState;
  command.payload.sensorMask = sensorMask;
  command.configFields = CFG_FIELDS_ALL;
  return controlApplyNodeConfig(command, options);
}

bool controlMarkNodeConfigConverged(const char* nodeId,
                                    uint16_t appliedConfigVersion) {
  NodeInfo* node = findRegisteredNode(nodeId);
  if (!node) return false;

  const NodeDesiredConfig desired = getDesiredConfig(nodeId);
  if (desired.configVersion == 0 ||
      appliedConfigVersion < desired.configVersion) return false;

  // Update the registry even when this is a repeated convergence observation.
  // The dispatcher result can already be CONVERGED after a cold wake while the
  // RAM registry has just been restored from NVS; returning early in that case
  // used to leave recordingPaused/configVersionApplied stale.
  const bool nowPaused = desired.targetState == 3;
  const bool registryChanged =
      appliedConfigVersion > node->configVersionApplied ||
      node->wakeIntervalMin != desired.wakeIntervalMin ||
      node->recordingPaused != nowPaused || node->stateChangePending ||
      node->pendingTargetState != PENDING_NONE;
  if (registryChanged) {
    if (appliedConfigVersion > node->configVersionApplied) {
      node->configVersionApplied = appliedConfigVersion;
    }
    node->wakeIntervalMin = desired.wakeIntervalMin;
    node->recordingPaused = nowPaused;
    node->stateChangePending = false;
    node->pendingTargetState = PENDING_NONE;
    node->lastStateAppliedMs = millis();
    savePairedNodes();
  }

  // A matching CONFIG_ACK, NODE_HELLO, or snapshot also advances the retained
  // command result when a dispatcher binding exists. Legacy/local desired state
  // without a binding still repairs the reported registry above.
  uint32_t revision = 0;
  if (dispatcherRevisionForConfigVersion(nodeId, appliedConfigVersion,
                                         &revision)) {
    dispatcherMarkConverged(nodeId, revision);
  }
  return dispatcherEnsureDurable();
}
