#include "command_dispatcher.h"
#include <Preferences.h>
#include <string.h>

// ---------------------------------------------------------------------------
// In-RAM authoritative state (mirrored to NVS on every accepted change).
// ---------------------------------------------------------------------------
struct NodeSlot {
  DispatchNodeConfig cfg;
  char     pendingCmdId[CMD_ID_LEN];  // last accepted, not-yet-converged command
  uint32_t pendingRevision;
  uint16_t wireConfigVersion;         // NODE_CONFIG version assigned by registry
  bool     inUse;
  bool     converged;
};

struct GlobalSlot {
  char pendingCmdId[CMD_ID_LEN];
  uint32_t pendingRevision;
  uint8_t recordingIntervalMin;
  bool inUse;
  bool converged;
};

// Pre-wire-version layout used by the first dispatcher firmware. Kept only for
// a one-way NVS migration so an in-field revision/result ring is not discarded.
struct LegacyNodeSlot {
  DispatchNodeConfig cfg;
  char     pendingCmdId[CMD_ID_LEN];
  uint32_t pendingRevision;
  bool     inUse;
  bool     converged;
};

static uint32_t      gRevision = 0;
static NodeSlot      gNodes[CMD_MAX_NODES];
static CommandResult gResults[CMD_MAX_RESULTS];
static uint8_t       gResultHead = 0;   // next write index (ring)
static uint8_t       gResultCount = 0;
static CmdSource     gLastChangeSource = SRC_LOCAL_UI;
static uint32_t      gGeneration = 0;

static Preferences   gNvs;
static const char*   kNs = "dispatch";
static const char*   kStateA = "state_a";
static const char*   kStateB = "state_b";
static constexpr uint32_t kStateMagic = 0x464D4453UL;  // FMDS
static constexpr uint16_t kStateVersion = 2;

struct DispatcherStateRecordV1 {
  uint32_t      magic;
  uint16_t      version;
  uint16_t      size;
  uint32_t      generation;
  uint32_t      revision;
  NodeSlot      nodes[CMD_MAX_NODES];
  CommandResult results[CMD_MAX_RESULTS];
  uint8_t       resultHead;
  uint8_t       resultCount;
  uint8_t       lastChangeSource;
  uint8_t       reserved;
  uint32_t      checksum;
};

struct DispatcherStateRecord {
  uint32_t      magic;
  uint16_t      version;
  uint16_t      size;
  uint32_t      generation;
  uint32_t      revision;
  NodeSlot      nodes[CMD_MAX_NODES];
  GlobalSlot    global;
  CommandResult results[CMD_MAX_RESULTS];
  uint8_t       resultHead;
  uint8_t       resultCount;
  uint8_t       lastChangeSource;
  uint8_t       reserved;
  uint32_t      checksum;
};

static GlobalSlot gGlobal{};

const char* cmdOutcomeStr(CmdOutcome o) {
  switch (o) {
    case OUT_ACCEPTED:          return "ACCEPTED";
    case OUT_REVISION_CONFLICT: return "REVISION_CONFLICT";
    case OUT_INVALID:           return "INVALID";
    case OUT_SUPERSEDED:        return "SUPERSEDED";
    case OUT_REPLAY:            return "REPLAY";
    case OUT_CONVERGED:         return "CONVERGED";
    default:                    return "??";
  }
}

static uint32_t checksumFor(DispatcherStateRecord record) {
  record.checksum = 0;
  const uint8_t* bytes = reinterpret_cast<const uint8_t*>(&record);
  uint32_t hash = 2166136261UL;
  for (size_t i = 0; i < sizeof(record); ++i) {
    hash ^= bytes[i];
    hash *= 16777619UL;
  }
  return hash;
}

static uint32_t checksumForV1(DispatcherStateRecordV1 record) {
  record.checksum = 0;
  const uint8_t* bytes = reinterpret_cast<const uint8_t*>(&record);
  uint32_t hash = 2166136261UL;
  for (size_t i = 0; i < sizeof(record); ++i) {
    hash ^= bytes[i];
    hash *= 16777619UL;
  }
  return hash;
}

