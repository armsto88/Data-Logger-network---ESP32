#include "config/deployment_epoch.h"
#include "config/node_registry.h"

#include <string.h>

// ---------------------------------------------------------------------------
// Injected collaborators (see header for why these are seams, not includes)
// ---------------------------------------------------------------------------
static DeploymentConfigApplyFn   gConfigApply   = nullptr;
static DeploymentLegacyBacklogFn gLegacyBacklog = nullptr;
static DeploymentNowFn           gNow           = nullptr;
static DeploymentArchiveSinkFn   gArchiveSink   = nullptr;
static DeploymentCloudEventsEnabledFn gCloudEventsEnabled = nullptr;

void deploymentSetConfigApplyFn(DeploymentConfigApplyFn fn)     { gConfigApply = fn; }
void deploymentSetLegacyBacklogFn(DeploymentLegacyBacklogFn fn) { gLegacyBacklog = fn; }
void deploymentSetNowFn(DeploymentNowFn fn)                     { gNow = fn; }
void deploymentSetArchiveSinkFn(DeploymentArchiveSinkFn fn)     { gArchiveSink = fn; }
void deploymentSetCloudEventsEnabledFn(DeploymentCloudEventsEnabledFn fn) {
  gCloudEventsEnabled = fn;
}

static bool cloudEventsEnabled() {
  return gCloudEventsEnabled ? gCloudEventsEnabled() : true;
}

static uint32_t nowUnix() { return gNow ? gNow() : 0; }

static bool applyDesiredState(const String& nodeId, uint8_t targetState) {
  // No hook wired (unit test / early boot): treat as success so the state
  // machine itself stays testable in isolation.
  return gConfigApply ? gConfigApply(nodeId, targetState) : true;
}

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

static NodeInfo* findNode(const String& nodeId) {
  for (auto& n : registeredNodes) {
    if (n.nodeId == nodeId) return &n;
  }
  return nullptr;
}

static DeploymentOpResult makeResult(DeploymentOpStatus status,
                                     const String& message,
                                     uint16_t epoch = 0) {
  DeploymentOpResult r;
  r.status  = status;
  r.message = message;
  r.epoch   = epoch;
  return r;
}

static bool slotHasActiveDeployment(const DeploymentSlot& s) {
  return s.startedUnix != 0 && s.endedUnix == 0;
}

static String slotDisplayLabel(const DeploymentSlot& s) {
  const String nodeId(s.nodeId);
  const String name = getNodeName(nodeId);
  if (name.length()) return name;
  const String userId = getNodeUserId(nodeId);
  if (userId.length()) return userId;
  return nodeId;
}

// Build the outbox event for a node's CURRENT deployment state.
static void fillEvent(const DeploymentSlot& s, DeploymentEvent& e) {
  memset(&e, 0, sizeof(e));
  deploymentMakeEventId(s.nodeId, s.epoch, e.eventId, sizeof(e.eventId));
  strncpy(e.nodeId, s.nodeId, sizeof(e.nodeId) - 1);
  e.deploymentEpoch       = s.epoch;
  e.deploymentStartedUnix = s.startedUnix;
  e.deploymentEndedUnix   = s.endedUnix;

  const String nodeId(s.nodeId);
  const String userId = getNodeUserId(nodeId);
  const String name   = getNodeName(nodeId);
  strncpy(e.userId, userId.c_str(), sizeof(e.userId) - 1);
  strncpy(e.name,   name.c_str(),   sizeof(e.name) - 1);

  const NodeInfo* n = findNode(nodeId);
  e.latitude  = n ? n->latitude  : NAN;
  e.longitude = n ? n->longitude : NAN;
  e.inUse = true;
}

// ---------------------------------------------------------------------------
// Number guard
// ---------------------------------------------------------------------------

DeploymentGuardResult checkNumberAvailable(const String& number,
                                           const String& excludeNodeId) {
  DeploymentGuardResult out;
  out.free = true;

  const String wanted = normalizeUserId(number);
  if (wanted.length() == 0) return out;   // nothing to collide with

  // Walk the registry (not the store) so a node that has never been through the
  // store still participates in the guard.
  for (const auto& n : registeredNodes) {
    if (n.nodeId == excludeNodeId) continue;
    const DeploymentSlot* s = deploymentFindByNodeId(n.nodeId.c_str());
    if (!s || !slotHasActiveDeployment(*s)) continue;   // ended deployments do not hold
    if (normalizeUserId(getNodeUserId(n.nodeId)) != wanted) continue;

    out.free         = false;
    out.holderNodeId = n.nodeId;
    out.holderLabel  = slotDisplayLabel(*s);
    return out;
  }
  return out;
}

