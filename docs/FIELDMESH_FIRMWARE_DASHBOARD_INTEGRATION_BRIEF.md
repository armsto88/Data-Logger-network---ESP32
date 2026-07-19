# FieldMesh — Firmware Info & Config Control: Dashboard Integration Brief

**Audience:** the frontend + backend team (and their LLM) building the FieldMesh dashboard.
**Author:** firmware team. **Date:** 2026-07-17.
**Companion docs (this repo):** `mothership/docs/FIELDMESH_OTA_FIRMWARE_UPDATE_PLAN.md` (the full firmware plan — read §0 for status, §4–§5, §13 for contracts), `docs/FIELDMESH_CLOUD_UPLOAD_PROTOCOL.md`, `docs/FIELDMESH_SUPABASE_SCHEMA_CONFIRMED.md`, `docs/NATIVE_APP_FULL_CONCEPT_AND_SCHEMAS.md`.

This document covers three things you asked for:
1. **Displaying firmware information** for the mothership and every node.
2. **Config controls** — data (recording) interval, pause/resume, undeploy.
3. **A workflow for updating the mothership's firmware from the dashboard.**

It ends with a **reference section of concrete data shapes** and a **ready-to-use prompt** you can hand to your own LLM.

---

## 0. TL;DR / the five rules that shape everything

1. **The dashboard never talks to a device directly.** It writes *intent* to a durable backend queue. The mothership pulls that intent during its next scheduled LTE check-in. There is no live socket to a mothership or node — ever.
2. **The mothership is the single authority.** It owns one monotonic `stateRevision`, validates every change (local *and* dashboard) through one dispatcher, and is the only thing that talks to nodes (over ESP-NOW).
3. **Everything is eventually-consistent with real latency.** A dashboard change is delivered to a node one — and up to ~two — sync intervals *after* the mothership's next check-in. The UI must show lifecycle states (`QUEUED → … → CONVERGED`), never imply real-time control.
4. **Desired ≠ applied.** Show the *requested* config next to the *device-confirmed* config. A value is not "applied" until a node acknowledges it.
5. **Firmware is signed; trust is cryptographic.** Releases are Ed25519-signed. The device holds only the public key. The dashboard can *request* a named, pre-approved release — it can never supply an arbitrary binary or URL.

```
Dashboard ──writes intent──> Backend queue ──(next LTE check-in)──> Mothership dispatcher ──(next sync window)──> Node
    ^                                                                     │                                         │
    └──────────────── reported state / results (each check-in) ──────────┴──── CONFIG_ACK / snapshot ──────────────┘
```

---

## 1. What the firmware already provides vs. what the backend must add

Being precise here saves you from designing against the wrong contract.

### Reported to the cloud **today** (per the existing upload protocol)
- **Mothership:** `firmwareVersion` (currently the coarse `"v1.0.0"`), `firmwareBuild` (now the **git build id**, e.g. `8397884-dirty`), battery, upload/queue diagnostics.
- **Per node** (in the `status.nodes[]` array): `nodeId`, `name`, `mac`, `state` (`DEPLOYED`/`PAIRED`/`UNPAIRED`/…), `lastSeenUnix`, `wakeIntervalMin`, `lastReportedBatV`, `configVersion` (**applied** — what the node last confirmed), `recordingPaused`, `deployPending`, `stateChangePending`, `pendingTargetState`, `latitude`/`longitude`, `deployedSinceUnix`, plus configured-sensor state.

### Implemented in firmware, **not yet in the cloud payload** (Phase 1 additions — firmware team will add)
- **Rich firmware identity** for both roles: `role`, semantic `version`, `buildId` (git), `hwTarget` (e.g. `mothership-v1`, `node-v3`), `protocolVersion`, and (once releases are cut) `releaseId`. Today this exists in firmware (`firmware_identity.h`) and prints at boot; it needs to be surfaced into the status payload.
- **Per-node firmware version + hardware revision + OTA capability + OTA state.** The node identity module exists; the `FW_CAPS` report that carries it to the mothership is Phase 1.
- **Desired vs applied config version per node** (today only *applied* `configVersion` is sent).
- **Mothership control block:** `stateRevision`, last command cursor, last change source, recent command results. Exposed **locally** today at `GET /api/control`; the cloud mirror is the backend build.

### The backend must build (nothing firmware can do alone)
- A **durable per-mothership command queue** the dashboard writes and only the authenticated mothership drains at check-in.
- **Command identity + ordering:** immutable `commandId`, monotonic `cursor`/`sequence`, `expiresAtUnix`, target binding, and an audit record of the initiating user.
- **Separate storage of *queued intent* vs *mothership-reported state*** so a queued value never looks device-confirmed.
- **Unique per-mothership auth** for the check-in (not a shared secret), TLS-only.
- For OTA: an **approved, immutable release service** (see §4).

> **Design against the full contract in §5**, not just today's payload. The firmware team will land the additive fields per `mothership/docs/MOTHERSHIP_STATUS_REPORTING_PLAN.md` (Tier 1 = mothership firmware identity + control; Tier 2 = per-node desired-vs-applied; Tier 3 = per-node firmware via `FW_CAPS`). Build the UI/data model assuming they exist and degrade gracefully while they roll out.

