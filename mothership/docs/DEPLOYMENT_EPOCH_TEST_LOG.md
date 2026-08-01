# Deployment epochs — production-readiness test log (FieldHub firmware)

Running log of what has actually been executed against the deployment-epoch
firmware, and what has not. The backend/frontend half of this log lives in the
FieldMeshDashboard repo at `docs/node-deployment-epoch-test-log.md`; the two are
meant to be read together, because the feature is one cross-repo contract.

**Rule for this document: only record what was run.** Compilation is not test
execution and must never be logged as though it were. Anything predicted or
reasoned-about belongs in the roadmap section, not in the results.

Branch: `feat/node-deployment-epochs` (both repos).

---

## Status summary

| Phase | Scope | Status |
|---|---|---|
| 0 | Backend pre-flight against production data | **PASS** — see dashboard log |
| 1 | Backend migration rehearsal on a clone | **PASS** — see dashboard log |
| 2 | **Firmware on-device bench tests** | **PASS (2a)** — 2026-08-01, real hardware. 2b/2c outstanding |
| 3 | End-to-end hardware → hub → backend → dashboard | **NOT RUN** — blocking |
| 4 | Staged rollout | DB migrated; **Edge Function NOT deployed** — blocks firmware |
| 5 | Post-deploy monitoring | **NOT STARTED** |

---

## What has been executed

### Compilation only — 2026-07-31

All four PlatformIO environments build clean with no warnings attributable to
this work.

| Environment | Result | Notes |
|---|---|---|
| `mothership-v1-main` | SUCCESS | Flash **85.2%** (1,339,309 / 1,572,864 B), RAM 18.5% |
| `mothership-v2-test-deployment-epoch` | SUCCESS | Flash 22.9% |
| `mothership-v2-test-upload-queue` | SUCCESS | — |
| `mothership-v2-test-spectral-pipeline` | SUCCESS | — |

```
pio run -e mothership-v1-main \
        -e mothership-v2-test-deployment-epoch \
        -e mothership-v2-test-upload-queue \
        -e mothership-v2-test-spectral-pipeline
```

**These images have never been flashed.** Every one of them asserts on device
and prints `RESULT|SUMMARY|n/n|OVERALL:PASS`; none of those assertions has
executed. `test_deployment_epoch` in particular writes `/deploy.bin` to
LittleFS, so its crash-safety and round-trip claims are entirely unverified on
real flash.

Flash headroom is worth watching: the main image is at 85.2% of the 1.5 MB app
partition. Confirm the A/B OTA scheme still has room for a cloud update of this
build before relying on remote flashing.

---

## Defects found and fixed during review of this work

Found by reading, not by running — which is precisely why Phase 2 still
matters.

| Severity | Defect |
|---|---|
| P2 | `beginNewDeployment` ignored the result of its rollback commit. If that commit failed, flash still held the pending `DEPLOY_OP_START` and recovery would finish it on the next boot — publishing a deployment the operator had just been told did not happen, and applying a staged number they may since have given to another node. |
| P2 | `ensureFirstDeployment` never published the staged identity. A deployment committed through that path was archived with a blank number and name, and could never be corrected: the node then has an ACTIVE deployment, which makes both "Start new deployment" and a further staged edit unreachable. |
| P2 | An ENDED node is still `state == DEPLOYED`, so identity edits were refused and the setup wizard answered "Saved" having written nothing. |
| P2 | An ended deployment was labelled **"Paused"** in all three state renderers (stations list, node detail header, live-refresh JS), because End queues STANDBY. Pause and End have different consequences and must not share a word. The live-refresh path needed a new `deploymentEnded` field or it repainted "Paused" on the next poll, undoing the server-rendered chip. |
| P2 | `statusRejected` was set and never read: a suppressed status object silently costs a whole session of deployment events, with nothing surfaced to the operator. |
| P3 | `deploymentStoreWasCreatedFresh()` documented as "no `/deploy.bin` and no `/deploy.bak`" but also true when both exist and fail checksum. Comment corrected; behaviour degrades safely (readings quarantine rather than merging into an old site). |

A regression test for the staged-identity fix was added to
`tests/test_deployment_epoch.cpp`
(`testFirstDeploymentPublishesStagedIdentity`). **It compiles; it has not run.**

---

## Phase 2 — Firmware on the bench (BLOCKING, not started)

### 2a. Run the on-device suites

```
pio run -t upload -e mothership-v2-test-deployment-epoch   # writes /deploy.bin
pio run -t upload -e mothership-v2-test-upload-queue
pio run -t upload -e mothership-v2-test-spectral-pipeline
pio run -t upload -e mothership-v2-test-node-config-control
```

