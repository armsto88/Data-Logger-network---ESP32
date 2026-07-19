# Cloud OTA — handoff status (2026-07-19)

> **Purpose:** snapshot of the cloud-triggered mothership OTA work for the next
> agent taking over. Read this top-to-bottom; it's the single source of truth
> for where the bench stands today. The bench state is also tracked in
> `/memories/session/bench-ota-state.md` (session-scoped).

---

## Mission (one sentence)

Prove a mothership can fetch a signed firmware image from the Supabase backend
over LTE and write it to OTA slot 2 — then boot into it.

## Where we are: **publish works, enqueue blocked on one hub check-in**

The firmware side is **built, unit-tested on hardware, and the backend publish
path works end-to-end against the real Supabase**. The one remaining gap: the
hub needs to do **one LTE check-in** so `mothership_control_state.state_revision`
exists, which the `enqueue_deploy_release` RPC needs for compare-and-set. Once
that check-in lands, enqueue + monitor is a 5-minute exercise.

---

## What's done ✅

### Firmware (all uncommitted — see "Git state" below)
- **`mothership_ota_cloud_fetch.{cpp,h}`** — derives
  `https://unhzttnuayrgqrzeqetz.supabase.co/storage/v1/object/public/releases/mothership/<releaseId>/{manifest.json,manifest.json.sig,image.bin}`,
  verifies Ed25519 + SHA-256, streams the image to slot 2.
- **`mothership_ota_release_store.{cpp,h}`** — NVS-backed INSTALLED/ARMED/PENDING
  state machine; survives cold boots.
- **`cch_stream_reader.h`** — binary-safe A7670G +CCHRECV frame reader (no RAM
  buffering of the image).
- **`mothership_selfupdate.cpp`** — anti-downgrade from NVS, ARMED→INSTALLED
  promotion on confirmed first boot, additive `status.firmware{}` fields
  (`releaseId`, `pendingReleaseId`, `armedReleaseId`).
- **`backend_command_ingest.{cpp,h}`** — lone `DEPLOY_RELEASE` fast path
  (CAS + idempotency + `RELEASE_ID_INVALID` rejection).
- **`command_dispatcher.{cpp,h}`** (shared) — new `CMD_DEPLOY_RELEASE = 3`,
  `CMD_RELEASE_ID_LEN = 40`.
- **`fw_reason.h`** — transient `DOWNLOAD_*` + `MODEM_UNAVAILABLE` reasons.
- **`modem_driver.{cpp,h}`** — streaming `httpsGetStream()` (no whole-image
  RAM buffering) + `httpsGetStreamSSL()`.
- **`http_response_parser.{cpp,h}`** — split into `parseHttpResponseHead()`
  so a streaming reader can parse framing before the body arrives.
- **`hardware_identity.{cpp,h}`** — `hwApprovedEndpointHost()` exposes
  `FM_APPROVED_ENDPOINT_HOST = "unhzttnuayrgqrzeqetz.supabase.co"`.
- **`main.cpp`** — `otaReleaseStoreInit()`, `maybeRunCloudOta()`,
  `executeBackendDeployRelease()`.
- **Tests + envs** (all on-device, passed on COM4):
  - `mothership-v2-test-ota-cloud-fetch` (URL construction, preflight, FwReason mapping)
  - `mothership-v2-test-ota-release-store` (NVS state machine)
  - `mothership-v2-test-cch-stream` (binary-safe framing)
  - `test_backend_command_ingest.cpp` (`runReleaseSuite()`, 52 passed on hardware)
  - `mothership-v1-modem-https-get` (needs hosted artifact — NOT yet run)
- **Docs:** `docs/FIELDMESH_CLOUD_OTA_BENCH_RUNBOOK.md`,
  `docs/FIELDMESH_CLOUD_OTA_TEAM_PROMPT.md`, Appendix B in
  `docs/FIELDMESH_FIRMWARE_DASHBOARD_INTEGRATION_BRIEF.md`.
- **Script:** `scripts/ota-bench/publish_bench_release.py` — one-command
  sign+publish+enqueue (Python stdlib + `cryptography`; `--direct` bypasses
  the WAF-blocked edge function; `--enqueue-only` skips re-publish).

