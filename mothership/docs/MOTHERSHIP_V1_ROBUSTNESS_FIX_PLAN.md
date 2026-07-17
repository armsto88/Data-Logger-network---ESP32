# Mothership V1 Robustness Fix Plan

**Status:** Proposed
**Date:** 2026-06-21
**Scope:** Non-breaking, staged hardening of Mothership V1 firmware against 15 critical/medium bugs identified in two independent robustness reviews.

## Consolidated execution record

**Execution prompts created:** 2026-06-22

**Merged into this plan:** 2026-07-17

The former `MOTHERSHIP_V1_ROBUSTNESS_FIX_PROMPTS.md` expanded Stages 3-10
below into individual coding-agent prompts. At the time that prompt set was
written, Stages 1 and 2 were recorded as completed and Stages 3-10 remained the
ordered execution sequence. Its durable instructions were:

1. Implement one stage at a time and do not skip ahead.
2. Run every validation command for that stage before continuing.
3. Keep prior-stage and existing bring-up targets compiling.
4. Make only the files and behaviours named by the active stage.
5. Preserve the already working autonomous sync flow; changes are additive and
   defensive.
6. Finish with `pio run -e mothership-v1-main` from
   `mothership/firmware/v1/`.

The stage-specific files, fixes, tests, validation targets, risks, and scope
boundaries from those prompts are retained in the corresponding sections of
this plan. The 2026-06-22 completion labels are historical status, not a claim
about which code is currently flashed in deployed hardware.

## Critical constraint

The system WORKS today. It has been validated across 13 bring-up tests and full autonomous sync cycles (node -> ESP-NOW -> mothership -> flash log -> power off -> repeat). **No stage may regress the working sync flow.** Every change is additive and defensive. Each stage compiles independently (`pio run -e mothership-v1-main`) and is testable on hardware without breaking existing bring-up tests 1-13.

## Bug inventory and staging

| Stage | Bug(s) | Group | Files | Risk |
|-------|--------|-------|-------|------|
| 0 | (design) | - | `mothership/docs/` | - |
| 1 | 2, 3, 4 | A | `time/rtc_alarm.{h,cpp}` | low |
| 2 | 1, 7 | A | `time/rtc_alarm.{h,cpp}`, `main.cpp` | medium |
| 3 | 5, 6 | A | `time/rtc_alarm.{h,cpp}`, `system/wake_reason.{h,cpp}`, `main.cpp` | low-medium |
| 4 | 13, 14 | D | `storage/flash_logger.{h,cpp}` | low |
| 5 | 8 | B | `comms/espnow_sync.{h,cpp}`, `main.cpp` | **highest** |
| 6 | 11, 12 | C | `storage/upload_queue.{h,cpp}`, `storage/flash_logger.cpp` | medium |
| 7 | 9 | C | `storage/upload_queue.{h,cpp}` | low-medium |
| 8 | 15 | E | `config/config_server.cpp` | medium |
| 9 | 10 | C | `storage/upload_queue.{h,cpp}`, `main.cpp` | low |
| 10 | watchdog, daily mode, string safety, strncpy | medium | `main.cpp`, `config/config_server.cpp`, `config/transmission_settings.cpp`, `comms/espnow_sync.cpp` | low |

Stages 1, 5, 6, 8 block on the Stage 0 design addendum. Stages 2, 3, 4, 7, 9, 10 may proceed in parallel with Stage 0.

## Stage 0 - Design review (Designer)

Produce a short addendum covering four decisions Coder needs before implementing the higher-risk stages:

1. **Rescue alarm semantics** - confirm the arm -> verify -> clear ordering and that the rescue alarm uses `A1M4=1` (any-day) with `DEFAULT_SYNC_INTERVAL_MIN`.
2. **Atomic purge recovery state machine** - which of `{datalog.csv, datalog_tmp.csv, datalog_bak.csv}` wins on boot for each combination of present/absent files.
3. **Dual-copy NVS anchor record layout** - magic, version, generation, phase, interval, mode, CRC; and the migration path from legacy `sync_last_unix`.
4. **ESP-NOW queue contract** - queue depth, slot size, drop policy when full, and the deinit-before-upload guarantee.

