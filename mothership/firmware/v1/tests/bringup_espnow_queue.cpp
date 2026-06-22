// Mothership V1 bringup: ESP-NOW callback-to-main-task snapshot queue.

#include <Arduino.h>
#include <LittleFS.h>
#include "system/pins.h"
#include "storage/flash_logger.h"

#include "../src/comms/espnow_sync.cpp"

namespace {

int gPassed = 0;
int gFailed = 0;

void reportResult(const char* name, bool passed) {
  Serial.printf("[%s] %s\n", passed ? "PASS" : "FAIL", name);
  passed ? ++gPassed : ++gFailed;
}

node_snapshot_t makeSnapshot(uint32_t sequence) {
  node_snapshot_t snap{};
  strncpy(snap.command, "NODE_SNAPSHOT", sizeof(snap.command) - 1);
  strncpy(snap.nodeId, "QUEUE_TEST", sizeof(snap.nodeId) - 1);
  snap.seqNum = sequence;
  snap.batVoltage = 3.8f;
  return snap;
}

void inject(const uint8_t mac[6], uint32_t sequence) {
  node_snapshot_t snap = makeSnapshot(sequence);
  onEspNowRecv(mac, reinterpret_cast<const uint8_t*>(&snap), sizeof(snap));
}

}  // namespace

void setup() {
  pinMode(PIN_PWR_HOLD, OUTPUT);
  digitalWrite(PIN_PWR_HOLD, HIGH);

  Serial.begin(115200);
  delay(400);
  Serial.println();
  Serial.println("=== Mothership V1 ESP-NOW Queue Bring-up ===");

  reportResult("Flash initialization", initFlash());
  const bool espNowReady = initEspNowSyncOnly(ESPNOW_CHANNEL);
  reportResult("ESP-NOW sync initialization", espNowReady);

  const uint8_t mac[6] = {0x02, 0, 0, 0, 0, 1};
  initSnapQueue(4);
  for (uint32_t i = 0; i < 4; ++i) inject(mac, i);

  EspNowSnapSlot slots[4]{};
  const int drained = drainSnapQueue(slots, 4);
  bool ordered = drained == 4;
  for (int i = 0; i < drained; ++i) {
    ordered = ordered && slots[i].snap.seqNum == static_cast<uint32_t>(i);
  }
  reportResult("All queued snapshots drain in order", ordered);

  initSnapQueue(2);
  for (uint32_t i = 0; i < 6; ++i) inject(mac, i);
  EspNowSnapSlot overflowSlots[2]{};
  const int overflowDrained = drainSnapQueue(overflowSlots, 2);
  reportResult("Queue overflow increments drop counter", getSnapDropCount() == 4);
  reportResult("Drop-oldest policy keeps newest snapshots",
               overflowDrained == 2 && overflowSlots[0].snap.seqNum == 4 &&
               overflowSlots[1].snap.seqNum == 5);

  deinitEspNowSync();
  reportResult("Deinit removes snapshot queue", drainSnapQueue(slots, 1) == 0);

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