**RUN 2026-08-01 on the bench FieldHub (COM4, CH340).** Board reset via DTR/RTS
before each capture, because these sketches print once from `setup()` and a
monitor attached after boot misses everything.

| Environment | Assertions | Result | Notes |
|---|---|---|---|
| `test-deployment-epoch` | **150/150** | **OVERALL:PASS** | real LittleFS, real store |
| `test-upload-queue` | **27/27** | **OVERALL:PASS** | 30→31 schema migration |
| `test-spectral-pipeline` | — | **RESULT: PASS** | emitted `"deploymentEpoch":7` as column 31 |
| `test-node-config-control` | — | **CRASHES** | pre-existing, unrelated — see below |

### The 10 initial failures were all test defects, not firmware defects

First-ever hardware run returned `131/141 OVERALL:FAIL`. Every failure was in the
test, and the firmware behaviour each one contradicted turned out to be correct
and deliberate. Recorded because "the test was wrong" is a conclusion that must
be justified, not assumed.

| Cluster | Failures | Diagnosis |
|---|---|---|
| Outbox-full | 6 | Filler events were built with `DeploymentEvent e{}`, leaving `deploymentStartedUnix = 0`. `deploymentOutboxUpsert()` **correctly refuses** a zero start (the backend records it as a conflict and never acks it, so it would wedge the outbox forever). 34 fillers were rejected, the outbox never filled, and the assertions were measuring an ordinary successful Start. |
| Failed-End | 2 | Same zero-start fixture bug, plus a deeper one: the node's own epoch-1 event was still queued from the preceding `beginNewDeployment`, so End's event has the same deterministic id and **upserts in place**, needing no free slot. That is correct — ending a deployment whose Start event is still queued should just stamp the end onto it. The fixture now clears the event first, simulating a backend ack. |
| Rollback | 3 | A single `gConfigApplyOk` flag failed both the ACTIVE queue *and* the compensating STANDBY queue, forcing the code down its documented "leave START pending for recovery" branch while the test asserted clean-unwind semantics. The firmware is right: a failed apply can mean *durably persisted but unacknowledged*, so the node may still converge to ACTIVE, and unwinding would leave it recording against a discarded epoch. |
| End timestamp | 1 | `json.indexOf("\"deploymentEndedUnix\":0,") < 0` scanned the whole outbox. The **active** epoch-2 event must report `ended:0`, so the assertion failed on correct output. Now scoped to the epoch-1 object. |

Fixes added coverage rather than just silencing: `gStandbyApplyOk` was split out
so both failure branches are reachable, and
`testStartLeavesIntentPendingWhenCompensationFails` now asserts the
pending-retained path end to end, including that recovery completes the Start
forward and publishes the staged number on the next boot. 141 → 150 assertions.

### `test-node-config-control` crashes — pre-existing, NOT from this work

```
### FieldMesh node config control test ###
Guru Meditation Error: Core 1 panic'ed (Double exception).
EXCCAUSE: 0x00000002  EXCVADDR: 0xffffffe0   [repeating backtrace]
```

Faults immediately after the banner, before any assertion runs — i.e. inside
`dispatcherInit()`, which this work does not touch.

**Verified by A/B on hardware:** restoring `node_config_control.cpp` to its
pre-change version (`git show ea98b05^:...`) and reflashing produced the
identical crash. The deployment-epoch change is not the cause.

Strongest lead for whoever triages it: `dispatcherInit()` value-initialises two
`DispatcherStateRecord` locals **on the stack**, and the repeating backtrace plus
double-exception is the classic signature of blowing the 8 KB Arduino loop-task
stack — the same failure mode `deployment_store.cpp` documents avoiding by
hashing in place instead of copying a ~9.3 KB record. Note the production
firmware calls `dispatcherInit()` at every boot and has run in the field for
days without incident, so this is most likely specific to the cut-down test
environment rather than a production defect. **Confirm that by watching a normal
boot of `mothership-v1-main` on this board before drawing conclusions.**

**Coverage gap this leaves:** the one-line guard added to
`controlResolveBackendNodeConfig` (refuse a backend PAUSE/RESUME once
`deploymentEndedUnix != 0`) is currently unverified on device. It is defended at
three other layers — the FieldHub detail form, the batch action, and the
dashboard's `canBatchControlNode` — and the backend independently sets
`nodes.state = 'ENDED'`, so the lifecycle is not open. But this specific guard
rests on code review alone.

Pay particular attention to these named cases, which cover the riskiest logic:

- `interrupted start: epoch completed forward to 2`
- `interrupted start failure: START remains pending`
- `slot rollback: *` (whole-slot restore after a failed config queue)
- `outbox full: Start refused`
- `first deploy: staged number published to node_meta` (the new regression test)
- `tornWriteRecovery` / `survivesReboot`

