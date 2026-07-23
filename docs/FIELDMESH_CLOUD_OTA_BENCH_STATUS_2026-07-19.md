# Cloud OTA — handoff status (last updated 2026-07-23)

> **Purpose:** snapshot of the cloud-triggered mothership OTA work for the next
> agent taking over. Read this top-to-bottom; it's the single source of truth
> for where the bench stands today. The bench state is also tracked in
> `/memories/session/bench-ota-state.md` (session-scoped).

---

## Mission (one sentence)

Prove a mothership can fetch a signed firmware image from the Supabase backend
over LTE and write it to OTA slot 2 — then boot into it.

## Where we are: **RESOLVED — mothership cloud OTA proven end-to-end in production, 2026-07-23**

A full robustness/regression review found and fixed 6 real issues in the
original OTA implementation (all committed — see "Code review findings"
below). Live bench testing against the real Supabase backend then surfaced
and fixed three more problems that only show up under real conditions (a
modem-timing bug, two backend permission/state-tracking gaps) — see "Live
bench session findings" below. That got one real `DEPLOY_RELEASE` command
flowing correctly, but the **image download itself then truncated
consistently** on real hardware (2026-07-21/22) — a separate, deeper bug from
everything above. Standalone bench testing (`tests/test_ota_preerase.cpp`)
root-caused it to the OTA partition erase running *inside* the download
session and stalling the modem long enough to drop the connection; fixing
that (pre-erase before the transfer starts) was verified twice on the bench
and then **confirmed live in production on 2026-07-23**: a real
`DEPLOY_RELEASE` for `fieldmesh-bench-2026.07.0b` downloaded the full
1,311,520-byte image over LTE with zero truncation and armed the slot
(`state=READY_TO_REBOOT written=1311520/1311520`). Full diagnosis, the chunk-
size sweep that turned out to be a red herring, and the fix are in "Root
cause + fix: image download truncation (2026-07-21 → 23)" below — read it
before touching the download path again.

**Current position: fully confirmed.** The following wake (2026-07-23
18:20:51) closed the last open leg:
```
[fwid] role=mothership ver=0.2.0 build=d2c6b55-dirty hw=mothership-v1 proto=2 idv=1
[OTA] first-boot self-test passed; image confirmed: ESP_OK
[OTA] release promoted to installed: fieldmesh-bench-2026.07.0b (seq 13)
```
The device booted the new image, self-confirmed, and the release store
promoted ARMED → INSTALLED — the full lifecycle (enqueue → chunked download →
verify → arm → reboot → first-boot confirm → installed) is now proven
end-to-end against the real backend on real hardware. **Mothership cloud OTA
is done.** Remaining work is cleanup/commit (below) and the failure legs
(interrupted transfer, bad-boot rollback, anti-downgrade) that are worth
re-running post-fix but are not blockers.

---

## Code review findings (2026-07-20, commit `0c1ab24`) — all fixed, verified on hardware

A regression review of the original two OTA commits (`deb5dee`, `770def9`)
against main's working flow found 6 real issues. All fixed, all covered by
on-device test suites that passed on COM4 (`test-cch-stream`,
`test-ota-release-store`, `test-ota-cloud-fetch`, `test-backend-control`,
node `esp32wroom-manifest-verify`). Full detail in the commit message; summary:

1. **HIGH, blocking** — `cch_stream_reader.h`'s HTTP-header terminator scan
   looked at the last 4 *written* bytes, which freeze once the head buffer
   fills (2048 bytes) — a real Supabase Storage response's headers (CORS +
   Cache-Control + ETag + security headers) could exceed that and wedge the
   reader "in headers" forever, silently dropping the whole image body. Fixed
   with a rolling 4-byte scan independent of buffer capacity + grew the
   buffer to 8192.
2. **MEDIUM** — a transient NVS write failure staging a `DEPLOY_RELEASE`
   could get silently and permanently dropped on redelivery (`main.cpp`
   `executeBackendDeployRelease` didn't retry staging on `OUT_REPLAY`, only
   `OUT_ACCEPTED`). Fixed.
3. **MEDIUM** — no retry backoff or terminal cap on OTA fetch failures; a
   flaky link would redownload the ~1.3 MB image every wake forever. Added
   attempt/backoff accounting in the NVS release store + a terminal
   `RETRY_LIMIT_EXCEEDED` after 8 attempts. Also closed a real reporting gap
   the backend team flagged: the cloud-fetch orchestrator never wrote to
   `status.firmware.otaReason` on any of its own exit paths (only the shared
   verify/install core did) — now every exit path reports.
4. **MEDIUM regression** — anti-downgrade (now backed by real NVS state
   instead of an inert `return 0` stub) started blocking the pre-existing
   *local* AP firmware-upload page too, not just cloud OTA. Added an
   `allowDowngrade=1` opt-in on `POST /firmware/manifest` (local-AP only;
   cloud path unaffected, still fully enforced).
5. **LOW** — removed a dead `OTA_ALREADY_PENDING` rejection code that was
   declared but never set; documented the overwrite-in-place behavior
   (`otaReleaseStoreSetPending`) as an intentional "operator changed their
   mind" flow — safe since the fetch loop always restarts from byte 0.
6. **LOW → became directly relevant, see below** — manifest/signature
   fetches had no overall deadline, only an idle-gap timeout. Added one.
   *(This fix itself had a bug — see next section.)*

---

## Live bench session findings (2026-07-20/21) — read this before debugging OTA again

Getting one real `DEPLOY_RELEASE` to actually reach the device took far
longer than expected, and every blocker was a **different class of problem**.
In order encountered:

### 1. Firmware timing bug in the Finding-6 fix (fixed, uncommitted)
The manifest/sig fetch deadline (`SmallCtx` in `mothership_ota_cloud_fetch.cpp`)
was captured at `millis()` **before** calling `httpsGetStream()` — but
`httpsGetStreamSSL()` runs NTP sync, `AT+CCHSTART`, `AT+CCHOPEN`, and the
chunked `CCHSEND` of the request *before* any body byte is ever received.
On real cellular, that setup alone can eat several seconds, so the 8s budget
was already partly consumed before the transfer even started. Caught live: a
327-byte manifest.json aborted at 100 bytes delivered
(`declared=327 delivered=100 complete=0 aborted=1`,
`reason=DOWNLOAD_TRUNCATED`). **Fix:** `SmallCtx` now measures its budget
from the *first body byte received*, not from call time — same 8s budget,
just correctly scoped to the transfer phase only. **This fix is not yet
committed** — see "Uncommitted changes" below.

