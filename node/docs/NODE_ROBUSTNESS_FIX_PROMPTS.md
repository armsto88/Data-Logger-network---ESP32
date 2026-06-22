# Node Firmware Robustness Fix — Implementation Prompts

**Date:** 2026-06-22
**Purpose:** Step-by-step prompts for a repo coding agent to implement robustness fixes for the node firmware.

## Instructions

1. Read the full node firmware at `node/firmware/src/main.cpp` and `node/firmware/src/storage/local_queue.cpp` before starting any stage.
2. Execute ONE stage at a time. Do not skip ahead.
3. After each stage, verify compilation by running ALL the commands listed in that stage's "Validation" section.
4. Do not proceed to the next stage until the current stage compiles cleanly and previous stages' tests still compile.
5. The system WORKS today. It has been validated in full autonomous sync cycles with the mothership. No change may regress the working sync flow. Every change is additive and defensive.
6. After completing all stages, run `pio run -e esp32wroom` (from `node/firmware/`) one final time to confirm the full firmware compiles.

## Bug inventory and staging

| Stage | Bug(s) | Severity | Files | Risk |
|-------|--------|----------|-------|------|
| 1 | ESP-NOW callback I2C/NVS races + espnowSendWithRecover from callback | High | `main.cpp` | **highest** |
| 2 | Alarm read-back verification + clear-after-verify | Medium | `main.cpp` | low |
| 3 | Queue persist failure revert + backup key | Medium | `local_queue.{h,cpp}` | medium |
| 4 | Flush retry limit / backoff | Medium | `main.cpp` | low |
| 5 | RTC lost power diagnostic flag | Medium | `main.cpp` | low |
| 6 | CONFIG_SNAPSHOT re-arm trigger | Medium | `main.cpp` | trivial |
| 7 | I2C scan gating | Medium | `main.cpp` | trivial |
| 8 | Low-priority cleanup (strncpy, getString, dead code, etc.) | Low | `main.cpp`, `local_queue.cpp` | low |

---

## Stage 1: ESP-NOW callback safety (High — highest risk)

### Files to modify
- `node/firmware/src/main.cpp`

### Context
The ESP-NOW receive callback `onDataReceived()` runs on the Wi-Fi task. It currently performs I2C writes (`rtc.adjust()`, `ds3231DisableAlarmInterrupt()`, `clearDS3231_AlarmFlags()`), NVS writes (`persistNodeConfig()`, `local_queue::clear()`), and calls `espnowSendWithRecover()` which can `esp_now_deinit()` from within the callback. The `Wire` and `Preferences` libraries are not thread-safe. Concurrent access from the Wi-Fi task and main task can corrupt I2C state or NVS data.

The codebase already has a deferral pattern: `g_rearmAlarmsPending` and `g_deployBootstrapPending` flags are set in the callback and serviced from the main loop. This stage extends that pattern to ALL I2C and NVS operations.

### Bug: I2C and NVS races from onDataReceived() callback

**Fix:** Introduce a command queue (or a set of pending-action flags) that the callback sets instead of performing I2C/NVS work directly. The main loop drains the queue and performs the operations.

#### Approach: Pending action flags

Add the following volatile flags (already have `g_deployBootstrapPending` and `g_rearmAlarmsPending`):

```cpp
// Pending actions set by ESP-NOW callback, serviced from main loop
static volatile bool g_pendingTimeSync = false;
static time_sync_response_t g_pendingTimeSyncData;  // copied in callback, applied in loop
static volatile bool g_pendingPairNode = false;
static volatile bool g_pendingUnpair = false;
static volatile bool g_pendingConfigSnapshot = false;
static config_snapshot_message_t g_pendingConfigSnapshotData;
static volatile bool g_pendingDeployAck = false;  // DEPLOY_ACK to send from main loop
static uint8_t g_pendingDeployAckMac[6];
static volatile bool g_pendingPairingResponse = false;
static pairing_response_t g_pendingPairingResponseData;
```

