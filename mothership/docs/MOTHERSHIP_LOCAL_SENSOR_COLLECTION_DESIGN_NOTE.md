# Mothership Local Sensor Collection — Design Note

Status: **proposed, nothing implemented.** Written 2026-09-30 from a read-only review of the
node firmware, hub firmware and docs. Intended as the starting point for the firmware work once
the hardware is in hand.

Related notes:

- `MOTHERSHIP_POWER_AND_WAKE_DESIGN_NOTE.md` — power gating, RTC alarms, reserved pins (§5).
- `MOTHERSHIP_SYNC_RENDEZVOUS_DESIGN_NOTE.md` — the sync-window scalability work. **Read it
  before touching hub wake scheduling** (see §6.1 below).
- `FIRMWARE_TEST_BACKLOG_2026-09-30.md` — outstanding test debt to clear first.

## 1. Goal

Let the hub optionally collect its own local sensor data, using the same sensor acquisition
code as the node:

- when **enabled**, the hub wakes at a configured *collection interval*, reads its local
  sensors, and stores the result through the existing hub pipeline (flash + SD → upload queue →
  LTE). It keeps waking for normal sync and keeps doing every existing hub duty;
- when **disabled**, behaviour is identical to today.

## 2. Unverified assumptions (check before building)

Nothing below has been confirmed against the schematic or the backend.

1. **Pin map vs. hardware.** `src/system/pins.h` says it matches the *V1* schematic. The
   current board is *FieldHub V2* (`hardware/FieldHub_v2`). Confirm every pin against the V2
   schematic before trusting it, especially anything used for the reed input.
2. **A free, broken-out GPIO for the reed sensor** (see §3). The V2 BOM/Gerbers show no obvious
   free header.
3. **Sensor power and bus.** Which rail powers local sensors, whether it is switched, and
   whether the I2C bus (GPIO21/22, pull-ups to MAIN_3V3) is exposed on a connector.
4. **Backend acceptance of a hub-typed node** (§7). The backend repo was not reviewed.
5. **Hidden node-`main.cpp` dependencies** of the sensor backends (mux select, `NODE_ID`,
   battery pin macros). Backends reference `extern TwoWire WireRtc`; other couplings were not
   audited.
6. **Flash headroom.** Deployment-epoch work left the main image at ~85% of its partition
   (`DEPLOYMENT_EPOCH_TEST_LOG.md` §2d). Adding SHT4x/AS7341/ADS libraries may not fit an A/B OTA
   layout.

## 3. Reed sensor GPIO

Hub pins in use: 4, 13, 14, 16, 17, 18, 19, 21, 22, 23, 25, 26, 27, 32, 33, 34, 35.
Avoided by the power/wake note (§5.2): 0, 1, 2, 3, 5, 6–11, 12, 15.

**Recommendation: GPIO36 (SVP) or GPIO39 (SVN).**

- Input-only, **no internal pull-up** — fit an external ~10 k to MAIN_3V3 and a ~100 nF cap to
  ground. The node backend's `pinMode(INPUT_PULLUP)` is silently ignored on these pins, which is
  harmless.
- Interrupt-capable; the node backend counts falling edges in an ISR.
- Override without code changes: `-DREED_WIND_PIN=36`.
- **Do not use GPIO4** even though the node does — it is `LTE_STATUS` on the hub.
- Only usable if the V2 board actually routes 36/39 to a pad/header. If neither is available,
  the fallback is a bodge wire; strap pins are a last resort.

Reed pulse counting only works while the hub is awake: the node backend uses a 2 s probe and a
10 s window (`sensors_reed_wind.cpp`). That is fine for a dedicated collection wake but must be
budgeted (§6.3).

## 4. How the current cycles work

**Node.** Two DS3231 alarms: A1 = data interval (`g_intervalMin`), A2 = phase-locked sync. A data
wake runs `captureSensorsToQueue()` (node `main.cpp:1208`): battery ADC, then
`buildReadingsArray()` from the sensor registry, assembles a `node_snapshot_v2_t`, enqueues it in
`local_queue`, and powers down. A sync wake flushes the queue over ESP-NOW.