## Stage 1 - Alarm safety core (Bugs 2, 3, 4)

**Files:** `src/time/rtc_alarm.{h,cpp}`

### Bug 2 - Clear alarm flag before writing new alarm
In `armNextSyncAlarm`, `armNextSyncAlarmPhase`, and `armDailyAlarm`: move `clearAlarmFlag()` to **after** `writeAlarm1Exact()` succeeds. Insert a `verifyAlarmSet()` call between the write and the clear; if verification fails, do not clear the flag and return `false`.

### Bug 3 - verifyAlarmSet doesn't verify the actual alarm time
Rewrite `verifyAlarmSet()` to accept the expected `DateTime` (or the 4 raw bytes written), read back all 4 alarm registers, and compare exact bytes (BCD values and mask bits). New signature:
```cpp
bool verifyAlarmSet(const DateTime& expected);
```
Keep a no-arg overload that only checks control bits for backward compatibility with existing `bringup_rtc_alarm.cpp`.

### Bug 4 - No guard against past alarms
In `armNextSyncAlarmPhase`, after pre-wake subtraction, if `nextSyncUnix <= nowUnix + 5`, advance by one more `periodSec`. Apply the same guard in `armNextSyncAlarm`.

### Test
New `tests/bringup_alarm_verify.cpp`:
- arm an alarm, corrupt one register, confirm `verifyAlarmSet` fails;
- arm a near-past time, confirm it advances one period.

### Validation
`pio run -e mothership-v1-main`; re-run `bringup_rtc_alarm.cpp`.

## Stage 2 - Rescue alarm at boot + bounded failure paths (Bugs 1, 7)

**Files:** `src/time/rtc_alarm.{h,cpp}`, `src/main.cpp`

### Bug 1 - No rescue alarm at boot
Add:
```cpp
bool armRescueAlarm(int intervalMin);
```
Arms Alarm 1 for `now + intervalMin` with `A1M4=1`, verifies, returns bool. Conservative default: `DEFAULT_SYNC_INTERVAL_MIN`.

In `setup()`, immediately after `assertPwrHold()` and `Serial.begin`:
1. call `initRTC()`;
2. if it succeeds, call `armRescueAlarm(DEFAULT_SYNC_INTERVAL_MIN)` and log the result;
3. then proceed to the existing `detectWakeSources()` / `clearAlarmFlag()` / branch.

A rescue alarm is therefore always armed before any long sync/upload work.

### Bug 7 - Failure paths drain battery
Replace each `while(true){delay(1000);}` in `main.cpp` (RTC init fail, alarm arm fail, verify fail) with a `boundedRetryAlarm()` helper:
- up to 3 attempts with I2C bus recovery (`Wire.begin()` re-init) between attempts;
- on final failure, arm a best-effort rescue alarm and `releasePwrHold()` instead of looping forever.

The rescue alarm from Bug 1 is the safety net.

### Test
New `tests/bringup_rescue_alarm.cpp`:
- boot, confirm rescue alarm armed within 2 s;
- confirm normal flow still clears/re-arms;
- with RTC disconnected, confirm board releases PWR_HOLD after bounded retries.

### Validation
Full sync cycle must still complete; confirm board powers off even if RTC is disconnected.

### Risk note
Changes boot ordering. Keep `detectWakeSources` call **after** the rescue arm so wake classification is unaffected.

## Stage 3 - RTC validity + wake reason safety (Bugs 5, 6)

**Files:** `src/time/rtc_alarm.{h,cpp}`, `src/system/wake_reason.{h,cpp}`, `src/main.cpp`

