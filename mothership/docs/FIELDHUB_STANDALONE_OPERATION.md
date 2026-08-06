# FieldHub standalone operation

This note records the local-first contract implemented on
`feat/fieldhub-standalone-ui`. It is an operating and field-acceptance guide;
executed checks are recorded separately below so build results are not confused
with hardware evidence.

## Operating modes

Local fleet management never requires a FieldMesh account. During the existing
physically gated 30-minute configuration session an operator can discover and
pair nodes, configure sensors and recording intervals, start/end deployments,
inspect the bounded deployment history, and download raw CSV files.

The data destination is one explicit choice:

- **Local storage only** — the default. The LTE modem is not used for upload.
- **FieldMesh** — optional provisioning enables paid remote history, charts,
  alerts, collaboration, remote control, and managed backup. Local functions
  remain available. Complete deployment records still retained internally are
  backfilled through the bounded cloud outbox over successive syncs, so a hub
  may connect after being commissioned locally.
- **Custom HTTPS** — sends readings to an operator-owned ingest service. It does
  not enable FieldMesh dashboard or remote-control features.

The captive portal does not implement subscription or login checks. FieldMesh
features are gated by a valid provisioning credential at the service boundary;
local operation does not depend on that credential.

## Local retention

LittleFS is always the upload cache and internal fallback. Upload acknowledgement
advances a delivery cursor but no longer deletes a current-schema CSV. Because
the 768 KB partition is shared and bounded, retention starts at 65% total use and
rewrites the readings file to keep roughly the newest 40% of rows. When internal
storage is the active archive, the Data page reports the cumulative number of
rows removed by this limit. Downloading never deletes data.

When a compatible SD card is mounted, each accepted snapshot is also appended to
`fieldmesh_readings.csv`. Completed deployment records are appended idempotently
to `fieldmesh_deployments.csv`. These files are not purged after upload; their
limit is the usable card capacity. An incompatible existing canonical filename is
renamed with a `.legacy-N` suffix rather than overwritten. "Complete SD archive"
therefore means all writes made while a writable card was mounted; the UI exposes
write failures instead of claiming otherwise.

LittleFS retains the newest four completed deployment records per node. A real
v1-to-v2 store migration preserves known epoch/start/end/outbox information and
marks unavailable older labels, locations, or end times as unknown. It never
fabricates those values or silently reseeds epochs.

## Custom HTTPS receiver contract

The FieldHub makes HTTPS `POST` requests with `Content-Type: application/json`.
If configured, the token is sent as `Authorization: Bearer <token>` and is never
placed in the URL or status JSON. The document uses the existing FieldMesh batch
shape:

```json
{
  "readings": [{ "datetime": "...", "nodeId": "...", "seqNum": 1 }],
  "meta": { "firmwareVersion": "..." },
  "status": { "...": "present on the first data POST of a session" }
}
```

Receivers must ignore fields they do not need and should deduplicate retries by
`(nodeId, datetime, seqNum)`. Return any 2xx only after the batch is durably
stored. A non-2xx response leaves the delivery cursor unchanged. Custom mode does
not send FieldMesh deployment lifecycle events, accept remote commands, or run
cloud OTA. The full field reference remains in
[`docs/FIELDMESH_PAYLOAD_REFERENCE.md`](../../docs/FIELDMESH_PAYLOAD_REFERENCE.md).

## Required field acceptance before release

1. Upgrade a hub carrying a genuine v1 deployment store; confirm epochs,
   pending lifecycle state, and known history survive two boots.
2. Run the deployment-epoch and upload-queue test images on hardware, then use
   the documented wipe image before returning the hub to production firmware.
   **Bench only:** the wipe removes the hub's complete local deployment store,
   including genuine archived deployments. It is not part of a normal field
   upgrade and must only be used where that history is disposable or backed up.
3. Exercise local setup with no key/SIM/backend, commission a node, end and
   redeploy it, reboot, and download both CSVs.
4. Fill LittleFS beyond its retention threshold and power-cycle during a rewrite;
   confirm one valid data file survives and removed-row reporting is truthful.