#### Changes to onDataReceived():

For each command handler that currently does I2C/NVS work, change it to ONLY copy the data and set a flag:

1. **TIME_SYNC handler:** Instead of `rtc.adjust()`, `persistNodeConfig()`, and setting `g_rearmAlarmsPending`:
   - Copy the response into `g_pendingTimeSyncData`
   - Set `g_pendingTimeSync = true`
   - Return immediately

2. **PAIR_NODE / PAIRING_RESPONSE handler:** Instead of `ds3231DisableAlarmInterrupt()`, `clearDS3231_AlarmFlags()`, `local_queue::clear()`, `persistNodeConfig()`:
   - Copy mothership MAC into a pending buffer
   - Set `g_pendingPairNode = true` (or `g_pendingPairingResponse = true` with the response data)
   - Return immediately

3. **UNPAIR_NODE handler:** Instead of `ds3231DisableAlarmInterrupt()`, `clearDS3231_AlarmFlags()`, `persistNodeConfig()`, `local_queue::clear()`:
   - Set `g_pendingUnpair = true`
   - Return immediately

4. **DEPLOY_NODE handler:** This is the most complex. Currently does `rtc.adjust()`, `persistNodeConfig()`, sends DEPLOY_ACK via `espnowSendWithRecover()`, sets `g_deployBootstrapPending`:
   - Copy the deployment command data into a pending buffer
   - Copy the sender MAC for DEPLOY_ACK
   - Set `g_pendingDeploy = true` (new flag) and `g_pendingDeployAck = true`
   - Do NOT call `rtc.adjust()`, `persistNodeConfig()`, or `espnowSendWithRecover()` from the callback
   - Return immediately

5. **CONFIG_SNAPSHOT handler:** Instead of `persistNodeConfig()` and `esp_now_send()` for the ACK:
   - Copy the snapshot data into `g_pendingConfigSnapshotData`
   - Set `g_pendingConfigSnapshot = true`
   - For the ACK: copy the MAC and set `g_pendingConfigAck = true` (new flag)
   - Return immediately

6. **SET_SCHEDULE / SET_SYNC_SCHED handlers:** These already set `g_rearmAlarmsPending` (good) but also call `persistNodeConfig()`:
   - Remove the `persistNodeConfig()` call from the callback
   - Set a `g_pendingPersistConfig = true` flag instead
   - The main loop will persist

7. **DEPLOY_ACK send:** Replace `espnowSendWithRecover(mac, ...)` with a flag + main-loop send. Use raw `esp_now_send()` from the callback ONLY if the peer is already registered (no recovery path). Better: defer entirely to the main loop.

#### Changes to loop():

Add a section at the top of `loop()` (after `processPowerCut()`) to service pending actions:

```cpp
// --- Service pending ESP-NOW callback actions (main task context) ---

if (g_pendingTimeSync) {
  g_pendingTimeSync = false;
  // Apply the time sync: rtc.adjust(), update rtcSynced, lastTimeSyncUnix, persistNodeConfig
  // Set g_rearmAlarmsPending if deployed
  // (move the existing TIME_SYNC handler logic here)
}

if (g_pendingPairNode) {
  g_pendingPairNode = false;
  // Apply pairing: set mothership MAC, clear alarms, clear queue, persist
  // (move the existing PAIR_NODE handler logic here)
}

if (g_pendingUnpair) {
  g_pendingUnpair = false;
  // Apply unpair: clear MAC, clear alarms, clear queue, persist, set g_postUnpairHold
  // (move the existing UNPAIR handler logic here)
}

if (g_pendingDeploy) {
  g_pendingDeploy = false;
  // Apply deployment: rtc.adjust(), set interval/sync/phase, persist, set g_deployBootstrapPending
  // (move the existing DEPLOY_NODE handler logic here)
}

if (g_pendingDeployAck) {
  g_pendingDeployAck = false;
  // Send DEPLOY_ACK from main task using espnowSendWithRecover
  espnowSendWithRecover(g_pendingDeployAckMac, ...);
}

if (g_pendingConfigSnapshot) {
  g_pendingConfigSnapshot = false;
  // Apply config snapshot: update interval/sync/phase, persist, set g_rearmAlarmsPending
  // (move the existing CONFIG_SNAPSHOT handler logic here)
  // Also fix Bug: set g_rearmAlarmsPending = true when schedule changes (Stage 6)
}

if (g_pendingPersistConfig) {
  g_pendingPersistConfig = false;
  persistNodeConfig();
}
```