**Hub.** Uses **Alarm 1 only**, armed to the next phase-locked sync slot
(`armNextSyncAlarmPhase`). Wake reasons: RTC / config button / USB. An RTC wake always calls
`handleSyncWake()`: NVS loads, init RTC/SD/flash/queue, coordinated ESP-NOW window (~105 s), modem
upload inside a 300 s session budget, re-arm A1, release `PWR_HOLD`. The hub **cold-boots on every
wake** (power-gated, not deep sleep). Received snapshots are persisted by `processSnapshot()`
(`main.cpp:266`) to flash and SD, then flow to the upload queue.

Naming trap: `gWakeIntervalMin` on the hub is the interval it *pushes to nodes*, not a hub
interval. A hub interval needs a distinct name (e.g. `localCollectIntervalMin`).

## 5. Feasibility

Feasible. The sensor stack is well isolated (`shared/sensors.h` already lives in
`node/firmware/shared`, which the hub already builds against), and the hub already has a
persistence path. The real risks are scheduling, the node registry, and data volume — not sensor
code.

## 6. Design issues and decisions

### 6.1 Scheduling — use DS3231 Alarm 2 for collection, leave Alarm 1 (sync) alone

Rationale: the sync path is the most fragile, most recently debugged part of the hub. Alarm 2 is
unused on the hub, so local collection can be added without changing sync arming.

Consequences to handle:

- `clearAlarmFlag()` clears **A1F and A2F together** (`rtc_alarm.cpp:307`). It would swallow a
  pending A2 wake. Wake detection and clearing must become per-alarm.
- `WAKE_RTC_ALARM` currently means "sync". Add `WAKE_LOCAL_COLLECT` and decide by reading A1F/A2F
  separately.
- Every re-arm path — rescue (`armRescueAlarm`), daily (`armDailyAlarm`), config-shutdown and
  post-sync (`main.cpp:1953, 1979, 2174`) — must re-arm/preserve A2 too, or a config session
  silently kills collection.
- Both alarms share one INT line/latch; ensure a simultaneous A1+A2 is handled as one wake.
- **Coordinate with the rendezvous work.** Its Stage 0 raises the hub pre-wake lead to ~45 s
  (`rtc_alarm.cpp:225`) and moves storage init after the window. A collection wake inside that
  lead, or storage init reordering, changes what "hub awake" means. Implement one of the two
  first, then rebase the other.

Rejected: merging both schedules into A1 with `min(nextCollect, nextSync)` — touches the proven
sync arming and the daily-sync mode.

### 6.2 Coincident wakes

- Constrain the collection interval to divide the sync interval, or accept irregular alignment.
- If a collection slot coincides with a sync slot, do **one** wake and one collection.
- Never run a blocking read (reed up to 10 s, AS7341) *inside* the ESP-NOW listen/grant window —
  it can delay node ACKs. Collect before the window opens or after nodes are released, and stamp
  the nominal slot time.
- **Open question:** how much delay does the rendezvous tolerate before the window opens? Given
  the rendezvous aperture analysis (`25 − t_bootH`), an extra collection step before the window is
  exactly the kind of latency that note warns about. Default to **after nodes are released, before
  the modem upload**, unless measurement says otherwise.

### 6.3 Collection wake budget

A local-only wake is: boot → init RTC/flash/SD → read sensors → persist → re-arm → power off. No
radio, no modem. Expect a few seconds; the reed adds 2–10 s. This must be measured, not assumed.

### 6.4 Sensor code reuse

Extract the node sensor stack (`node/firmware/src/sensors/*`, `shared/sensors.h`) into a shared
library consumed by both PlatformIO projects. Put a small hardware-abstraction seam in front of
what is currently hard-wired:

- I2C bus: backends use `extern TwoWire WireRtc`. On the hub, alias it to `Wire` (GPIO21/22).
  SHT41 (0x44) does not clash with the DS3231 (0x68).
- Pin macros (`REED_WIND_PIN`, battery ADC pin/divider), already `#ifndef`-overridable.
- Registry mask (`g_expectedSensorMask`) and `initSensors()` semantics.

Gate at two levels: a compile-time flag (so a disabled/unsupported build is byte-for-byte
unchanged in behaviour) and a runtime NVS enable.

The node build must be proven unchanged after the extraction — that is its own milestone.

### 6.5 Persistence — reuse the hub pipeline, do not loop back through ESP-NOW

Build a `DecodedSnapshot` in-process and persist it through the same code path as received
snapshots. Extract the flash + SD logging out of `processSnapshot()` into a shared
`persistSnapshot()`; both callers use it. Do not fake an ESP-NOW receive.

### 6.6 Data model