---

## 2. Displaying firmware information

### 2.1 Mothership firmware card
Fields (target contract): `role=mothership`, `version` (semver), `buildId` (short git + `-dirty` marker), `hwTarget`, `releaseId`, `protocolVersion`, running slot, OTA state, battery, **last check-in time**.

UI guidance:
- Lead with **version + release id**; keep `buildId` as secondary/monospace detail. A `-dirty` build id means "built from uncommitted source" — surface it as a subtle "dev build" chip, not an error.
- Every firmware value must carry an **"as of <last check-in>"** timestamp. Never imply it's live.
- Show OTA state when a rollout is active (see §4 state list).

### 2.2 Node fleet view
One row per node with: `name`/`nodeId`, `state`, firmware `version`, `hwTarget`, `configVersion` **desired vs applied** + a convergence indicator, `wakeIntervalMin`, `recordingPaused`, battery, `lastSeenUnix`, OTA state/target.

Convergence indicator (drive it from desired vs applied config version + pending flags):
- **Converged** — applied == desired, no pending change.
- **Pending / waiting for sync** — desired > applied, or `stateChangePending`/`deployPending` true. Show "will apply at next sync".
- **Stale** — `lastSeenUnix` older than ~2 sync intervals → the node may be missing; dim it and flag.

### 2.3 Reason & state vocabularies (map to human text)
Use the firmware's stable codes; don't invent your own. Full lists in §5.5. Examples: `REVISION_CONFLICT` → "Someone changed this first — refresh and re-apply"; `INCOMPATIBLE_HARDWARE` → "This release isn't for this node's board"; `DEFERRED_LOW_BATTERY` → "Deferred: battery too low".

---

## 3. Config controls: data interval, pause/resume, undeploy

All three are the **same command** to the firmware: a per-node desired-state change carried by `SET_NODE_CONFIG`, gated by the mothership dispatcher, then delivered to the node via the existing versioned `NODE_CONFIG` flow at the next sync window.

### 3.1 The controllable fields
| Control | Field + value | Semantics |
|---|---|---|
| **Data / recording interval** | `wakeIntervalMin` = 1–240 | How often the node wakes to sample. |
| **Pause** | `targetState = 3` (STANDBY) | Node **stops recording but stays deployed and keeps syncing** (remotely resumable). |
| **Resume** | `targetState = 2` (DEPLOYED/ACTIVE) | Returns to active recording. |
| **Undeploy** | `targetState = 0` (UNPAIRED) | Removes the node from active duty (heavier than pause). |
| (Sensor selection) | `sensorMask` (uint16) | Which sensors the node treats as installed (see Appendix A.2). |

`targetState` enum: `0=UNPAIRED, 1=PAIRED, 2=DEPLOYED/ACTIVE, 3=STANDBY`. So **pause = 3, resume = 2, undeploy = 0**. The distinction the operator must understand: **pause is reversible standby (still in the mesh); undeploy removes it.** All are applied via the sync-window `NODE_CONFIG` reconcile and confirmed by an advancing `configVersion`. **See Appendix A for the exact, source-grounded contract.**

### 3.2 Command lifecycle the UI must render
A dashboard change is **not "applied"** the moment it's submitted. Track and show:

| State | Meaning |
|---|---|
| `QUEUED` | Backend has the intent; mothership hasn't received it yet. |
| `RECEIVED` | Mothership pulled and durably stored it. |
| `ACCEPTED` | Preconditions passed; a mothership `stateRevision` was assigned. |
| `REJECTED` | Invalid / expired / unauthorized / incompatible / **revision conflict**. |
| `WAITING_FOR_SYNC` | Accepted; awaiting the next node sync window. |
| `SENT_TO_NODE` | `NODE_CONFIG` transmitted; not yet confirmed. |
| `CONVERGED` | Node acknowledged (CONFIG_ACK / matching snapshot). ✅ done. |
| `SUPERSEDED` | A newer accepted change replaced this one before it converged. |
| `FAILED` / `CANCELLED` | Ended with a stable reason code. |

### 3.3 Compare-and-set (so local and cloud don't clobber each other)
- Every dashboard instruction includes **`expectedStateRevision`** — the last mothership revision the user saw.
- If it's stale (a local operator or another dashboard change moved the revision first), the mothership returns **`REVISION_CONFLICT`** with the current revision/state. The dashboard should **refresh and ask the user to deliberately re-submit** — never silently overwrite.
- Prefer sending a **full desired node config** over partial patches.

### 3.4 What's implemented now (so you know what's real)
- The mothership **dispatcher** (compare-and-set revision, supersession, idempotent replay, NVS-persisted) is built and **proven on real hardware** — a pause recorded as `ACCEPTED rev=1` and survived reboot.
- The mothership's **local UI** already performs interval / pause / resume / undeploy and now routes them through the dispatcher; `GET /api/control` (local, config-mode only) returns `{revision, results[]}`.
- **Missing for you:** the backend queue + the mothership's LTE *command ingestion* (pulling `SET_NODE_CONFIG` from the check-in response) + cloud mirroring. That's the main backend deliverable; the firmware side of ingestion is Phase 2 in the plan.

