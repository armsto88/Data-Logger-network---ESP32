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
| 2 | **Firmware on-device bench tests** | **NOT RUN** — blocking |
| 3 | End-to-end hardware → hub → backend → dashboard | **NOT RUN** — blocking |
| 4 | Staged rollout | **NOT STARTED** |
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

Capture the serial `RESULT|SUMMARY|n/n|OVERALL:` line for each and paste below.

| Environment | Assertions | Result | Date |
|---|---|---|---|
| `test-deployment-epoch` | | | |
| `test-upload-queue` | | | |
| `test-spectral-pipeline` | | | |
| `test-node-config-control` | | | |

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

### 2c. CSV schema drain (30 → 31 columns)

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
