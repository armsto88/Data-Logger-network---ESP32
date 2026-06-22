# Mothership V1 Robustness Fix — Implementation Prompts

**Date:** 2026-06-22
**Purpose:** Step-by-step prompts for a repo coding agent to implement Stages 3-10 of the robustness fix plan.

## Instructions

1. Read `mothership/docs/MOTHERSHIP_V1_ROBUSTNESS_FIX_PLAN.md` before starting any stage.
2. Execute ONE stage at a time. Do not skip ahead.
3. After each stage, verify compilation by running ALL the commands listed in that stage's "Validation" section.
4. Do not proceed to the next stage until the current stage compiles cleanly and the previous stages' tests still compile.
5. The system WORKS today. No change may regress the working sync flow. Every change is additive and defensive.
6. After completing all stages, run `pio run -e mothership-v1-main` one final time to confirm the full firmware compiles.

## Completed stages

- **Stage 1** ✅ — Alarm safety core (Bugs 2, 3, 4): write→verify→clear ordering, `verifyAlarmSet(DateTime)` exact read-back, past-alarm guard
- **Stage 2** ✅ — Rescue alarm at boot + bounded failure paths (Bugs 1, 7): `armRescueAlarm()` before wake detection, `boundedRetryAndShutdown()` replaces infinite loops

---

## Stage 3: RTC validity + wake reason safety (Bugs 5, 6)

### Files to modify
- `mothership/firmware/v1/src/time/rtc_alarm.h`
- `mothership/firmware/v1/src/time/rtc_alarm.cpp`
- `mothership/firmware/v1/src/system/wake_reason.h`
- `mothership/firmware/v1/src/system/wake_reason.cpp`
- `mothership/firmware/v1/src/main.cpp`

### Context
Stages 1 and 2 are already implemented:
- Stage 1: alarm functions write→verify→clear; `verifyAlarmSet(DateTime)` does exact read-back; past-alarm guard
- Stage 2: `armRescueAlarm()` arms fallback at boot before wake detection; `boundedRetryAndShutdown()` replaces infinite loops; `rtcAlarmPendingAtBoot` preserves RTC wake classification

### Bug 5: initRTC() returns true on invalid time

`initRTC()` logs `lostPower()` but still returns `true`. Scheduling proceeds with garbage time (year 2000).

**Fix:** Add to `rtc_alarm.h`:
```cpp
enum RtcInitStatus { RTC_OK, RTC_PRESENT_TIME_INVALID, RTC_ABSENT };
RtcInitStatus initRTC();
bool rtcTimeValid();  // !lostPower() && year >= 2024
```

Update `initRTC()` in `rtc_alarm.cpp`:
- Return `RTC_ABSENT` if `gRTC.begin()` fails
- Return `RTC_PRESENT_TIME_INVALID` if `gRTC.lostPower()` is true OR current year < 2024
- Return `RTC_OK` if present and time valid
- Keep all existing Serial output

**Backward compatibility:** `setup()` in `main.cpp` currently calls `initRTC()` and checks the return as bool. Since `RTC_OK = 0`, `if (!initRTC())` would be true on success — that's backwards. Update ALL call sites in `main.cpp` to use the enum:
- In `setup()`: `RtcInitStatus rtcStatus = initRTC();` — if `RTC_OK`, arm rescue alarm (existing Stage 2 logic). If `RTC_PRESENT_TIME_INVALID`, still arm rescue alarm (it uses `now + interval` which is garbage but the rescue is a safety net — the board will wake and retry). If `RTC_ABSENT`, skip rescue arm (no RTC to arm against).
- In `handleSyncWake()`: if `initRTC()` returns `RTC_PRESENT_TIME_INVALID` or `RTC_ABSENT`, do NOT proceed with scheduling. Log a fault, call `boundedRetryAndShutdown("RTC time invalid")` (from Stage 2), and return. Do not power off without a rescue alarm if RTC is present but time is invalid — the rescue alarm from `setup()` is already armed, so `boundedRetryAndShutdown` will re-arm it.
- In `handleConfigWake()`: still proceed even with invalid time — the user needs to set the clock. Log a warning.

Add `rtcTimeValid()` to `rtc_alarm.cpp`:
```cpp
bool rtcTimeValid() {
  if (!gRTCInitialized) return false;
  if (gRTC.lostPower()) return false;
  DateTime now = gRTC.now();
  return now.year() >= 2024;
}
```