---

## 4. Workflow brainstorm: updating the **mothership firmware** from the dashboard

### 4.1 What's already proven (local path)
On real hardware, SD-free, the mothership can self-update over HTTP today:
- `POST /firmware/manifest` (body = signed `manifest.json`, `?sig=<hex>`) → Ed25519 verify + role/hardware/anti-downgrade check.
- `POST /firmware/image` (the matching binary, streamed) → SHA-256 gate → write inactive slot → reboot → **first-boot self-test confirm**, with **automatic bootloader rollback** if the new image fails to boot. Interrupted uploads leave the running firmware untouched.

The remote (LTE, dashboard-initiated) path reuses all of this; it swaps the local HTTP upload for a backend-queued instruction + an HTTPS pull.

### 4.2 Proposed remote workflow
```
1. Operator picks an APPROVED release in the dashboard (from a release catalog).
2. Dashboard shows compatibility (role, hardware target, min versions, release notes) and a confirm step.
3. On confirm, backend enqueues  DEPLOY_RELEASE { releaseId, policy }  for that mothership.
   (The dashboard supplies a release *ID*, never a URL or binary.)
4. Mothership pulls it at its next LTE check-in, resolves releaseId against the approved release host,
   fetches + verifies the signed manifest, runs preflight (battery, version policy, free slot, …).
5. Mothership streams the signed image over HTTPS into its inactive slot, verifying SHA-256.
6. Reboot → first-boot self-test → confirm, or automatic rollback on failure.
7. Mothership reports progress/result at each check-in; dashboard advances the status.
```

### 4.3 Status states to render for a mothership OTA
`QUEUED → ACCEPTED → PREFLIGHT → DOWNLOADING (bytes/total) → VERIFYING → READY_TO_REBOOT → PENDING_BOOT_VALIDATION → CONFIRMED` — or a terminal `FAILED` / `ROLLED_BACK` with a reason code. Because delivery rides the check-in, show **"next check-in ≈ <time>"** rather than a spinner implying live progress.

### 4.4 Safety rules the UI should reflect
- **Attended vs unattended.** Until each device runs a bootstrap release with a proven recovery path, treat field OTA as needing physical access; the dashboard should not imply "fire and forget".
- **Battery gate.** Low battery **defers** (not fails) an update — show a "deferred, will retry" state, not an error.
- **Rollback is normal, not a crash.** If first boot fails, the device auto-reverts to the prior firmware and reports `ROLLED_BACK`. Present that as "update didn't take; device safe on previous version."
- **One authority.** A local physical/service lock on the mothership overrides a remote resume/cancel; reflect a `LOCAL_SERVICE_LOCK` state instead of retrying.

### 4.5 What must exist to enable it
- **Firmware:** streaming HTTPS GET in the modem driver (plan §7.2), `DEPLOY_RELEASE` command handling, progress/result reporting. (Node fleet OTA additionally needs an SD release store — currently blocked by no SD card.)
- **Backend:** an **approved, immutable release service** (HTTPS, content-addressed artifacts, no arbitrary URLs), `DEPLOY_RELEASE` queueing, and status mirroring.
- **Release tooling (exists):** `scripts/release_sign.py` produces `manifest.json` + detached Ed25519 signature and the embeddable public key. Your release service serves exactly those bytes.

---

## 5. Reference: concrete data shapes

> These are the real shapes the firmware uses. Field names are authoritative. Where a block isn't in the cloud payload yet, it's marked **(proposed / additive)**.

### 5.1 Firmware identity (per device) — **(proposed additive to status payload)**
```json
{
  "role": "mothership",            // or "node"
  "version": "0.2.0",              // semantic version
  "buildId": "8397884-dirty",      // git short hash (+ "-dirty" if built from uncommitted source)
  "hwTarget": "mothership-v1",     // or "node-v3"
  "protocolVersion": 2,
  "releaseId": "fieldmesh-2026.08.0" // null until installed via a signed release
}
```

### 5.2 Mothership control block — `GET /api/control` today; **(proposed additive to cloud status)**
```json
{
  "revision": 43,
  "results": [
    { "cmdId": "018f-…", "outcome": "ACCEPTED", "revision": 43 }
  ]
}
```

### 5.3 Per-node status — **shipping today** (subset; see `buildNodesStatusJson`)
```json
{
  "nodeId": "ENV_D13F98", "name": "North field", "mac": "68:09:47:D1:3F:98",
  "state": "DEPLOYED", "lastSeenUnix": 1784285000,
  "wakeIntervalMin": 10, "recordingPaused": false,
  "configVersion": 17,                 // APPLIED (node-confirmed)
  "pendingTargetState": "NONE", "stateChangePending": false,
  "lastReportedBatV": 3.72, "latitude": null, "longitude": null
  // proposed additive: "firmwareVersion","hwTarget","desiredConfigVersion",
  //                    "otaCapable","otaState","otaTargetVersion"
}
```

