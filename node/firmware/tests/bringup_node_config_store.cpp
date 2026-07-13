#include <Arduino.h>
#include <Preferences.h>
#include <nvs_flash.h>

#include "storage/node_config_store.h"

#ifndef PWR_HOLD_PIN
#define PWR_HOLD_PIN 23
#endif

#ifndef PWR_HOLD_ACTIVE_HIGH
#define PWR_HOLD_ACTIVE_HIGH 1
#endif

static bool g_pass = true;

static void report(const char* name, bool ok) {
  Serial.printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
  if (!ok) g_pass = false;
}

static void resetNamespace() {
  Preferences prefs;
  if (prefs.begin("node_cfg", false)) {
    prefs.clear();
    prefs.end();
  }
  nodeConfigStoreResetForTest();
}

static NodeConfigStoreRecord makeRecord(uint16_t version, uint8_t wakeMin) {
  NodeConfigStoreRecord record{};
  record.mothershipMac[0] = 0xAA;
  record.mothershipMac[1] = 0xBB;
  record.mothershipMac[2] = 0xCC;
  record.mothershipMac[3] = 0xDD;
  record.mothershipMac[4] = 0xEE;
  record.mothershipMac[5] = static_cast<uint8_t>(version & 0xFF);
  record.state = 2;
  record.rtcSynced = true;
  record.deployed = true;
  record.rtcPowerLost = false;
  record.recoveryReason = 0;
  record.wakeIntervalMin = wakeMin;
  record.syncIntervalMin = 15;
  record.syncPhaseUnix = 1800000000UL + version;
  record.lastTimeSyncUnix = 1800000100UL + version;
  record.lastSyncSlot = 42UL + version;
  record.appliedConfigVersion = version;
  return record;
}

static bool writeInterruptedSlotA() {
  Preferences prefs;
  if (!prefs.begin("node_cfg", false)) return false;
  const uint8_t partial[3] = {0x4E, 0x43, 0x46};
  const size_t written = prefs.putBytes("node_cfg_a", partial, sizeof(partial));
  prefs.end();
  return written == sizeof(partial);
}

static bool writeLegacyConfig() {
  Preferences prefs;
  if (!prefs.begin("node_cfg", false)) return false;
  bool ok = true;
  ok = ok && prefs.putUChar("state", 2) == 1;
  ok = ok && prefs.putBool("rtc_synced", true) == 1;
  ok = ok && prefs.putBool("deployed", true) == 1;
  ok = ok && prefs.putUChar("interval", 5) == 1;
  ok = ok && prefs.putULong("lastSync", 1800000200UL) == 4;
  ok = ok && prefs.putUShort("syncMin", 20) == 2;
  ok = ok && prefs.putULong("syncPhase", 1800000000UL) == 4;
  ok = ok && prefs.putULong("syncSlot", 1234UL) == 4;
  ok = ok && prefs.putUShort("cfgVer", 77) == 2;
  ok = ok && prefs.putString("msmac", "AABBCCDDEEFF") == 12;
  prefs.end();
  return ok;
}