5. Insert/remove/fill an SD card and interrupt power around snapshot and End
   writes; confirm no canonical file is overwritten and replayed Ends are not
   duplicated.
6. Test a custom endpoint through success, timeout, lost response, 4xx, and 5xx;
   confirm only a durable 2xx advances delivery and retries deduplicate.
7. After at least one local-only ended deployment and one active deployment,
   provision FieldMesh and confirm retained readings resolve to the backfilled
   deployments over successive acknowledgements. Then confirm dashboard control
   convergence and OTA remain unchanged.

Build success alone does not satisfy these hardware and backend acceptance gates.

## Planned — cloud-provisioning integrity + UX rework (not yet accepted)

Landed in source and building clean as of 2026-08-06. **Nothing below has been
run on hardware**, so none of it counts as acceptance evidence; it is recorded
here as planned work with its own gates, deliberately kept out of the evidence
section below. Flashing waits on Journey A phases 3-5 sign-off.

Outstanding hardware checks:

1. **Forged endpoint rejected.** POST `url=https://evil.example/x` to
   `/save-settings`; the stored FieldMesh endpoint is unchanged.
2. **Poisoned state neutralised, durable, visibly stuck.** Pre-store a
   disallowed endpoint. Confirm upload disabled, key erased, and the
   `fieldmesh_reprovision_required` marker set. **Reboot** and confirm the
   warning is still shown on Home, Settings and `/provision` — remediation flips
   the mode to local-only, so the raw detection condition is false from then on;
   only the separate durable marker keeps the warning alive.
   `/set-data-destination` must refuse `mode=fieldmesh` while it is set.
3. **Key rotation.** After revoking via the existing dashboard revoke action,
   the old key is rejected (401). Issuing a new key without revoking is *not*
   recovery.
4. **Save failure surfaces at every caller.** Build
   `mothership-v2-hook-tx-save-failure`, POST `/test/tx-fail-save?on=1`, then
   confirm `/provision-apply`, `/set-data-destination` (both branches),
   `/set-custom-destination` and `/save-settings` all report failure — four
   callers, not just provisioning. (Never flash this image for field use.)
5. **Queue init.** One `[UQ] init:` line per boot; no `nvs_erase_key fail`; with
   the `mothership-v2-test-upload-queue` hook, a failed init makes manual upload
   abort and pages read "unavailable", never zeros.
6. **Copy control.** Android Chrome and iOS Safari: genuinely copies, or honestly
   reports the fallback. With JS disabled the MAC still selects as one value.
7. **Fragment entry.** `/provision#FM1.<payload>` opens step 2 in its
   confirmation state, not step 1.
8. **iOS scanning.** In-page scan hidden; the camera-app route offered as a
   normal option (WebKit's Barcode Detection issue is open, not a permanent
   absence).
9. **No truncation.** `/`, `/settings`, `/setup` and `/provision` each contain
   the `<!--FM-PAGE-END-->` marker.
10. **No `arg missing value: 0`** after "Looks good".
11. **Wizard renumbering.** `/setup` has 7 steps, not 8; a fresh local-only
    walkthrough reaches the final step; Back out of the Time step returns to
    step 1 when no cloud connection was made.
12. **Status honesty.** Settings shows "FieldMesh configured" and "Last
    successful upload: …" as two separate facts; no page claims "Connected".
13. **SIM/APN.** A hub with no `SimSettings` still connects on the preserved
    default; a changed APN takes effect at the next upload with no reflash; a
    `"` or newline in the APN field is rejected by the form.
14. **Real convergence.** The next sync produces a successful authenticated
    upload **and** `last_seen` advances for this FieldHub in the intended
    project. Presence in the project proves nothing on its own — a hub is listed
    from the moment it is registered, so the check is a delta.

## Acceptance evidence

### 2026-08-02 — bench FieldHub on COM4

Executed from commit `91f5411` on the physical ESP32 FieldHub
(`48:9d:31:f8:16:a8`):

- `mothership-v2-test-deployment-epoch`: **193/193, OVERALL:PASS**. This
  included local archive retention, more than 16 standalone lifecycle records,
  bounded backfill fixtures, and v1 deployment-store migration.
