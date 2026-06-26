#include <Arduino.h>
#include <nvs_flash.h>
#include "protocol.h"
#include "storage/local_queue.h"

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

static node_snapshot_t makeSnap(uint32_t marker) {
  node_snapshot_t snap{};
  strncpy(snap.command, "NODE_SNAPSHOT", sizeof(snap.command) - 1);
  strncpy(snap.nodeId, "ENV_TEST", sizeof(snap.nodeId) - 1);
  snap.nodeTimestamp = 1800000000UL + marker;
  snap.sensorPresent = SNAP_PRESENT_BAT_V;
  snap.batVoltage = 4.1f;
  return snap;
}

void setup() {
  pinMode(PWR_HOLD_PIN, OUTPUT);
  digitalWrite(PWR_HOLD_PIN, PWR_HOLD_ACTIVE_HIGH ? HIGH : LOW);

  Serial.begin(115200);
  delay(1500);
  Serial.println("=== Node local queue robustness test ===");

  esp_err_t err = nvs_flash_init();
  if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    err = nvs_flash_init();
  }
  report("NVS init", err == ESP_OK);

  local_queue::resetForTest();
  report("queue begin", local_queue::begin());
  local_queue::clear();

  report("enqueue #1", local_queue::enqueue(makeSnap(1)));
  report("enqueue #2", local_queue::enqueue(makeSnap(2)));
  report("queue count=2", local_queue::count() == 2);

  node_snapshot_t head{};
  report("peek head", local_queue::peek(head));
  report("head seq is acknowledged", local_queue::acknowledgeHead(head.seqNum));
  report("wrong seq rejected", !local_queue::acknowledgeHead(head.seqNum));
  report("queue count=1", local_queue::count() == 1);

  local_queue::QueueStats s = local_queue::stats();
  Serial.printf("[STATS] dropped=%lu persistFail=%lu recoveredSecondary=%lu corrupt=%lu\n",
                (unsigned long)s.droppedDueToCapacity,
                (unsigned long)s.persistenceFailures,
                (unsigned long)s.recoveredFromSecondary,
                (unsigned long)s.corruptRecords);

  Serial.printf("RESULT: %s\n", g_pass ? "PASS" : "FAIL");
}

void loop() {
  static uint32_t last = 0;
  if (millis() - last > 5000UL) {
    last = millis();
    Serial.printf("[queue] idle result=%s\n", g_pass ? "PASS" : "FAIL");
  }
  delay(250);
}

