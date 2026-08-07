#include "config/deployment_store.h"

#include <LittleFS.h>
#include <string.h>
#include <stddef.h>
#include <stdlib.h>

// ---------------------------------------------------------------------------
// On-flash record
// ---------------------------------------------------------------------------

static const char* kDeployFile   = "/deploy.bin";
static const char* kDeployTmp    = "/deploy.tmp";
static const char* kDeployBak    = "/deploy.bak";
static const char* kDeployV1Bak  = "/deploy.v1";
static const char* kDeployV1Tmp  = "/deploy.v1.tmp";

static constexpr uint32_t kDeployMagic   = 0x4650444DUL;  // "FPDM"
static constexpr uint16_t kDeployVersion = 2;

// Frozen version-1 shapes. Do not replace these with the live structs: version
// 2 adds the local archive to DeploymentSlot, so using the live type here would
// make an installed v1 record unreadable and silently reseed deployment epochs.
struct DeploymentSlotV1 {
  uint8_t  mac[6];
  char     nodeId[kDeployNodeIdLen];
  uint16_t epoch;
  uint32_t startedUnix;
  uint32_t endedUnix;
  DeploymentBoundary history[kBoundaryHistory];
  uint8_t  historyCount;
  uint8_t  pendingOp;
  uint16_t pendingEpoch;
  uint32_t pendingUnix;
  char     stagedUserId[kDeployUserIdLen];
  char     stagedName[kDeployNameLen];
  float    stagedLat;
  float    stagedLon;
  bool     hasStagedIdentity;
  bool     inUse;
};

struct DeploymentEventV1 {
  char     eventId[kDeployEventIdLen];
  char     nodeId[kDeployNodeIdLen];
  uint16_t deploymentEpoch;
  uint32_t deploymentStartedUnix;
  uint32_t deploymentEndedUnix;
  char     userId[kDeployUserIdLen];
  char     name[kDeployNameLen];
  float    latitude;
  float    longitude;
  char     conflictReason[96];
  bool     inUse;
};

struct DeploymentRecordV1 {
  uint32_t magic;
  uint16_t version;
  uint16_t size;
  uint32_t generation;
  DeploymentSlotV1  nodes[kMaxDeployNodes];
  DeploymentEventV1 outbox[kMaxOutboxEvents];
  uint8_t  outboxCount;
  uint8_t  reserved[3];
  uint32_t epochClampCount;
  uint32_t checksum;
};

struct DeploymentRecord {
  uint32_t magic;
  uint16_t version;
  uint16_t size;              // sizeof(DeploymentRecord) — guards struct drift
  uint32_t generation;        // higher wins when .bin and .bak both validate
  DeploymentSlot  nodes[kMaxDeployNodes];
  DeploymentEvent outbox[kMaxOutboxEvents];
  uint8_t  outboxCount;
  uint8_t  reserved[3];
  uint32_t epochClampCount;
  uint32_t checksum;          // FNV-1a over the record with checksum zeroed
};

// ~10 KB — deliberately a file static, never a stack local.
static DeploymentRecord gRecord;
static bool gReady        = false;
static bool gCreatedFresh = false;

// Pin the size so the outbox/table cannot be grown carelessly into something
// that strains a 768 KB LittleFS shared with the reading buffer.
static_assert(sizeof(DeploymentRecord) < 32768,
              "DeploymentRecord too large for the shared LittleFS partition");
static_assert(sizeof(DeploymentRecordV1) < sizeof(DeploymentRecord),
              "The v1 migration shape must remain smaller than the live record");

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static uint32_t fnv1a32(const uint8_t* data, size_t len) {
  uint32_t h = 2166136261UL;
  for (size_t i = 0; i < len; ++i) {
    h ^= data[i];
    h *= 16777619UL;
  }
  return h;
}

// By CONST REFERENCE, and hashing the prefix rather than a zeroed copy.
//
// This used to take the record by value to zero the checksum field before
// hashing. DeploymentRecord is ~9.3 KB, so every call pushed a 9.3 KB copy onto
// a stack the Arduino loop task sizes at 8 KB — deploymentStoreCommit() alone
// reserved 9,344 bytes. Hashing [0, offsetof(checksum)) is equivalent (checksum
// is the last member, so there is no trailing padding to skip) and costs no
// stack at all.
static uint32_t checksumFor(const DeploymentRecord& record) {
  return fnv1a32(reinterpret_cast<const uint8_t*>(&record),
                 offsetof(DeploymentRecord, checksum));
}

static uint32_t checksumForV1(const DeploymentRecordV1& record) {
  return fnv1a32(reinterpret_cast<const uint8_t*>(&record),
                 offsetof(DeploymentRecordV1, checksum));
}