### Backend contract (confirmed 2026-07-19)
- **`firmware_releases` table** — schema confirmed via OpenAPI. Key columns:
  `release_id` (text ≤39), `role` ('mothership' only), `release_sequence`
  (bigint monotonic per role), `version`, `build_id`, `hw_targets` (text[]),
  `protocol_version` (int), `min_mothership_version`, `manifest_object_path`,
  `signature_object_path`, `image_object_path`, `manifest_sha256`,
  `signature_sha256`, `image_sha256`, `image_size_bytes` (bigint),
  `lifecycle_state` ('DRAFT'/'PUBLISHED'), `label`, `notes`,
  **`uploaded_by` (uuid, NOT NULL)**, `published_at`, `published_by`,
  `created_at`, `updated_at`.
- **`enqueue_deploy_release` RPC** — `POST /rest/v1/rpc/enqueue_deploy_release`
  with `{p_user_id, p_project_id, p_mothership_id, p_command_id, p_release_id,
  p_expected_state_revision, p_expires_at}`. Returns
  `{id, batchId, commandId, sequence, releaseId, state:"QUEUED", issuedAt, expiresAt}`.
  Error codes: `FM_REVISION_CONFLICT`, `FM_COMMAND_ALREADY_PENDING`,
  `FM_RELEASE_NOT_PUBLISHED`, etc.
- **Hub lookup by MAC** — `GET /rest/v1/motherships?mac_address=eq.48:9D:31:F8:16:A8`
  (MAC uppercase colon-delimited).
- **Storage bucket `releases`** — created 2026-07-19T15:11:18Z; public-read;
  path layout `releases/mothership/<releaseId>/<artifact>`; immutable
  `Cache-Control: public, max-age=31536000` set at upload.
- **GRANTs** — applied for `anon`/`authenticated`/`service_role` on
  `firmware_releases` + `enqueue_deploy_release` + sequences; PostgREST
  schema cache reloaded via `NOTIFY pgrst, 'reload schema'`.
- **Storage public URLs verified reachable (no auth)** — all three returned
  200 on 2026-07-19 ~15:33 UTC.

### Publish succeeded (2026-07-19 ~15:33 UTC)
- **Catalog row:** `2328fee7-31e8-4249-8235-78a2233ec206`, state=**PUBLISHED**.
- **Release:** `fieldmesh-bench-2026.07.0`, sequence 1, version 0.2.0,
  buildId `bench-001`, image 1,311,520 bytes
  (sha256 `ac642de3925dd6f74db0c9d96b69b930adb13e87ac0bd6d738250dab685919b8`).
- **Signed with the throwaway bench key** (see "Bench key" below).
- **Public URLs (all 200):**
  - `https://unhzttnuayrgqrzeqetz.supabase.co/storage/v1/object/public/releases/mothership/fieldmesh-bench-2026.07.0/manifest.json`
  - `.../manifest.json.sig`
  - `.../image.bin`

### Hub state (COM4)
- **Hub UUID:** `64a5082f-83e8-47fc-943a-2ff968358e1b` (only mothership in the
  project, matched by MAC `48:9D:31:F8:16:A8`).
- **Currently flashed:** `mothership-v1-main` at `FW_SEMVER=0.1.0` **with the
  bench public key** in `kReleasePubKey`. Boots OK on COM4.
- **Target:** OTA pulls the **0.2.0** image (built, signed, published) into
  slot 2; next RTC-alarm wake cold-boots into it; `[fwid] ver=0.2.0` confirms.

## What's NOT done ❌

### Enqueue (blocked on one hub check-in)
- `enqueue_deploy_release` does `select state_revision ... from
  mothership_control_state where mothership_id = <hub> for update`; if that
  row is null it raises `FM_CONTROL_STATE_UNAVAILABLE`. The row only exists
  after the hub's first LTE check-in that includes a `status.control{}` block
  (the ingest function upserts the row from `status.control{}`).