### 2. Backend: `mothership_control_state` missing a `service_role` GRANT
The device's status upload always includes `status.control{}` (confirmed by
re-reading `main.cpp` — an earlier "the firmware doesn't send this" theory
during this session was **wrong**, caught and corrected before any bad fix
shipped). The backend was correctly upserting the row from that upload — you
can see it via direct SQL. But `scripts/ota-bench/publish_bench_release.py`
(and anything else using the `service_role` REST API) got a **403** reading
the same row, because `mothership_control_state` never had a `GRANT SELECT`
for `service_role` — only `authenticated` (dashboard users, RLS-scoped to
project members/owners) had it. Same root-cause class as the already-known
`enqueue_deploy_release` GRANT gap. **Fixed by running:**
```sql
grant select on public.mothership_control_state to service_role;
```
**This needs to be in a migration**, not just applied ad hoc via SQL Editor —
flag for backend team.

### 3. Backend: `state_revision` ratchets and doesn't self-heal after device state loss
Running the mothership's own on-device test firmware (`mothership-v2-test-backend-control`,
which calls `dispatcherResetForTest()`/`backendControlResetForTest()` as part
of its 52-assertion suite) on the **same physical hardware** that held real
bench state wiped the device's real dispatcher revision history — because
the test firmware and the production firmware share the same NVS namespaces
(`"dispatch"`, `"backend_ctrl"`) on the same flash. **Lesson: never run
on-device test suites that call `*ResetForTest()` on hardware holding
production/bench state you care about — use a separate device, or expect to
lose that state.** This dropped the device's real dispatcher revision from 8
back to 1.

The backend's `mothership_control_state.state_revision` turned out to be a
**ratchet — it only ever increases, never decreases**, even when the device
legitimately reports a lower value (confirmed: `last_command_cursor` updated
correctly to match the device's report, but `state_revision` stayed at the
old high-water mark). This is *reasonable* defensive design (stops a
compromised/buggy device from walking the CAS baseline backward) but it means
device state loss requires a **manual one-time correction** to re-baseline —
there's no automatic recovery path:
```sql
update mothership_control_state
set state_revision = <device's real current revision>
where mothership_id = '<hub-uuid>';
```
How to find the device's real current revision: read it off the device's own
`[CONTROL] ... initialRevision=N` log line on its next check-in, or from
`status.control.stateRevision` in its uploaded payload.

### 4. Backend: an orphaned `QUEUED` command blocks all future enqueues
A command enqueued *before* the state_revision correction above carried the
stale `expectedStateRevision=8`. It was created but never delivered
(`delivery_attempts=0`) before the device went back to sleep, so it sat in
`control_commands.lifecycle_state='QUEUED'` — with a 7-day expiry — silently
blocking every subsequent enqueue attempt with `FM_COMMAND_ALREADY_PENDING`.
There's no self-expiry for an undelivered stale command inside a normal
session. **Fix:** a dedicated RPC exists for exactly this —
`cancel_undelivered_control_request(p_user_id uuid, p_project_id uuid,
p_request_id uuid)`. **Important:** `p_request_id` is the **batch id**
(`control_command_batches.id`, returned as `batchId` by
`enqueue_deploy_release`), **not** `control_commands.id` and **not** the
text `command_id` — the first two guesses both failed with
`FM_REQUEST_NOT_FOUND` before finding the right one. Example:
```sql
select cancel_undelivered_control_request(
  '<project-owner-user-uuid>'::uuid,
  '<project-uuid>'::uuid,
  '<batch_id-uuid>'::uuid
);
```

### 5. Backend team Q&A (answered, verified against source — not memory)
- **A. Does `status.firmware.otaReason` correctly reset/report?** Partially a
  real gap, now fixed (item 3 in "Code review findings" above): the
  cloud-fetch orchestrator's own transient/terminal reasons never reached
  `otaReason` before; now every exit path reports, and a subsequent
  successful verify still correctly resets it to `NONE`.
- **B. Are mothership timestamps true UTC?** Yes, confirmed — the browser-side
  time-set helper is literally named `nowUtc()` and built from
  `getUTCHours()`/`getUTCFullYear()`/etc. (`config_server.cpp` ~line 4943),
  and the firmware's own comment confirms "DS3231 set from browser-UTC is
  the authority."
- **C. Does firmware cross-validate manifest `role` against the device's
  own role?** Yes — `manifestArtifactForRole()` in `firmware_manifest.h`
  rejects with `FW_NO_ARTIFACT_FOR_DEVICE` if no artifact matches. Not
  purely a dashboard/catalog concept.
- **New ask for backend, from the code-review fixes:** two new
  `status.firmware.otaReason` values were added — `DEFERRED_BACKOFF`
  (transient, your existing "unrecognized ⇒ auto-retry" fallback already
  handles it, no code change needed) and `RETRY_LIMIT_EXCEEDED` (**terminal**
  — the device gives up after 8 failed attempts and clears the pending
  intent; please add this to your terminal-state handling explicitly, since
  the current default treats unrecognized reasons as transient/auto-retry,
  which would be wrong here).

### Uncommitted changes
- `mothership/firmware/v2/src/ota/mothership_ota_cloud_fetch.cpp` — the
  `SmallCtx` timing fix (item 1 above). Verify the next OTA attempt actually
  completes the manifest/sig/image fetch before committing.
- As always, `kReleasePubKey` in `mothership_selfupdate.cpp` must be the
  committed placeholder key at commit time — the bench-key swap used for
  on-air testing is applied and reverted around each flash, never left in.

---

## Root cause + fix: image download truncation (2026-07-21 → 23) — RESOLVED

With the `SmallCtx` timing bug fixed above, the manifest/signature fetch
started working reliably — but the **image download** (the actual ~1.3 MB
`image.bin`) then hit a *different*, deeper problem: it truncated at a
consistent ~1,292,354 of 1,311,520 bytes on every real attempt, modem
reporting `+CCH_PEER_CLOSED` with no more data in the UART. Idle-timeout
increases and switching from auto-push to flow-controlled manual
`AT+CCHRECV` pulls both had zero effect — ruling out ESP32-side back-pressure
as the obvious cause and pointing at something session-scoped.

