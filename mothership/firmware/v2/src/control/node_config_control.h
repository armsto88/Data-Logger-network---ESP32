#pragma once

#include <Arduino.h>
#include "command_dispatcher.h"

// Optional local-only schedule metadata. Dashboard SET_NODE_CONFIG controls
// wakeIntervalMin/targetState/sensorMask and leaves the existing sync rendezvous
// fields untouched.
struct NodeConfigApplyOptions {
  bool     allowUnpair = false;
  bool     overrideSyncSchedule = false;
  uint16_t syncIntervalMin = 0;
  uint32_t syncPhaseUnix = 0;
};

struct NodeConfigApplyResult {
  CommandResult command{};
  bool          durable = false;
  bool          registryApplied = false;
  uint16_t      wireConfigVersion = 0;
};

// Pure preflight for dashboard SET_NODE_CONFIG. Resolves omitted wake/sensor
// fields from the node's durable desired configuration and rejects remote
// unpair/unknown nodes without changing dispatcher or registry state.
bool controlResolveBackendNodeConfig(const Command& requested,
                                     Command& resolved,
                                     CmdOutcome& rejection);

// The one mothership-owned path used by both local Field UI mutations and
// backend commands. It persists the dispatcher result first, then the existing
// NodeDesiredConfig, binds the dispatcher revision to the NODE_CONFIG version,
// and sets the existing pending flags.
NodeConfigApplyResult controlApplyNodeConfig(
    const Command& command,
    const NodeConfigApplyOptions& options = NodeConfigApplyOptions{});

// Convenience for deliberate local UI changes. Generates a local command ID
// and submits with expectedRevision equal to the live authoritative revision.
NodeConfigApplyResult controlApplyLocalNodeConfig(
    const char* nodeId, uint8_t wakeIntervalMin, uint8_t targetState,
    uint16_t sensorMask,
    const NodeConfigApplyOptions& options = NodeConfigApplyOptions{});

// Reconcile a CONFIG_ACK, NODE_HELLO, or matching snapshot version back to the
// durable registry and dispatcher revision. Repeated observations also repair
// the RAM registry after a cold wake.
bool controlMarkNodeConfigConverged(const char* nodeId,
                                    uint16_t appliedConfigVersion);