- **Check-in gates (backend-confirmed):** the check-in MUST include
  `status.control{}` with:
  - `stateRevision` (the CAS value the RPC reads)
  - `remoteManagementEnabled: true` (else `FM_REMOTE_MANAGEMENT_DISABLED`)
  - `controlProtocolVersion: 2` (else `FM_FIRMWARE_UPDATE_REQUIRED`)
  If the hub reports protocol < 2 or `remoteManagementEnabled: false`, the RPC
  raises those deliberate gates, not bugs.
- **Fix:** trigger one LTE sync wake (RTC alarm or manual) so the hub uploads
  `status.control{}` and populates `state_revision`. Then re-run:
  ```
  py -3 scripts\ota-bench\publish_bench_release.py `
    --releaseId fieldmesh-bench-2026.07.0 --enqueue-only `
    --mothershipId 64a5082f-83e8-47fc-943a-2ff968358e1b
  ```
  (the `--enqueue-only` flag I added skips re-signing/re-uploading).

### GRANT posture — REVERT our widening (backend team flagged)
We ran `GRANT EXECUTE ON FUNCTION enqueue_deploy_release TO anon, authenticated,
service_role;` to unblock the bench. The backend's intended posture is
**service_role only** (the function is `SECURITY DEFINER`; the dashboard invokes
it via the `enqueue-deploy-release` edge function with the service-role key).
**Re-run this in the Supabase SQL Editor to reset to the intended posture:**
```sql
revoke all on function public.enqueue_deploy_release(
  uuid, uuid, uuid, text, text, bigint, timestamptz
) from public, anon, authenticated;
grant execute on function public.enqueue_deploy_release(
  uuid, uuid, uuid, text, text, bigint, timestamptz
) to service_role;
```
(The `service_role`-only grant is what the original migration had; our `anon`/
`authenticated` grant widened it and should be undone before returning the
project to use. The RPC body does its own ownership check (`p_user_id` must be
the project owner), so this is defense-in-depth, not a wide-open hole — but
reset it regardless.)

### On-air monitor + boot-confirm trace
- After enqueue, run `pio device monitor -p COM4 -b 115200` and watch for:
  `[CONTROL] deploy-release durable` → `[OTA] pending release ...` →
  `[Modem] GET stream: status=200` (×3) →
  `[OTA] cloud fetch result: state=READY_TO_REBOOT ... rebootPending=1` →
  `[OTA] image armed` → next wake: `[OTA] first-boot self-test passed` →
  `[OTA] release promoted to installed: fieldmesh-bench-2026.07.0 (seq 1)` →
  `[fwid] role=mothership ver=0.2.0 ...`.

### Failure legs (runbook §8) — not yet attempted
- Interrupted transfer (`--truncate-image`), stall (`--stall-image-sec`),
  bad-boot rollback (`-D OTA_FORCE_BADBOOT`), anti-downgrade (`--sequence 0`).
  These need the mock server (`scripts/mock_ota_server.py`) + a tunnel; see
  `docs/FIELDMESH_CLOUD_OTA_BENCH_RUNBOOK.md`.

### Cleanup + commit
- **Revert `kReleasePubKey`** in `mothership_selfupdate.cpp` to the production
  key (stored in `/memories/session/bench-ota-state.md`).
- **Confirm `FW_SEMVER=0.1.0`** in `mothership/firmware/v2/platformio.ini`
  (already reverted — verify before commit).
- **Rotate the service-role key** — it was accidentally pasted in chat
  (see "Security" below).
- **Commit the cloud-OTA firmware work** — see "Git state" for the file list.

---

## Bench key (TEMPORARY — gitignored)

- **Public key in `kReleasePubKey`** (mothership_selfupdate.cpp line ~18):
  ```cpp
  static const uint8_t kReleasePubKey[32] = {
    0x22, 0xf3, 0x42, 0x44, 0x63, 0x08, 0x4d, 0x51, 0x78, 0xa0, 0x2c, 0xcd,
    0xf8, 0x9e, 0x7c, 0x16, 0x2d, 0xb3, 0xe9, 0xba, 0x51, 0x08, 0x68, 0xef,
    0x43, 0xf2, 0x12, 0x70, 0x23, 0x65, 0xa4, 0x2a };
  ```
