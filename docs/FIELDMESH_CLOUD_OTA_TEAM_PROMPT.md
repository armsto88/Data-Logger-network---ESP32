# Prompt: build the cloud/dashboard side of mothership firmware OTA

Hand this to the backend/frontend team (or their LLM). The **firmware side is
already built and bench-proven**; this describes only what the cloud must add so
an operator can update a hub's firmware from the dashboard. The authoritative,
source-grounded wire contract is **Appendix B of
`FIELDMESH_FIRMWARE_DASHBOARD_INTEGRATION_BRIEF.md`** — follow it exactly; the
summary below is orientation, Appendix B wins on any detail.

---

## System context
A fleet of solar ESP32 **nodes** is coordinated by a **mothership** (ESP32 +
LTE) that periodically POSTs status to the backend (Supabase) and reads back a
**command envelope** in the HTTP response. That reverse-command channel already
carries node config commands (pause / interval / sensors). You are adding one
new command, `DEPLOY_RELEASE`, plus the release hosting and dashboard UI to
drive a **mothership self-update**.

Non-negotiable architecture (already true for existing commands): the dashboard
never contacts a device. It writes intent to a durable per-mothership queue; the
mothership pulls it on its next check-in, and reports state on later check-ins.
Everything is eventually-consistent and "as of last check-in" — never real-time.

## What to build

### 1. Release catalog + immutable artifact hosting
- Let an operator upload a built, signed mothership firmware release: a
  `manifest.json`, its detached `manifest.json.sig` (Ed25519, 128 hex), and the
  `image.bin`. These are produced by the firmware team's `scripts/release_sign.py`
  and are opaque to you — **serve the exact bytes, never regenerate them** (the
  signature covers the exact manifest bytes).
- Serve them over **HTTPS** at these **exact** paths on the approved host the
  mothership is pinned to (`FM_APPROVED_ENDPOINT_HOST` — same Supabase project
  host as ingest). The firmware fetches the **Supabase Storage public-read
  URLs** (bytes served verbatim — no re-serialization → no `SIGNATURE_INVALID`;
  CDN-cached + immutable; no edge-function cold start in the LTE hot path):
  ```
  https://<host>/storage/v1/object/public/releases/mothership/<releaseId>/manifest.json
  https://<host>/storage/v1/object/public/releases/mothership/<releaseId>/manifest.json.sig
  https://<host>/storage/v1/object/public/releases/mothership/<releaseId>/image.bin
  ```
  The firmware **derives** these URLs from `<releaseId>` — you do **not** send a
  URL. A Supabase Storage public bucket at that path layout is the natural fit;
  set `Cache-Control: public, max-age=31536000` at upload for immutability.
- Keep the release **immutable** and the URL **stable/long-lived**: firmware has
  no mid-transfer resume, so a download interrupted by a check-in boundary
  restarts from byte 0 on the next wake; the URL must stay fetchable across that
  window (a ~1 MB image over LTE can span multiple wakes).
- Store `releaseSequence` (monotonic per role) with each release — it drives the
  firmware's anti-downgrade check.

### 2. Durable command queue: `DEPLOY_RELEASE`
- When the operator clicks "Update" for a hub, enqueue **one**
  `DEPLOY_RELEASE { releaseId }` command for that mothership, in the existing
  envelope format. It must be the **only** command in its check-in response
  (firmware rejects it batched with anything else). Exact shape: Appendix B §B.1.
- `releaseId` ≤ 39 chars. `commandId` immutable, ≤ 23 chars. Carry
  `expectedStateRevision` (compare-and-set), `issuedAtUnix`, `expiresAtUnix`.
- Idempotent delivery: keep serving the command until the mothership's reported
  cursor passes it; a repeated `commandId` is safely de-duplicated by firmware.

### 3. Dashboard UI + status mirroring
Drive everything off the mothership's `status.firmware{}` (Appendix B §B.3) and
`status.control{}` (existing) blocks — all "as of last check-in":
- **Update available:** the hub's confirmed-running `releaseId` (or `version`) is
  older than the newest catalog release compatible with its `hwTarget`/role.
  ("Available" is a catalog-vs-reported comparison you compute — it is NOT a
  device field.)
- **Update in progress:** `pendingReleaseId` non-null = a staged/deferring
  install. Show "queued / downloading, next check-in ≈ <time>" — never a live
  spinner (delivery rides check-ins).
- **Active / confirmed:** running `releaseId` == target and `otaState` =
  `CONFIRMED` on the active slot. This is the only "Active" signal — driven by a
  **confirmed boot**, not by "command sent."
- **Rolled back:** `armedReleaseId` was the target on one check-in, but a later
  check-in shows a different running `releaseId` (and `armedReleaseId` cleared) →
  render `ROLLED_BACK` ("update didn't take; device safe on previous version").
- **Deferred / failed reasons:** render `status.firmware{}.otaReason` using the
  vocabulary in Appendix B §B.4 and brief §5.5. Distinguish **deferred/transient**
  (`DEFERRED_LOW_BATTERY`, `DEFERRED_BUSY`, `DOWNLOAD_*`) — auto-retries, no
  action needed — from **terminal** (`SIGNATURE_INVALID`, `HASH_MISMATCH`,
  `DOWNGRADE_REJECTED`, `INCOMPATIBLE_*`, …) — operator must issue a fresh
  command. The old firmware always remains in the other A/B slot until the next
  update overwrites it; present rollback as a safe outcome, not a crash.

## Safety rules to reflect in the UI
- TLS to the device gives **no** identity assurance (the modem doesn't verify
  certs); integrity is the manifest signature + SHA-256. Don't imply "secure
  because HTTPS."
- Treat field OTA as needing a proven recovery path; don't imply "fire and
  forget." A confirmed rollback path exists, but surface state honestly.
- One `DEPLOY_RELEASE` in flight per hub at a time.

## What you do NOT need to build
- Any device-side install/verify/rollback — done in firmware.
- URL construction — firmware derives it; you just host the bytes at the paths.
- Node firmware OTA — out of scope (different transport, not yet built).

## How to validate before firmware is in the field
The firmware team can bench-prove the whole flow against a mock today (see
`FIELDMESH_CLOUD_OTA_BENCH_RUNBOOK.md`). Once your real endpoints exist, the
same firmware talks to them unchanged if you match Appendix B's contract. A good
acceptance test: enqueue a `DEPLOY_RELEASE`, watch the hub's `status.firmware{}`
walk `pendingReleaseId → armedReleaseId → releaseId==target + CONFIRMED` across
successive check-ins, and a deliberately-bad image walk to `ROLLED_BACK`.