### Bug 5 - initRTC returns true on invalid time
`initRTC()` returns a status enum:
```cpp
enum RtcInitStatus { RTC_OK, RTC_PRESENT_TIME_INVALID, RTC_ABSENT };
RtcInitStatus initRTC();
bool rtcTimeValid();  // !lostPower() && year >= 2024
```
In `handleSyncWake`: if `RTC_PRESENT_TIME_INVALID`, do **not** schedule - arm rescue alarm, log fault, release PWR_HOLD. Config mode still proceeds (user can set time).

### Bug 6 - RTC status read failure misclassified as USB service
`selectWakeReason`: if `!sources.rtcStatusRead`, return `WAKE_UNKNOWN`. In `setup()` switch, handle `WAKE_UNKNOWN`: arm rescue alarm, log fault, release PWR_HOLD. Do not fall through to `handleServiceWake` (which releases without arming).

### Test
Extend `bringup_wake_reason.cpp`: simulate I2C failure, confirm `WAKE_UNKNOWN` and that PWR_HOLD releases only after rescue arm.

### Validation
Compile + existing wake-reason test. Confirm config button still works when RTC is healthy.

## Stage 4 - Flash/filesystem safety (Bugs 13, 14)

**Files:** `src/storage/flash_logger.{h,cpp}`

### Bug 13 - LittleFS.begin(true) formats on failure
`initFlash()` uses `LittleFS.begin(false)`. On mount failure, set `gFlashReady=false` and a new `gFlashMountFailed=true` flag exposed via `bool flashMountFailed()`. Do not format. Add `bool flashFormatExplicit()` for a future web-UI recovery action. In `handleConfigWake`, if mount failed, surface a fault line in the config server status JSON (additive field).

### Bug 14 - Flash append success not checked
`flashLogCSVRow`: check `f.println()` return value; compute expected bytes = `row.length() + 1`; if `f.getWriteError()` or bytes written mismatch, log `[FLASH] write failed` and return false. `logSnapshotRow` delegates to `flashLogCSVRow` and inherits the fix.

### Test
Extend `bringup_flash_purge.cpp`: fill LittleFS to near-full, confirm `flashLogCSVRow` returns false and logs warning, no silent success.

### Validation
Compile + `bringup_flash_purge.cpp`.

### Open question
Confirm with user that a web-UI "Format Flash" action is acceptable in lieu of auto-format.

## Stage 5 - ESP-NOW callback queue (Bug 8)  [HIGHEST RISK]

**Files:** `src/comms/espnow_sync.{h,cpp}`, `src/main.cpp`

### Bug 8 - ESP-NOW callback writes LittleFS from Wi-Fi task
Add a FreeRTOS queue of fixed-size slots:
```cpp
struct EspNowSnapSlot { uint8_t mac[6]; node_snapshot_t snap; };
static QueueHandle_t gSnapQueue;  // depth 8, drop-oldest if full
```
`onEspNowRecv` enqueues a copy and returns immediately - no LittleFS, no `registeredNodes` iteration. A drop counter is incremented when the queue is full.

`espnowSyncLoop()` drains the queue on the main task: for each slot, call `logSnapshotRow`/`flashLogCSVRow` and update `registeredNodes`. The existing 10 ms call site in `handleSyncWake` is unchanged.

Add `void deinitEspNowSync()` - calls `esp_now_deinit()` (or `esp_now_unregister_recv_cb`). Call it in `handleSyncWake` right before `performModemUpload` and before `uploadQueue.purgeUploaded()` / `emergencyPurgeIfFull()`. Re-init is not needed (board powers off after).

### Test
Extend `bringup_espnow_basic.cpp`: send bursts of snapshots, confirm all logged; confirm no LittleFS corruption when purge runs mid-burst (deinit first).

### Validation
Compile + `bringup_espnow_basic.cpp` + full sync cycle.

### Risk mitigations
Queue is fixed-size (no heap fragmentation). Drop-oldest policy with counter. Deinit guarantees no concurrent FS access. Designer confirms queue depth and drop policy in Stage 0.

