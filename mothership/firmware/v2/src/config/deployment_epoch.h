#pragma once

#include <Arduino.h>
#include "config/deployment_store.h"

// ---------------------------------------------------------------------------
// Deployment lifecycle — Start / End, the number guard, and epoch resolution.
// ---------------------------------------------------------------------------
// Pure logic over registeredNodes + the deployment store, kept out of
// config_server.cpp so it can be linked into a test env (config_server.cpp is
// excluded from every test env because it is tangled with WiFi/LittleFS/NVS
// globals).

// ---------------------------------------------------------------------------
// Number guard
// ---------------------------------------------------------------------------
struct DeploymentGuardResult {
  bool   free;          // the number is available
  String holderLabel;   // name / userId / nodeId of the ACTIVE holder ("" if free)
  String holderNodeId;
};

// A number is held only by an ACTIVE deployment (startedUnix != 0 && endedUnix
// == 0). An ended deployment keeps its number for the archive record but does
// not hold it — that is what makes the permute workflow (End every node, then
// Start each with new numbers) possible.
DeploymentGuardResult checkNumberAvailable(const String& number,
                                           const String& excludeNodeId);

// ---------------------------------------------------------------------------
// Lifecycle results
// ---------------------------------------------------------------------------
enum DeploymentOpStatus : uint8_t {
  DEPLOY_OK = 0,
  DEPLOY_ERR_UNKNOWN_NODE,
  DEPLOY_ERR_RTC_UNSET,       // hub clock implausible — never stamp a fake time
  DEPLOY_ERR_NUMBER_TAKEN,
  DEPLOY_ERR_NUMBER_MISSING,
  DEPLOY_ERR_STATE,           // e.g. Start on a node that already has an active deployment
  DEPLOY_ERR_LEGACY_BACKLOG,  // unstamped rows still queued (see uploadQueueHasLegacyRows)
  DEPLOY_ERR_EPOCH_OVERFLOW,
  DEPLOY_ERR_OUTBOX_FULL,
  DEPLOY_ERR_PERSIST,         // durable commit failed — state rolled back
  DEPLOY_ERR_CONFIG_QUEUE,    // desired-config apply failed
  DEPLOY_REPLAYED,            // expectedEpoch did not match: treat as a repeat, no action
};

struct DeploymentOpResult {
  DeploymentOpStatus status;
  String  message;        // operator-facing
  String  holderLabel;    // set for DEPLOY_ERR_NUMBER_TAKEN
  uint16_t epoch;         // resulting (or current) epoch
};

// Hook the desired-config apply so config_server can inject
// applyLocalDesiredConfig() without deployment_epoch.cpp depending on the whole
// config-server/dispatcher stack (and so tests can stub it).
// Returns true only when the change is durable AND applied to the registry AND
// accepted/replayed by the dispatcher.
typedef bool (*DeploymentConfigApplyFn)(const String& nodeId, uint8_t targetState);
void deploymentSetConfigApplyFn(DeploymentConfigApplyFn fn);

// Same for the legacy-backlog gate, so the epoch unit does not have to link the
// upload queue.
typedef bool (*DeploymentLegacyBacklogFn)(uint32_t* pendingRowsOut);
void deploymentSetLegacyBacklogFn(DeploymentLegacyBacklogFn fn);

// And for the hub clock.
typedef uint32_t (*DeploymentNowFn)();
void deploymentSetNowFn(DeploymentNowFn fn);

// Optional durable archive sink (the production SD card). The LittleFS store
// remains authoritative, so a sink failure is reported but never rolls back a
// committed lifecycle transition.
typedef bool (*DeploymentArchiveSinkFn)(const char* nodeId,
                                        const DeploymentEvent& event);
void deploymentSetArchiveSinkFn(DeploymentArchiveSinkFn fn);

