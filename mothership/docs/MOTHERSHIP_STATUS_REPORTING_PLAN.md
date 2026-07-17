# Mothership Cloud Status Reporting Plan — firmware & control fields

**Status:** **all three tiers implemented + compiling (2026-07-17)** — see the per-tier notes below. Not yet run on hardware; needs a mothership reflash (Tiers 1–3 receive/emit) and a node reflash (Tier 3 send), plus Supabase schema columns for the new fields. **Created:** 2026-07-17.
**Goal:** make the mothership report *everything the dashboard needs* into its cloud upload — most importantly a real **firmware version/identity** for the mothership and each node, plus OTA state, control revision, and desired-vs-applied node config.
**Companion docs:** `docs/FIELDMESH_FIRMWARE_DASHBOARD_INTEGRATION_BRIEF.md` (the contract this feeds), `docs/FIELDMESH_CLOUD_UPLOAD_PROTOCOL.md`, `docs/FIELDMESH_SUPABASE_SCHEMA_CONFIRMED.md`, `FIELDMESH_OTA_FIRMWARE_UPDATE_PLAN.md` §13.

---

## 1. Why

The dashboard brief (§2, §5) needs to display firmware info and config convergence. Today the upload underreports it:
- The mothership reports `meta.firmwareVersion = "v1.0.0"` (the legacy `FW_VERSION` constant) — **not** the real semantic version. The rich identity (`0.1.0`, `hwTarget`, `releaseId`, `protocolVersion`) exists in firmware (`firmware_identity.h`) but never reaches the cloud.
- The dispatcher's **control revision** and recent command results are only on the local `/api/control` endpoint, not in the cloud.
- Per node, only the **applied** `configVersion` is sent — not the **desired** one, so the dashboard can't show "change pending / converged".
- Per node, there is **no firmware version / hardware revision / OTA capability** at all (needs a node→mothership report that doesn't exist yet).

## 2. Where the payload is built (real touch points)

- **`mothership/firmware/v2/src/storage/json_payload.{h,cpp}`** — `buildJsonUpload()` emits the batch `{readings[], meta{}, status{}}`. Mothership-level telemetry comes in via the **`StatusContext`** struct (already carries `firmwareVersion`, `firmwareBuild`, fleet counts, `nodesJson`, `modemJson`, `diagnosticsJson`, …). New mothership fields go here.
- **`mothership/firmware/v2/src/config/node_registry.cpp`** — `buildNodesStatusJson()` builds the pre-rendered `status.nodes[]` array (fed into `StatusContext.nodesJson`). New per-node fields go here.
- **`mothership/firmware/v2/src/main.cpp`** — `handleSyncWake()` populates `StatusContext` and calls `buildJsonUpload()`. Wire the new values in here.
- **`node/firmware/shared/firmware_identity.h`** — the identity source. **`command_dispatcher.h`** — `dispatcherRevision()` / `dispatcherRecentResults()`. **`ota_installer.h`** — `otaIsPendingVerify()` + running partition for OTA state.
- Follow the existing **pre-built sub-object pattern** (`nodesJson`, `modemJson`): build a `firmwareJson`/`controlJson` string and hand it to `StatusContext`, so `json_payload.cpp` stays a simple assembler.

## 3. Target fields ("everything important")

### 3.1 Mothership block (new `status.firmware` + `status.control`)
| Field | Source | Notes |
|---|---|---|
| `firmware.version` | `FW_SEMVER` via `fwIdentity()` | the REAL semver (replaces the "v1.0.0" story) |
| `firmware.buildId` | `FW_GIT` | git short hash + `-dirty` |
| `firmware.hwTarget` | `fwIdentity()` | `mothership-v1` |
| `firmware.protocolVersion` | `NODE_PROTOCOL_VERSION` | currently 2 |
| `firmware.releaseId` | OTA state store | null until a signed release is installed |
| `firmware.runningSlot` | `esp_ota_get_running_partition()->label` | `app0`/`app1` |
| `firmware.otaState` | `mothershipOtaGetStatus()` / `esp_ota_get_state_partition` | `idle`/`pending_verify`/`confirmed` |
| `firmware.lastOtaResult` + `lastOtaReason` | OTA state store | after an update/rollback |
| `control.stateRevision` | `dispatcherRevision()` | monotonic, NVS-persisted |
| `control.lastChangeSource` | dispatcher | `LOCAL_UI`/`DASHBOARD` |
| `control.results[]` | `dispatcherRecentResults()` | same shape as `/api/control` |
| `control.lastCommandCursor` | (when ingestion lands) | backend cursor |

Keep the legacy `meta.firmwareVersion` for back-compat during migration, but populate it from `FW_SEMVER` too so it stops lying.

### 3.2 Per-node additions (in `buildNodesStatusJson`)
| Field | Source | Tier |
|---|---|---|
| `desiredConfigVersion` | `getDesiredConfig(nodeId).configVersion` | 2 (no node change) |
| `desiredTargetState` | `getDesiredConfig(nodeId).targetState` (0/1/2/3) | 2 |
| `desiredWakeIntervalMin` | `getDesiredConfig(nodeId).wakeIntervalMin` | 2 |
| `firmwareVersion` | node `FW_CAPS` report | 3 (needs node change) |
| `hardwareRevision` | node `FW_CAPS` report | 3 |
| `otaCapable` / `otaState` / `otaTargetVersion` | node `FW_CAPS` / OTA | 3 |

(Already shipping per node: `configVersion` (applied), `state`, `recordingPaused`, `wakeIntervalMin`, `pendingTargetState`, battery, `lastSeenUnix`, sensor state.)

## 4. Implementation tiers (smallest-risk first)

### Tier 1 — Mothership self-identity + control block — **IMPLEMENTED**
*Done: `status.firmware{}` (`mothershipFirmwareStatusJson()` in `ota/mothership_selfupdate.cpp`) + `status.control{}` (`dispatcherStatusJson()`, shared with `GET /api/control`) added to `StatusContext`/`json_payload.cpp`; `meta.firmwareVersion` now reports `FW_SEMVER` not `"v1.0.0"`. `GET /api/control` now returns `{"stateRevision":N,"lastChangeSource":"...","results":[...]}`.*

Highest value, zero node-side risk, unblocks the mothership firmware card in the dashboard.
1. Add `firmwareJson` + `controlJson` (pre-built strings) to `StatusContext`, mirroring `nodesJson`/`modemJson`.
2. Build them in `handleSyncWake()` from `fwIdentity(NODE_PROTOCOL_VERSION)`, the running partition, OTA state, and `dispatcherRevision()`/`dispatcherRecentResults()`. (Factor the `/api/control` serializer so the same code emits `control.results[]` locally and in the cloud.)
3. Emit `status.firmware{}` and `status.control{}` in `json_payload.cpp`; set `meta.firmwareVersion = FW_SEMVER`.
4. **Backend/Supabase:** add columns to `mothership_status` (`firmware_version`, `build_id`, `hw_target`, `release_id`, `running_slot`, `ota_state`, `state_revision`, `last_change_source`). Coordinate with the schema doc.

### Tier 2 — Per-node desired-vs-applied — **IMPLEMENTED**
*Done: `buildNodesStatusJson` now emits `desiredConfigVersion` / `desiredTargetState` / `desiredWakeIntervalMin` from `getDesiredConfig()` alongside applied `configVersion`.*

Unblocks the convergence indicator (pending vs converged) with data the mothership already holds.
1. In `buildNodesStatusJson`, add `desiredConfigVersion` / `desiredTargetState` / `desiredWakeIntervalMin` from `getDesiredConfig(nodeId)` next to the existing applied `configVersion`.
2. **Backend:** add the desired columns to the nodes status shape.

### Tier 3 — Per-node firmware identity via `FW_CAPS` — **IMPLEMENTED**
*Done: additive `fw_caps_message_t` in `protocol.h` (**96 bytes**, `static_assert` ≤ 250; exact-length + "FW_CAPS" tag dispatch, so legacy firmware ignores it). Node sends it once per wake via `sendFwCaps()` (from `sendNodeHello`). Mothership: `gCapsQueue` + `drainSyncCaps()` in `espnow_sync.cpp`, folded into the registry in `handleSyncWake` via `setNodeFirmwareCaps()`; `NodeInfo` gained fwVersion/fwBuildId/hwRevision/otaProtocolVersion/otaMaxImageSize/rollbackCapable/hasFirmwareCaps; `buildNodesStatusJson` emits `firmwareVersion`/`firmwareBuild`/`hardwareRevision`/`otaProtocolVersion`/`otaMaxImageSize`/`rollbackCapable`/`otaCapable` (or `otaCapable:false` until a node reports). Config-mode dispatch is command-gated, so it ignores FW_CAPS safely. **Requires a node reflash to take effect.***

This is "nodes report their firmware to the mothership so the mothership can report it up." Bigger — touches node firmware + protocol + registry (this is the plan's Phase 1 observability deliverable, §8.2).
1. Define an **additive** `FW_CAPS` node→mothership ESP-NOW message: `protocolVersion`, firmware `semver`, `buildId`, `hardwareRevision`, `maxImageSize`, `rollbackCapable`. Keep it within the 250-byte ESP-NOW ceiling; version-gate it; older nodes simply don't send it.
2. Node sends `FW_CAPS` right after `NODE_HELLO` in the sync window.
3. Mothership stores per-node caps in `NodeInfo` (new fields) and persists coarsely.
4. `buildNodesStatusJson` emits `firmwareVersion` / `hardwareRevision` / `otaCapable` per node.
5. **Backend:** add the node firmware columns.

## 5. Design rules
- **Additive only.** Never remove/rename existing payload fields — the dashboard must degrade gracefully as fields roll out (brief §1). Add new objects/fields alongside.
- **No high-frequency NVS writes.** Identity + revision are cheap (RAM; the revision is already coarsely persisted by the dispatcher). Don't persist OTA byte counters.
- **Everything is "as of last check-in."** These fields ride the existing upload; the dashboard timestamps them with the check-in and never implies real-time.
- **`FW_SEMVER` discipline.** It's currently hand-set (`0.1.0`) in `platformio.ini`; bump it per release. The release tool (`scripts/release_sign.py`) already records version + sequence — keep them in lockstep.
- **Keep `/api/control` and the cloud control block using one serializer** so the local and cloud views can't drift.

## 6. Proposed JSON (mothership additions)
```json
"status": {
  "firmware": {
    "version": "0.1.0", "buildId": "8397884-dirty", "hwTarget": "mothership-v1",
    "protocolVersion": 2, "releaseId": null,
    "runningSlot": "app1", "otaState": "confirmed",
    "lastOtaResult": "success", "lastOtaReason": "NONE"
  },
  "control": {
    "stateRevision": 43, "lastChangeSource": "LOCAL_UI", "lastCommandCursor": null,
    "results": [ { "cmdId": "L00037442", "outcome": "ACCEPTED", "revision": 43 } ]
  },
  "nodes": [
    { "nodeId": "ENV_D13F98", "state": "DEPLOYED",
      "configVersion": 17, "desiredConfigVersion": 18, "desiredTargetState": 3,
      "recordingPaused": true, "wakeIntervalMin": 10,
      "firmwareVersion": "0.1.0", "hardwareRevision": "node-v3", "otaCapable": true  // Tier 3
    }
  ]
}
```

## 7. Sequencing & rough effort (one engineer)
| Tier | Work | Estimate |
|---|---|---|
| 1 | Mothership `firmware`+`control` block (json_payload + main wiring + schema) | 0.5–1 day |
| 2 | Per-node desired-vs-applied in `buildNodesStatusJson` | ~0.5 day |
| 3 | `FW_CAPS` message + node send + registry + payload + schema | 2–3 days |

Tiers 1–2 are pure mothership-side and can ship immediately; they give the dashboard a real mothership firmware number and node convergence today. Tier 3 delivers per-node firmware numbers and depends on a node firmware release.

## 8. Verification
- **Bench:** the upload path already logs the POST body; confirm the emitted JSON contains `status.firmware`, `status.control`, and (Tier 2) `desiredConfigVersion`. A node bench-test env can assert `FW_CAPS` encode/size for Tier 3.
- **Real:** a mothership check-in POST carries the new fields; the Supabase ingest accepts them (coordinate schema first, or 400s will drop the batch — see the upload path's 400 = non-retryable behaviour).
- **End-to-end:** the dashboard renders the mothership firmware card and a node's desired-vs-applied convergence from live data.

## 9. Open coordination (with backend/frontend)
- Supabase `mothership_status` + nodes columns for the new fields (add before firmware emits them — the ingest 400s on unexpected-but-strict shapes; confirm the ingest is additive-tolerant).
- Whether to deprecate the legacy `meta.firmwareVersion` string once `status.firmware.version` lands.
- `FW_CAPS` field set + hardware-revision identifier strings (`node-v3`, etc.).