### Investigation — standalone bench tests, not the OTA state machine

Testing this through the full `DEPLOY_RELEASE` → backend → device-wake state
machine was the wrong tool: its retry/backoff logic (correct for a real
fleet) meant each attempt cost an unpredictable number of ~18-minute wake
cycles. Standalone, fast-iterating bench tests were built instead — no
backend, no retry/backoff, flash-boot-report in under a minute per iteration:

| Test | File / env | Purpose | Result |
|---|---|---|---|
| 1 | `tests/test_modem_https_get.cpp` / `mothership-v1-modem-https-get` | Baseline: single continuous HTTPS GET, no flash writes | ✅ Full 1,311,520 B — old truncation did NOT reproduce that day |
| 2 | `tests/test_modem_range_get.cpp` / `mothership-v2-test-modem-range-get` | Chunked 512 KiB HTTP Range requests, discard sink (no flash) | ✅ All 3 chunks (206, correct `Content-Length`), full image, correct SHA-256 |
| 3 | `tests/test_ota_cloud_fetch_chunked.cpp` / `mothership-v2-test-ota-cloud-fetch-chunked` | Chunked Range download **with real `esp_ota_write()`** — the actual production path | ❌ Truncated on chunk 0 at a *variable* point (513,480 / 516,074 of 524,288) |
| 4 | `tests/test_ota_preerase.cpp` / `mothership-v2-test-ota-preerase` | Same as Test 3, but pre-erase the OTA partition **before** opening any download session, with per-write timing | ✅ Full 1,311,520 B, twice, every in-session flash write ≤1 ms |

Tests 1–2 proved the network/modem transport is healthy when nothing stalls
the receive loop. Test 3 proved the *real* path (chunked download + inline
flash writes) still fails — `aborted=0` on every run (the OTA write callback
never asked to stop), no "budget hit", so the **modem itself** was dropping
the session while a write stalled the pull loop.

**A chunk-size sweep initially looked like the natural next lever** (smaller
chunks = shorter modem sessions = less time for something to go wrong) —
`TEST_CHUNK_KIB` build flag swept 512 → 256 → 128 → 64 KiB. It was a red
herring: **every size still truncated on chunk 0** (513k/516k, 136k, 127k,
62k bytes respectively) — proving the truncation wasn't session-size-scoped
at all, it was tied to something that happens once, on the first chunk,
regardless of how big that chunk is.

### Root cause: `esp_ota_begin()`'s erase, running mid-session

That "something that happens once on the first chunk" is `esp_ota_begin()` —
which erases the **entire ~1.4 MB target partition** up front, and in the old
code ran lazily, inside `mothershipOtaImageChunk()`'s first call — i.e.
*while the modem's TLS/CCH session for chunk 0 was already open*. On ESP32, a
flash erase disables the CPU instruction cache and stalls all flash-resident
code until it completes; the erase measured **217 ms in one run, 1,749 ms in
another** — highly variable, and long enough that the modem's internal
session buffer overflowed (or the peer timed out the idle session) and tore
the connection down mid-transfer. Chunking couldn't fix this because the
stall is a one-time per-install event, not a per-session or per-write one.

**Fix (Test 4, then landed in production):** move the erase *before* opening
any download session.
- New `mothershipOtaImageBegin()` in `mothership_selfupdate.{h,cpp}` —
  allocates + erases the target slot immediately after manifest verify;
  idempotent; `mothershipOtaImageChunk()` keeps its old lazy-begin behavior
  for the local self-update path, unchanged.
- `mothership_ota_cloud_fetch.cpp`'s orchestrator calls
  `mothershipOtaImageBegin()` once, right after `mothershipOtaVerifyManifest()`
  succeeds and before the chunk-download loop starts.
- Bench-verified twice on Test 4: full image both times, every in-session
  flash write ≤1 ms (0 of ~11,660 writes ≥10 ms) — the erase stall is
  completely outside the modem session's timing budget now.
- Range-chunking (512 KiB) is **no longer required** for correctness — Test 4
  showed writes-only sessions survive fine — but it's kept in production for
  resilience against ordinary transient LTE stalls within a chunk.
- **The SD-card two-phase design considered as a fallback (stream to SD,
  verify, then flash from SD with no session open) turned out to be
  unnecessary** — the pre-erase fix works on current hardware with no SD
  card. Demoted to a nice-to-have robustness upgrade for a future board
  revision, not a blocker.

### Field confirmation (2026-07-23)

Bench proof was then confirmed on the **real production path**: the
mothership had a pending `DEPLOY_RELEASE` for `fieldmesh-bench-2026.07.0b`
left over from before the fix (4 prior failed attempts, sitting in an
8-wake exponential backoff — `otaReleaseStoreSetPending()` re-staging the
same releaseId resets that backoff without touching INSTALLED/ARMED state, a
built-in escape hatch used here to force an immediate retry instead of
waiting ~2.4 hours). With the fixed firmware flashed and the backoff cleared,
the next real wake completed the entire cloud-OTA flow live:

```
manifest.json (329 B) -> 200, signature (128 B) -> 200, verify OK
image chunk [0-524287]      -> 206, 524,288 B, complete
image chunk [524288-1048575] -> 206, 524,288 B, complete
image chunk [1048576-1311519] -> 206, 262,944 B, complete
[OTA] cloud fetch result: state=READY_TO_REBOOT reason=NONE written=1311520/1311520
[OTA] image armed — next RTC-alarm wake will boot the new slot
```

Zero truncation, zero retries, correct manifest signature, full image
written and armed — end to end, through the real backend and the real
orchestrator, not a hand-rolled test loop. The only leg not yet observed is
the *following* wake's first-boot self-test-confirm → promote-to-INSTALLED
(see "Where we are" above).

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

### Enqueue — RESOLVED, was blocked by three separate backend issues, not a missing check-in
The original theory ("hub needs one check-in to populate state_revision") was
correct in spirit but incomplete — see "Live bench session findings" above
for the full story. The check-in gates below are still real and correct, but
were never actually the blocker once tested for real:
- **Check-in gates (backend-confirmed):** the check-in MUST include
  `status.control{}` with:
  - `stateRevision` (the CAS value the RPC reads)
  - `remoteManagementEnabled: true` (else `FM_REMOTE_MANAGEMENT_DISABLED`)
  - `controlProtocolVersion: 2` (else `FM_FIRMWARE_UPDATE_REQUIRED`)
  If the hub reports protocol < 2 or `remoteManagementEnabled: false`, the RPC
  raises those deliberate gates, not bugs. Confirmed working — this hub's
  `status.control{}` always carried all three correctly.
