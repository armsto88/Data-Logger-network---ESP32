// Node V2 Protocol Structure Tests
// Validates that the V2 snapshot struct is correctly packed and sized.
// These tests run on-device (ESP32) via PlatformIO.

#include <Arduino.h>
#include "protocol.h"

void setup() {
  Serial.begin(115200);
  delay(2000);
  
  Serial.println("=== V2 Protocol Structure Tests ===");
  int passed = 0, failed = 0;

  // Test 1: V2 header size
  #ifdef NODE_SNAPSHOT_V2_DEFINED
  {
    size_t expected = 48;
    size_t actual = sizeof(node_snapshot_v2_t);
    if (actual == expected) {
      Serial.println("[PASS] V2 header size = 48 bytes");
      passed++;
    } else {
      Serial.printf("[FAIL] V2 header size: expected %u, got %u\n", (unsigned)expected, (unsigned)actual);
      failed++;
    }
  }
  #else
  Serial.println("[SKIP] V2 struct not yet defined — Phase 1 not implemented");
  failed++;
  #endif

  // Test 2: V2 reading entry size (must be 6 bytes — packed)
  #ifdef V2_READING_DEFINED
  {
    size_t expected = 6;
    size_t actual = sizeof(v2_reading_t);
    if (actual == expected) {
      Serial.println("[PASS] V2 reading entry = 6 bytes (packed)");
      passed++;
    } else {
      Serial.printf("[FAIL] V2 reading entry: expected %u, got %u (packing issue?)\n", (unsigned)expected, (unsigned)actual);
      failed++;
    }
  }
  #else
  Serial.println("[SKIP] V2 reading struct not yet defined");
  failed++;
  #endif

  // Test 3: Field offsets
  #ifdef NODE_SNAPSHOT_V2_DEFINED
  {
    bool ok = true;
    ok &= (offsetof(node_snapshot_v2_t, command) == 0);
    ok &= (offsetof(node_snapshot_v2_t, nodeId) == 16);
    ok &= (offsetof(node_snapshot_v2_t, nodeTimestamp) == 32);
    ok &= (offsetof(node_snapshot_v2_t, seqNum) == 36);
    ok &= (offsetof(node_snapshot_v2_t, sensorCount) == 40);
    ok &= (offsetof(node_snapshot_v2_t, qualityFlags) == 42);
    ok &= (offsetof(node_snapshot_v2_t, configVersion) == 44);
    ok &= (offsetof(node_snapshot_v2_t, protocolVersion) == 46);
    ok &= (offsetof(node_snapshot_v2_t, reserved) == 47);
    if (ok) {
      Serial.println("[PASS] All V2 field offsets correct");
      passed++;
    } else {
      Serial.println("[FAIL] V2 field offsets mismatch");
      failed++;
    }
  }
  #endif

  // Test 4: Wire size calculation
  #ifdef NODE_SNAPSHOT_V2_DEFINED
  {
    // 0 readings: 48 bytes
    if (snapshotV2WireSize(0) == 48) {
      Serial.println("[PASS] Wire size(0) = 48");
      passed++;
    } else {
      Serial.printf("[FAIL] Wire size(0): expected 48, got %u\n", (unsigned)snapshotV2WireSize(0));
      failed++;
    }
    
    // 17 readings: 48 + 102 = 150 bytes
    if (snapshotV2WireSize(17) == 150) {
      Serial.println("[PASS] Wire size(17) = 150");
      passed++;
    } else {
      Serial.printf("[FAIL] Wire size(17): expected 150, got %u\n", (unsigned)snapshotV2WireSize(17));
      failed++;
    }
    
    // 33 readings: 48 + 198 = 246 bytes (max for ESP-NOW)
    if (snapshotV2WireSize(33) == 246) {
      Serial.println("[PASS] Wire size(33) = 246 (ESP-NOW max)");
      passed++;
    } else {
      Serial.printf("[FAIL] Wire size(33): expected 246, got %u\n", (unsigned)snapshotV2WireSize(33));
      failed++;
    }
  }
  #endif

  // Test 5: Detection helpers
  #ifdef NODE_SNAPSHOT_V2_DEFINED
  {
    // V1 detection
    node_snapshot_t v1snap{};
    strcpy(v1snap.command, "NODE_SNAPSHOT");
    if (isV1Snapshot((uint8_t*)&v1snap, sizeof(v1snap))) {
      Serial.println("[PASS] isV1Snapshot detects V1");
      passed++;
    } else {
      Serial.println("[FAIL] isV1Snapshot failed to detect V1");
      failed++;
    }
    
    // V2 detection
    node_snapshot_v2_t v2hdr{};
    strcpy(v2hdr.command, "NODE_SNAPSHOT2");
    v2hdr.sensorCount = 5;
    size_t v2len = snapshotV2WireSize(5);
    if (isV2Snapshot((uint8_t*)&v2hdr, v2len)) {
      Serial.println("[PASS] isV2Snapshot detects V2");
      passed++;
    } else {
      Serial.println("[FAIL] isV2Snapshot failed to detect V2");
      failed++;
    }
    
    // V2 with wrong length should be rejected
    if (!isV2Snapshot((uint8_t*)&v2hdr, 100)) {
      Serial.println("[PASS] isV2Snapshot rejects wrong length");
      passed++;
    } else {
      Serial.println("[FAIL] isV2Snapshot accepted wrong length");
      failed++;
    }
  }
  #endif

  // Test 6: MAX_READINGS_PER_SNAPSHOT
  #ifdef MAX_READINGS_PER_SNAPSHOT
  {
    if (MAX_READINGS_PER_SNAPSHOT == 33) {
      Serial.println("[PASS] MAX_READINGS_PER_SNAPSHOT = 33");
      passed++;
    } else {
      Serial.printf("[FAIL] MAX_READINGS_PER_SNAPSHOT: expected 33, got %u\n", (unsigned)MAX_READINGS_PER_SNAPSHOT);
      failed++;
    }
  }
  #endif

  // Test 7: SENSOR_ID_PAR exists
  #ifdef SENSOR_ID_PAR
  {
    if (SENSOR_ID_PAR != 0 && SENSOR_ID_PAR != SENSOR_ID_UNKNOWN) {
      Serial.printf("[PASS] SENSOR_ID_PAR = %u (not UNKNOWN)\n", (unsigned)SENSOR_ID_PAR);
      passed++;
    } else {
      Serial.println("[FAIL] SENSOR_ID_PAR is 0 or UNKNOWN");
      failed++;
    }
  }
  #else
  Serial.println("[SKIP] SENSOR_ID_PAR not defined yet");
  failed++;
  #endif

  // Summary
  Serial.println();
  Serial.printf("=== Results: %d passed, %d failed ===\n", passed, failed);
  if (failed == 0) {
    Serial.println("OVERALL PASS");
  } else {
    Serial.println("OVERALL FAIL — some tests skipped (implementation pending)");
  }
}

void loop() {
  delay(5000);
}