- **Identity:** a stable hub `nodeId` (e.g. `HUB-<mac suffix>`), used by
  `resolveEpochForSample()` and the CSV/upload rows.
- **Sequence:** a persistent NVS-backed monotonic `seqNum` (the node's
  `local_queue::nextSeq` does not exist on the hub). The rendezvous note (§11.5) already flags
  that hub-side `seqNum` de-duplication must be verified — verify it here too.
- **Battery:** the hub's own reading (`SENSOR_ID_BAT_V`, GPIO34, 220 k/100 k divider), not the
  node's divider.
- **Timestamps:** hub RTC is authoritative.

### 6.7 Registry hazard (highest-risk item)

Sync-window, missing-node, stale-recovery, config-replay and deployment logic iterate
`registeredNodes` and expect radio contact. If the hub is added as an ordinary node it will look
like a permanently missing node (waiting out the window, raising stale alerts, being pushed
configs). Give it an explicit `local`/`isHub` marker that **every** such loop skips, or keep it out
of `registeredNodes` and give it a separate identity only at the epoch/payload layer.
**Audit every `registeredNodes` consumer before choosing.** Also decide how the hub participates
in deployment epochs (start/end/pause).

### 6.8 Backend / dashboard

Confirm the backend can ingest readings for a hub-typed node: node registry row, sensor channel
mapping, deployment linkage, fault-mask semantics. Hub sensors would need the same per-sensor
identity keys as nodes. Coordinate contract changes with the backend repo before firmware ships;
note the CSV schema is already at 31 columns with a legacy-header upgrade path
(`DEPLOYMENT_EPOCH_TEST_LOG.md`), so **do not add columns** for this.

### 6.9 Storage and upload volume

At a 1-minute collection interval and a 60-minute sync the hub adds ~60 rows/hour of ~17 readings.
Not yet checked against: LittleFS retention/purge threshold (768 kB partition), `upload_queue`
capacity, the upload payload cap, and modem session time. Size this before choosing the minimum
allowed collection interval. Also note SD/LittleFS init cost is paid on every collection wake
(the rendezvous note shows storage init is already a major source of hub boot latency).

### 6.10 Config

- New NVS keys: `local_enabled`, `local_interval_min`, `local_sensor_mask` (reuse the node's
  capability-bit convention).
- Surface in the hub config UI first; remote (dashboard) control via `backend_command_ingest`
  is a later phase.
- Pairing is not involved.

## 7. Implementation plan

0. **Gate.** Clear the test backlog items marked *blocking* (see the backlog doc), and decide the
   order against rendezvous Stage 0.
1. **Hardware confirmation.** V2 schematic vs. `pins.h`; reed pin; sensor rail/bus; bench-verify
   each with the existing standalone bring-up sketches.
2. **Extract sensor stack** into a shared library with the HAL seam. Node behaviour unchanged;
   node build + `test_protocol_v2`, `test_queue_*`, `bringup_full_selftest` pass.
3. **Hub `local_collect` module.** NVS config, `WAKE_LOCAL_COLLECT`, A2 scheduling with per-alarm
   flag handling, `persistSnapshot()` extraction, in-process snapshot build.
4. **Identity and registry.** Hub marker; audit and patch all `registeredNodes` loops; backend
   contract agreed.
5. **Coincident-wake rule and config UI.**
6. **Tests** (below), then a soak.

## 8. Tests to write

Host-style suites following the existing `mothership-v2-test-*` pattern:

- alarm scheduling: A2 arm/verify/clear independent of A1; simultaneous A1+A2; re-arm after
  config, rescue, daily paths;
- wake-reason selection: sync vs. collect vs. both;
- coincident-slot logic and interval-divides-sync validation;
- `persistSnapshot()` equivalence between received and local snapshots;
- registry: hub marker excluded from missing-node/stale/config-replay logic;
- `seqNum` persistence and de-dup across reboot;
- disabled mode: sync wake timing and payload identical to baseline.

Hardware acceptance: collect wake duration measured; sync window attendance unchanged with
collection enabled (compare to the rendezvous instrumentation); power-loss mid-collect leaves a
consistent flash/SD state; reed sensor end-to-end through flash → CSV → upload (**not yet proven
even on the node**).

## 9. Out of scope

- Hub-attached sensors other than those the node stack already supports.
- Changing the ESP-NOW protocol or CSV schema.
- Hub-side wind direction / ultrasonic anemometer.
