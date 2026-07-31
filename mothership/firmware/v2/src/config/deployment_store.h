#pragma once

#include <Arduino.h>

// ---------------------------------------------------------------------------
// Deployment store — durable, crash-safe deployment identity for the FieldHub.
// ---------------------------------------------------------------------------
// A node's identity is its MAC-derived nodeId and nothing else, so moving a node
// from site A to site B stitches both sites into one series. A per-node
// deployment epoch fixes that: the FieldHub owns the counter, persists it, and
// stamps it onto every reading, letting the backend resolve (nodeId, epoch) to a
// distinct deployment.
//
// WHY LITTLEFS AND NOT NVS
// ------------------------
// The NVS partition is 20 KB (partitions.csv: nvs,data,nvs,0x9000,0x5000) and
// already carries paired_nodes / node_meta / node_dcfg / dispatch / tx. A
// 64-node deployment table plus an event outbox does not fit there safely — NVS
// near capacity fails writes rather than degrading. More importantly, the
// deployment state must be updated ATOMICALLY: a set of independent
// Preferences::put* calls is not atomic as a group, so a power loss between them
// yields a mixture of old and new state. Publishing a "count" key last does not
// protect an in-place update of an existing record.
//
// LittleFS gives us a genuinely atomic commit via the same temp->rename sequence
// upload_queue.cpp already uses for /datalog.csv, so the whole record moves from
// one consistent state to the next in one step:
//
//   write /deploy.tmp
//     -> rename /deploy.bin -> /deploy.bak
//     -> rename /deploy.tmp -> /deploy.bin
//     -> remove /deploy.bak
//
// Load picks whichever of .bin / .bak carries the higher generation AND a valid
// checksum, so an interrupted commit resolves to the complete previous record or
// the complete new one — never a mixture.
//
// Nothing here touches NVS; savePairedNodes() is deliberately left alone.

// ---------------------------------------------------------------------------
// Tunables
// ---------------------------------------------------------------------------
static constexpr size_t kMaxDeployNodes  = 64;  // matches kMaxPairedNodes
static constexpr size_t kBoundaryHistory = 4;   // epochs we can attribute exactly
static constexpr size_t kMaxOutboxEvents = 16;  // backend per-request cap is 64

static constexpr uint16_t kDeployUserIdLen = 4;   // "001" + NUL
static constexpr uint16_t kDeployNameLen   = 32;
static constexpr uint16_t kDeployNodeIdLen = 16;
static constexpr uint16_t kDeployEventIdLen = 24;

// A node clock below this is treated as unset rather than as a real sample time.
// Same floor espnow_config.cpp:900 uses to classify a node's HELLO clock.
static constexpr uint32_t kPlausibleSampleFloor = 1704067200UL;  // 2024-01-01

// 0 is the legacy/unresolved sentinel and must never be produced by a live
// deployment, so an epoch may not wrap.
static constexpr uint16_t kMaxEpoch = 65535;

// ---------------------------------------------------------------------------
// Record shapes (persisted verbatim — changing these needs a version bump)
// ---------------------------------------------------------------------------

// One (epoch, startedUnix) pair. Retained so a backlog delivered several
// deployments late can be attributed to the epoch it was actually taken under,
// rather than decremented by one and guessed at.
struct DeploymentBoundary {
  uint16_t epoch;
  uint32_t startedUnix;
};

// In-flight lifecycle transition. Start and End commit in opposite orders (see
// deployment_epoch.h), so recovery differs per op.
enum DeployPendingOp : uint8_t {
  DEPLOY_OP_NONE        = 0,
  DEPLOY_OP_START       = 1,  // epoch NOT yet committed
  DEPLOY_OP_END_STANDBY = 2,  // end IS committed; STANDBY not yet confirmed
};

struct DeploymentSlot {
  // Stable identity. The registry keys on MAC (registerNode matches by MAC), and
  // keying the persisted record by array index the way paired_nodes does would
  // silently reassociate deployments if the registry order ever changed.
  uint8_t  mac[6];
  char     nodeId[kDeployNodeIdLen];

  uint16_t epoch;              // 0 = never deployed
  uint32_t startedUnix;        // current deployment start (authoritative)
  uint32_t endedUnix;          // 0 = active

  DeploymentBoundary history[kBoundaryHistory];  // oldest -> newest
  uint8_t  historyCount;

  uint8_t  pendingOp;          // DeployPendingOp
  uint16_t pendingEpoch;
  uint32_t pendingUnix;        // staged timestamp, so recovery replays the same one

  // Identity proposed for a deployment that has not started yet. Held here
  // instead of written straight to node_meta so an abandoned wizard or a
  // rejected number collision cannot overwrite an ended deployment's archived
  // identity before its outbox event is written.
  char     stagedUserId[kDeployUserIdLen];
  char     stagedName[kDeployNameLen];
  float    stagedLat;
  float    stagedLon;
  bool     hasStagedIdentity;

  bool     inUse;
};