// ---------------------------------------------------------------------------
// Seeding + recovery
// ---------------------------------------------------------------------------

void deploymentSeedFromRegistry() {
  if (!deploymentStoreReady()) return;

  bool dirty = false;
  for (const auto& n : registeredNodes) {
    if (n.state != PAIRED && n.state != DEPLOYED) continue;
    if (deploymentFindByNodeId(n.nodeId.c_str())) continue;

    DeploymentSlot* s = deploymentSlotFor(n.mac, n.nodeId.c_str());
    if (!s) continue;

    // A node deployed under pre-epoch firmware becomes epoch 1 starting at
    // deployedSinceUnix, so its existing history stays one continuous series
    // and it keeps holding its number against the guard.
    if (n.state == DEPLOYED || n.deployedSinceUnix != 0) {
      // deploymentStartedUnix is MANDATORY and must be > 0 on every event the
      // backend receives, including End events (where it is the start of the
      // epoch being closed). A zero start is recorded as a conflict and never
      // acknowledged, so the outbox would retry it forever.
      //
      // deployedSinceUnix can legitimately be 0 here: registerNode() stamps it
      // from getRTCTime(), which returns 0 when the DS3231 was never found. Fall
      // back to the current hub clock, and if that is not plausible either, do
      // NOT seed — an unseeded node stamps epoch 0 and the backend falls back,
      // which is recoverable; a seeded node with a zero start is not.
      uint32_t seedStart = n.deployedSinceUnix;
      if (seedStart == 0) {
        const uint32_t nowFallback = nowUnix();
        if (nowFallback < kPlausibleSampleFloor) {
          Serial.printf("[DEPLOY] %s: cannot seed epoch 1 — no deploy time and hub clock unset\n",
                        n.nodeId.c_str());
          continue;
        }
        seedStart = nowFallback;
        Serial.printf("[DEPLOY] %s: deployedSinceUnix was 0; seeding start from hub clock\n",
                      n.nodeId.c_str());
      }
      s->epoch       = 1;
      s->startedUnix = seedStart;
      s->endedUnix   = 0;
      // Connected pre-epoch hubs were backend-migrated before this firmware was
      // allowed to ship. Standalone hubs have no such server record and must be
      // eligible for backfill if they connect later.
      s->activeFieldMeshAcked = cloudEventsEnabled();
      s->historyCount = 0;
      deploymentPushBoundary(*s, 1, seedStart);
      Serial.printf("[DEPLOY] Seeded %s as epoch 1 (since %lu)\n",
                    n.nodeId.c_str(), (unsigned long)seedStart);
    }
    dirty = true;
  }
  if (dirty) {
    deploymentStoreCommit();
    deploymentSyncRegistryMirror();
  }
}

// Publish a slot's staged identity to node_meta and the registry, then clear it.
//
// Staging exists so an abandoned wizard or a rejected number collision cannot
// overwrite an ended deployment's archived identity before its outbox event is
// written. That means NOTHING reaches node_meta until a deployment actually
// commits — so every path that commits one must call this, or the operator's
// number and name are written to a staging area with no consumer and silently
// never take effect.
static void publishStagedIdentity(DeploymentSlot& slot, const String& nodeId,
                                  NodeInfo* node) {
  if (slot.hasStagedIdentity) {
    if (slot.stagedUserId[0]) setNodeUserId(nodeId, String(slot.stagedUserId));
    if (slot.stagedName[0])   setNodeName(nodeId, String(slot.stagedName));
    if (node) {
      node->userId = getNodeUserId(nodeId);
      node->name   = getNodeName(nodeId);
      if (!isnan(slot.stagedLat) && !isnan(slot.stagedLon) &&
          (slot.stagedLat != 0.0f || slot.stagedLon != 0.0f)) {
        node->latitude  = slot.stagedLat;
        node->longitude = slot.stagedLon;
      }
    }
  }
  slot.hasStagedIdentity = false;
  slot.stagedUserId[0] = '\0';
  slot.stagedName[0]   = '\0';
  slot.stagedLat = NAN;
  slot.stagedLon = NAN;
}