### Bug 6: RTC status read failure misclassified as USB service

In `wake_reason.cpp`, `detectWakeSources()` reads the RTC alarm flag. If the I2C read fails, `rtcStatusRead=false`, `rtcAlarm=false`, and `selectWakeReason()` returns `WAKE_USB_SERVICE` — which immediately releases PWR_HOLD without arming anything.

**Fix:** Add `WAKE_UNKNOWN` to the `WakeReason` enum in `wake_reason.h`:
```cpp
enum WakeReason {
  WAKE_RTC_ALARM,
  WAKE_CONFIG_BUTTON,
  WAKE_USB_SERVICE,
  WAKE_UNKNOWN  // NEW
};
```

Update `selectWakeReason()` in `wake_reason.cpp`:
```cpp
WakeReason selectWakeReason(const WakeSources& sources) {
  if (sources.configRequested) return WAKE_CONFIG_BUTTON;
  if (sources.rtcAlarm) return WAKE_RTC_ALARM;
  if (!sources.rtcStatusRead) return WAKE_UNKNOWN;  // NEW: I2C read failed
  return WAKE_USB_SERVICE;
}
```

Update `setup()` in `main.cpp` switch statement to handle `WAKE_UNKNOWN`:
```cpp
case WAKE_UNKNOWN:
  Serial.println("[WAKE] Unknown wake reason (RTC read failed) — arming rescue and shutting down");
  boundedRetryAndShutdown("WAKE_UNKNOWN: RTC status read failed");
  break;
```

Also add `WAKE_UNKNOWN` to the `printWakeReason()` function in `wake_reason.cpp` with a descriptive string.

### Testing
Create `mothership/firmware/v1/tests/bringup_rtc_validity.cpp` with env `[env:mothership-v1-rtc-validity]` in `platformio.ini` (`build_src_filter = -<*> +<tests/bringup_rtc_validity.cpp>`, `lib_deps = adafruit/RTClib@^1.14.1`).

The test should:
1. Assert PWR_HOLD HIGH first
2. Call `initRTC()`, print the `RtcInitStatus` enum value
3. Call `rtcTimeValid()`, print result
4. If time is valid: arm a test alarm, verify, print PASS
5. If time is invalid: print "RTC time invalid — would skip scheduling in sync wake" (PASS — correct behavior)
6. Test `WAKE_UNKNOWN` path: simulate by not initializing I2C, call `detectWakeSources()`, confirm `rtcStatusRead=false`
7. Print clear PASS/FAIL
8. Loop: idle heartbeat

### Validation
1. `pio run -e mothership-v1-main` — must compile
2. `pio run -e mothership-v1-rtc-validity` — must compile
3. `pio run -e mothership-v1-rtc-alarm` — existing test must still compile
4. `pio run -e mothership-v1-rescue-alarm` — Stage 2 test must still compile

### What NOT to change
- Do NOT modify `config_server.cpp`, `espnow_sync.cpp`, `upload_queue.cpp`, `flash_logger.cpp`
- Do NOT change `handleConfigWake()` behavior when RTC is invalid — config mode must still work so the user can set the clock
- Do NOT change `armRescueAlarm()` — it already handles garbage time gracefully

---

## Stage 4: Flash/filesystem safety (Bugs 13, 14)

### Files to modify
- `mothership/firmware/v1/src/storage/flash_logger.h`
- `mothership/firmware/v1/src/storage/flash_logger.cpp`

### Context
Stages 1-3 are already implemented. This stage is independent — it only touches flash_logger.

### Bug 13: LittleFS.begin(true) formats on failure

`initFlash()` calls `LittleFS.begin(true)` with `formatOnFail=true`. A transient mount failure erases all data.

**Fix:** Change `initFlash()` in `flash_logger.cpp`:
- Use `LittleFS.begin(false)` (no auto-format)
- On mount failure: set `gFlashReady = false`, set a new `gFlashMountFailed = true` flag
- Do NOT format
- Add `bool flashMountFailed()` accessor to `flash_logger.h`
- Add `bool flashFormatExplicit()` to `flash_logger.h`/`.cpp` — explicitly formats LittleFS and recreates the CSV header. For future web-UI recovery. Should: `LittleFS.format()`, then `LittleFS.begin(false)`, then `flashCreateCSVHeader()`, set `gFlashReady = true`, `gFlashMountFailed = false`, return true/false.