## Stage 6 - Atomic purge + cursor validation (Bugs 11, 12)

**Files:** `src/storage/upload_queue.{h,cpp}`, `src/storage/flash_logger.cpp`

### Bug 11 - Purge remove-then-rename not atomic
`purgeUploaded()` and `emergencyPurgeIfFull()`:
1. rename old `datalog.csv` -> `datalog_bak.csv`;
2. rename `datalog_tmp.csv` -> `datalog.csv`;
3. `remove(datalog_bak.csv)`.

Add `recoverDataFile()` called from `uploadQueue.init()`:
- if `datalog.csv` missing but `datalog_bak.csv` exists, rename bak -> real;
- if both `datalog.csv` and `datalog_tmp.csv` exist, prefer `datalog.csv` and remove tmp;
- if only `datalog_tmp.csv` exists, rename to real.

### Bug 12 - validateCursor doesn't check row boundary
`validateCursor()`: after bounds check, open file, seek to `offset-1`, confirm byte is `\n` (or `offset == headerEndOffset()`). If not, scan forward to next `\n` and set cursor there; if none found, reset to header end. Save corrected cursor.

### Test
New `tests/bringup_atomic_purge.cpp`: create `datalog.csv`, trigger purge, reset between rename steps, confirm `recoverDataFile` restores valid file. Extend `bringup_upload_cursor.cpp` for row-boundary validation.

### Validation
Compile + `bringup_flash_purge.cpp` + `bringup_upload_cursor.cpp`.

### Risk note
Boot recovery state machine - Designer review in Stage 0.

## Stage 7 - Chunked upload payload (Bug 9)

**Files:** `src/storage/upload_queue.{h,cpp}`

### Bug 9 - 256 KB String OOM + row-split + byteLength tracking
Rewrite `getNewData()`:
- check `ESP.getFreeHeap()` before allocation; if < 2 x maxBytes, reject with empty payload and log;
- stream into a pre-allocated buffer (4-16 KB chunks, grown once) via `reserve()` + `concat(buf, n)` instead of char-by-char;
- compute exact complete-row `endOffset`: read until `bytesRead >= maxBytes` **and** last byte is `\n`; if mid-row, continue to next `\n`;
- set `payload.byteLength = endOffset - startOffset` exactly.

`advanceCursor` already takes the exact offset.

### Test
Extend `bringup_upload_cursor.cpp`: fill 300 KB of rows, request 256 KB, confirm payload ends on `\n`, `byteLength` matches file offset, no OOM.

### Validation
Compile + cursor bringup.

## Stage 8 - NVS phase-anchor integrity (Bug 15)

**Files:** `src/config/config_server.cpp`

### Bug 15 - Phase-anchor NVS writes unchecked and non-redundant
Define a packed struct:
```cpp
struct SyncAnchorRecord {
  uint32_t magic;
  uint16_t version;
  uint16_t generation;
  uint32_t phaseUnix;
  uint16_t intervalMin;
  uint8_t  mode;
  uint8_t  reserved;
  uint32_t crc;
};
```
Persist two copies in NVS keys `sync_anchor_a` / `sync_anchor_b` (alternating by generation). `saveSyncRuntimeGuardsToNVS` writes the next copy, reads back, verifies CRC; on mismatch retries once. `loadSyncRuntimeGuardsFromNVS` reads both, picks the one with valid magic + CRC + plausible phase (year >= 2024, interval in allowed set); if both invalid, fall back to legacy `sync_last_unix`, then to per-node desired configs; if still invalid, set `gLastSyncBroadcastUnix = 0` and log a desync warning.

**Migration:** on first load with new keys absent, read legacy `sync_last_unix` and write it into anchor A.

### Test
New `tests/bringup_nvs_anchor.cpp`: write anchor, corrupt one copy, confirm load picks the valid one; corrupt both, confirm fallback to legacy.