// Complete a Start from the intent already persisted in the slot.
//
// This is the whole reason phase 1 records the epoch, timestamp AND identity
// before the node is told to go ACTIVE: once ACTIVE is durably queued the node
// will start recording, so abandoning the transition would leave readings
// stamped against epoch 0 or the previous ENDED epoch. Every persisted
// intermediate state must therefore be completable without any in-RAM context.
//
// Idempotent: safe to call again after an interrupted commit.
static void finalizeStartFromPending(DeploymentSlot& slot, const String& nodeId,
                                     NodeInfo* node) {
  const uint16_t newEpoch = slot.pendingEpoch;
  const uint32_t startedAt = slot.pendingUnix;

  publishStagedIdentity(slot, nodeId, node);

  slot.epoch       = newEpoch;
  slot.startedUnix = startedAt;
  slot.endedUnix   = 0;
  slot.activeFieldMeshAcked = false;
  deploymentPushBoundary(slot, newEpoch, startedAt);

  slot.pendingOp    = DEPLOY_OP_NONE;
  slot.pendingEpoch = 0;
  slot.pendingUnix  = 0;

  DeploymentEvent event;
  fillEvent(slot, event);
  if (cloudEventsEnabled()) deploymentOutboxUpsert(event);
}

void deploymentRecoverPending() {
  if (!deploymentStoreReady()) return;

  bool dirty = false;
  for (auto& n : registeredNodes) {
    const DeploymentSlot* cs = deploymentFindByNodeId(n.nodeId.c_str());
    if (!cs || cs->pendingOp == DEPLOY_OP_NONE) continue;

    DeploymentSlot* s = deploymentSlotFor(n.mac, n.nodeId.c_str());
    if (!s) continue;

    if (s->pendingOp == DEPLOY_OP_START) {
      // OP_START is only persisted once the full intent is durable, and it is
      // only reachable at boot if we crashed at or after the point where ACTIVE
      // was being queued. Completing forward is the only safe direction: the
      // node may already be recording, and those readings must land on the
      // epoch the operator asked for rather than on the previous deployment.
      Serial.printf("[DEPLOY] Recovery: %s completing interrupted START -> epoch %u\n",
                    n.nodeId.c_str(), (unsigned)s->pendingEpoch);
      // Re-assert ACTIVE BEFORE publishing the new epoch. If phase 2 completed
      // before the crash, the durable desired-config layer reports success
      // idempotently. If it did not, a failed queue attempt must leave the full
      // phase-1 intent pending so the next wake can retry; clearing pending here
      // would report an active deployment while the node remained in standby.
      if (applyDesiredState(n.nodeId, 2)) {
        finalizeStartFromPending(*s, n.nodeId, &n);
        dirty = true;
      } else {
        Serial.printf("[DEPLOY] Recovery: %s ACTIVE could not be queued; START remains pending\n",
                      n.nodeId.c_str());
      }
    } else if (s->pendingOp == DEPLOY_OP_END_STANDBY) {
      // End commits the ended-ness FIRST, so the archive record is safe; only
      // the STANDBY request is outstanding. Re-queue it. Also replay the SD
      // append: its deterministic event ID makes this idempotent and closes the
      // power-loss gap between the LittleFS commit and card write.
      Serial.printf("[DEPLOY] Recovery: %s ended but STANDBY unconfirmed — re-queuing\n",
                    n.nodeId.c_str());
      DeploymentEvent endedEvent;
      fillEvent(*s, endedEvent);
      if (gArchiveSink && !gArchiveSink(s->nodeId, endedEvent)) {
        Serial.println("[DEPLOY] Recovery: SD deployment archive remains unavailable");
      }
      if (applyDesiredState(n.nodeId, 3)) {
        s->pendingOp   = DEPLOY_OP_NONE;
        s->pendingUnix = 0;
        dirty = true;
      }
    }
  }
  if (dirty) {
    deploymentStoreCommit();
    deploymentSyncRegistryMirror();
  }
}

// ---------------------------------------------------------------------------
// Start
// ---------------------------------------------------------------------------