static bool recordValid(const DeploymentRecord& record) {
  if (record.magic != kDeployMagic) return false;
  if (record.version != kDeployVersion) return false;
  if (record.size != sizeof(DeploymentRecord)) return false;
  if (record.outboxCount > kMaxOutboxEvents) return false;
  return checksumFor(record) == record.checksum;
}

static bool recordV1Valid(const DeploymentRecordV1& record) {
  if (record.magic != kDeployMagic || record.version != 1) return false;
  if (record.size != sizeof(DeploymentRecordV1)) return false;
  if (record.outboxCount > kMaxOutboxEvents) return false;
  return checksumForV1(record) == record.checksum;
}

// Reads straight into the caller's record — never through a temporary. The
// caller passes gRecord, so no copy of the ~9.3 KB record exists anywhere.
static bool readRecordFile(const char* path, DeploymentRecord& out) {
  if (!LittleFS.exists(path)) return false;
  File f = LittleFS.open(path, "r");
  if (!f) return false;
  if (f.size() != sizeof(DeploymentRecord)) {
    f.close();
    return false;
  }
  const size_t got = f.read(reinterpret_cast<uint8_t*>(&out), sizeof(out));
  f.close();
  if (got != sizeof(out)) return false;
  return recordValid(out);
}

static bool readV1RecordFile(const char* path, DeploymentRecordV1& out) {
  if (!LittleFS.exists(path)) return false;
  File f = LittleFS.open(path, "r");
  if (!f) return false;
  if (f.size() != sizeof(DeploymentRecordV1)) {
    f.close();
    return false;
  }
  const size_t got = f.read(reinterpret_cast<uint8_t*>(&out), sizeof(out));
  f.close();
  return got == sizeof(out) && recordV1Valid(out);
}

// Just enough of the header to compare generations without loading a whole
// record. Lets deploymentStoreBegin() decide which file to load without ever
// holding two records at once.
struct DeploymentRecordHeader {
  uint32_t magic;
  uint16_t version;
  uint16_t size;
  uint32_t generation;
};

static bool peekRecordGeneration(const char* path, uint32_t& generationOut) {
  if (!LittleFS.exists(path)) return false;
  File f = LittleFS.open(path, "r");
  if (!f) return false;
  DeploymentRecordHeader hdr{};
  const size_t got = f.read(reinterpret_cast<uint8_t*>(&hdr), sizeof(hdr));
  const size_t fileSize = f.size();
  f.close();
  if (got != sizeof(hdr) || fileSize != sizeof(DeploymentRecord)) return false;
  if (hdr.magic != kDeployMagic || hdr.version != kDeployVersion) return false;
  if (hdr.size != sizeof(DeploymentRecord)) return false;
  generationOut = hdr.generation;
  return true;
}

static void initFreshRecord() {
  memset(&gRecord, 0, sizeof(gRecord));
  gRecord.magic   = kDeployMagic;
  gRecord.version = kDeployVersion;
  gRecord.size    = sizeof(DeploymentRecord);
  gRecord.generation = 0;
}

static void copyStr(char* dst, size_t dstLen, const char* src) {
  if (dstLen == 0) return;
  if (!src) { dst[0] = '\0'; return; }
  strncpy(dst, src, dstLen - 1);
  dst[dstLen - 1] = '\0';
}

static void archiveAppend(DeploymentSlot& slot, const DeploymentArchive& value) {
  for (uint8_t i = 0; i < slot.archiveCount; ++i) {
    if (slot.archive[i].epoch == value.epoch) {
      slot.archive[i] = value;
      return;
    }
  }
  if (slot.archiveCount < kLocalDeploymentArchive) {
    slot.archive[slot.archiveCount++] = value;
    return;
  }
  for (size_t i = 1; i < kLocalDeploymentArchive; ++i) {
    slot.archive[i - 1] = slot.archive[i];
  }
  slot.archive[kLocalDeploymentArchive - 1] = value;
}

static void archiveFromEvent(DeploymentSlot& slot, const DeploymentEvent& event) {
  if (event.deploymentEpoch == 0 || event.deploymentEndedUnix == 0) return;
  DeploymentArchive value{};
  value.epoch       = event.deploymentEpoch;
  value.startedUnix = event.deploymentStartedUnix;
  value.endedUnix   = event.deploymentEndedUnix;
  value.latitude    = event.latitude;
  value.longitude   = event.longitude;
  if (value.startedUnix > 0) value.known |= DEPLOY_ARCHIVE_START_KNOWN;
  value.known |= DEPLOY_ARCHIVE_END_KNOWN | DEPLOY_ARCHIVE_IDENTITY_KNOWN;
  if (!isnan(value.latitude) && !isnan(value.longitude) &&
      (value.latitude != 0.0f || value.longitude != 0.0f)) {
    value.known |= DEPLOY_ARCHIVE_LOCATION_KNOWN;
  }
  copyStr(value.userId, sizeof(value.userId), event.userId);
  copyStr(value.name, sizeof(value.name), event.name);
  archiveAppend(slot, value);
}