static bool validRecord(const DispatcherStateRecord& record) {
  return record.magic == kStateMagic && record.version == kStateVersion &&
         record.size == sizeof(record) &&
         record.resultHead < CMD_MAX_RESULTS &&
         record.resultCount <= CMD_MAX_RESULTS &&
         record.lastChangeSource <= SRC_DASHBOARD &&
         record.checksum == checksumFor(record);
}

static bool readRecord(Preferences& prefs, const char* key,
                       DispatcherStateRecord& record) {
  return prefs.getBytesLength(key) == sizeof(record) &&
         prefs.getBytes(key, &record, sizeof(record)) == sizeof(record) &&
         validRecord(record);
}

static bool readRecordV1(Preferences& prefs, const char* key,
                         DispatcherStateRecordV1& record) {
  return prefs.getBytesLength(key) == sizeof(record) &&
         prefs.getBytes(key, &record, sizeof(record)) == sizeof(record) &&
         record.magic == kStateMagic && record.version == 1 &&
         record.size == sizeof(record) &&
         record.resultHead < CMD_MAX_RESULTS &&
         record.resultCount <= CMD_MAX_RESULTS &&
         record.lastChangeSource <= SRC_DASHBOARD &&
         record.checksum == checksumForV1(record);
}

static bool generationNewer(uint32_t a, uint32_t b) {
  return static_cast<int32_t>(a - b) > 0;
}

// ---- NVS persistence (checksummed A/B durable state) ----------------------
static bool persist() {
  DispatcherStateRecord candidate{};
  candidate.magic = kStateMagic;
  candidate.version = kStateVersion;
  candidate.size = sizeof(candidate);
  candidate.generation = gGeneration + 1U;
  candidate.revision = gRevision;
  memcpy(candidate.nodes, gNodes, sizeof(gNodes));
  candidate.global = gGlobal;
  memcpy(candidate.results, gResults, sizeof(gResults));
  candidate.resultHead = gResultHead;
  candidate.resultCount = gResultCount;
  candidate.lastChangeSource = static_cast<uint8_t>(gLastChangeSource);
  candidate.checksum = checksumFor(candidate);
  const char* key = (candidate.generation & 1U) ? kStateA : kStateB;

  if (!gNvs.begin(kNs, false)) return false;
  const bool wrote = gNvs.putBytes(key, &candidate, sizeof(candidate)) ==
                     sizeof(candidate);
  gNvs.end();
  if (!wrote || !gNvs.begin(kNs, true)) return false;
  DispatcherStateRecord verify{};
  const bool verified = readRecord(gNvs, key, verify) &&
                        memcmp(&verify, &candidate, sizeof(candidate)) == 0;
  gNvs.end();
  if (verified) gGeneration = candidate.generation;
  return verified;
}

