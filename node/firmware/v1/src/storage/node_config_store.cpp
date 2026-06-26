#include "node_config_store.h"

#include <Preferences.h>
#include <string.h>

namespace {

static constexpr const char* kNamespace = "node_cfg";
static constexpr const char* kSlotA = "node_cfg_a";
static constexpr const char* kSlotB = "node_cfg_b";
static constexpr uint32_t kMagic = 0x4E434647UL;  // "NCFG"
static constexpr uint16_t kSchema = 1;

struct __attribute__((packed)) StoredNodeConfig {
  uint32_t magic;
  uint16_t schema;
  uint16_t length;
  uint32_t generation;
  uint32_t checksum;
  uint8_t mothershipMac[6];
  uint8_t state;
  uint8_t rtcSynced;
  uint8_t deployed;
  uint8_t rtcPowerLost;
  uint8_t recoveryReason;
  uint8_t wakeIntervalMin;
  uint16_t syncIntervalMin;
  uint32_t syncPhaseUnix;
  uint32_t lastTimeSyncUnix;
  uint32_t lastSyncSlot;
  uint16_t appliedConfigVersion;
  uint16_t reserved;
};

static uint32_t g_generation = 0;
static char g_activeSlot = 'a';
static bool g_loaded = false;

uint32_t fnv1a32(const uint8_t* data, size_t len) {
  uint32_t h = 2166136261u;
  for (size_t i = 0; i < len; ++i) {
    h ^= data[i];
    h *= 16777619u;
  }
  return h;
}

uint32_t checksumFor(StoredNodeConfig rec) {
  rec.checksum = 0;
  return fnv1a32(reinterpret_cast<const uint8_t*>(&rec), sizeof(rec));
}

bool generationNewer(uint32_t a, uint32_t b) {
  return a != b && static_cast<int32_t>(a - b) > 0;
}

bool validWakeInterval(uint8_t minutes) {
  switch (minutes) {
    case 0: case 1: case 5: case 10: case 20: case 30: case 60:
      return true;
    default:
      return false;
  }
}

bool plausibleUnixOrZero(uint32_t value) {
  return value == 0 || (value >= 1704067200UL && value <= 2145916800UL);
}

bool validRecord(const StoredNodeConfig& rec) {
  if (rec.magic != kMagic || rec.schema != kSchema ||
      rec.length != sizeof(StoredNodeConfig)) {
    return false;
  }
  if (rec.state > 2) return false;
  if (!validWakeInterval(rec.wakeIntervalMin)) return false;
  if (rec.syncIntervalMin > 24U * 60U) return false;
  if (!plausibleUnixOrZero(rec.syncPhaseUnix)) return false;
  if (!plausibleUnixOrZero(rec.lastTimeSyncUnix)) return false;
  return rec.checksum == checksumFor(rec);
}

StoredNodeConfig toStored(const NodeConfigStoreRecord& record, uint32_t generation) {
  StoredNodeConfig out{};
  out.magic = kMagic;
  out.schema = kSchema;
  out.length = sizeof(StoredNodeConfig);
  out.generation = generation;
  memcpy(out.mothershipMac, record.mothershipMac, sizeof(out.mothershipMac));
  out.state = record.state;
  out.rtcSynced = record.rtcSynced ? 1 : 0;
  out.deployed = record.deployed ? 1 : 0;
  out.rtcPowerLost = record.rtcPowerLost ? 1 : 0;
  out.recoveryReason = record.recoveryReason;
  out.wakeIntervalMin = record.wakeIntervalMin;
  out.syncIntervalMin = record.syncIntervalMin;
  out.syncPhaseUnix = record.syncPhaseUnix;
  out.lastTimeSyncUnix = record.lastTimeSyncUnix;
  out.lastSyncSlot = record.lastSyncSlot;
  out.appliedConfigVersion = record.appliedConfigVersion;
  out.checksum = checksumFor(out);
  return out;
}

NodeConfigStoreRecord fromStored(const StoredNodeConfig& stored) {
  NodeConfigStoreRecord out{};
  memcpy(out.mothershipMac, stored.mothershipMac, sizeof(out.mothershipMac));
  out.state = stored.state;
  out.rtcSynced = stored.rtcSynced != 0;
  out.deployed = stored.deployed != 0;
  out.rtcPowerLost = stored.rtcPowerLost != 0;
  out.recoveryReason = stored.recoveryReason;
  out.wakeIntervalMin = stored.wakeIntervalMin;
  out.syncIntervalMin = stored.syncIntervalMin;
  out.syncPhaseUnix = stored.syncPhaseUnix;
  out.lastTimeSyncUnix = stored.lastTimeSyncUnix;
  out.lastSyncSlot = stored.lastSyncSlot;
  out.appliedConfigVersion = stored.appliedConfigVersion;
  return out;
}

bool readSlot(Preferences& prefs, const char* key, StoredNodeConfig& out) {
  if (prefs.getBytesLength(key) != sizeof(out)) return false;
  const size_t n = prefs.getBytes(key, &out, sizeof(out));
  return n == sizeof(out) && validRecord(out);
}

bool writeVerifySlot(const char* key, const StoredNodeConfig& record) {
  Preferences prefs;
  if (!prefs.begin(kNamespace, false)) return false;

  const size_t written = prefs.putBytes(key, &record, sizeof(record));
  StoredNodeConfig verify{};
  const bool readOk = readSlot(prefs, key, verify);
  prefs.end();

  return written == sizeof(record) &&
         readOk &&
         memcmp(&record, &verify, sizeof(record)) == 0;
}

bool parseMacHex(const char* macHex, uint8_t out[6]) {
  if (!macHex || strlen(macHex) != 12) return false;
  for (int i = 0; i < 6; ++i) {
    char byteText[3] = {macHex[i * 2], macHex[i * 2 + 1], '\0'};
    char* end = nullptr;
    unsigned long value = strtoul(byteText, &end, 16);
    if (!end || *end != '\0' || value > 0xFFUL) return false;
    out[i] = static_cast<uint8_t>(value);
  }
  return true;
}

bool loadLegacy(NodeConfigStoreRecord& out) {
  Preferences prefs;
  if (!prefs.begin(kNamespace, true)) return false;

  const bool hasLegacy =
      prefs.isKey("state") ||
      prefs.isKey("msmac") ||
      prefs.isKey("rtc_synced") ||
      prefs.isKey("deployed") ||
      prefs.isKey("interval") ||
      prefs.isKey("syncMin") ||
      prefs.isKey("syncPhase") ||
      prefs.isKey("cfgVer");
  if (!hasLegacy) {
    prefs.end();
    return false;
  }

  memset(&out, 0, sizeof(out));
  out.state = prefs.getUChar("state", 0);
  out.rtcSynced = prefs.getBool("rtc_synced", false);
  out.deployed = prefs.getBool("deployed", false);
  out.wakeIntervalMin = prefs.getUChar("interval", 1);
  out.lastTimeSyncUnix = prefs.getULong("lastSync", 0);
  out.syncIntervalMin = prefs.getUShort("syncMin", 15);
  out.syncPhaseUnix = prefs.getULong("syncPhase", 0);
  out.lastSyncSlot = prefs.getULong("syncSlot", 0xFFFFFFFFUL);
  out.appliedConfigVersion = prefs.getUShort("cfgVer", 0);

  char macHex[13] = {0};
  prefs.getString("msmac", macHex, sizeof(macHex));
  prefs.end();

  parseMacHex(macHex, out.mothershipMac);
  if (out.state > 2) out.state = 0;
  if (!validWakeInterval(out.wakeIntervalMin)) out.wakeIntervalMin = 1;
  if (out.syncIntervalMin > 24U * 60U) out.syncIntervalMin = 15;
  if (!plausibleUnixOrZero(out.syncPhaseUnix)) out.syncPhaseUnix = 0;
  if (!plausibleUnixOrZero(out.lastTimeSyncUnix)) out.lastTimeSyncUnix = 0;
  return true;
}

}  // namespace

