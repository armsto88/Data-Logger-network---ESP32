# Firmware Test Backlog — 2026-09-30

A read-only scan of the node and hub firmware, docs and git history for things that are
untested, unverified on hardware, or known-open. **This is a scan of the repo's own records, not a
test run.** Where the docs may be stale, it says so. Priorities are suggestions.

Legend: **[B]** blocking for the local-sensor-collection work (`MOTHERSHIP_LOCAL_SENSOR_COLLECTION_DESIGN_NOTE.md`),
**[H]** high, **[M]** medium, **[L]** low.

## 1. Scalability / sync rendezvous (the work you were remembering)

Source: `MOTHERSHIP_SYNC_RENDEZVOUS_DESIGN_NOTE.md` — **currently untracked in git** (commit it or
it can be lost).

Finding: the 4-node deployment loses 31–38% of sync windows; the aperture is `25 − t_bootH` seconds
(hub boot latency) and drain caps out around 2–4 nodes. **The note is a design only; the code still
has the old constants** — checked: `kJoinPostSlotSec = 15`, `kJoinCapMs = 45000`, `kGrantQuota = 4`
in hub `main.cpp:481–486`, and the 10 s pre-wake lead at `rtc_alarm.cpp:225`. None of Stage 0 is
implemented.

- **[B/H] Confirming experiment (§10.4).** Add a bench-only `HUB_TEST_BOOT_DELAY_MS`, sweep 0→45 s
  against the real fleet, plot attendance. Cheapest high-information test; it confirms or kills the
  whole hypothesis before any protocol work.
- **[B/H] Stage 0 hub-only fixes + instrumentation (§8, §9).** Pre-wake lead 10→45 s; join end
  anchored to slot+25 s (and `kJoinCapMs` → 75000 or it silently truncates); keep beaconing and
  accepting joiners through the whole window; defer purge/SD init past the window. The grant loop
  must become index-based (appending to `responders` during a range-for is UB). Do **not** move
  `deploymentBootstrap()` after the window.
- **[H] Rendezvous logic is currently untestable** — it is inline in a ~400-line function. The note
  says the extraction into `sync_rendezvous.{cpp,h}` is a prerequisite. Missing envs:
  `mothership-v2-test-sync-rendezvous`, `esp32wroom-node-sync-listen`,
  `esp32wroom-node-queue-drain`, and a mock-fleet extension of `bringup_mock_mothership_sync.cpp`.
- **[H] Drain rate (Stage 2).** `local_queue::popN(k)` single-commit; requires **verifying hub-side
  `seqNum` de-duplication first** (not yet verified). Expected ~7× capacity.
- **[M] `inferredWakeIntervalMin` reads 0 in production** (`espnow_manager.cpp:904`) — suspected
  broken, undiagnosed (§9.4).
- **[M] Fleet size is unproven beyond 3 nodes** (HardwareX manuscript §7.5). The live fleet is
  ~3–4 nodes on one hub. Any claim above that is arithmetic, not measurement; >12 needs real boards.
- **[B] Sequencing.** The local-collection design touches the same hub wake/alarm code
  (`rtc_alarm.cpp`, `handleSyncWake`). Decide which lands first.

## 2. Hub — field acceptance still open (`FIELDHUB_STANDALONE_OPERATION.md`)

The doc's "Required field acceptance before release" lists 7 gates and says build success does
not satisfy them. I found no record that all seven were completed; the 2026-08 entries cover parts.
Re-check against your own records:

1. **[H]** Upgrade a hub with a genuine v1 deployment store; epochs/lifecycle survive two boots.
2. **[H]** Epoch + upload-queue test images on hardware, then the wipe image (bench only — it
   destroys the deployment store).
3. **[H]** Local setup with no key/SIM/backend; commission, end, redeploy, reboot, download CSVs.
4. **[H]** Fill LittleFS past the retention threshold and power-cycle mid-rewrite.
5. **[M]** SD insert/remove/fill and power interruption around writes and End events.
6. **[M]** Custom HTTPS endpoint: success, timeout, lost response, 4xx, 5xx; only durable 2xx
   advances delivery.
7. **[M]** Local-only → provision cloud: retained readings backfill to the right deployments.

Also recorded as not yet observed on hardware (2026-08-07 entry):