void dispatcherInit() {
  memset(gNodes, 0, sizeof gNodes);
  memset(&gGlobal, 0, sizeof gGlobal);
  memset(gResults, 0, sizeof gResults);
  gRevision = 0; gResultHead = 0; gResultCount = 0;
  gLastChangeSource = SRC_LOCAL_UI;
  gGeneration = 0;

  if (!gNvs.begin(kNs, true)) return;
  DispatcherStateRecord a{}, b{};
  const bool aValid = readRecord(gNvs, kStateA, a);
  const bool bValid = readRecord(gNvs, kStateB, b);
  if (aValid || bValid) {
    const DispatcherStateRecord& selected =
        aValid && bValid ? (generationNewer(b.generation, a.generation) ? b : a)
                         : (aValid ? a : b);
    gRevision = selected.revision;
    memcpy(gNodes, selected.nodes, sizeof(gNodes));
    gGlobal = selected.global;
    memcpy(gResults, selected.results, sizeof(gResults));
    gResultHead = selected.resultHead;
    gResultCount = selected.resultCount;
    gLastChangeSource = selected.lastChangeSource == SRC_DASHBOARD
        ? SRC_DASHBOARD : SRC_LOCAL_UI;
    gGeneration = selected.generation;
    gNvs.end();
    return;
  }

  // One-way migration from the checksummed protocol-1 record. Preserve the
  // authoritative revision/results/node mappings while introducing an empty
  // FieldHub-wide command slot.
  DispatcherStateRecordV1 oldA{}, oldB{};
  const bool oldAValid = readRecordV1(gNvs, kStateA, oldA);
  const bool oldBValid = readRecordV1(gNvs, kStateB, oldB);
  if (oldAValid || oldBValid) {
    const DispatcherStateRecordV1& selected = oldAValid && oldBValid
        ? (generationNewer(oldB.generation, oldA.generation) ? oldB : oldA)
        : (oldAValid ? oldA : oldB);
    gRevision = selected.revision;
    memcpy(gNodes, selected.nodes, sizeof(gNodes));
    memcpy(gResults, selected.results, sizeof(gResults));
    gResultHead = selected.resultHead;
    gResultCount = selected.resultCount;
    gLastChangeSource = selected.lastChangeSource == SRC_DASHBOARD
        ? SRC_DASHBOARD : SRC_LOCAL_UI;
    gGeneration = selected.generation;
    gNvs.end();
    persist();
    return;
  }

  // One-way migration from the original multi-key dispatcher layout.
  gRevision = gNvs.getUInt("rev", 0);
  if (gNvs.getBytesLength("nodes2") == sizeof gNodes) {
    gNvs.getBytes("nodes2", gNodes, sizeof gNodes);
  } else if (gNvs.getBytesLength("nodes") == sizeof(LegacyNodeSlot) * CMD_MAX_NODES) {
    LegacyNodeSlot legacy[CMD_MAX_NODES]{};
    if (gNvs.getBytes("nodes", legacy, sizeof legacy) == sizeof legacy) {
      for (uint8_t i = 0; i < CMD_MAX_NODES; ++i) {
        gNodes[i].cfg = legacy[i].cfg;
        memcpy(gNodes[i].pendingCmdId, legacy[i].pendingCmdId, CMD_ID_LEN);
        gNodes[i].pendingRevision = legacy[i].pendingRevision;
        gNodes[i].inUse = legacy[i].inUse;
        gNodes[i].converged = legacy[i].converged;
      }
    }
  }
  if (gNvs.getBytesLength("results") == sizeof gResults) {
    gNvs.getBytes("results", gResults, sizeof gResults);
  }
  const uint8_t storedHead = gNvs.getUChar("rhead", 0);
  const uint8_t storedCount = gNvs.getUChar("rcount", 0);
  gResultHead = storedHead < CMD_MAX_RESULTS ? storedHead : 0;
  gResultCount = storedCount <= CMD_MAX_RESULTS ? storedCount : CMD_MAX_RESULTS;
  const uint8_t source = gNvs.getUChar("source", SRC_LOCAL_UI);
  gLastChangeSource = source == SRC_DASHBOARD ? SRC_DASHBOARD : SRC_LOCAL_UI;
  gNvs.end();

  // Complete legacy migration into the checksummed A/B record.
  persist();
}

void dispatcherResetForTest() {
  gNvs.begin(kNs, false);
  gNvs.clear();
  gNvs.end();
  memset(gNodes, 0, sizeof gNodes);
  memset(&gGlobal, 0, sizeof gGlobal);
  memset(gResults, 0, sizeof gResults);
  gRevision = 0; gResultHead = 0; gResultCount = 0;
  gLastChangeSource = SRC_LOCAL_UI;
  gGeneration = 0;
}

uint32_t dispatcherRevision() { return gRevision; }