static void markFieldMeshEventAcked(const DeploymentEvent& event) {
  for (size_t n = 0; n < kMaxDeployNodes; ++n) {
    DeploymentSlot& slot = gRecord.nodes[n];
    if (!slot.inUse ||
        strncmp(slot.nodeId, event.nodeId, kDeployNodeIdLen) != 0) continue;
    if (event.deploymentEndedUnix == 0 && slot.epoch == event.deploymentEpoch) {
      slot.activeFieldMeshAcked = true;
    }
    for (uint8_t i = 0; i < slot.archiveCount; ++i) {
      if (slot.archive[i].epoch == event.deploymentEpoch &&
          event.deploymentEndedUnix > 0) {
        slot.archive[i].fieldMeshAcked = true;
        break;
      }
    }
    return;
  }
}

static void copyEventFromV1(const DeploymentEventV1& src, DeploymentEvent& dst) {
  memset(&dst, 0, sizeof(dst));
  copyStr(dst.eventId, sizeof(dst.eventId), src.eventId);
  copyStr(dst.nodeId, sizeof(dst.nodeId), src.nodeId);
  dst.deploymentEpoch       = src.deploymentEpoch;
  dst.deploymentStartedUnix = src.deploymentStartedUnix;
  dst.deploymentEndedUnix   = src.deploymentEndedUnix;
  copyStr(dst.userId, sizeof(dst.userId), src.userId);
  copyStr(dst.name, sizeof(dst.name), src.name);
  dst.latitude  = src.latitude;
  dst.longitude = src.longitude;
  copyStr(dst.conflictReason, sizeof(dst.conflictReason), src.conflictReason);
  dst.inUse = src.inUse;
}

static bool writeV1RecoveryCopy(const DeploymentRecordV1& record) {
  File f = LittleFS.open(kDeployV1Tmp, "w", true);
  if (!f) return false;
  const size_t written = f.write(reinterpret_cast<const uint8_t*>(&record), sizeof(record));
  const bool failed = f.getWriteError() || written != sizeof(record);
  f.close();
  if (failed) {
    LittleFS.remove(kDeployV1Tmp);
    return false;
  }
  LittleFS.remove(kDeployV1Bak);
  if (!LittleFS.rename(kDeployV1Tmp, kDeployV1Bak)) {
    LittleFS.remove(kDeployV1Tmp);
    return false;
  }
  return true;
}

enum V1MigrationResult : uint8_t {
  V1_MIGRATION_NONE = 0,
  V1_MIGRATION_OK,
  V1_MIGRATION_FAILED,
};

