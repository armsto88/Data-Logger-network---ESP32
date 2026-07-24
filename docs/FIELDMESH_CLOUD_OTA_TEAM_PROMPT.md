# Prompt: build the cloud/dashboard side of mothership firmware OTA

Hand this to the backend/frontend team (or their LLM), in the dashboard/backend
repo. It is self-contained — you should not need to open the firmware repo to
act on it. The **firmware side is fully built and field-proven end-to-end as
of 2026-07-23**: a real device, on real cellular, pulled a real signed release
through the real backend and armed it for boot with zero errors (see "Proof
this works" below). This describes only what the cloud/dashboard side must add
so an operator can trigger and observe that from a browser.

---

## System context

A fleet of solar ESP32 **nodes** is coordinated by a **mothership** (ESP32 +
LTE) that periodically POSTs status to the backend (Supabase) and reads back a
**command envelope** in the HTTP response. That reverse-command channel
already carries node config commands (pause / interval / sensors). You are
adding one new command, `DEPLOY_RELEASE`, plus release hosting and a dashboard
UI to drive a **mothership self-update**.

Non-negotiable architecture (already true for existing commands): the
dashboard never contacts a device directly. It writes intent to a durable
per-mothership queue; the mothership pulls it on its own next check-in and
reports state on later check-ins. Everything is eventually-consistent and "as
of last check-in" — **never real-time, never a live progress bar.**

**Out of scope:** node (sensor-node) firmware OTA. Different transport
(ESP-NOW relay through the mothership, not direct LTE), not yet built — a
plan exists in `FIELDMESH_CLOUD_OTA_BENCH_STATUS_2026-07-19.md` §"Node OTA"
for later reference, no action needed from this team yet.

## Proof this works (2026-07-23)

Everything below was validated against a real device today, not just planned:
firmware built, deployed, and driven through the **actual backend** you'll be
building against — `enqueue_deploy_release`, the real `mothership_status`
check-in/response cycle, real Supabase Storage-hosted release artifacts. The
device downloaded a full ~1.3 MB signed image over live LTE in three chunked
HTTPS requests, verified its Ed25519 signature and SHA-256, wrote it to the
inactive OTA slot, and armed it for boot — zero errors, zero retries needed.
The stock ESP32 bootloader's automatic rollback-on-bad-boot has also been
separately bench-proven. **The one thing not yet independently observed is
the very next wake's first-boot self-test-confirm** (i.e., the final "did it
actually boot and stay running" step) — expect that to land imminently; ask
the firmware team for current status if it matters for your testing.

Bottom line: you are building a UI/API layer on top of a firmware path that
has already moved real, correctly-verified bytes through your real backend.
Any bug you hit integrating is much more likely to be in the backend/dashboard
layer than in an unproven device.

## The wire contract (authoritative — read this, don't guess field names)

### Command: `DEPLOY_RELEASE`
Delivered in the mothership's check-in response command envelope, same
mechanism as existing commands. Must be the **only** command in its response
(firmware rejects it batched with anything else).
- `releaseId` — string, ≤39 chars
- `commandId` — string, immutable once issued, ≤23 chars
- `expectedStateRevision` — the CAS value (see RPC design below)
- `issuedAtUnix`, `expiresAtUnix`
- Idempotent: a repeated `commandId` is safely de-duplicated by firmware —
  you can re-send the same command without fear of double-effect.

### Status: `status.firmware{}` (in every check-in upload)
Exact field names as serialized by firmware (`mothershipFirmwareStatusJson()`):
```json
{
  "role": "mothership",
  "version": "0.2.0",
  "buildId": "bench-001",
  "hwTarget": "mothership-v1",
  "protocolVersion": 2,
  "releaseId": "fieldmesh-2026.08.0" ,      // confirmed-RUNNING release, or null
  "pendingReleaseId": "fieldmesh-2026.08.1", // staged fetch intent, or null
  "armedReleaseId": null,                    // flashed+boot-armed, awaiting first-boot confirm, or null
  "runningSlot": "app0",
  "otaState": "CONFIRMED",                   // this slot's boot-validation state
  "otaReason": "NONE",                       // current/last reason, see vocabulary below
  "lastOtaReason": "NONE",
  "slots": [ /* per-slot A/B detail — label/version/buildId/state/active/nextBoot/present */ ]
}
```
Derive dashboard state **only** from this block (plus the catalog comparison
below) — never infer state from "we sent a command," only from what the
device itself reports on a later check-in.

### `otaReason` vocabulary (authoritative — from firmware's `fw_reason.h`)
| Value | Class | Meaning |
|---|---|---|
| `NONE` | — | idle / last operation succeeded |
| `DEFERRED_LOW_BATTERY`, `DEFERRED_BUSY`, `DEFERRED_BACKOFF` | **transient** | not attempted yet this wake, auto-retries — no operator action |
| `DOWNLOAD_FAILED`, `DOWNLOAD_TIMEOUT`, `DOWNLOAD_TRUNCATED`, `MODEM_UNAVAILABLE` | **transient** | a fetch attempt failed, auto-retries with backoff |
| `RETRY_LIMIT_EXCEEDED` | **terminal** | gave up after repeated transient failures — device clears the pending intent, will NOT retry on its own |
| `SIGNATURE_INVALID`, `MANIFEST_INVALID`, `HASH_MISMATCH`, `SIZE_MISMATCH`, `IMAGE_INVALID`, `FLASH_WRITE_FAILED` | **terminal** | bad/corrupt release or install failure — needs a fresh command, likely a new release |
| `NO_ARTIFACT_FOR_DEVICE` / `INCOMPATIBLE_HARDWARE` / `INCOMPATIBLE_PROTOCOL` | **terminal** | release doesn't apply to this device's role/hardware/protocol — should have been filtered at catalog level; if you see this, the "what's compatible" check has a gap |
| `DOWNGRADE_REJECTED` | **terminal** | anti-downgrade blocked it — device already newer |
| `ALREADY_INSTALLED` | — | no-op success, already running the target release |

**Any value not in this table:** treat as transient/auto-retry (matches
firmware's own forward-compatible default) — **except** treat unrecognized
values the same as your default, but `RETRY_LIMIT_EXCEEDED` specifically
**must** be coded as terminal, not left to the generic fallback, since an
unrecognized-reason-as-transient default would be wrong for it.

## What to build

### 1. Release catalog + immutable artifact hosting
- Operator (or firmware team's script) publishes a built, signed release: a
  `manifest.json`, its detached `manifest.json.sig` (Ed25519, 128 hex chars),
  and `image.bin`. These are produced by the firmware team's tooling and are
  **opaque to you — serve the exact bytes, never regenerate or re-serialize
  them** (the signature covers the exact manifest bytes; re-serializing breaks
  it).
- Serve over **HTTPS** at these **exact** paths (Supabase Storage public-read,
  same project host the mothership already talks to for ingest):
  ```
  https://<host>/storage/v1/object/public/releases/mothership/<releaseId>/manifest.json
  https://<host>/storage/v1/object/public/releases/mothership/<releaseId>/manifest.json.sig
  https://<host>/storage/v1/object/public/releases/mothership/<releaseId>/image.bin
  ```
  The firmware **derives** these from `<releaseId>` alone — you never send a
  URL. Set `Cache-Control: public, max-age=31536000` at upload (releases are
  immutable).
- Keep releases immutable and their URLs stable/long-lived: a ~1 MB image over
  LTE can span multiple check-ins if interrupted (firmware restarts the
  download from byte 0 on the next wake — no partial-resume yet at the
  transport level, this is a known, accepted characteristic, not a bug).
- Store `releaseSequence` (monotonic per role) with each release — it drives
  firmware's anti-downgrade check (`DOWNGRADE_REJECTED` if violated).
- Schema already exists and is confirmed correct: `firmware_releases` table —
  `release_id`, `role`, `release_sequence`, `version`, `build_id`,
  `hw_targets[]`, `protocol_version`, `min_mothership_version`, the three
  `*_object_path`/`*_sha256` columns, `image_size_bytes`, `lifecycle_state`
  (`DRAFT`/`PUBLISHED`), `uploaded_by`, timestamps. No schema changes needed.

### 2. One RPC that hides all the sharp edges (the highest-leverage item)

Getting a single command safely enqueued during firmware bench-testing
surfaced three real backend footguns that must **never** reach an operator:
a stale cached `state_revision` causing spurious `REVISION_CONFLICT`s, an
orphaned undelivered command silently blocking all future enqueues
(`FM_COMMAND_ALREADY_PENDING`), and a missing GRANT that 403'd a legitimate
read. The fix isn't better error messages — it's **one atomic backend
entry point** that does everything by hand a human had to do today, so none
of it ever surfaces:

```sql
request_mothership_update(
  p_user_id        uuid,
  p_project_id     uuid,
  p_mothership_id  uuid,
  p_release_id     text
) RETURNS jsonb
```

Internal steps (all hidden from the caller):
1. **Read `state_revision` fresh, at call time** — never from a cached page
   load. This alone eliminates almost every `REVISION_CONFLICT`, since the
   CAS window shrinks from "however long the dashboard tab's been open" to
   "one RPC execution."
2. **Check for an existing non-terminal `DEPLOY_RELEASE` for this hub.** If
   undelivered (`delivery_attempts=0`) — stale/orphaned, safe to discard —
   auto-cancel it via `cancel_undelivered_control_request` (note:
   `p_request_id` for that function is the **batch id**
   `control_command_batches.id`, returned as `batchId` by
   `enqueue_deploy_release` — not `control_commands.id`, not the text
   `command_id`) and proceed. If already delivered
   (`delivery_attempts>0`), that's a real in-flight update — return
   `{ok:false, reason:"ALREADY_UPDATING"}`, do not cancel it.
3. **Verify the release is `PUBLISHED`** and compatible with the hub's
   `hwTarget`/role — `{ok:false, reason:"RELEASE_UNAVAILABLE"}` if not.
4. **Call `enqueue_deploy_release`** with the freshly-read revision.
5. **Return a closed, small vocabulary** — never raw Postgres/RPC error
   codes, never a revision number, never `REVISION_CONFLICT`/
   `FM_COMMAND_ALREADY_PENDING`/`FM_CANCEL_WINDOW_CLOSED`/etc.:
   ```json
   { "ok": true, "commandId": "...", "expiresAt": "..." }
   ```
   ```json
   { "ok": false, "reason": "ALREADY_UPDATING" | "RELEASE_UNAVAILABLE" | "TRY_AGAIN" }
   ```

The dashboard's "Update" button calls **only** this RPC — never
`enqueue_deploy_release` directly, never a multi-step client-side flow.

### 3. Dashboard UI — four states, one button, nothing else

| State | What the user sees | Trigger |
|---|---|---|
| **Up to date** | "Hub is running v0.3.0 ✓" | running `releaseId` == newest `PUBLISHED` release compatible with the hub's `hwTarget`/role (a catalog-vs-reported comparison **you** compute — not a device field) |
| **Update available** | "Update to v0.3.1 →" (one button) | catalog has a newer compatible release than the hub reports |
| **Updating…** | "Update queued — hub will install on next check-in (~18 min)" | `pendingReleaseId` non-null, or the RPC just returned `ok:true` — **never a live spinner**, delivery only happens on the hub's own check-in cadence, don't poll faster than that |
| **Update failed** | plain-English reason (see table below), e.g. "Update didn't take — hub is safe on v0.3.0" | terminal `otaReason`, or `armedReleaseId` was the target on a check-in but a later check-in shows a *different* running `releaseId` (rollback occurred) |

Reason → plain English (never show a raw code):
- `SIGNATURE_INVALID` / `HASH_MISMATCH` / `MANIFEST_INVALID` → "Release
  verification failed — contact the firmware team."
- `RETRY_LIMIT_EXCEEDED` → "Repeated download failures — check the hub's
  signal, then try again."
- `DOWNGRADE_REJECTED` → "Hub is already newer than this release."
- Rollback detected (see trigger above) → "Update didn't take — hub is safe
  on the previous version."

**The user never sees:** revision numbers, commandIds, batchIds, cursors,
any raw RPC/Postgres error code, the words "CAS"/"compare-and-set"/"ratchet"/
"orphaned command", slot numbers, partition tables, NVS namespaces.

**A/B slot backup needs no new work** — it's already the hardware behavior.
Old firmware stays in the other slot untouched until the *next* OTA overwrites
it. Rollback is a safe, expected outcome, not a crash — present it that way.

### 4. One admin-only diagnostic, not exposed on the main button
If a hub's self-reported state and the backend's `state_revision` diverge
across multiple consecutive check-ins, that's a real anomaly (seen once
during firmware bench-testing after an unrelated NVS reset) — a device with
corrupted local state could get silently stuck in permanent
`REVISION_CONFLICT` with zero operator-visible signal. Add an admin-only flag
("state mismatch — hub reports revision X, backend expects Y") rather than
auto-correcting it — a device unexpectedly reporting a *lower* revision is
exactly the scenario the revision ratchet exists to catch; a human should
confirm before overriding it.

## What needs to be built (prioritized)

| # | What | Team | Effort | Why |
|---|---|---|---|---|
| 1 | **`request_mothership_update` RPC** | Backend | ~1 day | Highest leverage — eliminates every `REVISION_CONFLICT`/orphaned-command class of bug bench-tested today |
| 2 | **`publish_release.py`** — one-command signed publish (firmware team already has a bench version to productionize) | Firmware | ~2 hrs | Strip bench-only flags; no `--direct`/`--enqueue-only`/`--mac` in the production version |
| 3 | **Dashboard "Update" button** — calls the RPC, renders the 4 states above | Frontend | ~1 day | Spec is complete above; one button, four states, no error codes surfaced |
| 4 | **Migration for GRANTs** — `grant select on mothership_control_state to service_role`; confirm `enqueue_deploy_release` is `service_role`-only (revoke any `anon`/`authenticated` widening from bench testing) | Backend | ~30 min | The missing `mothership_control_state` GRANT would silently break the *first* real command to *any* hub, not just a bench device — must be a migration, not ad-hoc SQL |
| 5 | **`RETRY_LIMIT_EXCEEDED` handled as terminal** | Backend | ~30 min | See reason vocabulary above — this specific value must not fall through to the generic transient-retry default |

Five items, three teams, ~3 days total. After that:

**Firmware team:** `python scripts/publish_release.py --image ... --releaseId ... --version ...`
**Operator:** sees "Update available" → clicks "Update" → done.

## Safety rules to reflect in the UI

- TLS to the device gives **no identity assurance** (the modem doesn't verify
  certs) — integrity comes entirely from the manifest's Ed25519 signature +
  SHA-256, not from HTTPS. Don't imply "secure because HTTPS."
- One `DEPLOY_RELEASE` in flight per hub at a time — the RPC design above
  already enforces this (`ALREADY_UPDATING`).
- Never let the dashboard poll faster than the hub's real check-in cadence
  (~18 min) — there is nothing to see between check-ins, and polling harder
  doesn't make the update happen sooner.

## How to validate

A real mothership successfully completed exactly this flow against your real
backend on 2026-07-23 — that's your integration reference. A good acceptance
test once your endpoints exist: call `request_mothership_update`, watch a
hub's `status.firmware{}` walk `pendingReleaseId → armedReleaseId →
releaseId==target with otaState=CONFIRMED` across successive check-ins, and
separately confirm a deliberately-bad release walks to `otaReason` terminal +
the UI's "Update failed" state without ever surfacing a raw error code.