DeploymentOpResult beginNewDeployment(const String& nodeId,
                                      const String& number,
                                      const String& name,
                                      float latitude,
                                      float longitude,
                                      uint16_t expectedEpoch) {
  NodeInfo* node = findNode(nodeId);
  if (!node) return makeResult(DEPLOY_ERR_UNKNOWN_NODE, "Node is not registered");
  if (!deploymentStoreReady()) {
    return makeResult(DEPLOY_ERR_PERSIST, "Deployment storage unavailable");
  }

  // A hub with an unset clock must not stamp a timestamp that merely looks like
  // a sentinel — refuse outright.
  const uint32_t now = nowUnix();
  if (now < kPlausibleSampleFloor) {
    return makeResult(DEPLOY_ERR_RTC_UNSET,
                      "Set the FieldHub clock before starting a deployment");
  }

  const String wanted = normalizeUserId(number);
  if (wanted.length() == 0) {
    return makeResult(DEPLOY_ERR_NUMBER_MISSING, "Enter a node number (e.g. 001)");
  }

  DeploymentSlot* slot = deploymentSlotFor(node->mac, nodeId.c_str());
  if (!slot) return makeResult(DEPLOY_ERR_PERSIST, "Deployment table is full");

  // Idempotency: a repeat of an already-applied Start (double submit, lost
  // response) carries the stale prior epoch. Report the current state instead
  // of incrementing again.
  if (expectedEpoch != slot->epoch) {
    return makeResult(DEPLOY_REPLAYED,
                      "This deployment has already been started", slot->epoch);
  }

  if (slotHasActiveDeployment(*slot)) {
    return makeResult(DEPLOY_ERR_STATE,
                      "End the current deployment before starting a new one",
                      slot->epoch);
  }

  if (slot->epoch >= kMaxEpoch) {
    return makeResult(DEPLOY_ERR_EPOCH_OVERFLOW,
                      "Deployment counter exhausted for this node", slot->epoch);
  }

  // Unstamped rows still queued would follow the backend's fallback into the
  // NEW deployment. Block until they drain rather than misattribute them.
  if (gLegacyBacklog) {
    uint32_t pendingRows = 0;
    if (gLegacyBacklog(&pendingRows)) {
      String msg = "Upload existing readings before starting a new deployment";
      if (pendingRows > 0) msg += " (" + String(pendingRows) + " rows pending)";
      return makeResult(DEPLOY_ERR_LEGACY_BACKLOG, msg, slot->epoch);
    }
  }

  const DeploymentGuardResult guard = checkNumberAvailable(wanted, nodeId);
  if (!guard.free) {
    DeploymentOpResult r = makeResult(
        DEPLOY_ERR_NUMBER_TAKEN,
        "Number " + wanted + " is in use by \"" + guard.holderLabel +
            "\" — end that deployment first",
        slot->epoch);
    r.holderLabel = guard.holderLabel;
    return r;
  }

  const uint16_t newEpoch = (uint16_t)(slot->epoch + 1);

  // Snapshot the COMPLETE slot, not just epoch/started/ended. Boundary history
  // and staged identity are part of the deployment's state; restoring a subset
  // would leave the record internally inconsistent after a failed Start.
  const DeploymentSlot slotBefore = *slot;

  // Outbox capacity is checked HERE, before any intent is persisted. Once
  // phase 2 has queued ACTIVE the transition can only go forward, so there is
  // no later point at which "no room for the event" can be reported without
  // either losing the record or stranding the node.
  if (cloudEventsEnabled()) {
    char eventId[kDeployEventIdLen];
    deploymentMakeEventId(nodeId.c_str(), newEpoch, eventId, sizeof(eventId));
    if (!deploymentOutboxHasRoomFor(eventId)) {
      return makeResult(DEPLOY_ERR_OUTBOX_FULL,
                        "Upload pending deployment changes before starting another",
                        slot->epoch);
    }
  }

  // --- Phase 1: persist the FULL intent, before the node is told anything --
  // Everything finalizeStartFromPending() needs must be durable here, because
  // from phase 2 onward the only safe direction is forward.
  slot->pendingOp    = DEPLOY_OP_START;
  slot->pendingEpoch = newEpoch;
  slot->pendingUnix  = now;
  slot->hasStagedIdentity = true;
  strncpy(slot->stagedUserId, wanted.c_str(), sizeof(slot->stagedUserId) - 1);
  slot->stagedUserId[sizeof(slot->stagedUserId) - 1] = '\0';
  strncpy(slot->stagedName, name.c_str(), sizeof(slot->stagedName) - 1);
  slot->stagedName[sizeof(slot->stagedName) - 1] = '\0';
  if (!isnan(latitude) && !isnan(longitude) &&
      (latitude != 0.0f || longitude != 0.0f)) {
    slot->stagedLat = latitude;
    slot->stagedLon = longitude;
  }
  if (!deploymentStoreCommit()) {
    *slot = slotBefore;
    return makeResult(DEPLOY_ERR_PERSIST, "Could not save deployment state",
                      slotBefore.epoch);
  }

  // --- Phase 2: get the node durably queued ACTIVE ------------------------
  if (!applyDesiredState(nodeId, 2)) {
    // ACTIVE was NOT queued, so the node is not about to record under an epoch
    // that does not exist. Compensate to STANDBY first — the node may have been
    // left ACTIVE by an earlier attempt — and only then unwind.
    const bool compensated = applyDesiredState(nodeId, 3);
    if (compensated) {
      *slot = slotBefore;
      // The unwind is only real once it is DURABLE. If this commit fails, flash
      // still holds the phase-1 DEPLOY_OP_START record, and deploymentRecoverPending()
      // will complete this same Start forward on the next boot — publishing a
      // deployment the operator was just told did not happen, and applying a
      // staged number they may have since given to another node. Report the
      // pending outcome instead of claiming nothing changed.
      if (!deploymentStoreCommit()) {
        Serial.printf("[DEPLOY] %s: rollback commit failed; START stays pending for recovery\n",
                      nodeId.c_str());
        return makeResult(DEPLOY_ERR_PERSIST,
                          "Could not reach the node — the deployment will finish at the "
                          "next sync or restart",
                          slotBefore.epoch);
      }
      return makeResult(DEPLOY_ERR_CONFIG_QUEUE,
                        "Could not queue the node's configuration — nothing changed",
                        slotBefore.epoch);
    }
    // Could not queue ACTIVE and could not compensate. Leaving the pending
    // record in place is the safe choice: recovery completes the Start forward
    // rather than risk the node recording against an uncommitted epoch.
    Serial.printf("[DEPLOY] %s: ACTIVE queue failed and STANDBY compensation failed"
                  " — leaving START pending for recovery\n", nodeId.c_str());
    return makeResult(DEPLOY_ERR_CONFIG_QUEUE,
                      "Could not reach the node — the deployment will finish at the "
                      "next sync or restart",
                      slotBefore.epoch);
  }

  // --- Phase 3: finalise from the persisted intent ------------------------
  finalizeStartFromPending(*slot, nodeId, node);

  if (!deploymentStoreCommit()) {
    // ACTIVE is already queued, so we must NOT unwind to the old epoch — the
    // node may start recording at any sync window. The phase-1 record is still
    // on flash, so recovery completes this same Start on the next boot.
    Serial.printf("[DEPLOY] %s: final commit failed; START stays pending for recovery\n",
                  nodeId.c_str());
    return makeResult(DEPLOY_ERR_PERSIST,
                      "Deployment saved but not confirmed — it will finish on restart",
                      newEpoch);
  }

  deploymentSyncRegistryMirror();
  Serial.printf("[DEPLOY] %s started deployment %u as %s\n",
                nodeId.c_str(), (unsigned)newEpoch, wanted.c_str());
  return makeResult(DEPLOY_OK, "Deployment " + String(newEpoch) + " started", newEpoch);
}

