#include "ota/mothership_ota_release_store.h"

#include <Preferences.h>
#include <string.h>

namespace {

constexpr const char* kNamespace = "ota_release";
constexpr const char* kKeyA = "rec_a";
constexpr const char* kKeyB = "rec_b";
constexpr uint32_t kMagic = 0x464D4F52UL;   // "FMOR" (FieldMesh OTA Release)
constexpr uint16_t kVersion = 1;
constexpr size_t   kReleaseIdLen = 40;

struct ReleaseRecord {
  uint32_t magic;
  uint16_t version;
  uint16_t size;
  uint32_t generation;
  // Confirmed-running release.
  char     installedReleaseId[kReleaseIdLen];
  uint32_t installedSequence;
  // Flashed + set-boot, awaiting first-boot confirmation.
  char     armedReleaseId[kReleaseIdLen];
  uint32_t armedSequence;
  uint8_t  armedValid;
  // Fetch intent.
  char     pendingReleaseId[kReleaseIdLen];
  uint8_t  pendingValid;
  uint8_t  reserved[2];
  uint32_t checksum;
};

ReleaseRecord gRec{};
bool          gLoaded = false;

uint32_t checksumFor(ReleaseRecord record) {
  record.checksum = 0;
  const uint8_t* bytes = reinterpret_cast<const uint8_t*>(&record);
  uint32_t hash = 2166136261UL;
  for (size_t i = 0; i < sizeof(record); ++i) {
    hash ^= bytes[i];
    hash *= 16777619UL;
  }
  return hash;
}

bool validRecord(const ReleaseRecord& r) {
  return r.magic == kMagic && r.version == kVersion &&
         r.size == sizeof(ReleaseRecord) &&
         r.armedValid <= 1 && r.pendingValid <= 1 &&
         r.checksum == checksumFor(r);
}

bool readRecord(Preferences& prefs, const char* key, ReleaseRecord& out) {
  if (prefs.getBytesLength(key) != sizeof(out)) return false;
  return prefs.getBytes(key, &out, sizeof(out)) == sizeof(out) && validRecord(out);
}

bool generationNewer(uint32_t a, uint32_t b) {
  return static_cast<int32_t>(a - b) > 0;
}

// Persist gRec (with fields already mutated by the caller) to the older A/B
// slot with a read-back verify. Bumps generation. Returns false and leaves
// gRec's in-RAM value intact-on-failure only after committing the candidate.
bool persist() {
  ReleaseRecord candidate = gRec;
  candidate.magic = kMagic;
  candidate.version = kVersion;
  candidate.size = sizeof(candidate);
  candidate.generation = gRec.generation + 1U;
  candidate.reserved[0] = candidate.reserved[1] = 0;
  candidate.checksum = checksumFor(candidate);
  const char* key = (candidate.generation & 1U) ? kKeyA : kKeyB;

  Preferences prefs;
  if (!prefs.begin(kNamespace, false)) return false;
  if (prefs.isKey(key)) prefs.remove(key);
  const bool wrote = prefs.putBytes(key, &candidate, sizeof(candidate)) == sizeof(candidate);
  prefs.end();
  if (!wrote || !prefs.begin(kNamespace, true)) return false;
  ReleaseRecord verify{};
  const bool verified = readRecord(prefs, key, verify) &&
                        memcmp(&verify, &candidate, sizeof(candidate)) == 0;
  prefs.end();
  if (!verified) return false;
  gRec = candidate;
  return true;
}

void ensureLoaded() {
  if (gLoaded) return;
  otaReleaseStoreInit();
}

}  // namespace

void otaReleaseStoreInit() {
  memset(&gRec, 0, sizeof(gRec));
  ReleaseRecord a{}, b{};
  Preferences prefs;
  if (prefs.begin(kNamespace, true)) {
    const bool okA = readRecord(prefs, kKeyA, a);
    const bool okB = readRecord(prefs, kKeyB, b);
    prefs.end();
    if (okA && okB) {
      gRec = generationNewer(a.generation, b.generation) ? a : b;
    } else if (okA) {
      gRec = a;
    } else if (okB) {
      gRec = b;
    }
  }
  gLoaded = true;
}

uint32_t otaReleaseStoreInstalledSequence() {
  ensureLoaded();
  return gRec.installedSequence;
}

bool otaReleaseStoreInstalledReleaseId(char* out, size_t outLen) {
  ensureLoaded();
  if (out && outLen) strlcpy(out, gRec.installedReleaseId, outLen);
  return gRec.installedReleaseId[0] != '\0';
}

bool otaReleaseStoreRecordInstalled(const char* releaseId, uint32_t sequence) {
  ensureLoaded();
  if (!releaseId) return false;
  strlcpy(gRec.installedReleaseId, releaseId, sizeof(gRec.installedReleaseId));
  gRec.installedSequence = sequence;
  return persist();
}

bool otaReleaseStoreSetPending(const char* releaseId) {
  ensureLoaded();
  if (!releaseId || releaseId[0] == '\0') return false;
  strlcpy(gRec.pendingReleaseId, releaseId, sizeof(gRec.pendingReleaseId));
  gRec.pendingValid = 1;
  return persist();
}

bool otaReleaseStoreGetPending(char* out, size_t outLen) {
  ensureLoaded();
  if (out && outLen) strlcpy(out, gRec.pendingValid ? gRec.pendingReleaseId : "", outLen);
  return gRec.pendingValid != 0;
}

bool otaReleaseStoreClearPending() {
  ensureLoaded();
  if (!gRec.pendingValid) return true;
  gRec.pendingValid = 0;
  memset(gRec.pendingReleaseId, 0, sizeof(gRec.pendingReleaseId));
  return persist();
}

bool otaReleaseStoreSetArmed(const char* releaseId, uint32_t sequence) {
  ensureLoaded();
  if (!releaseId || releaseId[0] == '\0') return false;
  strlcpy(gRec.armedReleaseId, releaseId, sizeof(gRec.armedReleaseId));
  gRec.armedSequence = sequence;
  gRec.armedValid = 1;
  return persist();
}

bool otaReleaseStoreGetArmed(char* out, size_t outLen, uint32_t* outSequence) {
  ensureLoaded();
  if (out && outLen) strlcpy(out, gRec.armedValid ? gRec.armedReleaseId : "", outLen);
  if (outSequence) *outSequence = gRec.armedValid ? gRec.armedSequence : 0;
  return gRec.armedValid != 0;
}

bool otaReleaseStorePromoteArmed() {
  ensureLoaded();
  if (!gRec.armedValid) return false;
  strlcpy(gRec.installedReleaseId, gRec.armedReleaseId, sizeof(gRec.installedReleaseId));
  gRec.installedSequence = gRec.armedSequence;
  gRec.armedValid = 0;
  memset(gRec.armedReleaseId, 0, sizeof(gRec.armedReleaseId));
  gRec.armedSequence = 0;
  return persist();
}

bool otaReleaseStoreClearArmed() {
  ensureLoaded();
  if (!gRec.armedValid) return true;
  gRec.armedValid = 0;
  memset(gRec.armedReleaseId, 0, sizeof(gRec.armedReleaseId));
  gRec.armedSequence = 0;
  return persist();
}

void otaReleaseStoreResetForTest() {
  Preferences prefs;
  if (prefs.begin(kNamespace, false)) {
    prefs.clear();
    prefs.end();
  }
  memset(&gRec, 0, sizeof(gRec));
  gLoaded = false;
}