// One complete deployment snapshot destined for the backend. status.nodes[] is
// current-state reporting and cannot carry history: ending epoch 1 and starting
// epoch 2 before an upload would leave no trace of epoch 1. These events are the
// durable record.
struct DeploymentEvent {
  char     eventId[kDeployEventIdLen];
  char     nodeId[kDeployNodeIdLen];
  uint16_t deploymentEpoch;
  uint32_t deploymentStartedUnix;
  uint32_t deploymentEndedUnix;
  char     userId[kDeployUserIdLen];
  char     name[kDeployNameLen];
  float    latitude;
  float    longitude;
  // Most recent backend rejection reason, written as an operator-facing
  // sentence. A conflicted event STAYS queued: it did not commit, and the
  // common case (a number still held by another node's active deployment)
  // resolves itself once that node's End event lands, at which point the retry
  // succeeds with no operator action. The hub cannot see the backend's receipt
  // ledger, so this string is the only way to explain the wait.
  char     conflictReason[96];
  bool     inUse;
};

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

// Mount + load (or migrate, or create). Call after initFlash(). Returns false
// only if LittleFS is unusable — in which case the CSV buffer is dead too, so
// the hub is not logging anyway.
bool deploymentStoreBegin();

// True once deploymentStoreBegin() has succeeded.
bool deploymentStoreReady();

// True when deploymentStoreBegin() had to CREATE the record rather than load
// one — i.e. neither /deploy.bin nor /deploy.bak yielded a valid record. That
// covers the ordinary first boot, and also the (very unlikely) case where both
// copies exist but fail their checksums.
//
// deployment_epoch.cpp uses this to decide whether to seed epoch 1 from the
// existing registry; the store itself stays free of any registry dependency so
// it can be linked into a test env on its own.
//
// The double-corruption case therefore reseeds every registered node to epoch 1.
// That degrades safely rather than merging sites: the backend freezes ended_at
// on the original epoch-1 row, so readings restamped as epoch 1 fall outside its
// window and quarantine as POST_DEPLOYMENT instead of joining the old history.
bool deploymentStoreWasCreatedFresh();

// Push a boundary onto a slot's history ring, dropping the oldest when full.
void deploymentPushBoundary(DeploymentSlot& slot, uint16_t epoch, uint32_t startedUnix);

// ---------------------------------------------------------------------------
// Slot access
// ---------------------------------------------------------------------------
const DeploymentSlot* deploymentFindByNodeId(const char* nodeId);
const DeploymentSlot* deploymentFindByMac(const uint8_t* mac);

// Create-or-fetch a mutable slot. Returns nullptr when the table is full.
DeploymentSlot* deploymentSlotFor(const uint8_t* mac, const char* nodeId);

// Commit the current in-RAM record to flash atomically. Every mutation helper
// below leaves the record dirty; the caller commits once, so a whole lifecycle
// transition lands in a single atomic write.
bool deploymentStoreCommit();

// ---------------------------------------------------------------------------
// Outbox
// ---------------------------------------------------------------------------

// Deterministic: fnv1a32(nodeId) hex + "-" + epoch. Stable across retries, which
// is what makes a lost HTTP response safe to resend, and unique per (node,
// epoch), which is what stops a new deployment overwriting the previous one's
// event. The backend upserts on (project_id, node_id, epoch).
void deploymentMakeEventId(const char* nodeId, uint16_t epoch, char* out, size_t outLen);

// Upsert a full deployment snapshot into the outbox. Does NOT commit — the
// caller folds this into the same atomic write as the state change.
// Returns false when the outbox is full (the operation must then be refused
// rather than silently dropping history).
bool deploymentOutboxUpsert(const DeploymentEvent& event);

// True when an upsert of this eventId would succeed — either it is already
// queued (upsert replaces in place) or a free slot exists. Callers use this to
// refuse an operation BEFORE persisting any intent, since a transition that has
// already told a node to change state cannot be unwound just because the record
// of it has nowhere to go.
bool deploymentOutboxHasRoomFor(const char* eventId);

uint8_t deploymentOutboxCount();
const DeploymentEvent* deploymentOutboxAt(uint8_t index);

// Remove exactly the named event. ONLY for acknowledged events — an ack means
// the backend committed it. Does not commit the record.
bool deploymentOutboxRemove(const char* eventId);

// Record a backend rejection against a queued event and KEEP it queued. A
// conflict did not commit, so dropping it would lose the deployment record; the
// number-collision case in particular clears itself once the other node's End
// event lands. Returns false if the event is no longer queued.
bool deploymentOutboxNoteConflict(const char* eventId, const char* reason);

// Operator-facing text for the most recent unresolved conflict ("" if none), so
// the Field UI can explain why a deployment has not reached the dashboard yet.
String deploymentOutboxConflictSummary();

// Serialise the outbox as status.deploymentEvents[]. Empty array when none.
String deploymentOutboxToJson();

// ---------------------------------------------------------------------------
// Diagnostics
// ---------------------------------------------------------------------------

// Count of readings whose timestamp fell outside every retained boundary and
// were clamped to the oldest known epoch. Emitted as a status scalar so the
// backend can tell approximate attribution from exact.
uint32_t deploymentEpochClampCount();
void     deploymentNoteEpochClamp();

// Test seam: drop all in-RAM state (does not touch flash).
void deploymentStoreResetForTest();