### Validation
Compile + `bringup_config_latch.cpp`.

### Risk note
Schema change - Designer review in Stage 0.

## Stage 9 - Upload retry reset (Bug 10)

**Files:** `src/storage/upload_queue.{h,cpp}`, `src/main.cpp`

### Bug 10 - Permanent upload lockout after 3 retries
Replace the permanent latch with a time-based cooldown. Add `nextAttemptUnix` to the cursor. `maxRetriesExceeded` returns false if `nowUnix >= nextAttemptUnix`. On each failure, set `nextAttemptUnix = nowUnix + cooldownSec` (e.g. one sync interval). `resetRetryCount` is still called on success.

### Test
Extend `bringup_upload_cursor.cpp`: simulate 3 failures, confirm 4th wake retries after cooldown rather than locking out forever.

### Validation
Compile + cursor bringup.

## Stage 10 - Medium findings

**Files:** `src/main.cpp`, `src/config/config_server.cpp`, `src/config/transmission_settings.cpp`, `comms/espnow_sync.cpp`

### Session watchdog
In `handleSyncWake`, capture `bootMs = millis()` at entry. Before each major phase (ESP-NOW listen, upload, alarm arm), check `millis() - bootMs > 180000`; if exceeded, force `modem.gracefulShutdown()`, skip remaining work, go straight to alarm re-arm.

### Daily mode in sync wake
In `handleSyncWake`, call `loadSyncModeFromNVS()` and `loadDailySyncTimeFromNVS()`. If `gSyncMode == SYNC_MODE_DAILY`, call `armDailyAlarm(gSyncDailyHour, gSyncDailyMinute)` instead of `armNextSyncAlarmPhase`. Mirror the existing config-mode branch.

### Transmission settings string safety
Replace `prefs.getString("url", String(""))` etc. with fixed-buffer overloads `prefs.getString("url", buf, sizeof(buf))`. Size buffers to match `TransmissionSettings` field max lengths.

### Legacy ESP-NOW parsing strncpy safety
In `onEspNowData` legacy branch, zero-initialize `NodeSnapshot snap` and force `snap.nodeId[sizeof-1] = '\0'` after `strncpy`. (This branch moves to the queue in Stage 5 but the fix is cheap and independent.)

### Test
Extend `bringup_modem_powercycle.cpp` for watchdog; extend `bringup_config_wake_test.cpp` for daily mode.

### Validation
Compile + relevant bringup tests + full sync cycle in both interval and daily modes.

## Validation summary

- Every stage: `pio run -e mothership-v1-main` (from `mothership/firmware/v1/`).
- Every stage: re-run existing bringup tests 1-13; no regressions.
- Every stage: one full autonomous sync cycle (node snapshot -> flash log -> alarm re-arm -> power off -> next wake) must complete.
- New bringup tests: `bringup_alarm_verify`, `bringup_rescue_alarm`, `bringup_atomic_purge`, `bringup_nvs_anchor` (plus extensions to existing tests).

## Risks and open questions

1. **Stage 2 boot ordering** - rescue alarm is armed before `detectWakeSources`. Keep wake classification after the rescue arm.
2. **Stage 5 ESP-NOW queue** - highest-risk change; mitigated by fixed-size queue, drop-oldest policy, and deinit-before-upload.
3. **Stage 6 boot recovery** - new state machine; Designer review required.
4. **Stage 8 NVS schema** - migration path required; Designer review required.
5. **Stage 4 `begin(false)`** - could mask genuine corruption; needs user sign-off on a web-UI format action.
6. **Stage 10 daily mode** - first functional change to `handleSyncWake` branching; verify a daily-mode sync wake arms a daily alarm.

## Handoff

- **Next:** Designer produces the Stage 0 addendum.
- **Then:** Coder implements Stages 1-10 in order (parallel where noted).
- **Reviewer:** validates each stage against this plan and the bringup test suite before merging.