### Bug 14: Flash append success not checked

`flashLogCSVRow()` returns `true` if the file opened, regardless of whether `println()` wrote all bytes.

**Fix:** Update `flashLogCSVRow()` in `flash_logger.cpp`:
```cpp
bool flashLogCSVRow(const String& row) {
  if (!gFlashReady) return false;
  File f = LittleFS.open(kFlashFile, "a");
  if (!f) {
    Serial.println("[FLASH] Failed to open datalog.csv for append");
    return false;
  }
  size_t written = f.println(row);
  bool writeError = f.getWriteError();
  f.close();
  if (writeError || written != row.length() + 1) {
    Serial.printf("[FLASH] Write failed: wrote %u of %u bytes, error=%d\n",
                  (unsigned)written, (unsigned)(row.length() + 1), writeError);
    return false;
  }
  return true;
}
```

`logSnapshotRow()` delegates to `flashLogCSVRow()` and inherits the fix.

### Testing
Create `mothership/firmware/v1/tests/bringup_flash_write_check.cpp` with env `[env:mothership-v1-flash-write-check]`:
1. Assert PWR_HOLD HIGH first
2. Init flash
3. Write test rows in a loop until `flashLogCSVRow()` returns false (flash full)
4. Print how many rows succeeded before failure
5. Confirm the failure is logged with `[FLASH] Write failed` message
6. Print PASS if the function correctly returns false on full flash
7. Loop: idle heartbeat

### Validation
1. `pio run -e mothership-v1-main` — must compile
2. `pio run -e mothership-v1-flash-write-check` — must compile
3. `pio run -e mothership-v1-flash-purge` — existing test must still compile

### What NOT to change
- Do NOT modify `main.cpp`, `config_server.cpp`, `upload_queue.cpp`, or any other file
- Do NOT change the CSV header format or file path
- Do NOT add a web-UI format button yet — that's Stage 10

---

## Stage 5: ESP-NOW callback queue (Bug 8) — HIGHEST RISK

### Files to modify
- `mothership/firmware/v1/src/comms/espnow_sync.h`
- `mothership/firmware/v1/src/comms/espnow_sync.cpp`
- `mothership/firmware/v1/src/main.cpp`

### Context
Stages 1-4 are already implemented. This stage is the highest-risk change because it restructures the ESP-NOW receive path. The system has been working in practice because the sync window and upload/purge don't overlap in the current code flow. This stage makes the concurrency safety explicit and defensive.

### Bug 8: ESP-NOW callback writes LittleFS from Wi-Fi task

`onEspNowData()` in `main.cpp` runs on the ESP-NOW/Wi-Fi task. It calls `logSnapshotRow()` (LittleFS write) and iterates `registeredNodes`. Concurrent filesystem access from the main task during purge can corrupt data.

**Fix:** Move all filesystem and registry operations off the callback onto the main task using a FreeRTOS queue.

#### Changes to `espnow_sync.h`:
Add:
```cpp
#include "protocol.h"  // node_snapshot_t

struct EspNowSnapSlot {
  uint8_t mac[6];
  node_snapshot_t snap;
};

void initSnapQueue(int depth);
int drainSnapQueue(EspNowSnapSlot* outSlots, int maxSlots);
uint32_t getSnapDropCount();
void deinitEspNowSync();
```

#### Changes to `espnow_sync.cpp`:
Add a static FreeRTOS queue:
```cpp
static QueueHandle_t gSnapQueue = nullptr;
static int gSnapQueueDepth = 8;
static uint32_t gSnapDropCount = 0;
```

Update the internal `onEspNowRecv()`:
- Check if data is a `node_snapshot_t` (`len >= sizeof(node_snapshot_t)` and `command == "NODE_SNAPSHOT"`)
- If yes: copy snapshot + MAC into `EspNowSnapSlot`, `xQueueSendToBack(gSnapQueue, &slot, 0)` (non-blocking)
- If queue full: increment `gSnapDropCount`, log throttled warning
- If not a snapshot: ignore (legacy `sensor_data_message_t` path dropped — all nodes send `node_snapshot_t` now)
- **NO LittleFS access, NO `registeredNodes` iteration, NO Serial.printf in the callback**