### 5.4 Backend → mothership command envelope (in the check-in response) — **(contract)**
```json
{
  "controlProtocolVersion": 1,
  "serverTimeUnix": 1784217600,
  "nextCursor": 130,
  "commands": [
    {
      "commandId": "018f-example", "sequence": 130,
      "type": "SET_NODE_CONFIG",              // or DEPLOY_RELEASE, PAUSE_DEPLOYMENT, …
      "target": { "nodeId": "ENV_D13F98" },
      "expectedStateRevision": 43,
      "issuedAtUnix": 1784217500, "expiresAtUnix": 1784303900,
      "payload": { "wakeIntervalMin": 10, "targetState": "DEPLOYED", "sensorMask": 37 }
    }
  ]
}
```
Firmware rules: missing envelope = legacy upload, no commands; persist a command before advancing the cursor; a repeated `commandId` returns its stored result and never re-executes; process a bounded number per check-in.

### 5.5 Vocabularies (use these exact codes)
**Command outcomes** (config changes): `ACCEPTED`, `REVISION_CONFLICT`, `INVALID`, `SUPERSEDED`, `REPLAY`.
**Command lifecycle** (§3.2): `QUEUED`, `RECEIVED`, `ACCEPTED`, `REJECTED`, `WAITING_FOR_SYNC`, `SENT_TO_NODE`, `CONVERGED`, `SUPERSEDED`, `FAILED`, `CANCELLED`.
**OTA / release reasons:** `NONE`, `COMMAND_EXPIRED`, `COMMAND_DUPLICATE`, `REVISION_CONFLICT`, `SUPERSEDED`, `DEFERRED_LOW_BATTERY`, `DEFERRED_BUSY`, `DEFERRED_BACKOFF`, `LOCAL_SERVICE_LOCK`, `INCOMPATIBLE_ROLE`, `INCOMPATIBLE_HARDWARE`, `INCOMPATIBLE_PROTOCOL`, `DOWNGRADE_REJECTED`, `SD_MISSING`, `SD_FULL`, `MANIFEST_INVALID`, `SIGNATURE_INVALID`, `IMAGE_TOO_LARGE`, `SIZE_MISMATCH`, `HASH_MISMATCH`, `IMAGE_INVALID`, `FLASH_WRITE_FAILED`, `DOWNLOAD_TIMEOUT`, `DOWNLOAD_TRUNCATED`, `RETRY_LIMIT_EXCEEDED`, `BOOT_VALIDATION_FAILED`, `ROLLED_BACK`, `DATA_BACKLOG_BLOCKED`, `OPERATOR_CANCELLED`.

### 5.6 Release manifest + signature (what the release service serves)
```json
{"artifacts":[{"buildId":"…","hwTargets":["mothership-v1"],"protocolVersion":2,
  "role":"mothership","sha256":"<64 hex>","size":1204688,"version":"0.2.0",
  "minMothershipVersion":"0.1.0"}],
  "releaseId":"fieldmesh-2026.08.0","releaseSequence":12,"schemaVersion":1}
```
- Signature is a **detached Ed25519 signature over the exact bytes of `manifest.json`** (served alongside as `manifest.json.sig`, 64-byte hex). The device verifies the bytes it downloaded — no re-serialization. Produce both with `scripts/release_sign.py`.

### 5.7 Local mothership endpoints (config-mode AP, for reference/testing only)
| Method | Path | Purpose |
|---|---|---|
| GET | `/api/control` | `{revision, results[]}` |
| GET | `/firmware` | Human page: running identity, slot, staged manifest, last result |
| POST | `/firmware/manifest?sig=<hex>` | Stage + verify a signed manifest (body = `manifest.json`) |
| POST | `/firmware/image` | Multipart binary upload → install |
> These live on the mothership's config-mode Wi-Fi AP, not the cloud. They are the local equivalent of the remote flow and are useful as a reference implementation of the verify→install→confirm sequence.

---

## 6. Ownership boundary

| Concern | Owner |
|---|---|
| Operator intent capture, auth, audit, durable queue, release catalog/hosting | **Backend** |
| Rendering identity/state, lifecycle status, conflict prompts, OTA progress | **Frontend** |
| Revision assignment, compare-and-set, node protocol, install, verify, rollback, convergence | **Firmware** |
| The additive status fields + command ingestion on the mothership | **Firmware (Phase 1/2)** |

The backend **never** emits ESP-NOW; the dashboard **never** addresses a node. All compatibility, scheduling, retries, and convergence are the firmware's job.

---

## 7. Open questions for your team (please decide with firmware)
1. Will the existing status/ingest response carry the command envelope, or a separate small control check-in? (Firmware prefers reusing the existing response.)
2. What bounded command count + response size can the backend guarantee per check-in?
3. Acceptable max dashboard→node latency, given delivery rides the following sync window?
4. Which settings are remotely mutable vs local-service-locked vs never-mirrored? (Pairing, factory reset, credentials → not ordinary remote commands.)
5. Full desired node config vs versioned patches? (Firmware prefers full desired config.)
6. Which release host serves immutable HTTPS artifacts, and how are releases approved into the catalog?
7. Retention for command IDs/results and audit records?