#### Important constraints:
- The callback can still do `memcpy` (safe), `strcmp` (safe on stack data), and set volatile flags (safe)
- The callback must NOT do: `rtc.adjust()`, `persistNodeConfig()`, `local_queue::clear()`, `ds3231DisableAlarmInterrupt()`, `clearDS3231_AlarmFlags()`, `espnowSendWithRecover()`, or any I2C/NVS operation
- The callback CAN do: `esp_now_send()` with raw call (no recovery) for simple ACKs IF the peer is already registered — but deferring to the main loop is safer
- Keep the `g_syncWindowMarkerMs = millis()` assignment in the callback — it's just a volatile uint32 write, safe
- Keep the `g_deployBootstrapPending = true` and `g_rearmAlarmsPending = true` flag sets — they're already deferred

### Testing
Create `node/firmware/tests/bringup_callback_safety.cpp` with env `[env:esp32wroom-callback-safety]` in `node/firmware/platformio.ini`:
1. Assert PWR_HOLD HIGH first
2. Init RTC, NVS, load config
3. Simulate receiving a TIME_SYNC packet by calling the callback directly with test data
4. Confirm the callback returns quickly (no I2C/NVS blocking)
5. Confirm the pending flag is set
6. Run one loop iteration, confirm the time sync is applied
7. Print PASS/FAIL
8. Loop: idle heartbeat

### Validation
1. `pio run -e esp32wroom` (from `node/firmware/`) — must compile
2. `pio run -e esp32wroom-callback-safety` — must compile
3. Existing bringup tests that compile against the main env must still compile

### What NOT to change
- Do NOT modify `local_queue.cpp`, `rtc_manager.h`, `protocol.h`, or `platformio.ini` (except adding the new test env)
- Do NOT change the `onDataSent()` callback — it only sets volatile flags, which is safe
- Do NOT change `bringupEspNow()`, `shutdownEspNow()`, `espnowSendWithRecover()` — these are called from the main task
- Do NOT change `handleRtcWakeEvents()` or `finalizeWakeAndSleep()` — these run on the main task
- Do NOT remove the `g_syncWindowMarkerMs` assignment in the callback

---

## Stage 2: Alarm read-back verification (Medium)

### Files to modify
- `node/firmware/src/main.cpp`

### Context
Stage 1 is complete. The ESP-NOW callback no longer does I2C work. This stage adds alarm register read-back verification, matching the mothership's Stage 1 fix.

### Bug: No alarm read-back verification

`ds3231WriteA1()` and `ds3231WriteA2()` return `WireRtc.endTransmission() == 0` (I2C ACK only). No read-back of the actual register values. A partial write could leave the alarm at the wrong time.

**Fix:** Add read-back verification functions:

```cpp
static bool verifyAlarm1Registers(uint8_t expectedSec, uint8_t expectedMin, uint8_t expectedHour, uint8_t expectedDayReg) {
  uint8_t regs[4];
  WireRtc.beginTransmission(0x68);
  WireRtc.write(0x07);
  if (WireRtc.endTransmission(false) != 0) return false;
  WireRtc.requestFrom((uint8_t)0x68, (uint8_t)4);
  if (WireRtc.available() < 4) return false;
  for (int i = 0; i < 4; i++) regs[i] = WireRtc.read();
  return regs[0] == expectedSec && regs[1] == expectedMin && regs[2] == expectedHour && regs[3] == expectedDayReg;
}

static bool verifyAlarm2Registers(uint8_t expectedMin, uint8_t expectedHour, uint8_t expectedDayReg) {
  uint8_t regs[3];
  WireRtc.beginTransmission(0x68);
  WireRtc.write(0x0B);
  if (WireRtc.endTransmission(false) != 0) return false;
  WireRtc.requestFrom((uint8_t)0x68, (uint8_t)3);
  if (WireRtc.available() < 3) return false;
  for (int i = 0; i < 3; i++) regs[i] = WireRtc.read();
  return regs[0] == expectedMin && regs[1] == expectedHour && regs[2] == expectedDayReg;
}
```

Update `ds3231ArmNextInNMinutes()` to verify after write:
- After `ds3231WriteA1(secBCD, minBCD, hourBCD, dayReg)`, call `verifyAlarm1Registers(secBCD, minBCD, hourBCD, dayReg)`
- If verification fails, return false

Update `ds3231ArmSyncWake()` to verify after write:
- After `ds3231WriteA2(minBCD, hourBCD, dayReg)`, call `verifyAlarm2Registers(minBCD, hourBCD, dayReg)`
- If verification fails, return false

Update `armDeploymentWakeAlarms()`:
- Move `clearDS3231_AlarmFlags()` to AFTER both alarm writes AND verifications succeed
- If either verification fails, do NOT clear flags — return false
- The existing 3-retry loop in `finalizeWakeAndSleep()` handles the retry

### Testing
Create `node/firmware/tests/bringup_alarm_verify.cpp` with env `[env:esp32wroom-alarm-verify]`:
1. Assert PWR_HOLD HIGH first
2. Init RTC
3. Arm alarm 1 minute from now
4. Verify alarm registers match
5. Corrupt one register manually
6. Confirm verification fails
7. Re-arm, confirm verification passes
8. Print PASS/FAIL
9. Loop: idle heartbeat

### Validation
1. `pio run -e esp32wroom` — must compile
2. `pio run -e esp32wroom-alarm-verify` — must compile

### What NOT to change
- Do NOT modify `local_queue.cpp`, `rtc_manager.h`, `protocol.h`
- Do NOT change the alarm register addresses or BCD encoding
- Do NOT change the `ds3231WriteA1`/`ds3231WriteA2` function signatures

---

## Stage 3: Queue persist failure revert + backup (Medium)

### Files to modify
- `node/firmware/src/storage/local_queue.h`
- `node/firmware/src/storage/local_queue.cpp`

### Context
Stages 1-2 are complete. This stage makes the local queue more robust against NVS write failures.

### Bug: persist() failure doesn't revert in-memory mutation

`enqueue()` mutates `g_blob` (head/tail/used/nextSeq) BEFORE calling `persist()`. If `persist()` fails, the in-memory state is ahead of NVS. On power-off, the last snapshot is lost.

**Fix:** In `enqueue()`:
- Save a backup of `g_blob` before mutation
- Mutate `g_blob`
- Call `persist()`
- If `persist()` fails, restore `g_blob` from backup and return false

```cpp
bool enqueue(const node_snapshot_t& snap) {
  if (!g_ready && !begin()) return false;

  QueueBlob backup = g_blob;  // save state before mutation

  // ... existing mutation logic ...

  if (!persist()) {
    g_blob = backup;  // revert on failure
    Serial.println("[QUEUE] persist failed — reverting in-memory state");
    return false;
  }
  return true;
}
```