static V1MigrationResult migrateV1RecordIfPresent() {
  DeploymentRecordV1* legacy =
      static_cast<DeploymentRecordV1*>(malloc(sizeof(DeploymentRecordV1)));
  if (!legacy) {
    Serial.println("[DEPLOY] v1 migration: not enough temporary memory");
    return V1_MIGRATION_FAILED;
  }

  const char* candidates[] = {kDeployFile, kDeployBak, kDeployV1Bak};
  const char* bestPath = nullptr;
  uint32_t bestGeneration = 0;
  for (const char* path : candidates) {
    if (readV1RecordFile(path, *legacy) &&
        (!bestPath || legacy->generation > bestGeneration)) {
      bestPath = path;
      bestGeneration = legacy->generation;
    }
  }
  if (!bestPath || !readV1RecordFile(bestPath, *legacy)) {
    free(legacy);
    return V1_MIGRATION_NONE;
  }

  // Keep a separately checksummed v1 copy until the new record has survived a
  // complete boot. A failed conversion can therefore retry instead of falling
  // through to the dangerous "fresh store" epoch-1 reseed path.
  if (strcmp(bestPath, kDeployV1Bak) != 0) {
    if (!writeV1RecoveryCopy(*legacy) || !readV1RecordFile(kDeployV1Bak, *legacy)) {
      Serial.println("[DEPLOY] v1 migration: could not preserve recovery copy");
      free(legacy);
      return V1_MIGRATION_FAILED;
    }
  }

  initFreshRecord();
  gRecord.generation      = legacy->generation;
  gRecord.epochClampCount = legacy->epochClampCount;

  for (size_t i = 0; i < kMaxDeployNodes; ++i) {
    const DeploymentSlotV1& src = legacy->nodes[i];
    if (!src.inUse) continue;
    DeploymentSlot& dst = gRecord.nodes[i];
    memcpy(dst.mac, src.mac, sizeof(dst.mac));
    copyStr(dst.nodeId, sizeof(dst.nodeId), src.nodeId);
    dst.epoch       = src.epoch;
    dst.startedUnix = src.startedUnix;
    dst.endedUnix   = src.endedUnix;
    // Version 1 was cloud-first and shipped only after its active deployments
    // were backfilled server-side. Treat an active event as acknowledged unless
    // a still-queued v1 event below proves otherwise.
    dst.activeFieldMeshAcked = dst.startedUnix > 0 && dst.endedUnix == 0;
    dst.historyCount = min((size_t)src.historyCount, kBoundaryHistory);
    for (uint8_t h = 0; h < dst.historyCount; ++h) dst.history[h] = src.history[h];
    dst.pendingOp    = src.pendingOp;
    dst.pendingEpoch = src.pendingEpoch;
    dst.pendingUnix  = src.pendingUnix;
    copyStr(dst.stagedUserId, sizeof(dst.stagedUserId), src.stagedUserId);
    copyStr(dst.stagedName, sizeof(dst.stagedName), src.stagedName);
    dst.stagedLat = src.stagedLat;
    dst.stagedLon = src.stagedLon;
    dst.hasStagedIdentity = src.hasStagedIdentity;
    dst.inUse = true;

    // Version 1 only knew the boundary start. Preserve that partial knowledge
    // explicitly; never invent the old label, location, or end time.
    for (uint8_t h = 0; h < dst.historyCount; ++h) {
      const DeploymentBoundary& boundary = dst.history[h];
      if (boundary.epoch == 0) continue;
      if (boundary.epoch == dst.epoch && dst.endedUnix == 0) continue;
      DeploymentArchive partial{};
      partial.epoch = boundary.epoch;
      partial.startedUnix = boundary.startedUnix;
      if (partial.startedUnix > 0) partial.known |= DEPLOY_ARCHIVE_START_KNOWN;
      if (partial.epoch == dst.epoch && dst.endedUnix > 0) {
        partial.endedUnix = dst.endedUnix;
        partial.known |= DEPLOY_ARCHIVE_END_KNOWN;
      }
      archiveAppend(dst, partial);
    }
  }

  for (size_t i = 0; i < kMaxOutboxEvents; ++i) {
    copyEventFromV1(legacy->outbox[i], gRecord.outbox[i]);
    const DeploymentEvent& event = gRecord.outbox[i];
    if (!event.inUse) continue;
    for (size_t n = 0; n < kMaxDeployNodes; ++n) {
      DeploymentSlot& slot = gRecord.nodes[n];
      if (slot.inUse && strncmp(slot.nodeId, event.nodeId, kDeployNodeIdLen) == 0) {
        if (event.deploymentEndedUnix > 0) archiveFromEvent(slot, event);
        else if (slot.epoch == event.deploymentEpoch) slot.activeFieldMeshAcked = false;
        break;
      }
    }
  }
  gRecord.outboxCount = legacy->outboxCount;
  free(legacy);
  Serial.printf("[DEPLOY] Migrated deployment store v1 -> v2 (gen=%lu)\n",
                (unsigned long)gRecord.generation);
  return V1_MIGRATION_OK;
}