- `mothership-v2-test-upload-queue`: **29/29, OVERALL:PASS**. The new retention
  checks confirmed that a current-schema readings file is accepted and an
  upload acknowledgement does not delete current local history.
- `mothership-v2-wipe-deploy-store`: **6/6, OVERALL:PASS**. All deployment test
  files were absent afterward and `/datalog.csv` remained present at 367 bytes.
  Because the same files hold genuine local deployment history, any archive
  that existed on this test hub before the suite was intentionally removed.
- `mothership-v1-main` was restored successfully. Build `91f5411` booted without
  a panic and entered the expected USB-service path. A subsequent physical
  configuration wake served web UI requests, initialized the LittleFS upload
  queue, and completed the UI-requested Sync & Power Down path with the next RTC
  alarm armed.

No SD card was present on this device. SD archive, removal, capacity, canonical
filename, and interrupted-write acceptance checks were therefore **NOT RUN**,
not failed. The initial portal-start and SD-detection lines occurred before the
serial capture reconnected, so they are not claimed as evidence. Full
captive-portal workflow checks and the remaining manual/backend journeys are
still outstanding.

### 2026-08-04 — local pathway field session on COM4

Executed on the same physical FieldHub (AP MAC `48:9D:31:F8:16:A9`, SSID
`FieldHub(489D31F816A8)`), firmware reflashed repeatedly through the session
as fixes landed (final identity: `build=5b792b4-dirty`, `CSV schema=35`).
Local-only throughout: no cloud upload configured.

**Code review before flashing turned up three latent bugs in the uncommitted
33→35 CSV-schema work** (userId/name/latitude/longitude columns), found by
reproducing the exact validation/formatting logic outside the firmware, not
by running on-device tests:

- A named node's readings were silently dropped from every cloud upload.
  `json_payload.cpp`'s `validReadingRow()` ran a blanket numeric check over
  every column past index 6; `name` is free text, so any real node name
  failed it and the row was treated as malformed and skipped —
  `ok=true`, `rowCount=0`, nothing surfaced as an error. Fixed by making the
  validator schema-aware (sensor floats / epoch integer / free-text identity
  / numeric-or-nan location, each checked on its own terms).
- A comma (or CR/LF) in a node name reframed the CSV row: the column count no
  longer matched, the row's own width gate rejected it, and nothing reached
  flash *or* SD — the node would retry that reading forever. Fixed with a
  `csvSafeCell()` sanitizer at all three row-building call sites.
- The row-building buffer (`char row[512]`) accumulated append offsets past
  its own end via `snprintf(buf+offset, size-offset, ...)`; once offset
  exceeded 512 the `size_t` subtraction wrapped to ~4 GB, turning a bounds
  argument meant to refuse the write into one that permits it — a stack
  smash reachable by a node reporting large-but-finite sensor values.
  Pre-existing, not introduced this session, but the four new columns ate
  into its margin. Fixed with a bounded `appendFmt()` helper (skips writes
  once the buffer is full, preserving the existing overflow-detection
  behaviour) and the buffer widened to 640 bytes.

All three are covered by new assertions in `test_upload_queue.cpp`.
`mothership-v2-test-upload-queue` and every other affected test/production
environment build clean — **compile-only; not flashed or executed this
session.** Only `mothership-v1-main` was flashed and run live.

**Two further regressions surfaced only by running production firmware
against the hub's real, previously-field-populated storage — code review
alone could not have found either, since both depend on state code review
can't see:**

- **Flash logging went dark on first boot.** The hub's actual `/datalog.csv`,
  written by the real committed firmware (`ea98b05`) before any of this
  session's schema work, carries a 31-column header (`...,deploymentEpoch`,
  no identity/location columns) that had never been preserved as its own
  legacy constant when the schema was extended — `initFlash()` saw a header
  it didn't recognise and refused to touch the file:
  `[FLASH] Unknown CSV header; preserving file and refusing incompatible
  appends`, followed by `[SNAP] No storage accepted the snapshot` for every
  reading that sync. Fixed by restoring `kLegacyCSVHeader31` (byte-verified
  against the `ea98b05` commit) and wiring it into both `flash_logger.cpp`'s
  and `upload_queue.cpp`'s legacy-header checks.