---

## 8. Prompt for your LLM

> You are building the **FieldMesh dashboard** (frontend + backend) that manages a fleet of solar-powered ESP32 sensor **nodes** coordinated by a **mothership** (an ESP32 with LTE backhaul that uploads to Supabase). Build three capabilities: (a) **display firmware info** for the mothership and each node; (b) **config controls** — recording interval, pause/resume, undeploy; (c) **mothership firmware update** initiated from the dashboard.
>
> **Non-negotiable architecture:** the dashboard NEVER contacts a device directly. It writes intent to a durable per-mothership **backend command queue**. The mothership pulls queued commands during its scheduled LTE check-in, validates them through a single authority that owns a monotonic `stateRevision` (compare-and-set), then delivers node changes over ESP-NOW at the next sync window. All state is eventually-consistent: a change is `QUEUED → RECEIVED → ACCEPTED → WAITING_FOR_SYNC → SENT_TO_NODE → CONVERGED` (or `REJECTED`/`SUPERSEDED`). Show desired-vs-applied and "as of last check-in" everywhere; never imply real-time.
>
> **Config controls** are all one command, `SET_NODE_CONFIG { nodeId, wakeIntervalMin (1–240 = recording interval), targetState (DEPLOYED / paused-standby / UNPAIRED = undeploy), sensorMask }`, carrying `expectedStateRevision` for compare-and-set; on `REVISION_CONFLICT`, refresh and ask the user to re-submit. Pause = reversible standby (node stays in the mesh, stops recording); undeploy = unpair.
>
> **Mothership OTA:** the operator selects a pre-approved, immutable **release** (never a URL/binary); backend enqueues `DEPLOY_RELEASE { releaseId }`; the mothership pulls it, verifies an **Ed25519-signed manifest** (detached signature over `manifest.json`, per-artifact SHA-256, role/hardware/anti-downgrade checks), installs to an inactive slot, reboots with a first-boot self-test and **automatic rollback** on failure. Track `QUEUED → ACCEPTED → PREFLIGHT → DOWNLOADING → VERIFYING → READY_TO_REBOOT → PENDING_BOOT_VALIDATION → CONFIRMED` / `ROLLED_BACK` / `FAILED`. Low battery **defers** (not fails). Rollback is a safe outcome, not an error.
>
> **Backend must provide:** durable queue with immutable `commandId`, monotonic `cursor`, `expiresAtUnix`, target binding, per-mothership auth (TLS-only, unique key), user audit, separation of *queued intent* vs *reported state*, and an approved immutable release service. **Firmware provides:** revision assignment, validation, node protocol, install/verify/rollback, convergence, and (Phase 1/2) additive status fields + command ingestion. Use the exact field names and reason-code vocabularies from the integration brief §5; do not invent new ones.

---

## Appendix A — Source-grounded contract (real code, not proposals)

Added at the dashboard team's request so the backend queue is built against the *actual* firmware. Each item is marked **IMPLEMENTED** or **NOT-YET-BUILT**. File paths are in this repo.

### A.1 `GET /api/control` + cloud `status.control{}` — exact response — **IMPLEMENTED**
Source: `dispatcherStatusJson()` in `node/firmware/shared/command_dispatcher.cpp` — the ONE serializer used by both `GET /api/control` (local) and the cloud `status.control{}` block, so they are byte-identical:
```json
{"stateRevision":1,"lastChangeSource":"LOCAL_UI","results":[{"cmdId":"L00037442","outcome":"ACCEPTED","revision":1}]}
```
- `stateRevision` (uint32): the mothership's authoritative, NVS-persisted revision. Monotonic; 0 → 1 on the first accepted change; survives reboot. *(Note: earlier drafts of this doc called it `revision`; the field is now `stateRevision`.)*
- `lastChangeSource`: `LOCAL_UI` or `DASHBOARD` — the source of the last accepted change.
- `results[]`: up to `CMD_MAX_RESULTS = 8` most-recent results (ring buffer), each `{cmdId, outcome, revision}` where `revision` = assigned revision (0 if not accepted). Storage order, not strictly chronological.
- Now also emitted to the cloud as `status.control{}` in the upload payload (needs a mothership reflash + a Supabase column).

### A.2 `SET_NODE_CONFIG` payload — fields, types, ranges — **dispatcher IMPLEMENTED**
Two structs are involved. The dispatcher validates/revisions a controlled subset (`DispatchNodeConfig`, `command_dispatcher.h`); the value actually delivered to the node is the registry's `NodeDesiredConfig` (`node_registry.h`).

| Field | Type | Range / rule |
|---|---|---|
| `nodeId` | char[16] | non-empty; ≤15 chars |
| `wakeIntervalMin` | uint8 | **1–240** (0 or >240 → `INVALID`) |
| `targetState` | uint8 | `0`=UNPAIRED, `1`=PAIRED, `2`=DEPLOYED/ACTIVE, `3`=STANDBY (see §3.1) |
| `sensorMask` | uint16 | SNAP bits + selector + VALID (below) |