Add `initSnapQueue(int depth)`, `drainSnapQueue(EspNowSnapSlot*, int)`, `getSnapDropCount()`, `deinitEspNowSync()`.

`deinitEspNowSync()` should: `esp_now_unregister_recv_cb()`, `esp_now_deinit()`, delete the queue, log.

#### Changes to `main.cpp`:
- Remove `registerReceiveCallback(onEspNowData)` — the callback is now internal to espnow_sync.cpp
- After `initEspNowSyncOnly(ESPNOW_CHANNEL)`: call `initSnapQueue(8)`
- In the sync window loop, replace `espnowSyncLoop()` with queue drain:
```cpp
EspNowSnapSlot slots[4];
int drained = drainSnapQueue(slots, 4);
for (int i = 0; i < drained; i++) {
  processSnapshot(&slots[i].snap, slots[i].mac);
}
```
- Move snapshot processing from old `onEspNowData()` into a new `processSnapshot(const node_snapshot_t* snap, const uint8_t* mac)` function — same logic (log to flash, update registeredNodes) but now runs on main task
- Before `performModemUpload`: call `deinitEspNowSync()` to stop receiving
- Drop the legacy `sensor_data_message_t` path — add a comment noting this

### Testing
Create `mothership/firmware/v1/tests/bringup_espnow_queue.cpp` with env `[env:mothership-v1-espnow-queue]`:
1. Assert PWR_HOLD HIGH first
2. Init flash, init ESP-NOW sync, init snap queue
3. Simulate receiving snapshots by manually enqueuing test slots
4. Call `drainSnapQueue()`, confirm all slots received
5. Confirm drop counter works (overflow the queue, check `getSnapDropCount()`)
6. Call `deinitEspNowSync()`, confirm cleanup
7. Print PASS/FAIL
8. Loop: idle heartbeat

### Validation
1. `pio run -e mothership-v1-main` — must compile
2. `pio run -e mothership-v1-espnow-queue` — must compile
3. `pio run -e mothership-v1-espnow-basic` — existing test must still compile

### What NOT to change
- Do NOT modify `rtc_alarm.cpp`, `wake_reason.cpp`, `upload_queue.cpp`, `flash_logger.cpp`, `config_server.cpp`
- Do NOT change `broadcastSyncWindowOpen()` or `protocol.h`
- Keep `EspNowRecvCallback` typedef and `registerReceiveCallback()` for backward compat with existing tests

---

## Stage 6: Atomic purge + cursor validation (Bugs 11, 12)

### Files to modify
- `mothership/firmware/v1/src/storage/upload_queue.h`
- `mothership/firmware/v1/src/storage/upload_queue.cpp`
- `mothership/firmware/v1/src/storage/flash_logger.cpp` (boot recovery only)

### Context
Stages 1-5 are already implemented. This stage makes the file purge operation power-safe and validates cursor row boundaries.

### Bug 11: Purge remove-then-rename not atomic

`purgeUploaded()` and `emergencyPurgeIfFull()` do `LittleFS.remove("/datalog.csv")` then `LittleFS.rename("/datalog_tmp.csv", "/datalog.csv")`. If power dies between remove and rename, the file doesn't exist. All data lost.

**Fix:** Change both purge functions to use a backup-then-swap pattern:
1. Rename old `datalog.csv` → `datalog_bak.csv` (if it exists)
2. Rename `datalog_tmp.csv` → `datalog.csv`
3. Remove `datalog_bak.csv`

Add `recoverDataFile()` called from `uploadQueue.init()`:
- If `datalog.csv` missing but `datalog_bak.csv` exists: rename bak → real
- If both `datalog.csv` and `datalog_tmp.csv` exist: prefer `datalog.csv`, remove tmp
- If only `datalog_tmp.csv` exists: rename to real
- If only `datalog_bak.csv` exists: rename to real
- Log each recovery action

### Bug 12: validateCursor() doesn't check row boundary

`validateCursor()` checks file bounds but not whether the byte before the cursor is `\n`. A partial-row cursor could exist after a crash during purge.