// ---------------------------------------------------------------------------
// End
// ---------------------------------------------------------------------------

static DeploymentOpResult endDeploymentInternal(const String& nodeId,
                                                uint16_t expectedEpoch,
                                                bool checkExpected,
                                                bool queueStandby) {
  NodeInfo* node = findNode(nodeId);
  if (!node) return makeResult(DEPLOY_ERR_UNKNOWN_NODE, "Node is not registered");
  if (!deploymentStoreReady()) {
    return makeResult(DEPLOY_ERR_PERSIST, "Deployment storage unavailable");
  }

  DeploymentSlot* slot = deploymentSlotFor(node->mac, nodeId.c_str());
  if (!slot) return makeResult(DEPLOY_ERR_PERSIST, "Deployment table is full");

  if (checkExpected && expectedEpoch != slot->epoch) {
    return makeResult(DEPLOY_REPLAYED, "This deployment has already ended", slot->epoch);
  }
  if (!slotHasActiveDeployment(*slot)) {
    // Idempotent: ending an already-ended deployment is a no-op, not an error.
    return makeResult(DEPLOY_REPLAYED, "No active deployment to end", slot->epoch);
  }

  const uint32_t now = nowUnix();
  if (now < kPlausibleSampleFloor) {
    return makeResult(DEPLOY_ERR_RTC_UNSET,
                      "Set the FieldHub clock before ending a deployment");
  }

  // --- Commit the ended-ness FIRST ---------------------------------------
  // The archived record is the whole point of the feature; losing it to a
  // failed radio queue would be worse than a visible pending stop. The event
  // captures the identity the deployment had at this moment, before any wizard
  // can replace it.
  const DeploymentSlot slotBefore = *slot;
  slot->endedUnix = now;
  slot->pendingOp = queueStandby ? DEPLOY_OP_END_STANDBY : DEPLOY_OP_NONE;
  slot->pendingUnix = now;

  DeploymentEvent event;
  fillEvent(*slot, event);
  DeploymentEvent priorQueuedEvent{};
  const bool hadQueuedEvent = deploymentOutboxGet(event.eventId, priorQueuedEvent);
  const bool queueCloudEvent = cloudEventsEnabled() || hadQueuedEvent;
  if (queueCloudEvent && !deploymentOutboxUpsert(event)) {
    *slot = slotBefore;
    return makeResult(DEPLOY_ERR_OUTBOX_FULL,
                      "Upload pending deployment changes before ending another",
                      slot->epoch);
  }
  deploymentArchiveUpsert(*slot, event);
  if (!deploymentStoreCommit()) {
    *slot = slotBefore;
    if (hadQueuedEvent) deploymentOutboxUpsert(priorQueuedEvent);
    else if (queueCloudEvent) deploymentOutboxRemove(event.eventId);
    return makeResult(DEPLOY_ERR_PERSIST, "Could not save the end of the deployment",
                      slot->epoch);
  }

  // The LittleFS record above is authoritative for lifecycle recovery. SD is a
  // best-effort, capacity-limited long-term export, so a missing/full card must
  // never turn a correctly committed End back into a failed deployment action.
  if (gArchiveSink && !gArchiveSink(slot->nodeId, event)) {
    Serial.println("[DEPLOY] End committed, but SD deployment archive write failed");
  }

  const uint16_t epoch = slot->epoch;
  deploymentSyncRegistryMirror();

  // --- Then stop the node -------------------------------------------------
  // STANDBY (targetState 3), never PAIRED: PAIR_NODE clears the node's local
  // queue (node main.cpp:2755) and is a one-shot send a sleeping node normally
  // misses. STANDBY rides the version-gated NODE_CONFIG reconcile, keeps the
  // node's backlog, and leaves it remotely resumable.
  if (queueStandby) {
    if (applyDesiredState(nodeId, 3)) {
      slot->pendingOp = DEPLOY_OP_NONE;
      deploymentStoreCommit();
      return makeResult(DEPLOY_OK, "Deployment ended", epoch);
    }
    // Ended is committed; the stop is not. Leave the transition visible.
    Serial.printf("[DEPLOY] %s ended but STANDBY queue failed — pending\n", nodeId.c_str());
    return makeResult(DEPLOY_OK, "Ended — still stopping recording", epoch);
  }
  return makeResult(DEPLOY_OK, "Deployment ended", epoch);
}