// ---- result ring helpers ----
static CommandResult* findResult(const char* cmdId) {
  for (uint8_t i = 0; i < gResultCount; i++)
    if (strncmp(gResults[i].cmdId, cmdId, CMD_ID_LEN) == 0) return &gResults[i];
  return nullptr;
}

static CommandResult* recordResult(const CommandResult& r) {
  CommandResult* slot = &gResults[gResultHead];
  *slot = r;
  gResultHead = (gResultHead + 1) % CMD_MAX_RESULTS;
  if (gResultCount < CMD_MAX_RESULTS) gResultCount++;
  return slot;
}

bool dispatcherKnownCmd(const char* cmdId) { return findResult(cmdId) != nullptr; }

bool dispatcherResultFor(const char* cmdId, CommandResult* out) {
  CommandResult* r = findResult(cmdId);
  if (!r) return false;
  if (out) *out = *r;
  return true;
}

bool dispatcherCommandForResult(const char* cmdId, Command* out) {
  CommandResult* result = findResult(cmdId);
  if (!result || (result->outcome != OUT_ACCEPTED &&
                  result->outcome != OUT_CONVERGED)) return false;
  for (uint8_t i = 0; i < CMD_MAX_NODES; ++i) {
    if (!gNodes[i].inUse ||
        strncmp(gNodes[i].pendingCmdId, cmdId, CMD_ID_LEN) != 0) continue;
    if (out) {
      *out = {};
      strlcpy(out->cmdId, cmdId, CMD_ID_LEN);
      out->type = CMD_SET_NODE_CONFIG;
      out->source = SRC_DASHBOARD;
      out->expectedRevision = result->currentRevision;
      out->payload = gNodes[i].cfg;
      out->configFields = CFG_FIELDS_ALL;
    }
    return true;
  }
  if (gGlobal.inUse &&
      strncmp(gGlobal.pendingCmdId, cmdId, CMD_ID_LEN) == 0) {
    if (out) {
      *out = {};
      strlcpy(out->cmdId, cmdId, CMD_ID_LEN);
      out->type = CMD_SET_RECORDING_INTERVAL;
      out->source = SRC_DASHBOARD;
      out->expectedRevision = result->currentRevision;
      out->recordingIntervalMin = gGlobal.recordingIntervalMin;
    }
    return true;
  }
  return false;
}

uint8_t dispatcherRecentResults(CommandResult* out, uint8_t max) {
  uint8_t n = (gResultCount < max) ? gResultCount : max;
  for (uint8_t i = 0; i < n; i++) out[i] = gResults[i];
  return n;
}

CmdSource dispatcherLastChangeSource() { return gLastChangeSource; }

String dispatcherStatusJson() {
  String j = "{\"stateRevision\":" + String(gRevision);
  j += ",\"lastChangeSource\":\"";
  j += (gLastChangeSource == SRC_DASHBOARD) ? "DASHBOARD" : "LOCAL_UI";
  j += "\",\"results\":[";
  for (uint8_t i = 0; i < gResultCount; i++) {
    if (i) j += ",";
    j += "{\"cmdId\":\""; j += gResults[i].cmdId;
    j += "\",\"outcome\":\""; j += cmdOutcomeStr(gResults[i].outcome);
    j += "\",\"revision\":"; j += String(gResults[i].assignedRevision); j += "}";
  }
  j += "]}";
  return j;
}

// ---- node table helpers ----
static NodeSlot* findNode(const char* nodeId) {
  for (uint8_t i = 0; i < CMD_MAX_NODES; i++)
    if (gNodes[i].inUse && strncmp(gNodes[i].cfg.nodeId, nodeId, CMD_NODEID_LEN) == 0)
      return &gNodes[i];
  return nullptr;
}

static NodeSlot* findOrAddNode(const char* nodeId) {
  NodeSlot* n = findNode(nodeId);
  if (n) return n;
  for (uint8_t i = 0; i < CMD_MAX_NODES; i++)
    if (!gNodes[i].inUse) {
      gNodes[i].inUse = true;
      strlcpy(gNodes[i].cfg.nodeId, nodeId, CMD_NODEID_LEN);
      return &gNodes[i];
    }
  return nullptr;  // table full
}