// Whether FieldMesh lifecycle events should occupy the upload outbox. The
// default when no hook is installed is true (preserves tests and connected
// behavior). Standalone/custom operation keeps the local archive but must never
// fill a cloud-only outbox and block fleet management.
typedef bool (*DeploymentCloudEventsEnabledFn)();
void deploymentSetCloudEventsEnabledFn(DeploymentCloudEventsEnabledFn fn);

struct DeploymentBackfillResult {
  uint16_t queued;    // newly added to the durable outbox this call
  uint16_t deferred;  // known records waiting for a later free outbox slot
  bool     committed;
};

// Fill available outbox slots with complete local deployment records that
// FieldMesh has not acknowledged yet. Repeated calls are idempotent and allow a
// standalone history larger than the 16-event transport window to drain over
// successive connected syncs.
DeploymentBackfillResult deploymentQueueFieldMeshBackfill();

// ---------------------------------------------------------------------------
// Lifecycle operations
// ---------------------------------------------------------------------------

// Seed epoch 1 for nodes deployed under pre-epoch firmware. Safe to call every
// boot; only acts when the store was created fresh or a registry node has no
// slot yet.
void deploymentSeedFromRegistry();

// Replay an interrupted transition. Call once after deploymentStoreBegin() and
// loadPairedNodes().
//   OP_START       -> the epoch was NOT committed: clear the flag, keep the old
//                     epoch and the staged identity, report so the UI can prompt.
//   OP_END_STANDBY -> the End IS committed: re-queue STANDBY and keep the flag
//                     until it verifies.
void deploymentRecoverPending();

// Start a new deployment: guards, then epoch++ (or 1), startedUnix, identity,
// boundary and outbox event committed as ONE atomic write — but only after the
// ACTIVE desired config is durably queued, so we never end up with a live epoch
// the node knows nothing about.
DeploymentOpResult beginNewDeployment(const String& nodeId,
                                      const String& number,
                                      const String& name,
                                      float latitude,
                                      float longitude,
                                      uint16_t expectedEpoch);

// End the current deployment: endedUnix + outbox event committed FIRST (the
// archived record is the point of the feature and must not be lost to a failed
// radio queue), then STANDBY is queued. A failed queue leaves a visible
// OP_END_STANDBY transition that the caller surfaces and the sync reconcile
// retries.
DeploymentOpResult endDeployment(const String& nodeId, uint16_t expectedEpoch);

// unpair implies End. Captures the ended deployment (with its identity) into the
// outbox before the caller clears node_meta. No-op when there is no active
// deployment.
DeploymentOpResult endDeploymentForUnpair(const String& nodeId);

// action=start on a node that has never been deployed initialises epoch 1.
// Never increments; a node with an ended deployment is rejected so a redeploy
// cannot silently re-stitch two sites.
DeploymentOpResult ensureFirstDeployment(const String& nodeId);

// Stage identity for a deployment that has not started yet (never-deployed or
// ended). Nothing reaches node_meta until beginNewDeployment() commits.
bool deploymentStageIdentity(const String& nodeId,
                             const String* number,
                             const String* name,
                             const float* latitude,
                             const float* longitude);

// True when identity edits must be staged rather than written through.
bool deploymentIdentityIsStaged(const String& nodeId);

// Update the outbox event for a node's ACTIVE deployment after an in-place
// identity/location edit, so the backend upserts the new values.
bool deploymentTouchActiveEvent(const String& nodeId);

// ---------------------------------------------------------------------------
// Epoch resolution (hot path — called once per received snapshot)
// ---------------------------------------------------------------------------

// Returns the epoch the sample was actually taken under.
//   0                  unknown node / never deployed (backend falls back)
//   exact epoch        sampleUnix falls inside a retained boundary interval
//   oldest known epoch sample predates every retained boundary (clamped, counted)
uint16_t resolveEpochForSample(const char* nodeId, uint32_t sampleUnix);

// Mirror the store's per-node fields onto registeredNodes so the UI and
// status.nodes[] can read them without touching the store.
void deploymentSyncRegistryMirror();