### 2b. Power-cut crash-safety soak

The atomic-commit claim is the foundation of the whole store and **no unit test
can reach it**. `deploymentStoreCommit()` does
`write .tmp → rename .bin→.bak → rename .tmp→.bin → remove .bak`; the assertion
is that at every instant at least one complete record exists on flash.

Rig: relay-cut power at random during a continuous Start/End loop. After each
cycle, confirm on reboot that:

- `/deploy.bin` or `/deploy.bak` loads with a valid checksum,
- the epoch is either the old value or the new one — never absent, never mixed,
- `deploymentRecoverPending()` reports and completes any in-flight transition,
- no `[DEPLOY] Store unavailable` or `slot table full` appears.

Target a few hundred cycles. Record cycles run, interruptions, and failures.

| Cycles | Interruptions | Corrupt records | Notes |
|---|---|---|---|
| | | | |

### 2e. Production firmware on the bench hub — 2026-08-01 (PASS)

Flashed `mothership-v1-main` over USB after the suites. NVS and LittleFS both
survived, as predicted: no re-pairing, no lost cursor, no lost sync anchor.

```
[fwid] role=mothership ver=0.1.0 build=253f6c7 hw=mothership-v1 proto=2 idv=1
[FW] V2 snapshot decode; CSV schema=31; spectral metadata IDs=1109-1113
[REG] Loading 3 paired/deployed nodes from NVS
[REG] Load complete: 3 restored, 0 skipped
[DEPLOY] Store ready (gen=1, outbox=0, fresh=1)
[DEPLOY] Seeded ENV_D13F98 as epoch 1 (since 1785180891)
[DEPLOY] Seeded ENV_6C0AA0 as epoch 1 (since 1785180801)
[DEPLOY] Seeded ENV_6C0A80 as epoch 1 (since 1785180951)
[SYNC] Loaded anchor sync_anchor_a generation=6 phase=1785527760 interval=360 mode=1
[RTC] Alarm 1 armed for 2026-08-01 07:55:50 (phase-aligned, in 2424 sec)
```

**Hub-side seeding matches the backend backfill exactly.** `1785180801` is
2026-07-27 19:33:21Z, which is precisely the `started_at` the migration wrote to
`node_deployments` for Node 1 (same for 002 and 003). The hub's first deployment
events will therefore upsert onto the existing `BACKFILL` rows rather than
creating duplicates or tripping the active-number guard.

Also settles the open question from `test-node-config-control`: production
firmware calls `dispatcherInit()` on this same board and boots cleanly, so that
crash is specific to the cut-down test environment and is not a production
defect.

Sync anchor survived the reflash (`generation=6` loaded, saved as `7`), so the
hub stayed on its existing 6-hourly phase — next slot 09:56 Berlin, alarm armed
10 s early as the usual pre-roll.

### 2d. Deployment-store residue — FOUND AND FIXED

Running the epoch suite on a hub is not free. The suite writes a real
`/deploy.bin`, and production firmware then loaded it verbatim:

```
[DEPLOY] Store ready (gen=4, outbox=1, fresh=0)
```

`fresh=0` = loaded, not created. `outbox=1` = a queued fixture event. The last
test leaves an `ENV_A2` event claiming number **002**, which the live
`ENV_D13F98` deployment holds. On sync the backend would reject it against
`node_deployments_active_number_idx`, return CONFLICT, never acknowledge it, and
the hub would retry forever — a permanent conflict chip and a wedged outbox slot.

Caught before the hub synced. `mothership-v2-wipe-deploy-store` removed the
three store files and asserted `/datalog.csv` was byte-identical before and
after (367 bytes, header only). Production firmware then rebuilt the store
`fresh=1, outbox=0` as shown above.

**Always run the wipe env between the epoch suite and production firmware.**

### 2c. CSV schema drain (30 → 31 columns)

> **Not exercised against a real field buffer, and there is no remaining
> opportunity to do so.** `test_upload_queue` rewrote `/datalog.csv`, so by the
> time production firmware ran, the file was a bare 31-column header (367 bytes)
> with an empty queue and the legacy-drain path never triggered. The `rows=3432`
> in the cursor log is the lifetime uploaded counter, not pending rows.
>
> The fleet is **one hub** (`Backyard_Hub`), and it has now been upgraded, so no
> device remains that could upgrade from pre-epoch firmware carrying a genuine
> 30-column buffer. Any hub added later starts on the current firmware and never
> has a legacy buffer either.
>
> **Residual risk: low, and lower than this heading suggests.** The drain logic
> itself *is* covered on hardware — `test_upload_queue` passed 27/27 on this
> board and includes legacy 25- and 30-column headers, both empty and carrying
> rows, asserting that queued rows are preserved rather than deleted and that
> the header upgrades only once the queue drains. What was never observed is
> that logic meeting a genuine field buffer. And in the event it mattered here,
> it could not have: the hub's queue was already empty at upgrade, so there were
> no legacy rows to lose.