Do the same for `pop()`:
```cpp
bool pop() {
  if (!g_ready && !begin()) return false;
  if (g_blob.used == 0) return false;
  QueueBlob backup = g_blob;
  g_blob.tail = (uint16_t)((g_blob.tail + 1) % kCapacity);
  g_blob.used--;
  if (!persist()) {
    g_blob = backup;
    Serial.println("[QUEUE] pop persist failed — reverting");
    return false;
  }
  return true;
}
```

### Bug: Single NVS key, no redundancy

The entire queue is in one NVS key `"blob"`. If this key corrupts, all data is lost.

**Fix:** Add a backup key. In `persist()`:
- Write to `"blob"` (primary)
- Also write to `"blob_bak"` (backup)
- If primary write fails but backup succeeds, log a warning
- In `load()`: try primary first; if checksum fails, try backup; if backup is valid, restore it to primary

```cpp
bool persist() {
  g_blob.checksum = computeChecksum(g_blob);
  Preferences p;
  if (!p.begin("node_q", false)) return false;
  size_t n = p.putBytes("blob", &g_blob, sizeof(g_blob));
  size_t nBak = p.putBytes("blob_bak", &g_blob, sizeof(g_blob));
  p.end();
  if (n != sizeof(g_blob)) {
    Serial.printf("[QUEUE] primary write failed (%u/%u)\n", (unsigned)n, (unsigned)sizeof(g_blob));
  }
  return (n == sizeof(g_blob)) || (nBak == sizeof(g_blob));
}

bool load() {
  Preferences p;
  if (!p.begin("node_q", true)) return false;
  
  // Try primary
  size_t n = p.getBytesLength("blob");
  if (n == sizeof(g_blob)) {
    QueueBlob primary;
    p.getBytes("blob", &primary, sizeof(g_blob));
    if (primary.magic == kMagic && primary.version == kVersion && 
        primary.checksum == computeChecksum(primary) &&
        primary.head < kCapacity && primary.tail < kCapacity && 
        primary.used <= kCapacity) {
      g_blob = primary;
      p.end();
      // Restore backup if primary is valid but backup is stale
      return true;
    }
  }
  
  // Try backup
  n = p.getBytesLength("blob_bak");
  if (n == sizeof(g_blob)) {
    QueueBlob backup;
    p.getBytes("blob_bak", &backup, sizeof(g_blob));
    if (backup.magic == kMagic && backup.version == kVersion &&
        backup.checksum == computeChecksum(backup) &&
        backup.head < kCapacity && backup.tail < kCapacity &&
        backup.used <= kCapacity) {
      Serial.println("[QUEUE] primary corrupted, restoring from backup");
      g_blob = backup;
      p.end();
      // Write restored state back to primary
      persist();
      return true;
    }
  }
  
  p.end();
  return false;
}
```

### Testing
Extend existing bringup tests or create `node/firmware/tests/bringup_queue_robust.cpp` with env `[env:esp32wroom-queue-robust]`:
1. Assert PWR_HOLD HIGH first
2. Init queue, enqueue 5 snapshots
3. Corrupt the primary NVS key (write garbage to "blob")
4. Re-init queue — confirm it loads from backup
5. Confirm all 5 snapshots are intact
6. Test persist failure revert: fill queue, simulate persist failure (mock or comment), confirm in-memory state reverts
7. Print PASS/FAIL
8. Loop: idle heartbeat

### Validation
1. `pio run -e esp32wroom` — must compile
2. `pio run -e esp32wroom-queue-robust` — must compile

### What NOT to change
- Do NOT change the `QueueBlob` struct size or field layout
- Do NOT change the `kCapacity` value (24)
- Do NOT change the checksum algorithm (FNV-1a)
- Do NOT modify `main.cpp`

---

## Stage 4: Flush retry limit / backoff (Medium)

### Files to modify
- `node/firmware/src/main.cpp`

### Context
Stages 1-3 are complete. This stage prevents the node from getting stuck trying to flush the same snapshot forever when the mothership is unreachable.

### Bug: No retry limit on failed flush