static String jsonEscape(const char* v) {
  String out;
  if (!v) return out;
  for (const char* p = v; *p; ++p) {
    const char c = *p;
    if (c == '"' || c == '\\') { out += '\\'; out += c; }
    else if (c == '\n') out += "\\n";
    else if (c == '\r') out += "\\r";
    else if (c == '\t') out += "\\t";
    else out += c;
  }
  return out;
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

bool deploymentStoreBegin() {
  gReady = false;
  gCreatedFresh = false;

  // A commit that was interrupted mid-rename can leave the temp file behind.
  // It is never authoritative — the record is only live once it is at
  // kDeployFile — so drop it rather than trying to interpret it.
  if (LittleFS.exists(kDeployTmp)) {
    LittleFS.remove(kDeployTmp);
  }
  if (LittleFS.exists(kDeployV1Tmp)) {
    LittleFS.remove(kDeployV1Tmp);
  }

  // Load candidates directly into gRecord, one at a time. Holding two ~9.3 KB
  // records as locals here reserved 18,656 bytes of stack against an 8 KB loop
  // task — a guaranteed overflow on the first boot that found a record.
  const bool havePrimary = readRecordFile(kDeployFile, gRecord);
  const uint32_t primaryGen = havePrimary ? gRecord.generation : 0;

  uint32_t backupGen = 0;
  const bool backupPresent = peekRecordGeneration(kDeployBak, backupGen);

  bool loaded = havePrimary;
  if (backupPresent && (!havePrimary || backupGen > primaryGen)) {
    // The backup either is all we have, or it advanced further than the primary
    // (an interrupted commit). Either way it is a COMPLETE record — never a mix.
    if (readRecordFile(kDeployBak, gRecord)) {
      if (!havePrimary) {
        Serial.println("[DEPLOY] Primary record missing/corrupt — recovering from backup");
      }
      loaded = true;
    } else if (havePrimary) {
      // Backup failed its checksum after all; reload the primary we displaced.
      loaded = readRecordFile(kDeployFile, gRecord);
    }
  }

  const bool loadedCurrentVersion = loaded;
  bool migratedV1 = false;
  if (!loaded) {
    const V1MigrationResult migration = migrateV1RecordIfPresent();
    if (migration == V1_MIGRATION_FAILED) {
      Serial.println("[DEPLOY] Deployment-store migration failed; refusing to reseed epochs");
      return false;
    }
    migratedV1 = migration == V1_MIGRATION_OK;
    loaded = migratedV1;
  }

  if (!loaded) {
    initFreshRecord();
    gCreatedFresh = true;
    Serial.println("[DEPLOY] No deployment record — creating");
  }

  gReady = true;

  if ((gCreatedFresh || migratedV1) && !deploymentStoreCommit()) {
    Serial.println(migratedV1
        ? "[DEPLOY] Failed to commit migrated deployment record"
        : "[DEPLOY] Failed to create deployment record");
    gReady = false;
    return false;
  }

  // The recovery copy is only retired after a later boot has independently
  // loaded the v2 checksum. The migration boot itself always leaves it intact.
  if (loadedCurrentVersion && LittleFS.exists(kDeployV1Bak)) {
    LittleFS.remove(kDeployV1Bak);
  }

  Serial.printf("[DEPLOY] Store ready (gen=%lu, outbox=%u, fresh=%d)\n",
                (unsigned long)gRecord.generation,
                (unsigned)gRecord.outboxCount,
                gCreatedFresh ? 1 : 0);
  return true;
}

bool deploymentStoreReady() { return gReady; }
bool deploymentStoreWasCreatedFresh() { return gCreatedFresh; }

// ---------------------------------------------------------------------------
// Atomic commit
// ---------------------------------------------------------------------------

bool deploymentStoreCommit() {
  if (!gReady) return false;

  gRecord.magic   = kDeployMagic;
  gRecord.version = kDeployVersion;
  gRecord.size    = sizeof(DeploymentRecord);
  gRecord.generation++;
  gRecord.checksum = checksumFor(gRecord);

  {
    File f = LittleFS.open(kDeployTmp, "w", true);
    if (!f) {
      Serial.println("[DEPLOY] commit: cannot open temp file");
      gRecord.generation--;
      return false;
    }
    const size_t written = f.write(reinterpret_cast<const uint8_t*>(&gRecord),
                                   sizeof(gRecord));
    const bool writeError = f.getWriteError();
    f.close();
    if (writeError || written != sizeof(gRecord)) {
      Serial.printf("[DEPLOY] commit: short write (%u/%u)\n",
                    (unsigned)written, (unsigned)sizeof(gRecord));
      LittleFS.remove(kDeployTmp);
      gRecord.generation--;
      return false;
    }
  }

  // Backup-then-swap, same shape as upload_queue's commitTempDataFile(): at
  // every instant at least one complete record exists on flash.
  LittleFS.remove(kDeployBak);
  const bool hadPrimary = LittleFS.exists(kDeployFile);
  if (hadPrimary && !LittleFS.rename(kDeployFile, kDeployBak)) {
    Serial.println("[DEPLOY] commit: cannot move primary to backup");
    LittleFS.remove(kDeployTmp);
    gRecord.generation--;
    return false;
  }
  if (!LittleFS.rename(kDeployTmp, kDeployFile)) {
    Serial.println("[DEPLOY] commit: temp->primary rename failed; restoring backup");
    if (hadPrimary) LittleFS.rename(kDeployBak, kDeployFile);
    LittleFS.remove(kDeployTmp);
    gRecord.generation--;
    return false;
  }
  if (hadPrimary) LittleFS.remove(kDeployBak);
  return true;
}

// ---------------------------------------------------------------------------
// Slot access
// ---------------------------------------------------------------------------

const DeploymentSlot* deploymentFindByNodeId(const char* nodeId) {
  if (!gReady || !nodeId || !nodeId[0]) return nullptr;
  for (size_t i = 0; i < kMaxDeployNodes; ++i) {
    const DeploymentSlot& s = gRecord.nodes[i];
    if (s.inUse && strncmp(s.nodeId, nodeId, kDeployNodeIdLen) == 0) return &s;
  }
  return nullptr;
}

const DeploymentSlot* deploymentFindByMac(const uint8_t* mac) {
  if (!gReady || !mac) return nullptr;
  for (size_t i = 0; i < kMaxDeployNodes; ++i) {
    const DeploymentSlot& s = gRecord.nodes[i];
    if (s.inUse && memcmp(s.mac, mac, 6) == 0) return &s;
  }
  return nullptr;
}

DeploymentSlot* deploymentSlotFor(const uint8_t* mac, const char* nodeId) {
  if (!gReady) return nullptr;

  // MAC first — it is the registry's real identity and survives a nodeId change.
  if (mac) {
    for (size_t i = 0; i < kMaxDeployNodes; ++i) {
      DeploymentSlot& s = gRecord.nodes[i];
      if (s.inUse && memcmp(s.mac, mac, 6) == 0) {
        if (nodeId && nodeId[0]) copyStr(s.nodeId, kDeployNodeIdLen, nodeId);
        return &s;
      }
    }
  }
  if (nodeId && nodeId[0]) {
    for (size_t i = 0; i < kMaxDeployNodes; ++i) {
      DeploymentSlot& s = gRecord.nodes[i];
      if (s.inUse && strncmp(s.nodeId, nodeId, kDeployNodeIdLen) == 0) {
        if (mac) memcpy(s.mac, mac, 6);
        return &s;
      }
    }
  }
  for (size_t i = 0; i < kMaxDeployNodes; ++i) {
    DeploymentSlot& s = gRecord.nodes[i];
    if (!s.inUse) {
      memset(&s, 0, sizeof(s));
      if (mac) memcpy(s.mac, mac, 6);
      copyStr(s.nodeId, kDeployNodeIdLen, nodeId);
      s.inUse = true;
      return &s;
    }
  }
  Serial.println("[DEPLOY] slot table full");
  return nullptr;
}

void deploymentPushBoundary(DeploymentSlot& slot, uint16_t epoch, uint32_t startedUnix) {
  // Replace in place if this epoch is already the newest entry (a repeated
  // commit of the same transition must not grow the ring).
  if (slot.historyCount > 0 &&
      slot.history[slot.historyCount - 1].epoch == epoch) {
    slot.history[slot.historyCount - 1].startedUnix = startedUnix;
    return;
  }
  if (slot.historyCount < kBoundaryHistory) {
    slot.history[slot.historyCount].epoch = epoch;
    slot.history[slot.historyCount].startedUnix = startedUnix;
    slot.historyCount++;
    return;
  }
  for (size_t i = 1; i < kBoundaryHistory; ++i) {
    slot.history[i - 1] = slot.history[i];
  }
  slot.history[kBoundaryHistory - 1].epoch = epoch;
  slot.history[kBoundaryHistory - 1].startedUnix = startedUnix;
}

void deploymentArchiveUpsert(DeploymentSlot& slot, const DeploymentEvent& event) {
  archiveFromEvent(slot, event);
}

const DeploymentSlot* deploymentSlotAt(size_t tableIndex) {
  if (!gReady || tableIndex >= kMaxDeployNodes) return nullptr;
  return gRecord.nodes[tableIndex].inUse ? &gRecord.nodes[tableIndex] : nullptr;
}

// ---------------------------------------------------------------------------
// Outbox
// ---------------------------------------------------------------------------

void deploymentMakeEventId(const char* nodeId, uint16_t epoch, char* out, size_t outLen) {
  if (!out || outLen == 0) return;
  const char* id = (nodeId && nodeId[0]) ? nodeId : "UNKNOWN";
  const uint32_t h = fnv1a32(reinterpret_cast<const uint8_t*>(id), strlen(id));
  snprintf(out, outLen, "%08lX-%u", (unsigned long)h, (unsigned)epoch);
}

bool deploymentOutboxUpsert(const DeploymentEvent& event) {
  if (!gReady) return false;

  // deploymentStartedUnix is mandatory and must be positive on EVERY event,
  // including End events where it is the start of the epoch being closed. The
  // backend records a zero start as a conflict and never acknowledges it, so
  // queueing one would retry forever and wedge the outbox behind it. Refuse
  // here rather than let a bad record escape.
  if (event.deploymentStartedUnix == 0) {
    Serial.printf("[DEPLOY] Refusing event %s: deploymentStartedUnix is 0\n",
                  event.eventId);
    return false;
  }
  if (event.eventId[0] == '\0') {
    Serial.println("[DEPLOY] Refusing event with an empty eventId");
    return false;
  }

  for (size_t i = 0; i < kMaxOutboxEvents; ++i) {
    DeploymentEvent& e = gRecord.outbox[i];
    if (e.inUse && strncmp(e.eventId, event.eventId, kDeployEventIdLen) == 0) {
      const bool wasInUse = e.inUse;
      e = event;
      e.inUse = wasInUse;
      return true;
    }
  }
  for (size_t i = 0; i < kMaxOutboxEvents; ++i) {
    DeploymentEvent& e = gRecord.outbox[i];
    if (!e.inUse) {
      e = event;
      e.inUse = true;
      if (gRecord.outboxCount < kMaxOutboxEvents) gRecord.outboxCount++;
      return true;
    }
  }
  // Full. The caller must refuse the operation: dropping an event here would
  // lose exactly the deployment history this outbox exists to preserve.
  Serial.println("[DEPLOY] outbox full — refusing to drop a deployment event");
  return false;
}

bool deploymentOutboxHasRoomFor(const char* eventId) {
  if (!gReady) return false;
  for (size_t i = 0; i < kMaxOutboxEvents; ++i) {
    const DeploymentEvent& e = gRecord.outbox[i];
    if (!e.inUse) return true;                            // free slot
    if (eventId && eventId[0] &&
        strncmp(e.eventId, eventId, kDeployEventIdLen) == 0) {
      return true;                                        // upsert in place
    }
  }
  return false;
}

bool deploymentOutboxHasEvent(const char* eventId) {
  if (!gReady || !eventId || !eventId[0]) return false;
  for (size_t i = 0; i < kMaxOutboxEvents; ++i) {
    const DeploymentEvent& event = gRecord.outbox[i];
    if (event.inUse && strncmp(event.eventId, eventId, kDeployEventIdLen) == 0) {
      return true;
    }
  }
  return false;
}

bool deploymentOutboxGet(const char* eventId, DeploymentEvent& out) {
  if (!gReady || !eventId || !eventId[0]) return false;
  for (size_t i = 0; i < kMaxOutboxEvents; ++i) {
    const DeploymentEvent& event = gRecord.outbox[i];
    if (event.inUse && strncmp(event.eventId, eventId, kDeployEventIdLen) == 0) {
      out = event;
      return true;
    }
  }
  return false;
}

uint8_t deploymentOutboxCount() {
  if (!gReady) return 0;
  uint8_t n = 0;
  for (size_t i = 0; i < kMaxOutboxEvents; ++i) {
    if (gRecord.outbox[i].inUse) n++;
  }
  return n;
}

const DeploymentEvent* deploymentOutboxAt(uint8_t index) {
  if (!gReady) return nullptr;
  uint8_t n = 0;
  for (size_t i = 0; i < kMaxOutboxEvents; ++i) {
    if (!gRecord.outbox[i].inUse) continue;
    if (n == index) return &gRecord.outbox[i];
    n++;
  }
  return nullptr;
}

bool deploymentOutboxNoteConflict(const char* eventId, const char* reason) {
  if (!gReady || !eventId || !eventId[0]) return false;
  for (size_t i = 0; i < kMaxOutboxEvents; ++i) {
    DeploymentEvent& e = gRecord.outbox[i];
    if (e.inUse && strncmp(e.eventId, eventId, kDeployEventIdLen) == 0) {
      copyStr(e.conflictReason, sizeof(e.conflictReason),
              (reason && reason[0]) ? reason : "Rejected by the dashboard");
      return true;   // deliberately still queued — it did not commit
    }
  }
  return false;
}

String deploymentOutboxConflictSummary() {
  if (!gReady) return String();
  for (size_t i = 0; i < kMaxOutboxEvents; ++i) {
    const DeploymentEvent& e = gRecord.outbox[i];
    if (e.inUse && e.conflictReason[0]) return String(e.conflictReason);
  }
  return String();
}

bool deploymentOutboxRemove(const char* eventId) {
  if (!gReady || !eventId || !eventId[0]) return false;
  for (size_t i = 0; i < kMaxOutboxEvents; ++i) {
    DeploymentEvent& e = gRecord.outbox[i];
    if (e.inUse && strncmp(e.eventId, eventId, kDeployEventIdLen) == 0) {
      memset(&e, 0, sizeof(e));
      if (gRecord.outboxCount > 0) gRecord.outboxCount--;
      return true;
    }
  }
  return false;
}

String deploymentOutboxToJson() {
  String out = "[";
  if (gReady) {
    bool first = true;
    for (size_t i = 0; i < kMaxOutboxEvents; ++i) {
      const DeploymentEvent& e = gRecord.outbox[i];
      if (!e.inUse) continue;
      if (!first) out += ",";
      first = false;
      out += "{\"eventId\":\"";              out += jsonEscape(e.eventId);
      out += "\",\"nodeId\":\"";             out += jsonEscape(e.nodeId);
      out += "\",\"deploymentEpoch\":";      out += String((unsigned)e.deploymentEpoch);
      out += ",\"deploymentStartedUnix\":";  out += String(e.deploymentStartedUnix);
      out += ",\"deploymentEndedUnix\":";    out += String(e.deploymentEndedUnix);
      out += ",\"userId\":\"";               out += jsonEscape(e.userId);
      out += "\",\"name\":\"";               out += jsonEscape(e.name);
      out += "\"";
      char nb[16];
      out += ",\"latitude\":";
      if (isnan(e.latitude)) { out += "null"; }
      else { dtostrf(e.latitude, 1, 5, nb); out += nb; }
      out += ",\"longitude\":";
      if (isnan(e.longitude)) { out += "null"; }
      else { dtostrf(e.longitude, 1, 5, nb); out += nb; }
      out += "}";
    }
  }
  out += "]";
  return out;
}

// ---------------------------------------------------------------------------
// Diagnostics / test seam
// ---------------------------------------------------------------------------

uint32_t deploymentEpochClampCount() { return gReady ? gRecord.epochClampCount : 0; }

void deploymentNoteEpochClamp() {
  if (gReady) gRecord.epochClampCount++;
}

void deploymentStoreResetForTest() {
  initFreshRecord();
  gReady = true;
  gCreatedFresh = true;
}

// Drain deploymentEventAcks[] / deploymentEventConflicts[] from an upload
// response.
//
// The two lists are handled DIFFERENTLY:
//   acked     -> the backend committed it; remove from the outbox.
//   conflicted-> it did NOT commit; keep it queued and record the reason for the
//                operator. The common case is a number still held by another
//                node's active deployment, which clears itself once that node's
//                End event lands — dropping the event would permanently lose the
//                deployment record for a condition that resolves on its own.
//   neither   -> never reached us; stays queued and is resent. Safe because the
//                eventId is deterministic and the backend upserts on
//                (project, node, epoch).
//
// Deliberately a hand-rolled scan rather than a JSON parse: the response can be
// several KB and this runs on the modem path where heap is tight, matching how
// the rest of this file reads responses.
void deploymentIngestAckResponse(const String& body) {
  if (body.length() == 0 || deploymentOutboxCount() == 0) return;

  bool dirty = false;

  // --- Acks: bare quoted ids ---------------------------------------------
  const int ackAt = body.indexOf("\"deploymentEventAcks\"");
  if (ackAt >= 0) {
    const int arrStart = body.indexOf('[', ackAt);
    const int arrEnd   = arrStart >= 0 ? body.indexOf(']', arrStart) : -1;
    int scan = arrStart;
    while (arrStart >= 0 && arrEnd > arrStart && scan < arrEnd) {
      const int q1 = body.indexOf('"', scan);
      if (q1 < 0 || q1 > arrEnd) break;
      const int q2 = body.indexOf('"', q1 + 1);
      if (q2 < 0 || q2 > arrEnd) break;
      const String token = body.substring(q1 + 1, q2);
      scan = q2 + 1;
      DeploymentEvent ackedEvent{};
      const bool knownEvent = token.length() &&
          deploymentOutboxGet(token.c_str(), ackedEvent);
      if (knownEvent && deploymentOutboxRemove(token.c_str())) {
        markFieldMeshEventAcked(ackedEvent);
        Serial.printf("[DEPLOY] Event %s acked — cleared from outbox\n", token.c_str());
        dirty = true;
      }
    }
  }

  // --- Conflicts: {"eventId":"...","reason":"..."} objects ----------------
  const int confAt = body.indexOf("\"deploymentEventConflicts\"");
  if (confAt >= 0) {
    const int arrStart = body.indexOf('[', confAt);
    const int arrEnd   = arrStart >= 0 ? body.indexOf(']', arrStart) : -1;
    int scan = arrStart;
    while (arrStart >= 0 && arrEnd > arrStart && scan < arrEnd) {
      const int idKey = body.indexOf("\"eventId\"", scan);
      if (idKey < 0 || idKey > arrEnd) break;
      const int idq1 = body.indexOf('"', body.indexOf(':', idKey) + 1);
      if (idq1 < 0 || idq1 > arrEnd) break;
      const int idq2 = body.indexOf('"', idq1 + 1);
      if (idq2 < 0 || idq2 > arrEnd) break;
      const String eventId = body.substring(idq1 + 1, idq2);

      String reason;
      const int rKey = body.indexOf("\"reason\"", idq2);
      if (rKey > 0 && rKey < arrEnd) {
        const int rq1 = body.indexOf('"', body.indexOf(':', rKey) + 1);
        int rq2 = rq1 + 1;
        while (rq2 > 0 && rq2 < arrEnd) {          // honour \" inside the reason
          rq2 = body.indexOf('"', rq2);
          if (rq2 < 0) break;
          if (body[rq2 - 1] != '\\') break;
          rq2++;
        }
        if (rq1 > 0 && rq2 > rq1) reason = body.substring(rq1 + 1, rq2);
      }
      scan = idq2 + 1;

      if (eventId.length() &&
          deploymentOutboxNoteConflict(eventId.c_str(), reason.c_str())) {
        Serial.printf("[DEPLOY] Event %s CONFLICT (still queued): %s\n",
                      eventId.c_str(), reason.c_str());
        dirty = true;
      }
    }
  }

  if (dirty && !deploymentStoreCommit()) {
    // The commit failed, so the events are still on flash and will be resent.
    // Harmless: the backend upserts, so a duplicate send is a no-op.
    Serial.println("[DEPLOY] Outbox ack commit failed — events will be resent");
  }
}
