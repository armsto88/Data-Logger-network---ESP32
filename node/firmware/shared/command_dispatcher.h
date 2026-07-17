#pragma once
#include <Arduino.h>

// ===== Command dispatcher + revision store (plan §4.2, §4.3) =====
//
// The single entry point for BOTH local-UI and dashboard-originated changes to
// mothership-owned desired state. Guarantees:
//   - One monotonic, mothership-owned state revision.
//   - Compare-and-set: a command carrying a stale expectedRevision is rejected
//     (REVISION_CONFLICT) rather than silently clobbering a newer change.
//   - Idempotency: a repeated commandId returns its stored result and never
//     re-executes (safe backend re-delivery).
//   - Supersession: a newer accepted change for a node marks the earlier,
//     not-yet-converged command SUPERSEDED instead of "applied".
//   - Reboot-safe: revision, recent results, and desired configs persist in NVS.
//
// This module is transport-agnostic: local UI handlers and the backend command
// parser both build a Command and call dispatcherSubmit(). Node delivery
// (NODE_CONFIG) and convergence tracking sit downstream via dispatcherMarkConverged().

#define CMD_MAX_NODES    16
#define CMD_MAX_RESULTS  8
#define CMD_ID_LEN       24
#define CMD_NODEID_LEN   16

enum CmdType : uint8_t { CMD_REQUEST_STATUS = 0, CMD_SET_NODE_CONFIG = 1 };
enum CmdSource : uint8_t { SRC_LOCAL_UI = 0, SRC_DASHBOARD = 1 };

enum CmdOutcome : uint8_t {
  OUT_ACCEPTED = 0,
  OUT_REVISION_CONFLICT,   // expectedRevision != current
  OUT_INVALID,             // payload failed validation
  OUT_SUPERSEDED,          // replaced by a newer accepted change before converging
  OUT_REPLAY,             // duplicate commandId — stored result returned, not re-run
};
const char* cmdOutcomeStr(CmdOutcome o);

// Controlled subset the dispatcher revisions/mirrors. Semantics of targetState
// are owned by the node registry (e.g. 0=UNPAIRED, 2=DEPLOYED); the dispatcher
// stores the value opaquely. On accept these are applied to the registry's
// NodeDesiredConfig, which owns the 16-bit wire configVersion + NODE_CONFIG.
struct DispatchNodeConfig {
  char     nodeId[CMD_NODEID_LEN];
  uint8_t  wakeIntervalMin;   // data/recording interval
  uint8_t  targetState;       // registry NodeState value (opaque here)
  uint32_t sensorMask;
};

struct Command {
  char      cmdId[CMD_ID_LEN];   // dashboard-supplied, or local-generated
  CmdType   type;
  CmdSource source;
  uint32_t  expectedRevision;    // last revision the submitter saw (CAS)
  DispatchNodeConfig payload;      // for CMD_SET_NODE_CONFIG
};

struct CommandResult {
  char       cmdId[CMD_ID_LEN];
  CmdOutcome outcome;
  uint32_t   assignedRevision;   // revision assigned on accept (0 otherwise)
  uint32_t   currentRevision;    // authoritative revision at decision time
};

void          dispatcherInit();               // load persisted state from NVS
void          dispatcherResetForTest();        // wipe RAM + NVS (bench only)
uint32_t      dispatcherRevision();
CommandResult dispatcherSubmit(const Command& c);
const DispatchNodeConfig* dispatcherNodeConfig(const char* nodeId);
bool          dispatcherKnownCmd(const char* cmdId);          // seen before?
bool          dispatcherResultFor(const char* cmdId, CommandResult* out);
uint8_t       dispatcherRecentResults(CommandResult* out, uint8_t max);   // for status/UI
// Canonical control-status JSON: {"stateRevision":N,"lastChangeSource":"...","results":[...]}
// One serializer for both GET /api/control and the cloud status.control{} block.
String        dispatcherStatusJson();
CmdSource     dispatcherLastChangeSource();
// Node applied the desired state (CONFIG_ACK / snapshot). Clears its pending flag.
void          dispatcherMarkConverged(const char* nodeId, uint32_t revision);