`sensorMask` bits (`node/firmware/shared/protocol.h`):

| Bit | Constant | Meaning |
|---|---|---|
| 0 | `SNAP_PRESENT_AIR_TEMP` | air temperature |
| 1 | `SNAP_PRESENT_AIR_RH` | humidity |
| 2 | `SNAP_PRESENT_SPECTRAL` | all 8 spectral channels (group) |
| 3 | `SNAP_PRESENT_WIND` | wind speed + direction |
| 4 | `SNAP_PRESENT_SOIL1` | soil1 vwc + temp |
| 5 | `SNAP_PRESENT_SOIL2` | soil2 vwc + temp |
| 6 | `SNAP_PRESENT_AUX1` | aux1 |
| 7 | `SNAP_PRESENT_AUX2` | aux2 |
| 8 | `SNAP_PRESENT_BAT_V` | battery voltage |
| 9 | `NODE_SENSOR_CFG_WIND_ULTRASONIC` | wind selector: ultrasonic (set) vs reed-cup (clear); normalized to WIND for fault detection |
| 15 | `NODE_SENSOR_MASK_VALID` | **1 = mask is authoritative; 0 = auto-detect** (node registers whatever it finds) |

Operator-selectable bits: `NODE_SENSOR_CFG_ALL_BITS = 0x03FF` (bits 0–9). To make a selection stick you must OR in `NODE_SENSOR_MASK_VALID` (0x8000) — otherwise `0` means "auto", not "no sensors".

### A.3 The LTE check-in — **IMPLEMENTED as the data upload; command parsing NOT built**
- **It is the same request as the data upload — there is no separate check-in.** The mothership calls `modem.httpsPost(url, body, "application/json", authToken)` (`mothership/firmware/v2/src/comms/modem_driver.{h,cpp}`), one or more times per wake (chunked readings, plus a status-only heartbeat when there are zero rows).
- **Request:** HTTPS POST to the configured ingest URL (Supabase), `Content-Type: application/json`, per-mothership bearer-style auth token (e.g. `fm_bkyd_001_<uuid>`), body = the canonical batch `{ readings[], meta{firmwareVersion,…}, status{ batVoltage, nodes[…], … } }`.
- **Response parsed today:** `struct HttpsPostResult { bool success; int httpStatus; String responseBody; String errorDetail; }`. **`responseBody` is already captured** (via `AT+HTTPREAD`), but the upload logic branches **only on `httpStatus`** — 200 = success + advance cursor; 400/401 = non-retryable stop; 429/5xx/-1 = retry next window. The body is not inspected.
- **Can firmware parse a `commands[]` array added to the response? YES — the plumbing exists** (`responseBody` is in hand); only the parser is missing. That parser is the Phase-2 firmware work; the envelope is defined in plan §13.4 (mirrored in §5.4 here). Decide with firmware: which POST carries commands when a wake makes several (recommend the final/status POST), and the per-check-in cap.

### A.4 Dispatcher compare-and-set — **IMPLEMENTED**
Source: `dispatcherSubmit()` in `command_dispatcher.cpp`. `gRevision` starts at 0 (or the NVS-restored value) and is incremented **only** on an accepted state-changing command; the new value is the `assignedRevision`. Checks run in this order:
1. **`REPLAY`** — `cmdId` already in the result ring → return the stored result verbatim; never re-execute; no revision change. (This is the idempotency guarantee.)
2. **`CMD_REQUEST_STATUS`** → `ACCEPTED` no-op, no revision bump.
3. **`INVALID`** — `validPayload` fails (empty nodeId, or `wakeIntervalMin ∉ [1,240]`) or node table full. No revision change.
4. **`REVISION_CONFLICT`** — `c.expectedRevision != gRevision`; returns `currentRevision = gRevision`; no change. (Local UI passes the current revision, so only a stale dashboard command trips this.)
5. **`ACCEPTED`** — apply config → `gRevision++` → `assignedRevision = gRevision`. Before applying, any earlier accepted-but-not-yet-`converged` command for the *same node* with a different `cmdId` is flipped to **`SUPERSEDED`**.
- Convergence: `dispatcherMarkConverged(nodeId, revision)` marks a node's command converged (call when the node ACKs); a converged command is immune to later supersession.
- Persistence: `gRevision` + result ring + node table → NVS namespace `"dispatch"` on every accept; restored by `dispatcherInit()`.
- Enum `CmdOutcome` serializes as: `ACCEPTED`, `REVISION_CONFLICT`, `INVALID`, `SUPERSEDED`, `REPLAY`.