`flushQueuedToMothership()` pops only after delivery confirmation. If delivery consistently fails, the loop breaks on the first failure (preserving the queue). But there's no backoff — every sync wake attempts the same flush and fails. The queue fills up over time.

**Fix:** Add a per-snapshot retry counter. After 3 failed delivery attempts for the same snapshot (same seqNum), skip it and try the next one. Mark the skipped snapshot with a `QF_FLUSH_FAILED` flag (new flag, bit 0x0002 in qualityFlags). The mothership can detect this flag in the CSV.

Add a static retry counter in `flushQueuedToMothership()`:
```cpp
static uint8_t g_currentSnapRetryCount = 0;
static uint32_t g_currentSnapSeq = 0xFFFFFFFFUL;

// In the flush loop, after a failed send or delivery timeout:
if (snap.seqNum != g_currentSnapSeq) {
  g_currentSnapSeq = snap.seqNum;
  g_currentSnapRetryCount = 0;
}
g_currentSnapRetryCount++;

if (g_currentSnapRetryCount >= 3) {
  Serial.printf("[FLUSH] Skipping seq=%lu after 3 failed attempts\n", (unsigned long)snap.seqNum);
  // Mark with QF_FLUSH_FAILED and pop it — data is retained in the queue's
  // drop counter but this snapshot is sacrificed to unblock the queue
  // Actually: DON'T pop — just break and try again next sync. But after 3
  // consecutive sync wakes with the same seq stuck, force-pop it.
  break;
}
```

Actually, a simpler approach: add a persistent "consecutive flush failure" counter in NVS. If the same snapshot fails to flush for 3 consecutive sync wakes, force-pop it (data loss is better than a permanently stuck queue). Reset the counter on any successful flush.

Add to `persistNodeConfig()` / `loadNodeConfig()`:
```cpp
uint8_t g_consecutiveFlushFailures = 0;
// persist: p.putUChar("flush_fails", g_consecutiveFlushFailures);
// load: g_consecutiveFlushFailures = p.getUChar("flush_fails", 0);
```

In `flushQueuedToMothership()`, after the flush loop:
```cpp
if (sent == 0 && local_queue::count() > 0) {
  g_consecutiveFlushFailures++;
  persistNodeConfig();
  if (g_consecutiveFlushFailures >= 3) {
    Serial.printf("[FLUSH] 3 consecutive failures — force-popping stuck snapshot\n");
    node_snapshot_t stuck;
    if (local_queue::peek(stuck)) {
      local_queue::pop();
      g_consecutiveFlushFailures = 0;
      persistNodeConfig();
    }
  }
} else if (sent > 0) {
  g_consecutiveFlushFailures = 0;
  persistNodeConfig();
}
```

### Testing
No new test file needed — this is logic that only triggers after 3 consecutive sync failures. Verify by code review and compile.

### Validation
1. `pio run -e esp32wroom` — must compile

### What NOT to change
- Do NOT modify `local_queue.cpp`
- Do NOT change the `flushQueuedToMothership()` function signature
- Do NOT change the `waitForSendDelivery()` timeout (200ms)

---

## Stage 5: RTC lost power diagnostic flag (Medium)

### Files to modify
- `node/firmware/src/main.cpp`

### Context
Stages 1-4 are complete. This stage makes the RTC-lost-power condition visible rather than silent.

### Bug: RTC lost power silently undeploys

When `rtc.lostPower()` is true, `setup()` sets `rtcSynced=false`, `deployedFlag=false`, and persists. The node silently drops out of the fleet with no indication.

**Fix:** Add a persistent diagnostic flag:
```cpp
bool g_rtcPowerLost = false;  // set when RTC lost power, cleared on successful TIME_SYNC
```