const DispatchNodeConfig* dispatcherNodeConfig(const char* nodeId) {
  NodeSlot* n = findNode(nodeId);
  return n ? &n->cfg : nullptr;
}

bool dispatcherEnsureDurable() { return persist(); }

bool dispatcherBindNodeConfigVersion(const char* nodeId, uint32_t revision,
                                     uint16_t configVersion) {
  NodeSlot* n = findNode(nodeId);
  if (!n || n->pendingRevision != revision || configVersion == 0) return false;
  if (n->wireConfigVersion == configVersion) return persist();
  n->wireConfigVersion = configVersion;
  return persist();
}

bool dispatcherCurrentCommandForNode(const char* nodeId, const char* cmdId,
                                     uint32_t* revision,
                                     uint16_t* configVersion) {
  NodeSlot* n = findNode(nodeId);
  if (!n || !cmdId || strncmp(n->pendingCmdId, cmdId, CMD_ID_LEN) != 0) return false;
  if (revision) *revision = n->pendingRevision;
  if (configVersion) *configVersion = n->wireConfigVersion;
  return true;
}

bool dispatcherCurrentGlobalCommand(const char* cmdId, uint32_t* revision) {
  if (!gGlobal.inUse || !cmdId ||
      strncmp(gGlobal.pendingCmdId, cmdId, CMD_ID_LEN) != 0) return false;
  if (revision) *revision = gGlobal.pendingRevision;
  return true;
}

bool dispatcherRevisionForConfigVersion(const char* nodeId,
                                        uint16_t configVersion,
                                        uint32_t* revision) {
  NodeSlot* n = findNode(nodeId);
  if (!n || configVersion == 0 || n->wireConfigVersion == 0 ||
      configVersion < n->wireConfigVersion) return false;
  if (revision) *revision = n->pendingRevision;
  return true;
}

bool dispatcherMarkConverged(const char* nodeId, uint32_t revision) {
  NodeSlot* n = findNode(nodeId);
  if (!n || n->pendingRevision != revision) return false;
  if (n->converged) return false;
  n->converged = true;
  CommandResult* result = findResult(n->pendingCmdId);
  if (result && result->outcome == OUT_ACCEPTED) result->outcome = OUT_CONVERGED;
  return persist();
}

bool dispatcherMarkGlobalConverged(uint32_t revision) {
  if (!gGlobal.inUse || gGlobal.pendingRevision != revision ||
      gGlobal.converged) return false;
  gGlobal.converged = true;
  CommandResult* result = findResult(gGlobal.pendingCmdId);
  if (result && result->outcome == OUT_ACCEPTED) result->outcome = OUT_CONVERGED;
  return persist();
}

static bool validPayload(const Command& c) {
  if (c.type == CMD_REQUEST_STATUS) return true;
  if (c.type == CMD_SET_RECORDING_INTERVAL) {
    return c.recordingIntervalMin == 1 || c.recordingIntervalMin == 5 ||
           c.recordingIntervalMin == 10 || c.recordingIntervalMin == 20 ||
           c.recordingIntervalMin == 30 || c.recordingIntervalMin == 60;
  }
  if (c.type != CMD_SET_NODE_CONFIG) return false;
  if (c.payload.nodeId[0] == '\0') return false;
  if (c.payload.wakeIntervalMin < 1 || c.payload.wakeIntervalMin > 240) return false;
  if (c.payload.targetState != 0 && c.payload.targetState != 2 &&
      c.payload.targetState != 3) return false;
  if (c.payload.sensorMask > UINT16_MAX) return false;
  return true;
}