Flash the new firmware onto a hub carrying a **real** 30-column
`/datalog.csv` with queued rows. This is the upgrade path every existing hub
will take, and getting it wrong loses buffered field data.

Confirm:

- [ ] queued rows are preserved, not deleted or shifted
- [ ] new rows append with 31 columns under the still-legacy header
- [ ] `deploymentTrackingVersion` is **absent** from the upload while unstamped rows remain
- [ ] Start is refused with the legacy-backlog message until the queue drains
- [ ] `purgeUploaded()` upgrades the header to 31 columns once drained
- [ ] `deploymentTrackingVersion: 1` appears only after that

| Item | Result | Date |
|---|---|---|
| | | |

### 2d. OTA headroom

- [ ] Confirm an A/B cloud OTA of the 85.2%-full main image fits the partition scheme.

---

## Phase 3 — End-to-end on the bench (BLOCKING, not started)

One FieldHub, two nodes, pointed at a **clone** (not production). Walk all seven
lifecycle journeys and verify each **against the database, not the FieldHub
UI** — the UI reports intent, the database reports truth.

| # | Journey | Database assertion | Result |
|---|---|---|---|
| 1 | End Node 1 after a long deployment | `node_deployments.ended_at` set; receipt `APPLIED`; hub chip reads **Ended**, not "Paused" | |
| 2 | Move the same hardware to position Y | no new rows until Start | |
| 3 | Start a new deployment, reusing number 1 | new row `epoch = 2`; epoch-1 labels unchanged | |
| 4 | Attempt to Resume an ended deployment | refused at hub form, batch action, backend RPC and dashboard | |
| 5 | Number already held by another active deployment | receipt `CONFLICT`, event stays queued, clears itself when the other End lands | |
| 6 | Distinguish Pause / End / Remove | three distinct outcomes; Remove archives before removing | |
| 7 | Interrupted action (pull power mid-Start) | recovery completes forward; **exactly one** epoch increment | |

Two cases deserve explicit attention because they were bugs fixed in review and
only hardware proves them:

- [ ] Deploy a node through the **first-deployment path**; confirm the archived
      record carries the operator's number and name (was blank).
- [ ] Confirm a node with **no deployment record** still shows current values on
      the dashboard (was disappearing on every ingest).

---

## Cross-repo contract reference

| Firmware action | Uploaded | Backend result | Presentation |
|---|---|---|---|
| Start (wizard) | epoch++, outbox event, readings stamped col 31 | `node_deployments` insert | Green "Deployment N · 001 · since <date>" |
| Start with a held number | same event | unique index → `CONFLICT` receipt, not acked | Red chip with the backend's reason; wizard 409 → step 1 |
| Pause | `desiredTargetState 3` | `recording_paused`, deployment untouched | "Paused"; deployment still active |
| End | `deploymentEndedUnix > 0`, then STANDBY | `ended_at` set and frozen; `nodes.state = 'ENDED'` | **"Ended"** chip + "Start new deployment"; leaves live fleet |
| End then Start before one upload | two events, one payload | ends applied first, number released then reclaimed | Both epochs visible |
| Readings after End | epoch N, timestamp > `ended_at` | `POST_DEPLOYMENT`, retained | Excluded from charts/exports; counted as quarantined |
| Dead RTC (yr 2000 / 2165) | epoch stamped, bad timestamp | `IMPLAUSIBLE_TIME`; latest repointed | Excluded; never pins "last reading" |
| Remove / Unpair | End captured **first**; removal abandoned if it fails | deployment archived with identity | Archived list keeps number, name, coords, range |
| Cloud Pause/Resume on ended node | — | `controlResolveBackendNodeConfig` returns false | Dashboard disables the control with a reason |
| Legacy hub (no epoch column) | no `deploymentEpoch` key | resolved only if exactly one window contains it | Verified working — dashboard log R1 |

---

## Deployment ordering

**The backend must ship before the firmware.** New firmware against an old
backend is a silent trap: the old Edge Function ignores
`status.deploymentEvents`, so no acknowledgements ever return, the hub's
16-slot outbox fills, and every subsequent Start and End is refused with
`DEPLOY_ERR_OUTBOX_FULL` — locking the operator out of the lifecycle in the
field.

The reverse is safe by design and now verified: the dashboard log's R1 replay
confirms a pre-epoch upload still ingests and resolves correctly against the
migrated schema.