void setup() {
  pinMode(PWR_HOLD_PIN, OUTPUT);
  digitalWrite(PWR_HOLD_PIN, PWR_HOLD_ACTIVE_HIGH ? HIGH : LOW);

  Serial.begin(115200);
  delay(1500);
  Serial.println("=== Node config store robustness test ===");
  Serial.println("[WARN] This test clears the node_cfg NVS namespace.");

  esp_err_t err = nvs_flash_init();
  if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    err = nvs_flash_init();
  }
  report("NVS init", err == ESP_OK);

  resetNamespace();
  report("interrupted slot A written", writeInterruptedSlotA());
  nodeConfigStoreResetForTest();
  {
    NodeConfigStoreRecord out{};
    NodeConfigLoadStatus status = NodeConfigLoadStatus::LoadedPrimary;
    const bool ok = nodeConfigStoreLoad(out, &status);
    report("interrupted slot A rejected",
           !ok && status == NodeConfigLoadStatus::NoValidConfig);
  }

  resetNamespace();
  const NodeConfigStoreRecord rec1 = makeRecord(7, 5);
  report("save primary record", nodeConfigStoreSave(rec1));
  nodeConfigStoreResetForTest();
  {
    NodeConfigStoreRecord out{};
    NodeConfigLoadStatus status = NodeConfigLoadStatus::NoValidConfig;
    const bool ok = nodeConfigStoreLoad(out, &status);
    report("load primary record",
           ok &&
           status == NodeConfigLoadStatus::LoadedPrimary &&
           out.appliedConfigVersion == 7 &&
           out.wakeIntervalMin == 5);
  }

  const NodeConfigStoreRecord rec2 = makeRecord(8, 10);
  report("save secondary newer record", nodeConfigStoreSave(rec2));
  nodeConfigStoreResetForTest();
  {
    NodeConfigStoreRecord out{};
    NodeConfigLoadStatus status = NodeConfigLoadStatus::NoValidConfig;
    const bool ok = nodeConfigStoreLoad(out, &status);
    report("newer secondary wins",
           ok &&
           status == NodeConfigLoadStatus::LoadedSecondary &&
           out.appliedConfigVersion == 8 &&
           out.wakeIntervalMin == 10);
    report("corrupt active secondary", nodeConfigStoreCorruptActiveForTest());
  }
  nodeConfigStoreResetForTest();
  {
    NodeConfigStoreRecord out{};
    NodeConfigLoadStatus status = NodeConfigLoadStatus::NoValidConfig;
    const bool ok = nodeConfigStoreLoad(out, &status);
    report("corrupt newer falls back to older valid",
           ok &&
           status == NodeConfigLoadStatus::LoadedPrimary &&
           out.appliedConfigVersion == 7 &&
           out.wakeIntervalMin == 5);
  }

  resetNamespace();
  report("save single record", nodeConfigStoreSave(rec1));
  report("corrupt single active record", nodeConfigStoreCorruptActiveForTest());
  nodeConfigStoreResetForTest();
  {
    NodeConfigStoreRecord out{};
    NodeConfigLoadStatus status = NodeConfigLoadStatus::LoadedPrimary;
    const bool ok = nodeConfigStoreLoad(out, &status);
    report("single corrupt checksum rejected",
           !ok && status == NodeConfigLoadStatus::NoValidConfig);
  }

  resetNamespace();
  report("write legacy config", writeLegacyConfig());
  nodeConfigStoreResetForTest();
  {
    NodeConfigStoreRecord out{};
    NodeConfigLoadStatus status = NodeConfigLoadStatus::NoValidConfig;
    const bool ok = nodeConfigStoreLoad(out, &status);
    report("legacy migrated",
           ok &&
           status == NodeConfigLoadStatus::MigratedLegacy &&
           out.appliedConfigVersion == 77 &&
           out.wakeIntervalMin == 5 &&
           out.syncIntervalMin == 20 &&
           out.mothershipMac[0] == 0xAA &&
           out.mothershipMac[5] == 0xFF);
  }
  nodeConfigStoreResetForTest();
  {
    NodeConfigStoreRecord out{};
    NodeConfigLoadStatus status = NodeConfigLoadStatus::NoValidConfig;
    const bool ok = nodeConfigStoreLoad(out, &status);
    report("migrated record reloads from A/B",
           ok &&
           status == NodeConfigLoadStatus::LoadedPrimary &&
           out.appliedConfigVersion == 77);
  }

  resetNamespace();
  const NodeConfigStoreRecord stable = makeRecord(11, 5);
  report("save stable version", nodeConfigStoreSave(stable));
  NodeConfigStoreRecord bad = makeRecord(12, 5);
  bad.syncIntervalMin = 2000;
  report("invalid candidate rejected", !nodeConfigStoreSave(bad));
  nodeConfigStoreResetForTest();
  {
    NodeConfigStoreRecord out{};
    NodeConfigLoadStatus status = NodeConfigLoadStatus::NoValidConfig;
    const bool ok = nodeConfigStoreLoad(out, &status);
    report("failed candidate preserves version",
           ok &&
           out.appliedConfigVersion == 11 &&
           out.syncIntervalMin == stable.syncIntervalMin);
  }

  Serial.printf("RESULT: %s\n", g_pass ? "PASS" : "FAIL");
}

void loop() {
  static uint32_t last = 0;
  if (millis() - last > 5000UL) {
    last = millis();
    Serial.printf("[config-store] idle result=%s\n", g_pass ? "PASS" : "FAIL");
  }
  delay(250);
}