**Fix:** Update `validateCursor()`:
- After bounds check, open file, seek to `offset - 1`
- Confirm byte is `\n` (or `offset == headerEndOffset()` — the first data row)
- If not at a row boundary: scan forward to next `\n` and set cursor there
- If no `\n` found before EOF: reset to header end
- Save corrected cursor to NVS

### Testing
Create `mothership/firmware/v1/tests/bringup_atomic_purge.cpp` with env `[env:mothership-v1-atomic-purge]`:
1. Assert PWR_HOLD HIGH first
2. Init flash, init upload queue
3. Write test rows to `datalog.csv`
4. Trigger `purgeUploaded()` — confirm backup-then-swap works
5. Simulate power loss: after rename but before remove of backup, manually create a scenario where both `datalog.csv` and `datalog_bak.csv` exist
6. Re-init upload queue — confirm `recoverDataFile()` picks the valid file
7. Test cursor validation: manually set cursor to a mid-row offset, call `validateCursor()`, confirm it scans to next `\n`
8. Print PASS/FAIL
9. Loop: idle heartbeat

### Validation
1. `pio run -e mothership-v1-main` — must compile
2. `pio run -e mothership-v1-atomic-purge` — must compile
3. `pio run -e mothership-v1-flash-purge` — existing test must still compile
4. `pio run -e mothership-v1-upload-cursor` — existing test must still compile

