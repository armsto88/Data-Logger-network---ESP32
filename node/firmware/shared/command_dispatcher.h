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

enum CmdType : uint8_t {
  CMD_REQUEST_STATUS = 0,
  CMD_SET_NODE_CONFIG = 1,
  CMD_SET_RECORDING_INTERVAL = 2,
  // Mothership self-update: install a signed firmware release identified by
  // releaseId. Global (not node-targeted). The dispatcher provides only
  // CAS/revision + idempotency + result tracking; the releaseId itself is
  // staged durably in the separate OTA release store, so nothing here is added
  // to the persisted DispatcherStateRecord (no fleet NVS reset on upgrade).
  CMD_DEPLOY_RELEASE = 3,
};

#define CMD_RELEASE_ID_LEN 40
enum CmdSource : uint8_t { SRC_LOCAL_UI = 0, SRC_DASHBOARD = 1 };

enum CmdOutcome : uint8_t {
  OUT_ACCEPTED = 0,
  OUT_REVISION_CONFLICT,   // expectedRevision != current
  OUT_INVALID,             // payload failed validation
  OUT_SUPERSEDED,          // replaced by a newer accepted change before converging
  OUT_REPLAY,              // duplicate commandId - returned transiently, not re-run
  OUT_CONVERGED,           // node confirmed the assigned desired configuration
};
const char* cmdOutcomeStr(CmdOutcome o);

// Controlled subset the dispatcher revisions/mirrors. Semantics of targetState
// are owned by the node registry (e.g. 0=UNPAIRED, 2=DEPLOYED); the dispatcher
// stores the value opaquely. On accept these are applied to the registry's
// NodeDesiredConfig, which owns the 16-bit wire configVersion + NODE_CONFIG.
struct DispatchNodeConfig {
  char     nodeId[CMD_NODEID_LEN];
  uint8_t  wakeIntervalMin;   // data/recording interval
  uint8_t  targetState;       // 0=UNPAIRED, 2=DEPLOYED, 3=STANDBY
  uint32_t sensorMask;
};

struct Command {
  char      cmdId[CMD_ID_LEN];   // dashboard-supplied, or local-generated
  CmdType   type;
  CmdSource source;
  uint32_t  expectedRevision;    // last revision the submitter saw (CAS)
  DispatchNodeConfig payload;    // for CMD_SET_NODE_CONFIG
  // Backend SET_NODE_CONFIG may intentionally omit fields that it is not
  // changing. The FieldHub resolves those fields from its durable desired
  // configuration before submitting. Local callers use CFG_FIELDS_ALL.
  uint8_t   configFields;
  // FieldHub-wide recording interval for CMD_SET_RECORDING_INTERVAL. It is
  // deliberately not attached to a node target.
  uint8_t   recordingIntervalMin;
  // Release identifier for CMD_DEPLOY_RELEASE. Transient (carried to the
  // executor, which stages it in the OTA release store); never persisted in the
  // dispatcher NVS record, so adding it does not change DispatcherStateRecord.
  char      releaseId[CMD_RELEASE_ID_LEN];
};

enum CommandConfigField : uint8_t {
  CFG_FIELD_WAKE_INTERVAL = 1U << 0,
  CFG_FIELD_TARGET_STATE  = 1U << 1,
  CFG_FIELD_SENSOR_MASK   = 1U << 2,
  CFG_FIELDS_ALL = CFG_FIELD_WAKE_INTERVAL |
                   CFG_FIELD_TARGET_STATE |
                   CFG_FIELD_SENSOR_MASK,
};

struct CommandResult {
  char       cmdId[CMD_ID_LEN];
  CmdOutcome outcome;
  uint32_t   assignedRevision;   // revision assigned on accept (0 otherwise)
  uint32_t   currentRevision;    // authoritative revision at decision time
};

struct DispatchBatchItem {
  Command    command;
  CmdOutcome outcome;  // prevalidated terminal decision or OUT_ACCEPTED
};

void          dispatcherInit();               // load persisted state from NVS
void          dispatcherResetForTest();        // wipe RAM + NVS (bench only)
uint32_t      dispatcherRevision();
CommandResult dispatcherSubmit(const Command& c);
// Commit a fully prevalidated response as one durable dispatcher transaction.
// Every fresh accepted command compares against initialRevision, while accepted
// commands receive consecutive authoritative revisions. Existing command IDs
// are returned transiently as REPLAY and are never applied twice.
bool          dispatcherSubmitBatch(const DispatchBatchItem* items,
                                    uint8_t count,
                                    uint32_t initialRevision,
                                    CommandResult* outResults);
// Record a parser/transport rejection durably without mutating desired state.
CommandResult dispatcherReject(const char* cmdId, CmdSource source,
                               CmdOutcome outcome = OUT_INVALID);
// Re-write and verify the complete dispatcher state. A cloud command cursor
// must not advance unless this returns true.
bool          dispatcherEnsureDurable();
const DispatchNodeConfig* dispatcherNodeConfig(const char* nodeId);
bool          dispatcherKnownCmd(const char* cmdId);          // seen before?
bool          dispatcherResultFor(const char* cmdId, CommandResult* out);
// Restore the stored payload for an accepted command during cursor-loss/reboot
// replay. Returns false for terminal rejections or superseded commands.
bool          dispatcherCommandForResult(const char* cmdId, Command* out);
uint8_t       dispatcherRecentResults(CommandResult* out, uint8_t max);   // for status/UI
// Canonical control-status JSON: {"stateRevision":N,"lastChangeSource":"...","results":[...]}
// One serializer for both GET /api/control and the cloud status.control{} block.
String        dispatcherStatusJson();
CmdSource     dispatcherLastChangeSource();
// Associate a mothership revision with the existing 16-bit NODE_CONFIG version.
bool          dispatcherBindNodeConfigVersion(const char* nodeId,
                                               uint32_t revision,
                                               uint16_t configVersion);
// True only while cmdId is the current desired command for nodeId. This repairs
// a reset between dispatcher persistence and node-registry/cursor persistence.
bool          dispatcherCurrentCommandForNode(const char* nodeId,
                                              const char* cmdId,
                                              uint32_t* revision,
                                              uint16_t* configVersion);
bool          dispatcherCurrentGlobalCommand(const char* cmdId,
                                             uint32_t* revision);
// Resolve CONFIG_ACK's wire configVersion back to the dispatcher revision.
bool          dispatcherRevisionForConfigVersion(const char* nodeId,
                                                 uint16_t configVersion,
                                                 uint32_t* revision);
// Marks the retained command result CONVERGED exactly once.
bool          dispatcherMarkConverged(const char* nodeId, uint32_t revision);
bool          dispatcherMarkGlobalConverged(uint32_t revision);
