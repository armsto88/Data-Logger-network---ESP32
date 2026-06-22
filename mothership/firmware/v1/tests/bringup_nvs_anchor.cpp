// Mothership V1 bringup: dual-copy CRC-protected sync anchor records.

#include <Arduino.h>
#include <Preferences.h>
#include "system/pins.h"

namespace {

struct __attribute__((packed)) SyncAnchorRecord {
  uint32_t magic;
  uint16_t version;
  uint16_t generation;
  uint32_t phaseUnix;
  uint16_t intervalMin;
  uint8_t mode;
  uint8_t reserved;
  uint32_t crc;
};

constexpr uint32_t kMagic = 0x53594E43UL;
constexpr uint16_t kVersion = 1;
constexpr const char* kNamespace = "anchor_test";
constexpr const char* kAnchorA = "sync_anchor_a";
constexpr const char* kAnchorB = "sync_anchor_b";

int gPassed = 0;
int gFailed = 0;

uint32_t crcRecord(const SyncAnchorRecord& record) {
  const uint8_t* data = reinterpret_cast<const uint8_t*>(&record);
  uint32_t crc = 0xFFFFFFFFUL;
  for (size_t i = 0; i < offsetof(SyncAnchorRecord, crc); ++i) {
    crc ^= data[i];
    for (uint8_t bit = 0; bit < 8; ++bit) {
      crc = (crc >> 1) ^ (0xEDB88320UL & (0UL - (crc & 1UL)));
    }
  }
  return ~crc;
}

SyncAnchorRecord makeRecord(uint16_t generation, uint32_t phase) {
  SyncAnchorRecord record{};
  record.magic = kMagic;
  record.version = kVersion;
  record.generation = generation;
  record.phaseUnix = phase;
  record.intervalMin = 90;
  record.mode = 1;
  record.crc = crcRecord(record);
  return record;
}

bool valid(const SyncAnchorRecord& record) {
  return record.magic == kMagic && record.version == kVersion &&
         record.phaseUnix >= 1704067200UL && record.crc == crcRecord(record);
}

bool readRecord(Preferences& prefs, const char* key, SyncAnchorRecord& record) {
  return prefs.getBytesLength(key) == sizeof(record) &&
         prefs.getBytes(key, &record, sizeof(record)) == sizeof(record);
}

void reportResult(const char* name, bool passed) {
  Serial.printf("[%s] %s\n", passed ? "PASS" : "FAIL", name);
  passed ? ++gPassed : ++gFailed;
}

}  // namespace

void setup() {
  pinMode(PIN_PWR_HOLD, OUTPUT);
  digitalWrite(PIN_PWR_HOLD, HIGH);

  Serial.begin(115200);
  delay(400);
  Serial.println();
  Serial.println("=== Mothership V1 NVS Anchor Bring-up ===");

  Preferences prefs;
  if (!prefs.begin(kNamespace, false)) {
    reportResult("Open NVS namespace", false);
    return;
  }
  prefs.clear();

  SyncAnchorRecord a = makeRecord(0, 1782100800UL);
  SyncAnchorRecord b = makeRecord(1, 1782106200UL);
  prefs.putBytes(kAnchorA, &a, sizeof(a));
  prefs.putBytes(kAnchorB, &b, sizeof(b));
  SyncAnchorRecord readA{};
  SyncAnchorRecord readB{};
  reportResult("Valid anchor writes and reads back",
               readRecord(prefs, kAnchorA, readA) && valid(readA) &&
               memcmp(&a, &readA, sizeof(a)) == 0);

  uint8_t garbage[sizeof(SyncAnchorRecord)]{};
  memset(garbage, 0xA5, sizeof(garbage));
  prefs.putBytes(kAnchorA, garbage, sizeof(garbage));
  readA = {};
  readB = {};
  const bool validA = readRecord(prefs, kAnchorA, readA) && valid(readA);
  const bool validB = readRecord(prefs, kAnchorB, readB) && valid(readB);
  reportResult("Corrupt A selects valid B", !validA && validB && readB.phaseUnix == b.phaseUnix);

  prefs.putBytes(kAnchorB, garbage, sizeof(garbage));
  const uint32_t legacyPhase = 1782111600UL;
  prefs.putULong("sync_last_unix", legacyPhase);
  readA = {};
  readB = {};
  const bool bothInvalid = !(readRecord(prefs, kAnchorA, readA) && valid(readA)) &&
                           !(readRecord(prefs, kAnchorB, readB) && valid(readB));
  const uint32_t fallback = bothInvalid ? prefs.getULong("sync_last_unix", 0) : 0;
  reportResult("Both corrupt copies fall back to legacy anchor", fallback == legacyPhase);
  prefs.end();

  Serial.printf("=== RESULT: %s (%d passed, %d failed) ===\n",
                gFailed == 0 ? "PASS" : "FAIL", gPassed, gFailed);
}

void loop() {
  static uint32_t lastHeartbeatMs = 0;
  if (millis() - lastHeartbeatMs >= 5000) {
    lastHeartbeatMs = millis();
    Serial.printf("[IDLE] heartbeat - result remains %s\n",
                  gFailed == 0 ? "PASS" : "FAIL");
  }
  delay(50);
}