### What NOT to change
- Do NOT modify `main.cpp`, `config_server.cpp`, `espnow_sync.cpp`, `rtc_alarm.cpp`
- Do NOT change the CSV header format or the `getNewData()` function (that's Stage 7)

---

## Stage 7: Chunked upload payload (Bug 9)

### Files to modify
- `mothership/firmware/v1/src/storage/upload_queue.h`
- `mothership/firmware/v1/src/storage/upload_queue.cpp`

### Context
Stages 1-6 are already implemented. This stage fixes the OOM and row-splitting bugs in `getNewData()`.

### Bug 9: 256 KB String OOM + row-split + byteLength tracking

`getNewData()` builds a String up to 256 KB by appending one char at a time. On ESP32 with ~320 KB heap (less after WiFi/modem/LittleFS), this can OOM. Append failures are unchecked but `byteLength` still increments. The row-boundary logic is wrong (`hitNewline` means "any newline seen" not "payload ends at newline").

**Fix:** Rewrite `getNewData()`:
- Check `ESP.getFreeHeap()` before allocation; if < 2 × maxBytes, reject with empty payload and log `[UPLOAD] Insufficient heap for payload`
- Use `payload.csvData.reserve(maxBytes + 256)` once, then `concat(buf, n)` in chunks instead of char-by-char
- Read in 4 KB chunks into a `uint8_t buf[4096]` buffer, then `payload.csvData.concat((const char*)buf, n)`
- Compute exact complete-row `endOffset`: read until `bytesRead >= maxBytes` AND last byte is `\n`; if mid-row, continue reading to next `\n`
- Set `payload.byteLength = bytesRead` where bytesRead is the exact number of file bytes consumed (ending on `\n`)
- If a `concat()` call fails (String didn't grow), stop reading, log `[UPLOAD] String allocation failed`, set `payload.byteLength = 0` (don't return partial data)

### Testing
Extend `bringup_upload_cursor.cpp` OR create `bringup_upload_chunk.cpp` with env `[env:mothership-v1-upload-chunk]`:
1. Assert PWR_HOLD HIGH first
2. Init flash, init upload queue
3. Write 300 KB of test rows to flash
4. Call `getNewData(262144)` (256 KB)
5. Confirm payload ends with `\n`
6. Confirm `payload.byteLength` matches actual file bytes consumed
7. Confirm `payload.rowEstimate` is reasonable
8. Confirm no OOM (check `ESP.getFreeHeap()` before and after)
9. Print PASS/FAIL
10. Loop: idle heartbeat

### Validation
1. `pio run -e mothership-v1-main` — must compile
2. `pio run -e mothership-v1-upload-chunk` (or extended cursor test) — must compile
3. `pio run -e mothership-v1-upload-cursor` — existing test must still compile

### What NOT to change
- Do NOT modify `main.cpp`, `config_server.cpp`, `espnow_sync.cpp`, `rtc_alarm.cpp`, `flash_logger.cpp`
- Do NOT change `advanceCursor()` or `purgeUploaded()` — those are correct already
- Do NOT change the `UploadPayload` struct definition

---

## Stage 8: NVS phase-anchor integrity (Bug 15)

### Files to modify
- `mothership/firmware/v1/src/config/config_server.cpp`

### Context
Stages 1-7 are already implemented. This stage makes the phase-anchor NVS storage redundant and checksummed.

### Bug 15: Phase-anchor NVS writes unchecked and non-redundant

`saveSyncRuntimeGuardsToNVS()` ignores all `put` return values. No checksum, no read-back. If NVS write fails, `gLastSyncBroadcastUnix` reads as 0 on next boot, creating a new phase grid. All deployed nodes on old grid → permanent desync.

**Fix:** Define a packed struct in `config_server.cpp`:
```cpp
struct SyncAnchorRecord {
  uint32_t magic;       // 0x53594E43 = "SYNC"
  uint16_t version;     // 1
  uint16_t generation;   // alternates 0/1 between copies
  uint32_t phaseUnix;    // gLastSyncBroadcastUnix
  uint16_t intervalMin;  // gSyncIntervalMin
  uint8_t  mode;         // gSyncMode
  uint8_t  reserved;
  uint32_t crc;          // CRC32 of all fields above (excluding crc itself)
};
```

Persist two copies in NVS keys `sync_anchor_a` / `sync_anchor_b` (alternating by generation).

Update `saveSyncRuntimeGuardsToNVS()`:
- Build a `SyncAnchorRecord` from current globals
- Compute CRC32 (use `esp_crc32_le()` from ESP-IDF or a simple CRC32 implementation)
- Write to the next copy (alternate A/B by generation)
- Read back and verify CRC; on mismatch, retry once
- Log the write result

Update `loadSyncRuntimeGuardsFromNVS()`:
- Read both copies
- For each: check magic, verify CRC, validate phase is plausible (year >= 2024, interval in allowed set)
- Pick the one with valid magic + CRC + plausible phase
- If both valid: pick the one with higher generation (newer)
- If both invalid: fall back to legacy `sync_last_unix` key (read with `getULong("sync_last_unix", 0)`)
- If legacy also 0: set `gLastSyncBroadcastUnix = 0` and log `[WARN] Phase anchor lost — fleet may desync`
- **Migration:** on first load with new keys absent, read legacy `sync_last_unix` and write it into anchor A

### Testing
Create `mothership/firmware/v1/tests/bringup_nvs_anchor.cpp` with env `[env:mothership-v1-nvs-anchor]`:
1. Assert PWR_HOLD HIGH first
2. Write a valid anchor to NVS
3. Read it back, confirm CRC and values match
4. Corrupt one copy (write garbage to `sync_anchor_a`)
5. Load — confirm it picks the valid copy B
6. Corrupt both copies
7. Load — confirm fallback to legacy `sync_last_unix`
8. Print PASS/FAIL
9. Loop: idle heartbeat

### Validation
1. `pio run -e mothership-v1-main` — must compile
2. `pio run -e mothership-v1-nvs-anchor` — must compile
3. `pio run -e mothership-v1-config-latch` — existing test must still compile

### What NOT to change
- Do NOT modify `main.cpp`, `rtc_alarm.cpp`, `espnow_sync.cpp`, `upload_queue.cpp`, `flash_logger.cpp`
- Do NOT change the `gLastSyncBroadcastUnix` global variable name or type
- Do NOT change the existing `loadSyncRuntimeGuardsFromNVS()` / `saveSyncRuntimeGuardsToNVS()` function signatures — update their internals only

---

## Stage 9: Upload retry reset (Bug 10)

### Files to modify
- `mothership/firmware/v1/src/storage/upload_queue.h`
- `mothership/firmware/v1/src/storage/upload_queue.cpp`
- `mothership/firmware/v1/src/main.cpp`

### Context
Stages 1-8 are already implemented. This stage fixes the permanent upload lockout.

### Bug 10: Permanent upload lockout after 3 retries

`retryCount` persists in NVS and only resets on successful upload. After 3 failures across 3 sync wakes, `maxRetriesExceeded(3)` returns true forever. Uploads are permanently disabled.

**Fix:** Replace the permanent latch with a time-based cooldown:

Add `nextAttemptUnix` to the `UploadCursor` struct in `upload_queue.h`:
```cpp
struct UploadCursor {
  uint32_t byteOffset;
  uint32_t rowsUploaded;
  uint32_t lastUploadUnix;
  uint8_t  retryCount;
  uint32_t wakeCounter;
  uint32_t nextAttemptUnix;  // NEW: earliest time to retry after failure
};
```

Update `maxRetriesExceeded()`:
```cpp
bool UploadQueue::maxRetriesExceeded(uint8_t maxRetries) const {
  // If cooldown has elapsed, allow retry regardless of retry count
  if (m_cursor.nextAttemptUnix > 0) {
    uint32_t nowUnix = getRTCTime();  // or accept a timestamp parameter
    if (nowUnix >= m_cursor.nextAttemptUnix) {
      return false;  // cooldown elapsed — allow attempt
    }
  }
  return m_cursor.retryCount >= maxRetries;
}
```

Note: `getRTCTime()` is in `rtc_alarm.h`. To avoid a hard dependency, accept a `uint32_t nowUnix` parameter instead:
```cpp
bool maxRetriesExceeded(uint8_t maxRetries, uint32_t nowUnix) const;
```

Update `incrementRetryCount()`:
```cpp
void UploadQueue::incrementRetryCount() {
  m_cursor.retryCount++;
  // Set cooldown to one sync interval from now (caller should pass it)
  // Or use a default cooldown of 60 minutes
  m_cursor.nextAttemptUnix = m_cursor.lastUploadUnix + 3600;  // 1 hour cooldown
  saveCursor();
}
```

Actually, the cleanest approach: add `incrementRetryCount(uint32_t nowUnix, uint32_t cooldownSec)`:
```cpp
void incrementRetryCount(uint32_t nowUnix, uint32_t cooldownSec) {
  m_cursor.retryCount++;
  m_cursor.nextAttemptUnix = nowUnix + cooldownSec;
  saveCursor();
}
```

Update `resetRetryCount()`:
```cpp
void UploadQueue::resetRetryCount() {
  m_cursor.retryCount = 0;
  m_cursor.nextAttemptUnix = 0;
  saveCursor();
}
```

Update `main.cpp` `performModemUpload()`:
- Change `uploadQueue.incrementRetryCount()` to `uploadQueue.incrementRetryCount(nowUnix, txSettings.uploadIntervalMin * 60)` (cooldown = one upload interval)
- Change `uploadQueue.maxRetriesExceeded(txSettings.maxRetriesPerWindow)` to `uploadQueue.maxRetriesExceeded(txSettings.maxRetriesPerWindow, getRTCTime())`

Add `nextAttemptUnix` to NVS save/load in `upload_queue.cpp`:
- `prefs.putUInt("next_attempt_unix", m_cursor.nextAttemptUnix)`
- `m_cursor.nextAttemptUnix = prefs.getUInt("next_attempt_unix", 0)`

### Testing
Extend `bringup_upload_cursor.cpp`:
1. Simulate 3 failures (call `incrementRetryCount` 3 times with timestamps)
2. Confirm `maxRetriesExceeded(3, nowUnix)` returns true (locked out)
3. Advance time past cooldown
4. Confirm `maxRetriesExceeded(3, advancedTime)` returns false (cooldown elapsed — retry allowed)
5. Call `resetRetryCount()`, confirm retryCount = 0 and nextAttemptUnix = 0
6. Print PASS/FAIL

### Validation
1. `pio run -e mothership-v1-main` — must compile
2. `pio run -e mothership-v1-upload-cursor` — must compile

### What NOT to change
- Do NOT modify `config_server.cpp`, `espnow_sync.cpp`, `rtc_alarm.cpp`, `flash_logger.cpp`
- Do NOT change the `UploadPayload` struct
- Do NOT remove the `retryCount` field — keep it for diagnostics

---

## Stage 10: Medium findings (watchdog, daily mode, string safety, strncpy)

### Files to modify
- `mothership/firmware/v1/src/main.cpp`
- `mothership/firmware/v1/src/config/config_server.cpp`
- `mothership/firmware/v1/src/config/transmission_settings.cpp`
- `mothership/firmware/v1/src/comms/espnow_sync.cpp` (legacy parse only, if still present)

### Context
Stages 1-9 are already implemented. This stage addresses the remaining medium-priority findings.

### Session watchdog
In `handleSyncWake()` in `main.cpp`, capture `bootMs = millis()` at entry. Before each major phase (ESP-NOW listen, upload, alarm arm), check:
```cpp
if (millis() - bootMs > 180000) {  // 3 minutes total session limit
  Serial.println("[WATCHDOG] Session timeout — forcing shutdown");
  // Force modem shutdown if active
  // Skip remaining work
  // Go straight to alarm re-arm
  break;  // or set a flag and jump to alarm re-arm
}
```

Add the check before the sync window loop, before `performModemUpload()`, and before the alarm re-arm. If the watchdog triggers during upload, force `modem.gracefulShutdown()` before proceeding to alarm re-arm.

### Daily mode in sync wake
In `handleSyncWake()` in `main.cpp`, after loading sync interval from NVS, also load sync mode:
```cpp
loadSyncModeFromNVS();
loadDailySyncTimeFromNVS();
```

Before the alarm re-arm at the end, branch:
```cpp
if (gSyncMode == SYNC_MODE_DAILY) {
  if (!armDailyAlarm(gSyncDailyHour, gSyncDailyMinute)) {
    Serial.println("[FATAL] Failed to arm daily alarm! Staying on.");
    boundedRetryAndShutdown("Daily alarm arm failed");
    return;
  }
} else {
  // Existing interval-mode alarm arming
  int syncInterval = (gSyncIntervalMin > 0) ? gSyncIntervalMin : DEFAULT_SYNC_INTERVAL_MIN;
  loadSyncRuntimeGuardsFromNVS();
  uint32_t phaseUnix = (uint32_t)gLastSyncBroadcastUnix;
  if (!armNextSyncAlarmPhase(syncInterval, phaseUnix)) {
    boundedRetryAndShutdown("Sync alarm arm failed");
    return;
  }
}
```

Also verify the alarm after either branch:
```cpp
if (!verifyAlarmSet()) {
  boundedRetryAndShutdown("Alarm verification failed");
  return;
}
```

### Transmission settings string safety
In `transmission_settings.cpp`, replace `prefs.getString("url", String(""))` etc. with fixed-buffer reads. Use a char buffer:
```cpp
char buf[256];
size_t len = prefs.getString("url", buf, sizeof(buf) - 1);
if (len > 0) buf[len] = '\0';
s.endpointUrl = String(buf);
```

Do this for all string fields: `url`, `token`, `site_id`, `deploy_id`. Size buffers to 256 (URLs can be long).

### Legacy ESP-NOW parsing strncpy safety
If the legacy `sensor_data_message_t` path still exists in `main.cpp` (it may have been removed in Stage 5), ensure:
- The `NodeSnapshot` struct is zero-initialized: `NodeSnapshot snap = {};`
- After `strncpy(snap.nodeId, msg->nodeId, sizeof(snap.nodeId) - 1)`, force: `snap.nodeId[sizeof(snap.nodeId) - 1] = '\0'`
- Same for `sensorType` and `sensorLabel`

If Stage 5 removed the legacy path entirely, skip this sub-task.

### Testing
- Extend `bringup_modem_powercycle.cpp` or create a new test to verify the session watchdog doesn't trigger prematurely (a normal power cycle completes in ~14s, well under 180s)
- Extend `bringup_config_wake_test.cpp` to verify daily mode: set daily mode via NVS, trigger a sync wake, confirm `armDailyAlarm` is called (check serial output for "Daily alarm armed")

### Validation
1. `pio run -e mothership-v1-main` — must compile
2. `pio run -e mothership-v1-modem-powercycle` — must compile
3. `pio run -e mothership-v1-config-wake-test` — must compile
4. Run `pio run -e mothership-v1-main` one final time — full firmware with all 10 stages must compile cleanly

### What NOT to change
- Do NOT modify `rtc_alarm.cpp`, `wake_reason.cpp`, `upload_queue.cpp`, `flash_logger.cpp` (unless the legacy strncpy fix requires a change in main.cpp's callback path)
- Do NOT change the WiFi AP password, SSID, or web server port
- Do NOT add new web UI routes — the session watchdog and daily mode fix are internal logic only

---

## Final validation

After all 10 stages are complete, run:
```
cd mothership/firmware/v1
pio run -e mothership-v1-main
```

This must compile cleanly. The firmware is now robustness-hardened for unattended field operation.