- `handleShutdown()`'s `gCloudConnectedThisSession` / pending-bytes branch — logic-reviewed only.
- The parallel gap in `handleSetTransmission()` — identified and deliberately left open.
- **[M] `updateStaleNodeStatus()`** uses the millis-based `lastSeen`, zero after every boot, so
  `missed` is always 0 and nothing is ever marked stale. Pre-existing, untouched. Matters for
  the local-collection registry work (stale logic must skip the hub).
- QR/camera onboarding copy has no human-factors check.

Recent commits (d0a75a2 deploy wizard re-pair, 73a8642 upload body, 89db580, 7b8de3d Finish
button) are UI/upload fixes; I did not find bench-evidence entries for the last few in the docs.
**[M]** Confirm each on hardware.

## 3. Deployment-epoch test log (`DEPLOYMENT_EPOCH_TEST_LOG.md`) — unchecked boxes

- **[H] 2c — legacy 30-column upgrade with a real buffered `/datalog.csv`.** The path every
  existing hub will take; getting it wrong loses field data. Six unchecked assertions (rows
  preserved, 31-col append under legacy header, `deploymentTrackingVersion` absent until drain,
  Start refused until drain, header upgrade on `purgeUploaded()`, version appears only after).
  Covered by `test_upload_queue` 27/27 but never met a genuine field buffer.
- **[H] 2d — OTA headroom.** Confirm an A/B cloud OTA of the 85.2%-full main image fits the
  partition scheme. **This directly limits how much sensor code can be added to the hub.**
- **[M] Phase 4 J-cases 1–7** (end / move / new deployment / resume refused / number conflict /
  pause-end-remove / interrupted action) — results columns are empty in the log.
- **[M]** Two hardware-only bug proofs: first-deployment path carries operator number/name; node
  with no deployment record still shows current values.

## 4. Node

- **[B/H] Reed-cup wind is untested end to end.** Passes standalone only; "not included in the
  integrated multi-sensor run; queue → sync → CSV path untested" (manuscript Table 2). It is the
  sensor the hub will use, so prove it on the node first.
- **[H] Stuck-awake fix not field-confirmed** (commit `c584a00`). Removed the legacy sync fallback
  that could strand `PWR_HOLD`. The doc says "the real test is time": watch for `[PWR_HOLD] release
  deferred` spam over further sessions.
- **[M] Sensor-fault detection** — `522aa99` persisted the fault state, but the 2026-08-05 note
  said there was no UI and item 12 was never verified. Confirm both after the fix.
- **[M] `ADDING_A_NEW_SENSOR_CHECKLIST.md`** wants verification "on a real sync window, not just a
  bench test" — apply to any sensor changes, including the shared-library extraction.
- **[L] Node OTA** has no delivery path (by design); signed install/rollback is bench-proven only.

## 5. System-level claims that are not yet evidence

From the HardwareX manuscript limitations (2026-07-26); still true unless newer data exists:

- No multi-week deployment; cloud path demonstrated over a single ~2 h 41 min session.
- **Battery life never measured.** Matters for adding extra hub wakes.
- Remotely initiated hub self-update implemented but never completed on live hardware.
- Soil moisture is uncalibrated voltage; no calibration curve shipped.
- Sensor accuracy vs. reference instruments not established.
- ESP-NOW range: 99.7% at 30 m LoS, 84–87% at 100 m obstructed (bench).
- Printed sensor housings never physically fabricated/tested.

## 6. Housekeeping observed during the scan

- Uncommitted: `MOTHERSHIP_SYNC_RENDEZVOUS_DESIGN_NOTE.md` (untracked), `.vscode/extensions.json`.
- The manuscript says hub microSD logging is "not implemented" while the hub code calls
  `sdLogCSVRow` in `processSnapshot()`; the docs and code disagree (older manuscript text, likely
  stale). Check before relying on SD as a second copy.
- `FIELDHUB_STANDALONE_OPERATION.md` records changes as "uncommitted / not yet committed" in the
  2026-08 entries; git log shows later commits, so the doc is probably out of date on that point.

## 7. Suggested order

1. Commit the rendezvous note. Run the §1 confirming experiment.
2. Clear the **[H]** epoch-log items 2c and 2d (2d gates hub flash budget).
3. Prove the reed sensor end to end on a node.
4. Decide rendezvous Stage 0 vs. local collection ordering.
5. Then start the local-collection milestones.