### A.5 Firmware identity + FW_CAPS
**`FirmwareIdentity` — IMPLEMENTED** (`node/firmware/shared/firmware_identity.h`):
```c
struct FirmwareIdentity {
  const char* role;             // "node" | "mothership"
  const char* semver;           // "0.1.0"
  const char* buildId;          // git short hash, e.g. "8397884-dirty"
  const char* hwTarget;         // "mothership-v1" | "node-v3"
  uint16_t    protocolVersion;  // = NODE_PROTOCOL_VERSION (currently 2)
  uint8_t     identityVersion;  // FW_IDENTITY_VERSION = 1
};
```
Real mothership value (serial): `role=mothership ver=0.1.0 build=8397884-dirty hw=mothership-v1 proto=2 idv=1`. `buildId` is injected at build time by `scripts/fw_version.py` (git short hash + `-dirty` on an uncommitted tree). `releaseId` is not in this struct yet. This identity is **not in the cloud payload yet** (Phase-1 additive change).

**`FW_CAPS` — IMPLEMENTED (2026-07-17; needs a node + mothership reflash to take effect).** Additive node→mothership ESP-NOW message `fw_caps_message_t` (`protocol.h`, **96 bytes**), sent once per wake after `NODE_HELLO`:
```c
struct fw_caps_message {
  char command[16];      // "FW_CAPS"
  char nodeId[16];
  uint16_t protocolVersion;
  char fwVersion[12];    // "0.1.0"
  char buildId[24];      // git short hash (+ "-dirty")
  char hwTarget[16];     // "node-v3"
  uint32_t maxImageSize; // inactive OTA slot capacity
  uint8_t rollbackCapable;
  uint8_t reserved[3];
};
```
The mothership folds it into the registry and emits per node in `status.nodes[]`: `firmwareVersion`, `firmwareBuild`, `hardwareRevision`, `otaProtocolVersion`, `otaMaxImageSize`, `rollbackCapable`, `otaCapable` (or `firmwareVersion:null, otaCapable:false` for a node that hasn't reported yet). So treat per-node firmware as "unknown" **only until the node's first post-reflash sync window**.

### A.6 commandId origin + limits
- **`commandId` origin:** for **local UI** changes, **firmware generates it** — `snprintf(cmdId, 24, "L%08lx", millis())`, e.g. `L00037442` (`controlRecordNodeChange`, config_server.cpp). For **dashboard** changes, the **backend must generate an immutable `commandId`**; the mothership uses it purely as the idempotency key.
- **`CMD_ID_LEN = 24`** (bytes incl. NUL → **≤23 usable chars**). ⚠️ A full UUID (36 chars) will be **truncated** by `strlcpy`, which can break idempotency if two IDs share the first 23 chars. Either keep backend IDs ≤23 chars (e.g. a base62 ULID prefix) **or** ask firmware to widen `CMD_ID_LEN` before UUIDs are used.
- **`expiresAtUnix`:** in the envelope contract but **NOT enforced by firmware yet** (ingestion isn't built). When it lands, expired commands are dropped as `COMMAND_EXPIRED`.
- **Bounded commands per check-in:** **not enforced yet.** Existing constants: `CMD_MAX_RESULTS = 8` (result ring), `CMD_MAX_NODES = 16`. Propose a small per-check-in cap (e.g. 8) so a check-in can't extend the powered session — open decision §7 Q2.

---

## Appendix B — Cloud-triggered mothership OTA: as-built (`DEPLOY_RELEASE`)

Added when the cloud OTA path was implemented. **This section supersedes §4.2's sketch** where they differ (the command carries a `releaseId`, and the firmware derives the fetch URLs — the backend never sends a URL or binary). Marked **IMPLEMENTED** below means the firmware side is built and unit/component-tested on hardware; the end-to-end on-air download is proven separately with a real hosted release.

### B.1 `DEPLOY_RELEASE` command — **IMPLEMENTED** (firmware side)
Rides the existing check-in response envelope (§5.4), protocol ≥ 2. It is a **lone global command**: it must be the **only** command in its response (mixed with any other command → the whole response is rejected `MIXED_GLOBAL_BATCH`, exactly like `SET_RECORDING_INTERVAL`).
```json
{
  "commandId": "018f-example", "sequence": 131,
  "type": "DEPLOY_RELEASE",
  "expectedStateRevision": 43,
  "issuedAtUnix": 1784217500, "expiresAtUnix": 1784303900,
  "payload": { "releaseId": "fieldmesh-2026.08.0" }
}
```
- **`releaseId`**: required, non-empty, **≤ 39 chars** (`CMD_RELEASE_ID_LEN = 40` incl. NUL). Empty/oversized → durable `RELEASE_ID_INVALID`. Never a URL or binary.
- CAS/idempotency identical to other commands: stale `expectedStateRevision` → `REVISION_CONFLICT`; a repeated `commandId` replays its stored result and never re-installs.
- On accept the mothership durably stages the intent (survives power-cycles) and reports `ACCEPTED` in `status.control{}.results[]`. Install progress is reported separately in `status.firmware{}` (B.3), not in the command result.

### B.2 Release artifact hosting — **BACKEND MUST PROVIDE**
The firmware **derives** the fetch URLs from its pinned approved host + role + `releaseId`; the backend must serve exactly these paths (all HTTPS, on `FM_APPROVED_ENDPOINT_HOST`). The firmware fetches the **Supabase Storage public-read URLs** (bytes served verbatim — no re-serialization, so no `SIGNATURE_INVALID`; CDN-cached + immutable; no edge-function cold start in the LTE hot path):
```
https://<approved-host>/storage/v1/object/public/releases/<role>/<releaseId>/manifest.json
https://<approved-host>/storage/v1/object/public/releases/<role>/<releaseId>/manifest.json.sig
https://<approved-host>/storage/v1/object/public/releases/<role>/<releaseId>/image.bin
```
`<role>` = `mothership`. The `.sig` is the detached Ed25519 signature (128 lowercase hex) over the exact `manifest.json` bytes; `manifest.json`/`image.bin` are produced by `scripts/release_sign.py`. **The URL path template is firmware-side and can change** — confirm it against `otaBuildManifestUrl()`/`otaBuildImageUrl()` in `mothership/firmware/v2/src/ota/mothership_ota_cloud_fetch.cpp` before wiring the release store. Each URL is re-checked against the approved host (`hwEndpointAllowed`) before any fetch; a URL that escapes the host aborts the install.
- **TLS gives you no identity assurance here** — the modem does not verify server certs. Integrity rests entirely on the manifest Ed25519 signature + per-artifact SHA-256. Do not rely on TLS to authenticate the release.
- **The image URL must stay fetchable across multiple check-ins.** There is **no mid-transfer resume**: every wake is a cold boot, so an interrupted or budget-truncated download restarts from byte 0 next wake. Use a stable/long-lived URL (or a signed URL valid for the whole install window), not a single-use one. At LTE rates a ~1 MB image may take more than one wake to land — do not promise fast turnaround.

### B.3 `status.firmware{}` additive fields — **IMPLEMENTED**
Source: `mothershipFirmwareStatusJson()` in `mothership/firmware/v2/src/ota/mothership_selfupdate.cpp`. New keys (all additive):
- **`releaseId`**: releaseId of the confirmed-running image (was always `null`; now populated once a release has been installed-and-confirmed through OTA).
- **`pendingReleaseId`**: a staged `DEPLOY_RELEASE` intent not yet installed (`null` if none). Non-null across wakes means a download is pending/retrying.
- **`armedReleaseId`**: an image flashed and boot-armed, awaiting first-boot confirmation (`null` if none). **Rollback inference:** if `armedReleaseId` is set on one check-in but a later check-in shows the running `releaseId` ≠ `armedReleaseId` (and `armedReleaseId` cleared), the trial image failed to boot and the bootloader auto-reverted — render `ROLLED_BACK`.
- Existing `activeSlot`/`nextBootSlot`/`slots[]`/`otaState` are unchanged; `otaState=CONFIRMED` on the active slot after a confirmed boot is the "Active" signal.

### B.4 Reason vocabulary + retry semantics — **IMPLEMENTED**
Lifecycle/reason strings follow §5.5. Firmware distinguishes **transient** (retried automatically next wake, intent stays staged — the backend need not re-issue) from **terminal** (intent cleared — the backend must issue a fresh `commandId` to retry):
- **Transient / deferred:** `DEFERRED_LOW_BATTERY`, `DEFERRED_BUSY` (session budget too low), `DEFERRED_BACKOFF`* (skipping this wake under retry backoff after a prior failure), `DOWNLOAD_TIMEOUT`, `DOWNLOAD_TRUNCATED`, `DOWNLOAD_FAILED`*, `MODEM_UNAVAILABLE`*.
- **Terminal:** `MANIFEST_INVALID`, `SIGNATURE_INVALID`, `INCOMPATIBLE_ROLE`, `INCOMPATIBLE_HARDWARE`, `INCOMPATIBLE_PROTOCOL`, `DOWNGRADE_REJECTED`, `SIZE_MISMATCH`, `HASH_MISMATCH`, `IMAGE_INVALID`, `IMAGE_TOO_LARGE`, `FLASH_WRITE_FAILED`, `RETRY_LIMIT_EXCEEDED`* (gave up after repeated transient download failures — operator must issue a fresh command).

  *`DOWNLOAD_FAILED`/`MODEM_UNAVAILABLE`/`DEFERRED_BACKOFF`/`RETRY_LIMIT_EXCEEDED` are firmware extensions beyond §5.5's list. Treat any unrecognised reason as a non-fatal "will retry / see detail" state — **except** `RETRY_LIMIT_EXCEEDED`, which is terminal (see the Terminal list): the intent is cleared and the release will NOT retry until the operator issues a fresh `commandId`.

### B.5 Anti-downgrade — **IMPLEMENTED (now enforced)**
Previously inert (`installedReleaseSequence()` returned 0). Now the confirmed-running release's `releaseSequence` is persisted in NVS and advanced only after a confirmed first boot, so a `DEPLOY_RELEASE` whose manifest `releaseSequence` is lower than the installed one is rejected `DOWNGRADE_REJECTED`. Keep `releaseSequence` monotonic per role in the release catalog.

---
*Questions? Talk to the firmware team — and see `FIELDMESH_OTA_FIRMWARE_UPDATE_PLAN.md` §4 (control model), §5 (release/manifest), §13 (status model), and §0 (what's built).*