DeploymentOpResult endDeployment(const String& nodeId, uint16_t expectedEpoch) {
  return endDeploymentInternal(nodeId, expectedEpoch, true, true);
}

DeploymentOpResult endDeploymentForUnpair(const String& nodeId) {
  // The node is leaving the registry, so there is nothing to put in standby —
  // but the ended deployment and its identity must still reach the backend.
  return endDeploymentInternal(nodeId, 0, false, false);
}

// ---------------------------------------------------------------------------
// action=start (first deployment only)
// ---------------------------------------------------------------------------

DeploymentOpResult ensureFirstDeployment(const String& nodeId) {
  NodeInfo* node = findNode(nodeId);
  if (!node) return makeResult(DEPLOY_ERR_UNKNOWN_NODE, "Node is not registered");
  if (!deploymentStoreReady()) {
    return makeResult(DEPLOY_ERR_PERSIST, "Deployment storage unavailable");
  }

  DeploymentSlot* slot = deploymentSlotFor(node->mac, nodeId.c_str());
  if (!slot) return makeResult(DEPLOY_ERR_PERSIST, "Deployment table is full");

  if (slotHasActiveDeployment(*slot)) {
    // stop -> start troubleshooting cycle: deliberately no epoch change.
    return makeResult(DEPLOY_OK, "", slot->epoch);
  }
  if (slot->epoch != 0) {
    // Ended: a plain start here would silently re-stitch two sites.
    return makeResult(DEPLOY_ERR_STATE,
                      "This node's deployment has ended — use Start new deployment",
                      slot->epoch);
  }

  const uint32_t now = nowUnix();
  if (now < kPlausibleSampleFloor) {
    return makeResult(DEPLOY_ERR_RTC_UNSET,
                      "Set the FieldHub clock before deploying");
  }

  // Snapshot before any mutation so a failure restores the staged identity too,
  // not just the epoch fields.
  const DeploymentSlot slotBefore = *slot;

  // This path commits a deployment, so it owns the staged identity exactly as
  // finalizeStartFromPending() does. Without this the number and name the
  // operator typed stay in the staging area forever: node_meta keeps its old
  // value, fillEvent() below reads that old value, and the deployment is
  // archived under the wrong label with no way to correct it — the node now has
  // an ACTIVE deployment, so neither Start new deployment nor a staged edit can
  // publish it. Must run BEFORE fillEvent().
  //
  // A failure below leaves node_meta already written. That is safe HERE and only
  // here: this function is reachable only at epoch 0, so nothing is archived yet
  // and a node with no active deployment does not hold its number against
  // checkNumberAvailable(). The slot restore puts the staged copy back so a
  // later Start republishes it.
  publishStagedIdentity(*slot, nodeId, node);

  slot->epoch       = 1;
  slot->startedUnix = now;
  slot->endedUnix   = 0;
  slot->activeFieldMeshAcked = false;
  deploymentPushBoundary(*slot, 1, now);

  DeploymentEvent event;
  fillEvent(*slot, event);
  const bool queueCloudEvent = cloudEventsEnabled();
  if (queueCloudEvent && !deploymentOutboxUpsert(event)) {
    *slot = slotBefore;
    return makeResult(DEPLOY_ERR_OUTBOX_FULL,
                      "Upload pending deployment changes before deploying", 0);
  }
  if (!deploymentStoreCommit()) {
    *slot = slotBefore;
    if (queueCloudEvent) deploymentOutboxRemove(event.eventId);
    return makeResult(DEPLOY_ERR_PERSIST, "Could not save the deployment", 0);
  }

  deploymentSyncRegistryMirror();
  return makeResult(DEPLOY_OK, "Deployment 1 started", 1);
}