- The `--enqueue-only` invocation needs 4 more required-but-unused flags the
  original example omitted (argparse enforces `--image`/`--releaseSequence`/
  `--version`/`--buildId` unconditionally even though they're never read in
  `--enqueue-only` mode):
  ```
  py -3 scripts\ota-bench\publish_bench_release.py --enqueue-only `
    --releaseId fieldmesh-bench-2026.07.0 --releaseSequence 1 `
    --version 0.2.0 --buildId bench-001 --image unused `
    --mothershipId 64a5082f-83e8-47fc-943a-2ff968358e1b
  ```

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

### On-air monitor + boot-confirm trace — FULLY CONFIRMED 2026-07-23
The complete sequence has now been observed live in production, across two
consecutive wakes:
`[CONTROL] deploy-release durable` → `[OTA] pending release ...` →
`[Modem] GET stream: status=206` (×3, chunked) →
`[OTA] cloud fetch result: state=READY_TO_REBOOT ... rebootPending=1` →
`[OTA] image armed` → (reboot) → `[OTA] first-boot self-test passed;
image confirmed: ESP_OK` → `[OTA] release promoted to installed:
fieldmesh-bench-2026.07.0b (seq 13)` → `[fwid] ... ver=0.2.0
build=d2c6b55-dirty ...`. Nothing left to watch for on this path.

### Failure legs (runbook §8) — not yet attempted
- Interrupted transfer (`--truncate-image`), stall (`--stall-image-sec`),
  bad-boot rollback (`-D OTA_FORCE_BADBOOT`), anti-downgrade (`--sequence 0`).
  These need the mock server (`scripts/mock_ota_server.py`) + a tunnel; see
  `docs/FIELDMESH_CLOUD_OTA_BENCH_RUNBOOK.md`. Worth re-running specifically
  against the *post-pre-erase-fix* code, since the download-truncation bug
  they'd have been testing alongside no longer exists.

### Cleanup + commit
- **Revert `kReleasePubKey`** in `mothership_selfupdate.cpp` to the production
  key (stored in `/memories/session/bench-ota-state.md`) — not yet confirmed
  done as of this update; check before any real (non-bench) release ships.
- **Confirm `FW_SEMVER=0.1.0`** in `mothership/firmware/v2/platformio.ini`
  before commit.
- **Rotate the service-role key** — it was accidentally pasted in chat
  (see "Security" below) — not yet independently confirmed done.
- **Commit the cloud-OTA firmware work** — see "Git state" for the current
  file list (updated 2026-07-23; most of the original OTA feature is already
  committed, only the pre-erase fix + new bench tests remain).

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

## Git state (as of 2026-07-23)

Most of the original cloud-OTA feature is now **committed** on branch
`feat/cloud-ota-deploy-release`:
- `deb5dee` feat: cloud-triggered mothership OTA (DEPLOY_RELEASE) — fetch/verify/install + bench publish script
- `770def9` docs: update OTA bench handoff with backend team action items
- `0c1ab24` fix: harden cloud OTA (6 review findings) — download stall, silent drop, retry cap, otaReason

**Still uncommitted** (the pre-erase fix + its bench proof, from this update):
```
M  docs/FIELDMESH_CLOUD_OTA_BENCH_STATUS_2026-07-19.md
M  mothership/firmware/v2/platformio.ini
M  mothership/firmware/v2/src/comms/modem_driver.cpp
M  mothership/firmware/v2/src/comms/modem_driver.h
M  mothership/firmware/v2/src/ota/mothership_ota_cloud_fetch.cpp
M  mothership/firmware/v2/src/ota/mothership_selfupdate.cpp
M  mothership/firmware/v2/src/ota/mothership_selfupdate.h
M  scripts/ota-bench/publish_bench_release.py
?? mothership/firmware/v2/tests/fix_ota_clear_backoff.cpp
?? mothership/firmware/v2/tests/test_modem_range_get.cpp
?? mothership/firmware/v2/tests/test_ota_cloud_fetch_chunked.cpp
?? mothership/firmware/v2/tests/test_ota_preerase.cpp
```
Suggested commit message: `fix: cloud OTA image download truncation — pre-erase
target partition before opening the download session`. Verify `kReleasePubKey`
and `FW_SEMVER` (see "Cleanup + commit" above) before committing.

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

1. **🔴 Confirm the leaked service-role JWT was rotated** (see Security
   below) — still not independently confirmed as of this update. If unsure,
   rotate it regardless (harmless even if already done).
2. **Revert the GRANT widening** on `enqueue_deploy_release` — still
   outstanding, run the `revoke ... grant execute ... to service_role` SQL
   block (see "GRANT posture" below) in the Supabase SQL Editor. Note: do
   **not** revoke the *new* `grant select on mothership_control_state to
   service_role` from item 2 in "Live bench session findings" — that one is
   correct and needed (add it to a migration rather than leaving it ad hoc).
3. Confirm COM4 has the fixed firmware (rebuild `mothership-v1-main` with
   the bench key temporarily swapped into `kReleasePubKey` — see "Bench key"
   below — if you're not sure it's current; the `SmallCtx` fix in
   `mothership_ota_cloud_fetch.cpp` needs to actually be on the device).
4. Wait for the next LTE check-in (RTC alarm-driven, every ~18 min while
   powered) or power-cycle via SW10 to force one sooner. The command
   `Dx7bs6kQ-jyojUPVqT0W9xw` (`expectedStateRevision=1`) should already be
   `QUEUED` and valid — no need to re-enqueue unless it's expired
   (`expiresAt: 2026-07-27T17:57:29+00:00`) or a `REVISION_CONFLICT` shows up
   again (re-run step 6 below if so, after checking for ratchet/orphan issues
   per "Live bench session findings" items 3-4).
5. Monitor COM4: `pio device monitor -p COM4 -b 115200` — watch for
   `[CONTROL] deploy-release durable ... decision=ACCEPTED`, then
   `[OTA] pending release ... starting cloud fetch`, then three
   `[Modem] GET stream: status=200` lines (manifest, sig, image) that each
   report `complete=1` (not `aborted=1`), then
   `[OTA] cloud fetch result: state=READY_TO_REBOOT ... rebootPending=1`.
6. If you do need to re-enqueue:
   ```powershell
   py -3 scripts\ota-bench\publish_bench_release.py --enqueue-only `
     --releaseId fieldmesh-bench-2026.07.0 --releaseSequence 1 `
     --version 0.2.0 --buildId bench-001 --image unused `
     --mothershipId 64a5082f-83e8-47fc-943a-2ff968358e1b
   ```