- **A standalone hub could never redeploy a node again after any CSV schema
  bump.** `beginNewDeployment()`'s legacy-backlog guard — meant to stop
  unstamped rows being misattributed to a new deployment's epoch by the
  cloud backend's fallback logic — blocks unconditionally on
  `uploadQueueHasLegacyRows()`, and every code path that clears that backlog
  only runs after a successful network upload. With upload disabled, this
  hub hit it live: *"Upload existing readings before starting a new
  deployment (45 rows pending)."* Since the risk the guard protects against
  cannot exist without an active upload destination, fixed by gating the
  check on `TransmissionSettings.enabled` — a standalone hub with upload off
  is no longer blocked.

**Hardware evidence, this session (all via live serial capture on COM4):**

- Reflash → boot → identity banner confirms `CSV schema=35` on every boot.
- A snapshot with the full 35-column row (`userId`, `name`,
  `latitude`/`longitude` = `nan`, unset for this node) round-tripped through
  a real sync: `columns=35`, `persisted=1`. Node number confirmed present
  (`001`) via the Field UI node detail page even though it wasn't visually
  obvious in the raw CSV — no header labels on the still-legacy on-disk
  header, expected until the backlog drains, not a bug.
- **Deployment lifecycle, one node (ENV_6C0A80), fully local:** End
  deployment 1 → archived. The node's own periodic 1-minute wake cycle was
  observed to stop (physically confirmed by the operator, not just the
  mothership's ACK) — real STANDBY convergence, not just a config-side
  handshake. Start deployment 2 was then blocked by the legacy-backlog gate
  (bug above); after the fix and a reflash it succeeded —
  `[DEPLOY] ENV_6C0A80 started deployment 2 as 001`, epoch=2, `NODE_CONFIG`
  pushed direct+broadcast OK. The node resumed its normal 1-minute recording
  cadence under epoch 2 at the next sync (`CONFIG_ACK converged: ENV_6C0A80
  v4 (ACTIVE)`, fresh snapshots from seq 106).
- **Fleet basics, two nodes:** Paired a second node (ENV_D13F98) through the
  full wizard; fleet count reflected 2 deployed. Unpaired it while deployed —
  exercised the deferred-unpair path (`NODE_CONFIG target=0` broadcast,
  `CONFIG_ACK unpair confirmed: ENV_D13F98 v3 — removing node`, registry
  dropped to 1, confirmed gone from the Field UI). Node 2 stopped waking
  entirely afterward (stronger than STANDBY, as expected for a full
  removal). Re-paired node 2 from scratch: prior number/name did **not**
  resurface — unpair fully clears `node_meta`, unlike End Deployment, which
  preserves identity for the archive.

**Not run / not resolved this session:**

- Sensor fault detection (fleet-basics item 12) — deferred to a future
  session; not exercised at all.
- Cloud upload path and induced power-loss recovery — deliberately out of
  scope; this session stayed local-only throughout.
- Node 1 (ENV_6C0A80) was observed to stay continuously powered rather than
  resuming its normal wake/sleep cycle after one sync. At the time this was
  treated as a probable hardware quirk on this specific unit, since the only
  uncommitted node-firmware diff at that point (deployment-epoch bookkeeping)
  didn't touch power management and hadn't fired that cycle. **Correction,
  2026-08-05: this was not hardware.** It recurred on a second, different
  physical node (ENV_D13F98) with direct serial evidence this time, and was
  root-caused to a real firmware bug — see the 2026-08-05 entry below.
- One transient ESP-NOW ack-send failure (`SNAP-ACK ... send=FAIL`, node 1
  seq 119 — the reading itself persisted fine) and a run of duplicate
  `CONFIG_ACK converged` log lines printed after a node's removal was already
  confirmed. Neither affected the outcome; not investigated further.
- `/datalog.csv` remains on its original 31-column header and will stay
  mixed-width (new 35-column rows appended under it) until an upload
  eventually drains the backlog — expected for a hub kept local-only
  throughout this session, not a defect.

### 2026-08-05 — continued local session: a real stuck-awake bug, two open items

**Node firmware — stuck-awake bug found, root-caused, and fixed.** The
"probable hardware quirk" noted above recurred, this time on node 2
(ENV_D13F98) with the node's own serial console attached directly (not just
the mothership's view). The evidence was unambiguous: after a sync, the node
printed `[PWR_HOLD] release deferred: critical node work still pending`
roughly every 100ms for well over a minute — continuing long after the
mothership had already closed its window and powered off — until the
operator cut power manually. Left alone this drains the battery and likely
ends in a watchdog reset (`[WDT] hardware watchdog armed (120s)` at every
boot) rather than a clean sleep.

Root cause: `node/firmware/src/main.cpp`'s sync-wake handler has a "legacy
fallback" path, built for a mothership old enough to only ever send a
lightweight `SYNC_WINDOW_OPEN` marker and never the full `SYNC_SESSION_OPEN`
handshake. If the marker arrived but the full session-open didn't within
2.5s, the node gave up waiting and blind-flushed its queue instead (no grant
negotiation, no durable-ACK retry). The mothership here is fully modern —
it just occasionally lost that timing race — so the real session-open would
still arrive, moments later, while the fallback flush's own event-servicing
loop was running. That set a `g_syncSessionOpenPending` flag with nothing
downstream ever positioned to clear it, and `hasCriticalPendingWork()` then
reported it forever, permanently blocking `PWR_HOLD` release.

Confirmed with the user that mothership-v1 is retired and never deployed
anywhere in this fleet — the fallback's entire reason for existing no longer
applies. Fixed by removing it outright rather than patching around it: the
listen loop now waits the node's full listen window for the real
session-open instead of bailing at 2.5s (making the race far less likely to
begin with), and a marker-without-session cycle now just skips and retries
at the next sync — the local queue is retained, nothing is lost, only
delayed. As defense in depth, `hasCriticalPendingWork()` now also gates the
three sync-session pending flags on `g_espNowReady`, so even a flag set
after the radio is shut down can no longer block power-off — this closes
the general failure shape, not just the one path that happened to trigger
it.

Both nodes reflashed (`build=71b8e3a-dirty`) and confirmed booting clean,
config preserved correctly across the flash. **Not yet field-confirmed**:
no sync cycle has been observed exercising the code that used to be the
fallback path, because the fix makes that path's trigger condition
substantially rarer — the real test is time, watching for the absence of
the stuck-awake symptom over further sessions.

**Open, unresolved:**

- **`NodeDesiredConfig` / deployment-epoch desync (mothership).** ~~After
  unpairing and re-pairing node 2, the Field UI showed it as "Ended" with no
  epoch and no identity — consistent with a node that was never formally
  redeployed — while it was in fact actively recording and syncing normally.~~
  **Resolved 2026-08-06 — see that entry below.** The real mechanism turned
  out to be neither of the candidates guessed here: not stale `node_dcfg`,
  not `registerNode()` on re-pairing. It was a stale RAM registry field read
  by the ESP-NOW deploy dispatcher. Root-caused, fixed, and confirmed across
  two independent sync cycles on hardware.
- **Sensor-fault detection has no UI, and item 12 was never actually
  verified.** `sensorFaultMask` is computed correctly in `main.cpp` but is
  exposed nowhere in the Field UI — only via `/api/live`. Worse, `/api/live`
  itself is unreliable for checking it: `expectedSensorMask`,
  `sensorPresentMask`, and `sensorFaultMask` are all RAM-only and only
  refresh during an actual sync wake, never during config mode — and the two
  boot paths are mutually exclusive per boot, so checking via the Field UI
  in config mode will always read `0` for all three regardless of true
  state. A plan to persist the fault state (mirroring how `lastReportedBatV`
  already survives reboots) and render it as a UI chip was drafted, then
  shelved for the night rather than implemented.
- **Soil-sensor fault detection cannot work as currently built.** Attempting
  to verify item 12 by pulling a soil probe produced no fault at all.
  Traced to `node/firmware/src/sensors/soil_moist_temp.cpp`: both soil
  probes share one ADS1115 ADC chip, and a disconnected/floating analog
  channel doesn't fail the way an I2C sensor read does — it just returns
  whatever stray voltage is on the pin, which runs through the calibration
  math and reports as a normal, successful (but meaningless) reading.
  `SNAP_PRESENT_SOIL1`/`SOIL2` can only ever reflect "is the ADS1115 chip
  itself alive," never "is this specific probe connected." A genuine,
  pre-existing architectural gap in fault coverage for ADC-based sensors,
  not something introduced this session — noted, not fixed. The air sensor
  (SHT41, genuine I2C) would be the correct choice for actually
  demonstrating fault detection works, and was not retested before the
  session ended.
- **Phase 5 (power-cycle recovery)** — deferred to the next session, not
  started.

### 2026-08-06 — deployment-epoch desync root-caused and fixed, live on COM4

Continuation of the 2026-08-05 open item, on the same physical fleet
(ENV_6C0A80 kept untouched as a control throughout; ENV_D13F98 as the
subject). Firmware `build=71b8e3a-dirty` at the start of the session.

**The candidate mechanisms guessed on 2026-08-05 were wrong, and one of them
was disproven by reading code before any hardware was touched.** A chain
starting from `handleNodeHello()` promoting a merely-PAIRED node to DEPLOYED
was proposed, then ruled out: `sendNodeHello()`'s three call sites in
`node/firmware/src/main.cpp` are gated on `rtcSynced`, an open sync session,
or `STATE_DEPLOYED`, and `PAIR_NODE` clears all of those — so a cleanly
paired, undeployed node sends no HELLO at all. Any explanation requiring one
assumed its own conclusion. Kept as a documented dead end rather than
deleted, since the reasoning that ruled it out is itself useful.

**Also established by reading code, then confirmed by hardware, and worth
recording since it corrects a wrong assumption from 2026-08-05:** unpairing a
deployed node does **not** reset its epoch to 0. `endDeploymentForUnpair()`
(`deployment_epoch.cpp`) only sets `endedUnix` on the epoch that was already
active — the epoch number itself is untouched. Only `beginNewDeployment()`
advances it. So the "Ended, no epoch" symptom was never "this node was never
deployed" — it was something actively producing epoch 0 on a **redeploy**.

**Root cause, confirmed on hardware:** `deploySelectedNodes()`
(`mothership/firmware/v2/src/comms/espnow_config.cpp`) built the
`DEPLOY_NODE` command's epoch field from `node.deploymentEpoch` — a RAM-only
field on the `NodeInfo` registry entry — instead of asking the deployment
store directly. That field is deliberately zeroed whenever a `NodeInfo`
record is freshly created (`node_registry.cpp`: "a newly discovered node has
no deployment until Start commits one"), and ENV_D13F98 got a **fresh**
record mid-session because its own periodic `NODE_STATUS` broadcast
re-registered it as UNPAIRED before it was ever paired through the UI. The
stale zero was never refreshed by the time the wizard's Start deployment
dispatched the command. Captured directly on both consoles:

- Hub: `[DEPLOY] ENV_D13F98: epoch=0 cfgV=2 wakeMin=1 syncMin=18 phase=...`
- Node: `[DEPLOY] epoch=0 new=1 persisted=1`

The following sync window proved the consequence live: the node was
genuinely recording and converging (`[SNAP] ... nodeId=ENV_D13F98 seq=178`,
`CONFIG_ACK converged: ENV_D13F98 v2 (ACTIVE)`, a real alarm-armed sleep
cycle on its own console) while the Field UI badge read "Ended" — the
2026-08-05 symptom reproduced on demand, with the exact mechanism named.

A secondary discovery along the way, load-bearing for interpreting the
above: **the current wizard has no UI path that pairs a node without also
deploying it.** `start_deployment`'s handler calls `pairNode()` and then
falls straight through to committing the deployment in the same request
whenever a node starts UNPAIRED — there is no separate "just pair" action
anywhere in `config_server.cpp`. `PAIRED` exists in the state enum and is
counted by `/api/live`, but through the real wizard it is never a state a
node rests in; it is crossed for a fraction of a second inside one HTTP
handler. This reframed the investigation: the drift could not be an
operator accidentally stopping short of redeploying, because that stopping
point does not exist in the UI.

**Fix:** `deploySelectedNodes()` now reads the epoch via
`deploymentFindByNodeId(node.nodeId.c_str())` — the deployment store, the
actual authority — falling back to the registry mirror only if the store has
no slot for that node. One call site, no changes to `targetState` handling,
unpair, or either recovery path. For a normal deploy this produces identical
output to before, since the store already holds the fresh epoch by the time
this runs; it only changes behavior for the stale-mirror case above.

**Hardware evidence, this session (COM3 + COM4, both live serial capture):**

- Build clean (`mothership-v1-main`, exit 0), flashed to COM4.
- Start a new deployment clicked directly on the already-registered node
  (deliberately not via a fresh unpair/re-pair, to isolate the
  `beginNewDeployment()` path from the registry-recreation confound above).
  Hub: `[DEPLOY] ENV_D13F98 started deployment 3 as 002` — the
  `beginNewDeployment()` success line, absent from every capture before the
  fix — immediately followed by `[DEPLOY] ENV_D13F98: epoch=3 cfgV=2 ...`,
  now matching.
  Node: `[DEPLOY] epoch=3 new=1 persisted=1`.
- Full sync window completed cleanly: real snapshots `seq=179`-`194`,
  repeated `CONFIG_ACK converged: ENV_D13F98 v2 (ACTIVE)`, node armed its
  next alarms and slept normally. Field UI stayed correct throughout — no
  "Ended" badge.
- A second, independent sync cycle later in the session also came back
  clean: node still correctly on deployment 3, no drift back to "Ended".
- **Not independently re-executed after the fix:** the exact original
  trigger sequence (unpair while deployed, wait for the node's own
  `NODE_STATUS` to re-register it, re-pair, then deploy). The fix is
  trigger-agnostic — it changes what `deploySelectedNodes()` reads
  regardless of why the registry mirror was stale — so it should hold for
  that sequence too, but this was not independently confirmed on hardware
  this session.

**Also found, not fixed — a separate, low-priority cosmetic bug noted for
the record:** during the unpair-only hardware test (before the epoch bug was
found), `loop()` (`node/firmware/src/main.cpp`) snapshots
`NodeState st = currentNodeState();` once at the top of each iteration. When
a pending UNPAIR is serviced later in that same iteration, `st` is not
recomputed, so branch logic and log output further down in that pass can
still act on a value that says DEPLOYED for one iteration after the node has
actually gone UNPAIRED. Observed as a stray `state=2` in the heartbeat line
and a `🟢 Deployed — work happens on each DS3231 alarm.` print immediately
after `[STATE] ... -> UNPAIRED`. The underlying flags (`deployedFlag`,
`mothershipMAC`) and the DS3231 alarm disarm calls are all correct and run
inside the same UNPAIR block before this — confirmed no functional effect,
display-only.

**Field UI changes made alongside the fix, same session:** since the
investigation established `PAIRED` is not a reachable resting state through
the current wizard, the "Connected" tile was removed from the dashboard's
three-box fleet summary (`config_server.cpp`), along with the now-dead
`pairedNodes` variable and its JS updater. The underlying `PAIRED` enum
value, `/api/live`'s `connected` JSON field, and the per-node "Connected"
chips elsewhere (station list, node detail page) were deliberately left
alone — they still reflect a real, if now rarely-observed, state, and
weren't part of what this pass touched. The KPI grid's CSS
(`.stats{grid-template-columns:1fr 1fr 1fr}`) was a fixed three-column
layout; added a `.stats--kpi{grid-template-columns:1fr 1fr}` override,
scoped to that one strip, so the remaining Active/New tiles fill the row
instead of leaving a blank third column. Both build-verified and flashed to
COM4 together with the epoch fix; confirmed visually correct in the field
session above.

**Not yet done:** committing these changes (staged, not committed pending
a decision on grouping); deciding whether to annotate the three historical
V1-era docs that reference the now-removed `mothership/firmware/v1/` tree
(unrelated repo-cleanup task done the same session, not a field-operation
change, so not otherwise recorded in this document).