// ---------------------------------------------------------------------------
// Staged identity
// ---------------------------------------------------------------------------

bool deploymentIdentityIsStaged(const String& nodeId) {
  const DeploymentSlot* s = deploymentFindByNodeId(nodeId.c_str());
  if (!s) return true;                       // no deployment yet
  return s->epoch == 0 || s->endedUnix != 0; // never deployed, or ended
}

bool deploymentStageIdentity(const String& nodeId,
                             const String* number,
                             const String* name,
                             const float* latitude,
                             const float* longitude) {
  NodeInfo* node = findNode(nodeId);
  if (!node || !deploymentStoreReady()) return false;

  DeploymentSlot* slot = deploymentSlotFor(node->mac, nodeId.c_str());
  if (!slot) return false;

  if (number) {
    const String norm = normalizeUserId(*number);
    strncpy(slot->stagedUserId, norm.c_str(), sizeof(slot->stagedUserId) - 1);
    slot->stagedUserId[sizeof(slot->stagedUserId) - 1] = '\0';
  }
  if (name) {
    strncpy(slot->stagedName, name->c_str(), sizeof(slot->stagedName) - 1);
    slot->stagedName[sizeof(slot->stagedName) - 1] = '\0';
  }
  if (latitude)  slot->stagedLat = *latitude;
  if (longitude) slot->stagedLon = *longitude;
  slot->hasStagedIdentity = true;
  return deploymentStoreCommit();
}

bool deploymentTouchActiveEvent(const String& nodeId) {
  const DeploymentSlot* current = deploymentFindByNodeId(nodeId.c_str());
  if (!current || !slotHasActiveDeployment(*current)) return false;
  const NodeInfo* node = findNode(nodeId);
  DeploymentSlot* slot = deploymentSlotFor(node ? node->mac : nullptr,
                                           nodeId.c_str());
  if (!slot) return false;

  DeploymentEvent event;
  fillEvent(*slot, event);
  slot->activeFieldMeshAcked = false;
  if (!cloudEventsEnabled() && !deploymentOutboxHasEvent(event.eventId)) {
    return deploymentStoreCommit();
  }
  if (!deploymentOutboxUpsert(event)) {
    // Preserve the dirty marker even when the transport window is full. The
    // connected backfill pass will queue the refreshed record after an ack
    // frees a slot.
    deploymentStoreCommit();
    return false;
  }
  return deploymentStoreCommit();
}

static void fillArchivedEvent(const DeploymentSlot& slot,
                              const DeploymentArchive& archived,
                              DeploymentEvent& event) {
  memset(&event, 0, sizeof(event));
  deploymentMakeEventId(slot.nodeId, archived.epoch,
                        event.eventId, sizeof(event.eventId));
  strncpy(event.nodeId, slot.nodeId, sizeof(event.nodeId) - 1);
  event.deploymentEpoch = archived.epoch;
  event.deploymentStartedUnix = archived.startedUnix;
  event.deploymentEndedUnix = archived.endedUnix;
  strncpy(event.userId, archived.userId, sizeof(event.userId) - 1);
  strncpy(event.name, archived.name, sizeof(event.name) - 1);
  event.latitude = (archived.known & DEPLOY_ARCHIVE_LOCATION_KNOWN)
      ? archived.latitude : NAN;
  event.longitude = (archived.known & DEPLOY_ARCHIVE_LOCATION_KNOWN)
      ? archived.longitude : NAN;
  event.inUse = true;
}