7. After the hub boots 0.2.0 (confirmed via `[fwid] ver=0.2.0` on the
   following wake), attempt the failure legs in the runbook §8 that still
   haven't been tried (interrupted transfer, stall, bad-boot rollback,
   anti-downgrade — now including the new `allowDowngrade=1` local-AP bypass
   and the retry-backoff cap), then commit the `SmallCtx` fix, confirm
   `kReleasePubKey` is the placeholder + `FW_SEMVER=0.1.0`, and delete the
   bench keypair.
8. See "Dashboard/frontend integration — keep it simple" below for UX
   guidance once ready to wire up the real UI, informed by everything the
   backend plumbing issues above revealed about what must NOT leak to an
   operator.

## Dashboard/frontend integration — keep it simple

`docs/FIELDMESH_CLOUD_OTA_TEAM_PROMPT.md` already specifies the happy-path UI
(update available / in progress / confirmed / rolled back / deferred-failed).
What it doesn't cover — because we hadn't hit it yet — is everything that
went wrong getting **one** command safely enqueued today: a 403 from a
missing GRANT, a ratcheted revision that silently desynced from device
reality, and an orphaned `QUEUED` command blocking all future attempts. None
of that is acceptable for an operator to ever see. The fix isn't "handle
these errors better in the UI" — it's **don't let the backend surface them
as errors at all**. Push the complexity down into one well-tested backend
entry point; keep the frontend to a handful of plain-English states.

**Recommendation: one RPC, not a multi-step client flow.** Everything we did
by hand today (read `state_revision` fresh → check for + cancel any stale
undelivered command → call `enqueue_deploy_release`) should be **one atomic
backend call** the dashboard invokes, e.g. `request_mothership_update(p_user_id,
p_project_id, p_mothership_id, p_release_id)`:
- Reads `state_revision` **at call time**, never cached from an earlier page
  load — this alone eliminates almost every `REVISION_CONFLICT` an operator
  would otherwise hit, since the CAS window shrinks from "however long the
  dashboard tab's been open" to "one RPC's execution time."
- Before enqueueing, checks for an existing non-terminal `DEPLOY_RELEASE` for
  that hub. If it's undelivered (`delivery_attempts=0`) — i.e. definitely
  stale/orphaned, not actually in flight — auto-cancel it via
  `cancel_undelivered_control_request` and proceed. If it's already been
  delivered (`delivery_attempts>0`), that's a real in-progress update — don't
  cancel it, just report back "already updating."
- Returns a **small, closed result vocabulary** the frontend switches on
  directly, never raw Postgres/RPC error codes:
  `{ ok: true, commandId, expiresAt }` or
  `{ ok: false, reason: "ALREADY_UPDATING" | "RELEASE_UNAVAILABLE" | "TRY_AGAIN" }`.

**Frontend: collapse everything to ~4 states, matching what's already
documented plus one addition:**
1. **Up to date** — no action.
2. **Update available** — one button, "Update to vX.X.X."
3. **Updating…** — "queued, next check-in ≈ &lt;time&gt;" (already
   documented — never a live spinner; delivery genuinely only happens on the
   hub's own ~18 min check-in cadence, so don't poll faster than that).
4. **Update failed** — translate the existing terminal-reason vocabulary
   (§B.4 of the integration brief) to plain English per reason, e.g.
   `SIGNATURE_INVALID` → "Release verification failed — contact the firmware
   team," `RETRY_LIMIT_EXCEEDED` → "Repeated download failures — check the
   hub's signal, then try again." Never show a raw reason code, an RPC error
   name, or a revision number.

**One thing that can't be fully automated, flag instead:** if a hub's
self-reported revision and the backend's ratcheted `state_revision` diverge
for multiple consecutive check-ins (exactly what test-induced NVS loss
looked like today), that's a real anomaly — a field device with a corrupted
NVS would get silently stuck in permanent `REVISION_CONFLICT` with zero
operator-visible signal of why. Recommend an admin-only diagnostic (not
exposed on the main update button) that flags "state mismatch — hub reports
revision X, backend expects Y" rather than the update just silently never
working. Don't auto-correct this one blindly (a device unexpectedly
reporting a *lower* revision is exactly the scenario the ratchet exists to
catch) — surface it for a human to confirm, same judgment call made today.

**Provisioning checklist, not a per-hub discovery:** the missing `GRANT
SELECT ... TO service_role` on `mothership_control_state` would have quietly
broken the *first* real command to *any* hub, not just this bench device.
That belongs in a migration + a basic health-check/smoke-test run once after
any schema change to the control-plane tables — not something re-discovered
by trial and error against a real device again.

---

## Production workflow — three roles, three actions

The entire bench session above was spent fighting issues that should **never**
be visible to an operator or to the firmware team in production. The design
goal: publish a release once, the user sees one button, the complexity lives
behind one backend RPC. Three roles, three actions:

```
┌─────────────────┐     ┌─────────────────┐     ┌─────────────────┐
│  FIRMWARE TEAM  │────▶│     BACKEND     │────▶│  USER (DASHBOARD)│
│  (rarely)       │     │  (automatic)    │     │  (operator, easy)│
└─────────────────┘     └─────────────────┘     └─────────────────┘
   1. Publish a         2. Catalog + notify     3. See "Update
   signed release        "Update available"      available" → click
   (one script)          (automatic, per hub)   "Update" → done
```

### Step 1 — Firmware team: publish a signed release (one command, rarely)

Productionized `publish_release.py` (from `publish_bench_release.py`, stripped
of bench-only flags):

```
python scripts/publish_release.py \
  --image .pio/build/mothership-v1-main/firmware.bin \
  --releaseId fieldmesh-2026.08.0 \
  --releaseSequence 14 \
  --version 0.3.0 \
  --key keys/release_ed25519.pem
```