bool dispatcherSubmitBatch(const DispatchBatchItem* items, uint8_t count,
                           uint32_t initialRevision,
                           CommandResult* outResults) {
  if (!items || count == 0 || count > CMD_MAX_RESULTS ||
      gRevision != initialRevision) return false;

  // Validate the complete set before changing the in-RAM desired mirrors.
  uint8_t newNodes = 0;
  uint8_t freeNodes = 0;
  uint8_t acceptedChanges = 0;
  for (uint8_t i = 0; i < CMD_MAX_NODES; ++i) {
    if (!gNodes[i].inUse) ++freeNodes;
  }
  for (uint8_t i = 0; i < count; ++i) {
    const Command& command = items[i].command;
    if (!command.cmdId[0]) return false;
    for (uint8_t j = 0; j < i; ++j) {
      if (strncmp(command.cmdId, items[j].command.cmdId, CMD_ID_LEN) == 0)
        return false;
    }
    if (findResult(command.cmdId)) continue;
    if (items[i].outcome != OUT_ACCEPTED &&
        items[i].outcome != OUT_INVALID &&
        items[i].outcome != OUT_REVISION_CONFLICT) return false;
    if (items[i].outcome != OUT_ACCEPTED) continue;
    if (command.type == CMD_REQUEST_STATUS) continue;
    if (!validPayload(command) || command.expectedRevision != initialRevision)
      return false;
    ++acceptedChanges;
    if (command.type == CMD_SET_NODE_CONFIG) {
      if (!findNode(command.payload.nodeId)) ++newNodes;
      for (uint8_t j = 0; j < i; ++j) {
        if (items[j].outcome == OUT_ACCEPTED &&
            !findResult(items[j].command.cmdId) &&
            items[j].command.type == CMD_SET_NODE_CONFIG &&
            strncmp(command.payload.nodeId, items[j].command.payload.nodeId,
                    CMD_NODEID_LEN) == 0) return false;
      }
    } else if (command.type == CMD_SET_RECORDING_INTERVAL) {
      for (uint8_t j = 0; j < i; ++j) {
        if (items[j].outcome == OUT_ACCEPTED &&
            !findResult(items[j].command.cmdId) &&
            items[j].command.type == CMD_SET_RECORDING_INTERVAL) return false;
      }
    }
  }
  if (newNodes > freeNodes ||
      acceptedChanges > UINT32_MAX - initialRevision) return false;

  const uint32_t revisionBefore = gRevision;
  const uint32_t generationBefore = gGeneration;
  const uint8_t resultHeadBefore = gResultHead;
  const uint8_t resultCountBefore = gResultCount;
  const CmdSource sourceBefore = gLastChangeSource;
  const GlobalSlot globalBefore = gGlobal;
  NodeSlot nodesBefore[CMD_MAX_NODES];
  CommandResult resultsBefore[CMD_MAX_RESULTS];
  memcpy(nodesBefore, gNodes, sizeof(gNodes));
  memcpy(resultsBefore, gResults, sizeof(gResults));

  CommandResult batchResults[CMD_MAX_RESULTS]{};
  for (uint8_t i = 0; i < count; ++i) {
    const Command& command = items[i].command;
    CommandResult result{};
    strlcpy(result.cmdId, command.cmdId, CMD_ID_LEN);
    result.currentRevision = initialRevision;

    CommandResult* prior = findResult(command.cmdId);
    if (prior) {
      result = *prior;
      result.outcome = OUT_REPLAY;
      result.currentRevision = gRevision;
      batchResults[i] = result;
      continue;
    }

    if (items[i].outcome != OUT_ACCEPTED) {
      result.outcome = items[i].outcome;
      recordResult(result);
      batchResults[i] = result;
      continue;
    }

    if (command.type == CMD_REQUEST_STATUS) {
      result.outcome = OUT_ACCEPTED;
      result.assignedRevision = gRevision;
      recordResult(result);
      batchResults[i] = result;
      continue;
    }

    if (command.type == CMD_SET_RECORDING_INTERVAL) {
      if (gGlobal.inUse && !gGlobal.converged &&
          strncmp(gGlobal.pendingCmdId, command.cmdId, CMD_ID_LEN) != 0) {
        CommandResult* superseded = findResult(gGlobal.pendingCmdId);
        if (superseded && superseded->outcome == OUT_ACCEPTED)
          superseded->outcome = OUT_SUPERSEDED;
      }
      ++gRevision;
      gLastChangeSource = command.source;
      gGlobal.inUse = true;
      gGlobal.converged = false;
      gGlobal.pendingRevision = gRevision;
      gGlobal.recordingIntervalMin = command.recordingIntervalMin;
      strlcpy(gGlobal.pendingCmdId, command.cmdId, CMD_ID_LEN);
      result.outcome = OUT_ACCEPTED;
      result.assignedRevision = gRevision;
      result.currentRevision = gRevision;
      recordResult(result);
      batchResults[i] = result;
      continue;
    }

    NodeSlot* node = findOrAddNode(command.payload.nodeId);
    if (!node) goto rollback;
    if (node->pendingCmdId[0] != '\0' && !node->converged &&
        strncmp(node->pendingCmdId, command.cmdId, CMD_ID_LEN) != 0) {
      CommandResult* superseded = findResult(node->pendingCmdId);
      if (superseded && superseded->outcome == OUT_ACCEPTED)
        superseded->outcome = OUT_SUPERSEDED;
    }

    node->cfg = command.payload;
    ++gRevision;
    gLastChangeSource = command.source;
    node->pendingRevision = gRevision;
    node->wireConfigVersion = 0;
    node->converged = false;
    strlcpy(node->pendingCmdId, command.cmdId, CMD_ID_LEN);

    result.outcome = OUT_ACCEPTED;
    result.assignedRevision = gRevision;
    result.currentRevision = gRevision;
    recordResult(result);
    batchResults[i] = result;
  }

  if (!persist()) goto rollback;
  if (outResults) memcpy(outResults, batchResults,
                         sizeof(CommandResult) * count);
  return true;

rollback:
  gRevision = revisionBefore;
  gGeneration = generationBefore;
  gResultHead = resultHeadBefore;
  gResultCount = resultCountBefore;
  gLastChangeSource = sourceBefore;
  gGlobal = globalBefore;
  memcpy(gNodes, nodesBefore, sizeof(gNodes));
  memcpy(gResults, resultsBefore, sizeof(gResults));
  return false;
}