DeploymentBackfillResult deploymentQueueFieldMeshBackfill() {
  DeploymentBackfillResult result{0, 0, true};
  if (!deploymentStoreReady()) {
    result.committed = false;
    return result;
  }

  const uint8_t completeArchive = DEPLOY_ARCHIVE_START_KNOWN |
      DEPLOY_ARCHIVE_END_KNOWN | DEPLOY_ARCHIVE_IDENTITY_KNOWN;
  bool dirty = false;

  auto queueEvent = [&](const DeploymentEvent& event) {
    if (deploymentOutboxHasEvent(event.eventId)) return;
    if (!deploymentOutboxHasRoomFor(event.eventId)) {
      result.deferred++;
      return;
    }
    if (deploymentOutboxUpsert(event)) {
      result.queued++;
      dirty = true;
    } else {
      result.deferred++;
    }
  };

  for (size_t n = 0; n < kMaxDeployNodes; ++n) {
    const DeploymentSlot* slot = deploymentSlotAt(n);
    if (!slot) continue;
    for (uint8_t i = 0; i < slot->archiveCount; ++i) {
      const DeploymentArchive& archived = slot->archive[i];
      if (archived.fieldMeshAcked || archived.epoch == 0 ||
          (archived.known & completeArchive) != completeArchive) continue;
      DeploymentEvent event;
      fillArchivedEvent(*slot, archived, event);
      queueEvent(event);
    }
    if (slot->startedUnix > 0 && slot->endedUnix == 0 &&
        !slot->activeFieldMeshAcked) {
      DeploymentEvent event;
      fillEvent(*slot, event);
      queueEvent(event);
    }
  }

  if (dirty && !deploymentStoreCommit()) {
    result.committed = false;
    Serial.println("[DEPLOY] FieldMesh history backfill could not be persisted");
  }
  if (result.queued || result.deferred) {
    Serial.printf("[DEPLOY] FieldMesh history backfill: queued=%u deferred=%u\n",
                  (unsigned)result.queued, (unsigned)result.deferred);
  }
  return result;
}

// ---------------------------------------------------------------------------
// Epoch resolution
// ---------------------------------------------------------------------------

uint16_t resolveEpochForSample(const char* nodeId, uint32_t sampleUnix) {
  const DeploymentSlot* s = deploymentFindByNodeId(nodeId);
  if (!s || s->epoch == 0) return 0;   // unknown / never deployed — backend falls back

  // A node with a flat coin cell reports 2000-01-01, which is below every
  // boundary. Correcting on that would misfile a live node's data into an
  // earlier deployment, so treat an implausible clock as "no information" and
  // keep the current epoch. Note this only rejects an obviously-unset clock; a
  // plausible-but-wrong RTC still lands in the wrong interval and nothing in the
  // current wire format can detect that (qualityFlags is hard-coded to 0 at
  // capture).
  if (sampleUnix < kPlausibleSampleFloor) return s->epoch;
  if (s->historyCount == 0) return s->epoch;

  // history[] is oldest -> newest; the sample belongs to the newest boundary it
  // is at or after. This is why it is an interval lookup and not "epoch - 1":
  // a backlog delivered during epoch 3 may belong to epoch 1.
  for (int i = (int)s->historyCount - 1; i >= 0; --i) {
    if (sampleUnix >= s->history[i].startedUnix) return s->history[i].epoch;
  }

  // Older than every boundary we retain. Clamp to the oldest known epoch and
  // count it, so the backend can tell approximate attribution from exact.
  deploymentNoteEpochClamp();
  return s->history[0].epoch;
}

// ---------------------------------------------------------------------------
// Registry mirror
// ---------------------------------------------------------------------------

void deploymentSyncRegistryMirror() {
  if (!deploymentStoreReady()) return;
  for (auto& n : registeredNodes) {
    const DeploymentSlot* s = deploymentFindByNodeId(n.nodeId.c_str());
    if (!s) {
      n.deploymentEpoch       = 0;
      n.deploymentStartedUnix = 0;
      n.deploymentEndedUnix   = 0;
      n.deploymentPendingOp   = DEPLOY_OP_NONE;
      continue;
    }
    n.deploymentEpoch       = s->epoch;
    n.deploymentStartedUnix = s->startedUnix;
    n.deploymentEndedUnix   = s->endedUnix;
    n.deploymentPendingOp   = s->pendingOp;
  }
}