In `setup()`, when `rtc.lostPower()`:
```cpp
if (rtc.lostPower()) {
  Serial.println("⚠️ RTC lost power since last run — node will need re-sync");
  g_rtcPowerLost = true;
  rtcSynced = false;
  // Do NOT clear deployedFlag — keep it so the node tries to re-sync
  // instead of silently going UNPAIRED. The node will request TIME_SYNC
  // in the loop and re-arm once time is restored.
  // Actually: keep the existing behavior of clearing deployedFlag for safety,
  // but set the diagnostic flag so it's visible.
  deployedFlag = false;
  lastTimeSyncUnix = 0;
  persistNodeConfig();
}
```

Add `g_rtcPowerLost` to `persistNodeConfig()` / `loadNodeConfig()`:
```cpp
// persist: p.putBool("rtc_lost", g_rtcPowerLost);
// load: g_rtcPowerLost = p.getBool("rtc_lost", false);
```

In the TIME_SYNC handler (now in the main loop from Stage 1), clear the flag:
```cpp
if (g_pendingTimeSync) {
  // ... apply time sync ...
  g_rtcPowerLost = false;
  persistNodeConfig();
}
```

In `sendNodeHello()` and `sendNodeStatusUpdate()`, include the flag:
```cpp
// In sendNodeStatusUpdate:
st.rescueMode = g_rescueModeActive ? 1 : 0;
// Add: st.rtcPowerLost = g_rtcPowerLost ? 1 : 0;  // if the protocol struct has room
// If no room in the struct, add it to the NODE_HELLO message instead
```

Note: if the protocol structs don't have a spare field for this, just log it prominently in serial output and skip the protocol change. The serial log is the primary diagnostic for now.

### Testing
No new test file needed — verify by code review and compile.

### Validation
1. `pio run -e esp32wroom` — must compile

### What NOT to change
- Do NOT modify `protocol.h` (unless adding a field to an existing struct with room)
- Do NOT change the `rtc.lostPower()` detection logic
- Do NOT prevent the node from going to UNPAIRED/PAIRED state — just make it visible

---

## Stage 6: CONFIG_SNAPSHOT re-arm trigger (Medium)

### Files to modify
- `node/firmware/src/main.cpp`

### Context
Stages 1-5 are complete. This is a one-line fix.

### Bug: CONFIG_SNAPSHOT handler doesn't trigger re-arm

When `CONFIG_SNAPSHOT` changes `g_intervalMin` or `g_syncIntervalMin`, the handler doesn't set `g_rearmAlarmsPending = true`. The node continues on the old schedule until the next wake fires.

**Fix:** In the CONFIG_SNAPSHOT handler (now in the main loop from Stage 1), after applying schedule changes:
```cpp
if (snap.wakeIntervalMin > 0 && snap.wakeIntervalMin != g_intervalMin) {
  g_intervalMin = snap.wakeIntervalMin;
  g_rearmAlarmsPending = true;  // ADD THIS
  Serial.printf("   ↪ wake interval updated: %u min\n", g_intervalMin);
}
if (snap.syncIntervalMin > 0) {
  g_syncIntervalMin = snap.syncIntervalMin;
  g_syncPhaseUnix   = snap.syncPhaseUnix;
  g_lastSyncSlot    = 0xFFFFFFFFUL;
  g_rearmAlarmsPending = true;  // ADD THIS
  Serial.printf("   ↪ sync schedule updated: %u min, phase=%lu\n", ...);
}
```

### Testing
No new test file needed — one-line fix, verify by compile.

### Validation
1. `pio run -e esp32wroom` — must compile

### What NOT to change
- Do NOT modify any other handler
- Do NOT change the CONFIG_SNAPSHOT ACK logic

---

## Stage 7: I2C scan gating (Medium)

### Files to modify
- `node/firmware/src/main.cpp`

### Context
Stages 1-6 are complete. This stage reduces boot time by gating the I2C scan.

### Bug: testI2CBusesMuxAndADS() runs on every boot