bool nodeConfigStoreLoad(NodeConfigStoreRecord& out,
                         NodeConfigLoadStatus* statusOut) {
  Preferences prefs;
  if (prefs.begin(kNamespace, true)) {
    StoredNodeConfig a{}, b{};
    const bool aValid = readSlot(prefs, kSlotA, a);
    const bool bValid = readSlot(prefs, kSlotB, b);
    prefs.end();

    if (aValid || bValid) {
      const bool useB = bValid && (!aValid || generationNewer(b.generation, a.generation));
      const StoredNodeConfig& chosen = useB ? b : a;
      out = fromStored(chosen);
      g_generation = chosen.generation;
      g_activeSlot = useB ? 'b' : 'a';
      g_loaded = true;
      if (statusOut) {
        *statusOut = useB ? NodeConfigLoadStatus::LoadedSecondary
                          : NodeConfigLoadStatus::LoadedPrimary;
      }
      return true;
    }
  }

  NodeConfigStoreRecord legacy{};
  if (loadLegacy(legacy)) {
    const uint32_t nextGeneration = 1;
    const StoredNodeConfig stored = toStored(legacy, nextGeneration);
    if (writeVerifySlot(kSlotA, stored)) {
      out = legacy;
      g_generation = nextGeneration;
      g_activeSlot = 'a';
      g_loaded = true;
      if (statusOut) *statusOut = NodeConfigLoadStatus::MigratedLegacy;
      return true;
    }
  }

  if (statusOut) *statusOut = NodeConfigLoadStatus::NoValidConfig;
  return false;
}

bool nodeConfigStoreSave(const NodeConfigStoreRecord& record) {
  if (record.state > 2 ||
      !validWakeInterval(record.wakeIntervalMin) ||
      record.syncIntervalMin > 24U * 60U ||
      !plausibleUnixOrZero(record.syncPhaseUnix) ||
      !plausibleUnixOrZero(record.lastTimeSyncUnix)) {
    return false;
  }

  const char* key = (!g_loaded || g_activeSlot == 'b') ? kSlotA : kSlotB;
  const char nextSlot = (!g_loaded || g_activeSlot == 'b') ? 'a' : 'b';
  const StoredNodeConfig stored = toStored(record, g_generation + 1);
  if (!writeVerifySlot(key, stored)) return false;

  g_generation = stored.generation;
  g_activeSlot = nextSlot;
  g_loaded = true;
  return true;
}

#ifdef NODE_CONFIG_STORE_TESTING
void nodeConfigStoreResetForTest() {
  g_generation = 0;
  g_activeSlot = 'a';
  g_loaded = false;
}

bool nodeConfigStoreCorruptActiveForTest() {
  if (!g_loaded) return false;
  const char* key = (g_activeSlot == 'a') ? kSlotA : kSlotB;

  Preferences prefs;
  if (!prefs.begin(kNamespace, false)) return false;
  StoredNodeConfig raw{};
  if (prefs.getBytesLength(key) != sizeof(raw) ||
      prefs.getBytes(key, &raw, sizeof(raw)) != sizeof(raw)) {
    prefs.end();
    return false;
  }
  raw.checksum ^= 0x5A5A5A5AUL;
  const size_t written = prefs.putBytes(key, &raw, sizeof(raw));
  prefs.end();
  return written == sizeof(raw);
}
#endif