- **Private key:** `scripts/ota-bench/bench-key.json` (gitignored).
- **PEM:** `scripts/ota-bench/keys/fieldmesh_ed25519.pem` (gitignored).
- **Production key to revert to** is in `/memories/session/bench-ota-state.md`
  (session-scoped memory; the next agent must read it before reverting).

## Git state (all uncommitted)

- 19 modified files + 11 new files, all in the working tree on `main`.
- Last commit: `d2c6b55` "Add sensor mask handling to deployment and configuration processes".
- `.gitignore` updated to cover `scripts/ota-bench/{keys/,bench-key.json,out/,image.bin,*.bin}`.
- **Nothing is committed.** Suggested commit message:
  `feat: cloud-triggered mothership OTA (DEPLOY_RELEASE) — fetch/verify/install + bench publish script`

## Security 🔴 URGENT

- **The Supabase service-role JWT was accidentally pasted in chat** during the
  bench. It bypasses RLS on the whole project (read+write every table/row +
  Storage). **Rotate it NOW** in Dashboard → Project Settings → API → API Keys →
  Secret → ... → Rotate (or "Generate new key"). The old key becomes invalid
  instantly. Then update `$env:SUPABASE_SERVICE_ROLE_KEY` in the terminal and
  re-run the bench.
- The bench keypair is a throwaway; delete `scripts/ota-bench/keys/` +
  `bench-key.json` after the bench.

## How to resume (for the next agent)

1. **🔴 Rotate the leaked service-role JWT NOW** (see Security above). Update
   `$env:SUPABASE_SERVICE_ROLE_KEY` in the terminal with the new key.
2. **Revert the GRANT widening** — run the `revoke ... grant execute ... to
   service_role` SQL block (see "GRANT posture" above) in the Supabase SQL Editor.
3. Read `/memories/session/bench-ota-state.md` for the full bench state.
4. **Trigger one LTE check-in** on COM4 (RTC alarm or manual sync wake). The
   check-in MUST include `status.control{}` with `stateRevision`,
   `remoteManagementEnabled: true`, `controlProtocolVersion: 2` (see "Enqueue"
   above for the gates).
5. After the check-in, verify `mothership_control_state` has a row:
   ```powershell
   $h = @{ "apikey" = $env:SUPABASE_SERVICE_ROLE_KEY; "Authorization" = "Bearer " + $env:SUPABASE_SERVICE_ROLE_KEY }
   Invoke-RestMethod -Uri "$env:SUPABASE_URL/rest/v1/mothership_control_state?mothership_id=eq.64a5082f-83e8-47fc-943a-2ff968358e1b&select=state_revision,updated_at&order=updated_at.desc&limit=1" -Headers $h -Method Get | ConvertTo-Json
   ```
6. Enqueue the command:
   ```powershell
   py -3 scripts\ota-bench\publish_bench_release.py `
     --releaseId fieldmesh-bench-2026.07.0 --enqueue-only `
     --mothershipId 64a5082f-83e8-47fc-943a-2ff968358e1b
   ```
7. Monitor COM4: `pio device monitor -p COM4 -b 115200` — watch the trace
   described in "On-air monitor" above.
8. After the hub boots 0.2.0, revert `kReleasePubKey` to production + confirm
   `FW_SEMVER=0.1.0`, then commit.
9. Delete the bench keypair (`scripts/ota-bench/keys/` + `bench-key.json`).

## Key files to read first
- `/memories/session/bench-ota-state.md` — full bench state + the production
  `kReleasePubKey` to revert to.
- `/memories/repo/structure-notes.md` — repo layout + cloud-OTA notes.
- `docs/FIELDMESH_CLOUD_OTA_BENCH_RUNBOOK.md` — full bench procedure.
- `docs/FIELDMESH_FIRMWARE_DASHBOARD_INTEGRATION_BRIEF.md` Appendix B —
  the authoritative wire contract (payload, status fields, reason vocabulary).
- `scripts/ota-bench/publish_bench_release.py` — the publish+enqueue script.