The I2C scan of 127 addresses runs on every boot, adding ~1-2 seconds. It runs before the alarm re-arm in `setup()`, so a bus hang delays re-arming.

**Fix:** Gate the scan behind a build flag:
```cpp
#ifdef NODE_I2C_SCAN_ON_BOOT
  testI2CBusesMuxAndADS();
#endif
```

In `platformio.ini`, do NOT add `-D NODE_I2C_SCAN_ON_BOOT` to the default build flags. Add it only to a debug build env if needed.

Alternatively, only run the scan on the first boot (persist a flag in NVS):
```cpp
Preferences p;
if (p.begin("node_cfg", true)) {
  bool scanned = p.getBool("i2c_scanned", false);
  p.end();
  if (!scanned) {
    testI2CBusesMuxAndADS();
    if (p.begin("node_cfg", false)) {
      p.putBool("i2c_scanned", true);
      p.end();
    }
  }
} else {
  testI2CBusesMuxAndADS();  // run if NVS read fails
}
```

Use the build-flag approach (simpler, no NVS change).

### Testing
No new test file needed.

### Validation
1. `pio run -e esp32wroom` — must compile (scan should NOT run by default)
2. `pio run -e esp32wroom -v` — verbose build, confirm the flag is not set

### What NOT to change
- Do NOT remove `testI2CBusesMuxAndADS()` — keep it for debugging
- Do NOT modify `platformio.ini` beyond adding the test envs from previous stages

---

## Stage 8: Low-priority cleanup (Low)

### Files to modify
- `node/firmware/src/main.cpp`
- `node/firmware/src/storage/local_queue.cpp`

### Context
Stages 1-7 are complete. This stage addresses the remaining low-priority findings.

### 8a: strcmp on potentially unterminated strings
In `onDataReceived()`, after `memcpy` into protocol structs, force null termination:
```cpp
discovery_response_t resp;
memcpy(&resp, incomingData, sizeof(resp));
resp.command[sizeof(resp.command) - 1] = '\0';  // ADD
```
Do this for ALL struct copies in `onDataReceived()`.

### 8b: loadNodeConfig() unsafe getString()
Replace `String macHex = p.getString("msmac", "");` with:
```cpp
char macHexBuf[16] = {0};
p.getString("msmac", macHexBuf, sizeof(macHexBuf) - 1);
String macHex = String(macHexBuf);
```

### 8c: g_intervalMin bounds check
In the SET_SCHEDULE handler (now in main loop from Stage 1):
```cpp
if (cmd.intervalMinutes > 0 && cmd.intervalMinutes <= 255) {
  g_intervalMin = (uint8_t)cmd.intervalMinutes;
} else {
  Serial.printf("⚠️ SET_SCHEDULE interval %d out of range, ignored\n", cmd.intervalMinutes);
}
```

### 8d: Remove dead code shouldSyncAt()
The function `shouldSyncAt()` (lines ~799-807) is never called. Remove it and its forward declaration. Also remove `nextSyncSlotUnix()` if it's only used by `shouldSyncAt()` (check first — it may be used elsewhere).

### 8e: CONFIG_SNAPSHOT ACK peer check
In the CONFIG_SNAPSHOT handler (now in main loop from Stage 1), replace raw `esp_now_send(mac, ...)` with `espnowSendWithRecover(mac, ...)` or at least add `ensureEspNowPeer(mac)` before sending.

### Testing
No new test files needed — these are minor code quality fixes.

### Validation
1. `pio run -e esp32wroom` — must compile
2. All previous test envs must still compile

### What NOT to change
- Do NOT change any protocol struct sizes or layouts
- Do NOT change the queue capacity or checksum algorithm
- Do NOT remove any functionality — only fix safety issues

---

## Final validation

After all 8 stages are complete, run:
```
cd node/firmware
pio run -e esp32wroom
```

This must compile cleanly. The node firmware is now robustness-hardened for unattended field operation.