CommandResult dispatcherSubmit(const Command& c) {
  DispatchBatchItem item{};
  item.command = c;
  if (dispatcherKnownCmd(c.cmdId) || c.type == CMD_REQUEST_STATUS) {
    item.outcome = OUT_ACCEPTED;
  } else if (!validPayload(c)) {
    item.outcome = OUT_INVALID;
  } else if (c.expectedRevision != gRevision) {
    item.outcome = OUT_REVISION_CONFLICT;
  } else {
    item.outcome = OUT_ACCEPTED;
  }
  CommandResult result{};
  if (!dispatcherSubmitBatch(&item, 1, gRevision, &result)) {
    strlcpy(result.cmdId, c.cmdId, CMD_ID_LEN);
    result.outcome = OUT_INVALID;
    result.currentRevision = gRevision;
  }
  return result;
}

CommandResult dispatcherReject(const char* cmdId, CmdSource source,
                               CmdOutcome outcome) {
  DispatchBatchItem item{};
  strlcpy(item.command.cmdId, cmdId ? cmdId : "", CMD_ID_LEN);
  item.command.source = source;
  item.outcome = outcome == OUT_REVISION_CONFLICT
      ? OUT_REVISION_CONFLICT : OUT_INVALID;
  CommandResult result{};
  if (!dispatcherSubmitBatch(&item, 1, gRevision, &result)) {
    strlcpy(result.cmdId, item.command.cmdId, CMD_ID_LEN);
    result.outcome = item.outcome;
    result.currentRevision = gRevision;
  }
  return result;
}