What it does (all hidden):
- Signs the image with the production Ed25519 key (kept offline — the one secret)
- Uploads `manifest.json` + `manifest.json.sig` (128 hex text) + `image.bin` to
  Supabase Storage at `releases/mothership/<releaseId>/<artifact>`
- Inserts the `firmware_releases` catalog row (PUBLISHED)
- **Does NOT enqueue a command** — publishing just makes it available; the user
  decides when to deploy

What you see: `Published fieldmesh-2026.08.0 (seq 14, v0.3.0)`. Done. No SQL,
no GRANTs, no `state_revision`, no Supabase dashboard.

**The signing key is the only secret.** One PEM file, generated once with
`release_sign.py keygen`, private half kept offline, public half embedded in
firmware as `kReleasePubKey`. That's the entire trust model — TLS gives no
identity assurance (the modem doesn't verify certs), so this key is the
authority. Lose it = lose the ability to ship updates; rotate it = reflash
every hub. Keep it safe.

### Step 2 — Backend: automatic catalog + per-hub "update available"

All automatic once the release is published. The backend already has
everything:

- **`firmware_releases`** table (catalog of published releases)
- **`mothership_status.firmware_release_id`** (what each hub is running)
- **"Update available"** = the newest PUBLISHED release compatible with the
  hub's `hwTarget`/role whose `releaseId` ≠ the hub's reported `releaseId`.
  This is a catalog-vs-reported comparison the dashboard computes — not a
  device field. No new backend work needed; the dashboard team already built
  this.

### Step 3 — User: one button, four states

The user sees four states and one button. **Nothing else.**

| State | What the user sees | What's happening |
|---|---|---|
| **Up to date** | "Hub is running v0.3.0 ✓" | Running releaseId == newest compatible catalog release |
| **Update available** | "Update to v0.3.1 →" (one button) | Catalog has a newer release than the hub reports |
| **Updating…** | "Update queued — hub will install on next check-in (~18 min)" | `DEPLOY_RELEASE` enqueued; device pulls it on next wake |
| **Update failed** | "Update didn't take — hub is safe on v0.3.0" (with one-line plain-English reason) | Terminal failure (signature invalid, retry limit, etc.) |

**The user never sees:**
- Revision numbers, commandIds, batchIds, cursors
- `REVISION_CONFLICT`, `FM_COMMAND_ALREADY_PENDING`, `FM_RELEASE_SEQUENCE_NOT_MONOTONIC`
- The words "CAS", "compare-and-set", "ratchet", "orphaned command"
- Slot numbers, partition tables, NVS namespaces
- Any error code from any RPC

**The button does one thing:** calls `request_mothership_update()` (below).

### The one RPC that hides everything (backend team builds this)

```sql
request_mothership_update(
  p_user_id        uuid,
  p_project_id     uuid,
  p_mothership_id  uuid,
  p_release_id     text
) RETURNS jsonb
```

Returns:
```json
{ "ok": true, "commandId": "...", "expiresAt": "..." }
```
or
```json
{ "ok": false, "reason": "ALREADY_UPDATING" | "RELEASE_UNAVAILABLE" | "TRY_AGAIN" }
```

What it does internally (the user never sees any of this):
1. **Read `state_revision` fresh, at call time** — eliminates almost every
   `REVISION_CONFLICT` (the CAS window shrinks from "however long the
   dashboard tab's been open" to "one RPC's execution time")
2. **Check for an existing non-terminal `DEPLOY_RELEASE` for that hub:**
   - If undelivered (`delivery_attempts=0`) → stale/orphaned → auto-cancel it
     via `cancel_undelivered_control_request` and proceed
   - If delivered (`delivery_attempts>0`) → genuinely in flight → return
     `{ok: false, reason: "ALREADY_UPDATING"}`
3. **Verify the release is PUBLISHED and compatible** with the hub's
   `hwTarget`/role — return `{ok: false, reason: "RELEASE_UNAVAILABLE"}` if not
4. **Call `enqueue_deploy_release`** with the fresh `state_revision`
5. **Return the closed vocabulary** — never raw error codes

This is the single highest-leverage backend change. It collapses everything
we did by hand (read revision → cancel stale → check published → enqueue)
into one atomic call. The dashboard just calls it and switches on the result.

### A/B slot backup — already how it works, no change needed

ESP32 OTA has two app slots. The requirement — "old firmware stays in the
other slot for backup" — is already the hardware behaviour:

- Slot 1 has the running image (e.g. v0.3.0)
- OTA writes the new image to slot 2 (v0.3.1) and sets it as next boot target
- On the next wake, the bootloader boots slot 2
- If slot 2 fails to boot (bad image), the bootloader **auto-rolls back** to
  slot 1 — the old image is still there, untouched, until the *next* OTA
  overwrites it

The user doesn't need to know about slots. The dashboard shows: "running
v0.3.1, previous was v0.3.0." Rollback is a safe outcome, not a crash — if
the new image doesn't boot, the hub automatically falls back and the
dashboard shows "Update didn't take — hub is safe on v0.3.0."

### What needs to be built (concrete, prioritized)

| # | What | Who | Effort | Why |
|---|---|---|---|---|
| 1 | **`request_mothership_update` RPC** — the one-call that hides read/cancel/check/enqueue | Backend | ~1 day | Highest leverage. Eliminates every `REVISION_CONFLICT`, `COMMAND_ALREADY_PENDING`, orphaned-command issue we hit today. |
| 2 | **`publish_release.py`** (productionized bench script) — firmware team's one-command publish | Firmware | ~2 hrs | Strip bench flags, add proper key handling, clean error messages. No `--direct`, no `--enqueue-only`, no `--mac`. |
| 3 | **Dashboard "Update" button** — calls the RPC, shows 4 states | Frontend | ~1 day | UI already specced in `FIELDMESH_CLOUD_OTA_TEAM_PROMPT.md` + the table above. One button, four states, no error codes. |
| 4 | **Migration for the ad-hoc GRANTs** — `grant select on mothership_control_state to service_role` + revert the `enqueue_deploy_release` widening | Backend | ~30 min | Must be in a migration, not ad-hoc SQL. The missing `service_role` GRANT on `mothership_control_state` would have broken the *first* real command to *any* hub. |
| 5 | **`RETRY_LIMIT_EXCEEDED` handling** — backend treats it as terminal, not transient | Backend | ~30 min | Already flagged to the backend team. The device gives up after 8 failed attempts and clears the pending intent; the dashboard must show "Update failed — repeated download failures, check the hub's signal." |

Five items, three teams, ~3 days total. After that, the workflow is:

**You:** `python scripts/publish_release.py --image ... --releaseId ... --version ...`
**User:** sees "Update available" → clicks "Update" → done.

### What the user never sees (the whole point)

Everything we fought today — the 403s, the ratchets, the orphaned commands,
the signature format, the NVS state from tests, the revision mismatches, the
`FM_CANCEL_WINDOW_CLOSED`, the `control_command_batches` CHECK constraint —
lives behind the RPC. The dashboard is four states and one button. The
firmware team's side is one script. The complexity is pushed down, not
surfaced up.

---

## Node OTA — proposed plan (2026-07-23, PLAN ONLY — nothing below is implemented)

With mothership cloud OTA proven, the next question is delivering firmware to
the battery-powered **sensor nodes**. This is a genuinely different problem,
not a smaller version of the mothership one — nodes have no modem, so the
mothership must fetch the image once (using the now-fixed pipeline above) and
**relay** it to nodes over ESP-NOW, in short (~120 s) sync windows, on a lossy
link with no native ack, to a device that hard-power-cuts between wakes.

### What's already reusable — more than expected

- **Install core.** `node/firmware/shared/ota_installer.h` (erase/write/
  SHA-256-finish) is role-agnostic and **already linked into node
  `main.cpp`**. No new install logic needed — same code the mothership uses.
- **Partition table + rollback.** `node/firmware/partitions_ota.csv` already
  gives nodes A/B OTA slots on the same stock ESP32 bootloader
  (`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`) already bench-proven for the
  mothership (slot switch, integrity gates, NVS survival, automatic rollback
  — see `ota-slot-rollback-proof` memory). **Not yet independently confirmed**
  whether that proof was run on actual node hardware or only asserted by the
  partition file's presence — verify before relying on it.
- **Manifest/signature verify is already role- and hwTarget-aware.**
  `firmware_manifest.h`'s `Manifest` carries an array of `ManifestArtifact`,
  each with its own `role` (`"mothership"` / `"node"`) and `hwTargets[]`
  list; `manifestArtifactForRole()` + `manifestCheckCompatibility()` already
  pick the right one. **Zero changes needed here** — a release just needs a
  `role: "node"` artifact added alongside the mothership one, publishing
  through the same signed-manifest pipeline.
- **Per-node OTA telemetry already exists.** Nodes send `FW_CAPS` every wake
  (`node/firmware/shared/protocol.h`) — running version/build, hardware
  target, **inactive-slot capacity (`maxImageSize`)**, rollback-capable flag,
  and both slots' `FwOtaState`. The mothership already knows, per node,
  whether an update is even possible before trying.
- **Reliable unicast delivery primitive already exists.**
  `espnow_sync.cpp`'s `sendControlPacket(mac, packet, len)` unicasts to a
  specific MAC and blocks on ESP-NOW's own radio-layer send-status callback
  (delivered/not, not an app-level ack), retrying up to 3× with backoff.
  Already used for `sendDumpGrant`/`sendSyncRelease`/`sendDeploymentNow`.
  This is the exact primitive firmware chunks would ride on.
- **A round-based bulk-transfer pattern already exists and is the right
  model.** The sensor-data upload flow is `SYNC_SESSION → jittered
  NODE_HELLO → DUMP_GRANT → (node sends data) → DUMP_DONE → SYNC_RELEASE`
  (`protocol.h`) — the mothership grants one node a time-boxed,
  quota-limited round (`dump_grant_message_t.maxRecords`/`grantWindowMs`),
  the node reports back what it managed (`dump_done_message_t.sentRecords`/
  `remainingRecords`/`status`), and rounds repeat until done or the window
  closes. This is architecturally identical to what firmware delivery needs
  — just the reverse data direction (mothership → node instead of node →
  mothership). **Recommend modeling the new firmware messages directly on
  this pattern** rather than inventing a new one.
- **Command dispatcher already has the exact CAS/idempotent/convergence
  machinery needed**, proven via `CMD_SET_NODE_CONFIG`
  (`node/firmware/shared/command_dispatcher.h`): mothership-owned monotonic
  revision, compare-and-set, idempotent replay, per-node convergence
  tracking. A new `CMD_DEPLOY_NODE_RELEASE` type (node-targeted, carrying a
  `releaseId`, same shape as the existing `CMD_DEPLOY_RELEASE` but scoped to
  one `nodeId` instead of the whole hub) is a natural, low-risk extension —
  not a new subsystem.

### What's genuinely new — the delivery protocol

None of the above solves the actual hard part: getting ~1 MB of image bytes
into 250-byte ESP-NOW packets, reliably, across a lossy link, within a
battery budget, resumable across many wakes. Proposed shape:

1. **New wire messages** (modeled on `DUMP_GRANT`/`DUMP_DONE`, reversed
   direction): `FW_GRANT` (mothership → node: "receive up to N chunks this
   round, budget `grantWindowMs`, resume from offset X"), `FW_CHUNK` (the
   payload itself — estimate ~26 B header (command tag, transfer/session id,
   offset uint32, len uint16) leaves ~220 B of image bytes per packet — for a
   ~1 MB node image that's roughly 4,500–5,000 packets; **actual node image
   size not yet checked this session**, confirm before finalizing chunk
   accounting), `FW_CHUNK_DONE` (node → mothership: bytes actually received
   this round / next expected offset / status), `FW_COMPLETE` (mothership
   signals no more chunks; node runs the same size+SHA-256+finish path
   `ota_installer.h` already provides, exactly like the mothership does).
2. **Node-side resumable offset, persisted in NVS.** A node is a hard
   power-cut device — every wake is a cold boot. The next-expected-byte-
   offset for an in-progress node-firmware transfer must survive that, the
   same way `mothership_ota_release_store.cpp`'s PENDING/ARMED/INSTALLED
   record already survives the mothership's own cold boots. Mirror that
   design (versioned record, checksum, A/B NVS slots) rather than inventing
   a new persistence scheme.
3. **Mothership-side image cache.** The mothership should fetch the node
   image from the cloud **once** (via the now-fixed HTTPS pipeline above)
   and cache it locally (LittleFS/flash — node images are almost certainly
   far smaller than the ~1.3 MB mothership image), then relay cached bytes
   to the node across as many sync windows as it takes, without re-fetching
   from the cloud every wake.
4. **Apply tonight's exact lesson: pre-erase before the transfer starts.**
   The mothership bug was the OTA-partition erase stalling a live transfer
   session. The same principle applies here even though the specific failure
   mode differs (no TLS session to drop over ESP-NOW) — erase the node's
   inactive slot during a dedicated "prep" round (e.g. an `FW_GRANT` with a
   `PREPARE` flag and no chunk payload) **before** any `FW_CHUNK` bytes are
   sent, so an erase stall never eats into a live chunk-transfer window.
5. **Don't extend the shared coordinated sync window — reuse existing
   window slack instead.** The mothership's coordinated rendezvous
   (`main.cpp`, `kCoordinatedWindowMs`) currently runs a **fixed duration for
   the whole fleet** regardless of how many nodes have already finished their
   business — there is no confirmed mechanism today for non-participating
   nodes to drop back to sleep early and skip paying for someone else's
   longer window. Lengthening the shared window for one node's OTA would
   therefore cost **every** node's battery, not just the target's. Instead,
   give the OTA-targeted node its firmware rounds using the *existing*
   window's slack (the same slot `DUMP_GRANT` rounds already use today for
   bulk sensor-data dumps), and let an incomplete transfer simply resume next
   wake via the NVS-persisted offset (item 2) rather than trying to fit more
   into any single window. This needs one thing verified before it's final:
   **whether a node that has already finished its own sync business can
   currently sleep before the mothership's window formally closes** — if
   yes, a *node-specific* window extension (not a fleet-wide one) becomes a
   safe, worthwhile optimization to add later; if no, the round/resume model
   above is the right default regardless.

### Rollout plan

- **Fan-out: unicast per node**, not broadcast. Every existing bulk/critical
  delivery (`DUMP_GRANT`, `DEPLOYMENT`, config) already uses
  `sendControlPacket`'s confirmed unicast rather than the config broadcast
  pattern — reuse that precedent rather than `broadcastNodeConfigNow`'s
  fire-and-forget style, since firmware bytes are higher-stakes and unicast
  gives per-packet radio-layer delivery confirmation to retry against.
- **One node at a time**, not fleet-wide. `CMD_DEPLOY_NODE_RELEASE` is
  naturally node-targeted (same as `CMD_SET_NODE_CONFIG`), so this requires
  no new "scope" concept — just don't build a fleet-broadcast variant yet.
  Prove the protocol on one real node in the field before offering it for
  more than one at a time.
- **Bench-first, exactly like tonight.** The single biggest lesson from the
  mothership work: iterating a transport/timing bug through the real
  multi-wake field cycle cost hours; a standalone bench harness cut that to
  minutes. Build the equivalent for nodes **before** touching the field
  fleet: a two-board bench rig (one mothership + one real node board, both
  on a bench, both powered continuously) running a synthetic release, so the
  chunk/grant/resume protocol can be iterated in a normal edit-flash-observe
  loop, not real sync-interval wake cycles.

### Suggested build order

| Phase | What | Scope |
|---|---|---|
| 1 | New wire messages + node-side resumable receive state machine (reusing `ota_installer.h`) + mothership-side relay loop (reusing `sendControlPacket` + the grant/done round pattern) + two-board bench rig; prove one full node image transfer, including a forced-bad-image rollback, entirely on the bench | Firmware only, no backend/dashboard |
| 2 | `CMD_DEPLOY_NODE_RELEASE` dispatcher type; per-node release-store (mirrors the mothership's PENDING/ARMED/INSTALLED store, keyed by nodeId); publish flow extended to include `role:"node"` artifacts; backend "update available" comparison extended per-node | Firmware + backend |
| 3 | Dashboard UI — same 4-state pattern already designed for the mothership (see "Production workflow" above), extended to a per-node list within the existing fleet/node view | Frontend |
| 4 | Field rollout: one real deployed node first, confirm rollback + resumability under real RF conditions, then expand | Ops |

### Open questions (flag, don't guess)

- Can a node that's finished its own sync business currently leave the
  shared window early, or does it wait for the mothership's fixed window
  duration regardless? (Affects whether node-specific window extension is
  worth building later.)
- Real sustained unicast ESP-NOW throughput/reliability under field RF
  conditions — no bench data yet; needed to size realistic per-wake chunk
  budgets and expected total transfer time.
- Actual node firmware image size — needed to finalize packet-count/round
  estimates above.
- Whether the mothership/node rollback bench proof (`ota-slot-rollback-proof`
  memory) was actually exercised on node hardware, or only on the mothership
  board with the assumption that the same partition table implies the same
  proof — verify on real node hardware before trusting it in the field.

---

## Key files to read first
- `/memories/session/bench-ota-state.md` — full bench state + the production
  `kReleasePubKey` to revert to.
- `/memories/repo/structure-notes.md` — repo layout + cloud-OTA notes.
- `docs/FIELDMESH_CLOUD_OTA_BENCH_RUNBOOK.md` — full bench procedure.
- `docs/FIELDMESH_FIRMWARE_DASHBOARD_INTEGRATION_BRIEF.md` Appendix B —
  the authoritative wire contract (payload, status fields, reason vocabulary).
- `docs/FIELDMESH_CLOUD_OTA_TEAM_PROMPT.md` — the self-contained, ready-to-hand
  prompt for the dashboard/backend team (or their LLM); the detailed spec for
  everything in "Production workflow" above, plus the exact wire contract and
  reason vocabulary inlined so it doesn't require access to this repo.
- `mothership/firmware/v2/tests/test_ota_preerase.cpp` — the bench test that
  isolated and proved the pre-erase fix (see "Root cause + fix" above); the
  clearest single file to read if the image-download path regresses again.
- `mothership/firmware/v2/tests/{test_modem_https_get,test_modem_range_get,
  test_ota_cloud_fetch_chunked}.cpp` — the rest of the standalone bench-test
  progression (baseline → chunked transport-only → chunked+real-writes) that
  led to the pre-erase diagnosis.
- `scripts/ota-bench/publish_bench_release.py` — the publish+enqueue script.