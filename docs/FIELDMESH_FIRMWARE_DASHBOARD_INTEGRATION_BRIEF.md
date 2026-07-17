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

> **Design against the full contract in §5**, not just today's payload. The firmware team will land the additive fields; you should build the UI/data model assuming they exist and degrade gracefully while they roll out.

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
| Control | Field | Semantics |
|---|---|---|
| **Data / recording interval** | `wakeIntervalMin` (1–240) | How often the node wakes to sample. |
| **Pause / resume** | `targetState` / standby | **Pause** = standby: node **stops recording but stays deployed and keeps syncing** (remotely resumable). **Resume** returns it to active recording. |
| **Undeploy** | `targetState = UNPAIRED` (unpair) | Removes the node from active duty. This is a heavier lifecycle change than pause. |
| (Sensor selection) | `sensorMask` | Which sensors the node should treat as installed. |

Note the distinction the operator must understand: **pause is reversible standby (still in the mesh); undeploy removes it.** The mothership already implements both (`recordingPaused` for pause, unpair for undeploy) and now records them through the dispatcher.

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
**OTA / release reasons:** `NONE`, `COMMAND_EXPIRED`, `COMMAND_DUPLICATE`, `REVISION_CONFLICT`, `SUPERSEDED`, `DEFERRED_LOW_BATTERY`, `DEFERRED_BUSY`, `LOCAL_SERVICE_LOCK`, `INCOMPATIBLE_ROLE`, `INCOMPATIBLE_HARDWARE`, `INCOMPATIBLE_PROTOCOL`, `DOWNGRADE_REJECTED`, `SD_MISSING`, `SD_FULL`, `MANIFEST_INVALID`, `SIGNATURE_INVALID`, `IMAGE_TOO_LARGE`, `SIZE_MISMATCH`, `HASH_MISMATCH`, `IMAGE_INVALID`, `FLASH_WRITE_FAILED`, `DOWNLOAD_TIMEOUT`, `DOWNLOAD_TRUNCATED`, `BOOT_VALIDATION_FAILED`, `ROLLED_BACK`, `DATA_BACKLOG_BLOCKED`, `OPERATOR_CANCELLED`.

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
*Questions? Talk to the firmware team — and see `FIELDMESH_OTA_FIRMWARE_UPDATE_PLAN.md` §4 (control model), §5 (release/manifest), §13 (status model), and §0 (what's built).*
