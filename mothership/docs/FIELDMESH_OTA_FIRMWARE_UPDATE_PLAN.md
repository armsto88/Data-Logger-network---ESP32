# FieldMesh OTA Firmware Update Plan

**Status:** implementation plan

**Created:** 2026-07-17

**Revised:** 2026-07-17 - added backend-queued control, local/dashboard state mirroring, and conflict handling

**Scope:** mothership self-update, mothership-to-node firmware distribution, and the firmware-side contract for backend-queued dashboard instructions

**Primary motivation:** deploy support for new sensor packages without opening every enclosure for a USB reflash

## 0. Implementation status (2026-07-17)

Bench = spare Node V3 (ESP32) on COM3. Hardware = the real mothership (MAC 48:9d:31:f8:16:a8), SD card absent.

**Done and proven on real hardware (mothership, SD-free):**
- Firmware identity — `node/firmware/shared/firmware_identity.h`, git build id via `scripts/fw_version.py`; wired into node + mothership, prints at boot.
- Rollback-capable bootloader — the stock Arduino bootloader already has `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`; automatic rollback works via the deferred-verify pattern (`verifyRollbackLater()` + self-test-gated confirm). Proven on the mothership (bad-boot image auto-rolled back).
- Signed release manifest — detached Ed25519 over `manifest.json` bytes; host tool `scripts/release_sign.py`; device verify + compat in `node/firmware/shared/firmware_manifest.h`.
- Streaming installer — `node/firmware/shared/ota_installer.h` (size + SHA-256 gate); mothership self-update module `mothership/firmware/v2/src/ota/mothership_selfupdate.{h,cpp}`.
- Local mothership self-update over HTTP — `/firmware`, `/firmware/manifest`, `/firmware/image`. Full cycle proven: manifest verify → install → reboot → confirm; plus interrupted-upload (no change) and bad-boot (rollback) edge cases.
- Command dispatcher (Phase 2 core) — `node/firmware/shared/command_dispatcher.{h,cpp}`; compare-and-set revision, supersession, idempotency, NVS-persisted. Wired into `config_server.cpp` (node-config + sensor handlers, `GET /api/control`). Recording a real pause on the mothership produced `[CTRL] … ACCEPTED rev=1`.
- Pinned node partition CSV — `node/firmware/partitions_ota.csv`.

**Bench-proven (node), not yet on the mothership:** corrupt-image / wrong-SHA rejection; NVS survival across slot switch.

**Not started (needs the backend contract, see §4.5):** backend command queue + LTE command ingestion; local↔cloud state mirroring; remote LTE `DEPLOY_RELEASE`; streaming HTTPS GET in the modem driver (§7.2); SD release store + node OTA (Phase 4+, blocked on SD).

**Front/backend handoff:** see `docs/FIELDMESH_FIRMWARE_DASHBOARD_INTEGRATION_BRIEF.md`.

## 1. Decision summary

FieldMesh should use a backend-queued, mothership-authoritative control model around the two-stage OTA architecture:

1. The dashboard writes a desired-state change or one-shot action into a durable backend queue. It never communicates directly with the mothership or a node.
2. During an existing scheduled LTE check-in, the mothership reports its current state and receives queued instructions in the authenticated backend response.
3. The mothership validates and durably records an instruction before acknowledging it, resolves it against any newer local-UI change, and assigns the accepted state revision.
4. Local UI and dashboard instructions pass through the same mothership command dispatcher and update the same authoritative desired-state store.
5. At the following node sync window, accepted node configuration is delivered through the existing versioned `NODE_CONFIG` flow and reconciled through `CONFIG_ACK` and snapshot echoes.
6. For an accepted OTA deployment, the mothership obtains a signed release over LTE/HTTPS or from a local operator upload.
7. The mothership updates itself through the ESP32 inactive OTA application slot and stores node firmware on SD card.
8. ESP-NOW is used as the node update control plane: offer, accept, schedule, progress, result, and cancel messages.
9. A temporary mothership Wi-Fi access point and HTTP server are used as the node firmware data plane.
10. Each node downloads into its inactive OTA application slot, verifies the image, reboots, runs a health check, and reports success or rollback.

The backend queue is a store-and-forward boundary, not a remote radio path. The backend records operator intent; the mothership remains the only authority that can accept that intent, change local desired state, and communicate with nodes.

Do not transfer complete firmware images as ESP-NOW data packets. The current FieldMesh protocol is deliberately designed around the 250-byte ESP-NOW payload ceiling, and a current node image would require thousands of acknowledged packets. ESP-NOW remains valuable for low-power rendezvous and coordination; Wi-Fi/TCP is the appropriate bulk-transfer path.

The first production OTA-capable release must still be installed by USB on every existing device. That bootstrap release establishes the fixed partition layout, rollback-capable bootloader, firmware identity, trust key, OTA state store, and recovery behaviour. Application OTA can then be used for later releases.

## 2. Goals and non-goals

### 2.1 Goals

- Update the mothership remotely over its existing A7670G LTE connection.
- Update one, several, or all compatible nodes through the mothership.
- Keep normal collection, queued data, pairing, schedules, calibration, and sensor configuration intact.
- Prevent a partial download, corrupt image, wrong hardware target, or failed first boot from bricking a deployed device.
- Make update state visible locally and through cloud status.
- Receive dashboard instructions only through an authenticated backend queue during scheduled LTE check-ins.
- Mirror accepted local-UI changes to backend reported state and accepted dashboard changes into the local UI.
- Use one revisioned desired-state store and command dispatcher so local and cloud changes cannot silently overwrite each other.
- Reuse the existing `NODE_CONFIG`/`CONFIG_ACK` reconciliation and sync-window choreography for node configuration changes.
- Support coordinated releases that include node firmware, mothership firmware, and sensor data-contract requirements.
- Permit safe deferral when battery, storage, radio, or compatibility checks fail.
- Preserve an operator-controlled maintenance workflow; updates must not silently roll across the whole fleet.

### 2.2 Non-goals for the first implementation

- Updating the ESP32 bootloader or partition table through routine OTA.
- Using ESP-NOW as the firmware byte-transfer mechanism.
- Updating every node simultaneously.
- Changing the main data logger from LittleFS to SD as part of the OTA project.
- Providing differential/binary-patch updates.
- Supporting arbitrary third-party firmware.
- Direct dashboard-to-mothership or dashboard-to-node communication.
- Giving the backend direct authority to transmit ESP-NOW messages or mutate mothership NVS.
- Guaranteeing immediate dashboard changes while the mothership or modem is powered down.
- Enabling ESP32 eFuse secure boot or flash encryption without a separate provisioning and recovery project.

The SD card can initially be dedicated to release storage while operational readings continue to use the current LittleFS path. Moving the data log to SD is a separate migration with separate failure and recovery requirements.

## 3. Current repository baseline

### 3.1 Mothership

- `mothership/firmware/v2/partitions.csv` already defines `otadata` plus two `0x180000` (1536 KiB) OTA application slots.
- The generated mothership firmware observed on 2026-07-17 is 1,176,976 bytes (about 1149 KiB), leaving about 387 KiB in each current slot.
- The same partition table provides a `0xC0000` (768 KiB) filesystem partition.
- The current modem driver provides HTTPS POST but not a streaming HTTPS GET suitable for a binary image.
- The current HTTPS POST result already exposes a response body, but the upload path does not yet parse it as a bounded, versioned control response.
- The current config server already runs `WebServer` on a Wi-Fi AP, so the codebase has a suitable local UI and HTTP foundation.
- Local UI handlers currently mutate desired node configuration directly. They must be routed through a shared dispatcher before cloud-originated changes are enabled.
- SD support and pin definitions exist, but current field operation uses LittleFS. SD readiness must be treated as a hardware acceptance gate, not assumed from source presence.
- The normal sync/upload path is power-gated and limited to a five-minute session. Firmware maintenance needs its own explicit session budget and power policy.
- In the current V2 wake flow, node sync completes before the LTE upload/check-in phase. Preserving that proven order means a cloud instruction received at the end of one wake is first offered to nodes in the following sync window.

### 3.2 Nodes

- `node/firmware/platformio.ini` currently uses the PlatformIO `esp32dev` default partition layout rather than a repo-pinned CSV.
- The installed framework currently resolves that to `otadata` and two `0x140000` (1280 KiB) OTA application slots.
- The generated node firmware observed on 2026-07-17 is 853,456 bytes (about 834 KiB), leaving about 447 KiB in each current slot.
- Nodes hard-power off by releasing `PWR_HOLD`; an update session must explicitly keep it asserted.
- Normal node sync listening is short and battery-conscious. OTA must be a separate maintenance state.
- `NODE_HELLO` reports config version, wake interval, queue depth, and state, but not a stable semantic firmware version or hardware compatibility identity.
- `NODE_CONFIG` already carries versioned node desired state, and nodes acknowledge application with `CONFIG_ACK`; this is the primary path to retain for dashboard-originated node changes.
- Incoming ESP-NOW control messages are dispatched into the main-loop event queue. OTA control must follow this established callback-safety pattern.

### 3.3 Immediate implications

- Current flash capacity is sufficient for full-image OTA on both device roles.
- The node partition layout must be copied into a checked-in CSV and pinned before OTA ships; relying on a framework default risks a future toolchain change silently altering the fleet layout.
- The current 768 KiB mothership filesystem cannot hold the current 834 KiB uncompressed node image. SD card release storage is therefore the preferred node OTA prerequisite.
- Mothership self-update can stream directly to the inactive app partition and does not require an SD card.
- Standard ESP32 rollback is disabled by default in many Arduino/PlatformIO bootloader builds. The bootstrap release must prove that the actual bootloader supports the chosen rollback behaviour.
- A dashboard action cannot be shown as applied merely because the backend returned it to the mothership. Backend delivery, mothership acceptance, node delivery, and node convergence are separate states.
- Because cloud instructions are received after the current node sync phase, end-to-end dashboard-to-node latency is normally one additional sync window after receipt and can approach two configured sync intervals when a command is queued just after a check-in.

## 4. Target architecture

```text
Release builder / CI
  | produces signed manifest + role-specific binaries
  v
Release host (HTTPS)
  ^ approved manifest/artifact fetch after command acceptance
  |

Dashboard
  | writes desired change/action; no device connection
  v
Backend command queue + reported-state mirror
  ^                              |
  | status/results               | queued instructions in next LTE response
  |                              v
Mothership command dispatcher + authoritative desired state
  ^                              |
  | local state/result           | accepted command
  |                              v
Local mothership UI        Mothership release manager
  |-- streams mothership.bin -> inactive ESP32 app slot
  |-- stores node.bin + manifest -> SD /fieldmesh/ota/
  |-- reports release state -> shared local/backend state
  |
  | next sync window: existing NODE_CONFIG/CONFIG_ACK for settings
  | OTA ESP-NOW control: offer / accept / start / result
  v
Selected node
  | joins temporary WPA2 mothership AP on ESPNOW_CHANNEL
  | HTTP GET /ota/<release>/<artifact>?token=<session-token>
  v
Inactive node app slot
  | verify hash and signed release identity
  | set boot partition and reboot
  v
First-boot self-test
  |-- valid -> confirm image and report FW_RESULT success
  `-- invalid/crash -> bootloader rollback and report previous version
```

### 4.1 Store-and-forward control path

The dashboard is only a producer of backend-queued intent. The firmware-side path is:

1. A dashboard user submits a full desired-state change or an OTA action to the backend.
2. The backend authenticates the user, records an immutable command ID, and leaves the instruction queued for a specific mothership.
3. The mothership completes its normal node sync window using the desired state already held locally.
4. During the existing LTE upload/status phase, the mothership sends its current reported state, last backend cursor, and pending command results.
5. The backend first ingests that reported revision, then selects a bounded list of still-valid instructions after that cursor for its response. This ordering prevents the backend from knowingly returning an instruction already made stale by an offline local change.
6. The mothership validates, persists, and dispatches each instruction through the same internal API used by the local UI.
7. The mothership immediately reports receipt/acceptance if the session budget permits; otherwise the durable result is returned at the next LTE check-in.
8. Accepted node changes are broadcast through the existing `NODE_CONFIG` flow in the following sync window and retried until `CONFIG_ACK` or a matching snapshot confirms convergence.
9. Every later check-in mirrors the authoritative mothership state and node convergence back to the backend.

Do not move LTE setup ahead of the established node sync merely to reduce dashboard latency in the first implementation. That would lengthen the radio rendezvous path, increase battery exposure, and couple working node collection to modem availability. If lower latency is later required, evaluate it as a separately tested pre-sync control poll.

### 4.2 One authoritative state with two views

The mothership owns the accepted state revision. The backend holds queued intent plus a mirror of the last state reported by the mothership; the local UI renders the live mothership state.

| Item | Authoritative owner | Mirroring rule |
|---|---|---|
| Node desired configuration | Mothership | Local changes are uploaded on the next LTE check-in; accepted dashboard changes appear locally immediately after ingestion |
| Node applied configuration | Node, observed by mothership | Mirror only after `CONFIG_ACK` or a matching snapshot; never infer from backend delivery |
| OTA rollout state | Mothership OTA coordinator | Local and backend views show the same persisted state, target, progress and reason codes |
| Queued dashboard instruction | Backend until delivered | Mothership mirrors it locally only after receipt; it is not yet accepted state |
| Secrets, AP credentials and service-only controls | Mothership/local provisioning | Never mirror secret values to the dashboard or accept them as general remote changes |

The backend should expose desired versus reported state separately. A dashboard submission is `QUEUED`, not `APPLIED`, until the mothership accepts it. The local UI should show the source and synchronization state of the last change, for example `local - pending cloud mirror`, `dashboard - waiting for node`, or `converged`.

Recommended initial mutability:

| Setting/action | Direction | Initial policy |
|---|---|---|
| Node wake/sync schedule, target state and sensor mask | Local UI and dashboard | Bidirectional through the shared dispatcher and existing `NODE_CONFIG` reconciliation |
| OTA deploy, pause, cancel and retry | Local UI and dashboard | Bidirectional through the single OTA coordinator; local physical safety lock wins |
| Node name/location/operator metadata | Local UI and dashboard where represented locally | Revisioned and mirrored, but does not create a node radio command unless node firmware needs it |
| Firmware/capability/battery/fault/applied-state telemetry | Mothership to backend | Report-only; dashboard cannot set observed facts |
| Backend endpoint, device credential, AP credential and project/device identity | Local provisioning only | Do not accept as ordinary remote commands and never mirror secret values |
| Pairing, permanent unpair, factory reset, bootloader/partition changes | Physical/service-confirmed only initially | Explicitly outside routine dashboard control |

### 4.3 Conflict and supersession rules

Use compare-and-set semantics rather than timestamp-based last-write-wins:

- Every dashboard instruction includes `expectedStateRevision`, which is the last mothership-reported revision the dashboard user viewed.
- The dashboard does not allocate the next revision. The mothership assigns a new revision only after accepting a command.
- A local UI change passes through the same dispatcher, applies to the current state, increments the mothership revision, and is queued for backend mirroring.
- If a dashboard instruction arrives with an older expected revision, reject it as `REVISION_CONFLICT` and report the current revision and state. Do not partially merge it silently.
- If a local change supersedes an already accepted dashboard state before the node applies it, retain only the latest desired node state for `NODE_CONFIG`, and report the earlier command as `SUPERSEDED` rather than `APPLIED`.
- Resource conflicts such as an active OTA and a configuration change that would invalidate its compatibility check return `BUSY`, `DEFERRED`, or `CONFLICT` using stable reason codes.
- A physical/local safety pause or service lock overrides remote resume/cancel requests. Ordinary configuration changes do not receive arbitrary local or cloud priority; revision order decides them.

Example:

1. Backend and mothership both report revision 42.
2. A local user changes Node 4, producing revision 43.
3. A dashboard instruction still based on revision 42 is delivered later.
4. The mothership rejects it with `REVISION_CONFLICT`, uploads revision 43, and the dashboard refreshes before offering a deliberate resubmission.

Keep the mothership control revision separate from the current 16-bit node wire `configVersion`. Initially, each accepted node desired-state change can allocate the next wire version and reuse `NODE_CONFIG`. Before remote changes make rollover plausible, introduce a capability-negotiated wider node revision because the current strictly-newer comparison cannot safely treat a wrap from 65535 to 1 as newer.

### 4.4 Control plane versus data plane

Keep these responsibilities separate:

| Plane | Transport | Responsibilities |
|---|---|---|
| Dashboard instruction intake | Dashboard to backend, then backend response to scheduled LTE check-in | Durable operator intent, command ID/cursor, expected revision, receipt and result |
| State mirroring | Existing authenticated JSON status/check-in | Mothership authoritative revision, desired/applied state, source, conflicts and results |
| Release acquisition | LTE/HTTPS after accepted command, or local web upload | Manifest and binary acquisition by mothership |
| Node configuration | Existing ESP-NOW `NODE_CONFIG` and `CONFIG_ACK` | Versioned desired state, retries and convergence |
| OTA fleet control | Additive ESP-NOW | Capability, offer, accept/defer, start, cancel, progress summary, final result |
| Node image data | Mothership Wi-Fi AP + HTTP | Ordered binary byte stream with content length and optional range/resume |
| Status reporting | Existing JSON/cloud status plus local UI | Installed versions, target versions, state, reason, attempts, timestamps |

This division keeps ESP-NOW messages small and preserves the current session choreography.

### 4.5 Backend/frontend handoff boundary

Backend and frontend implementation is outside this firmware plan, but firmware integration requires the other repository team to review and provide these contracts:

- A durable per-mothership queue that the dashboard writes and only the authenticated mothership consumes during check-in.
- Immutable command IDs, monotonic delivery cursor/sequence, issue/expiry times, target binding and an audit record of the initiating user.
- Atomic check-in handling that stores the mothership's newest reported revision before selecting queued instructions for the response.
- Separate queued desired intent and mothership-reported/applied state; backend storage must not make a queued value look device-confirmed.
- A bounded response envelope and an endpoint for immediate receipt/result acknowledgement when the active LTE session budget permits.
- An approved immutable release-ID/manifest service for `DEPLOY_RELEASE`; the dashboard must not supply arbitrary artifact URLs.
- Dashboard conflict handling that refreshes revision/state and asks the user to deliberately resubmit instead of silently overwriting a local change.
- Dashboard status labels that preserve the lifecycle distinction in section 13.4 and show the expected next mothership/node check-in rather than implying live connectivity.

The backend never emits ESP-NOW packets, and the dashboard never addresses a node. Firmware remains responsible for all compatibility checks, revision assignment, scheduling, retries, node protocol use and final convergence.

## 5. Release artifact and manifest contract

Every release must have an immutable release ID and a signed manifest. Do not infer compatibility from a filename.

### 5.1 Suggested release layout

```text
fieldmesh-2026.08.0/
  manifest.json
  mothership-esp32wroom.bin
  node-esp32wroom.bin
  release-notes.md
```

### 5.2 Suggested manifest

```json
{
  "schemaVersion": 1,
  "releaseId": "fieldmesh-2026.08.0",
  "publishedAt": "2026-08-01T12:00:00Z",
  "releaseSequence": 12,
  "notes": "Adds CWT TH-A package support",
  "artifacts": [
    {
      "role": "mothership",
      "version": "2.3.0",
      "buildId": "git-abc1234",
      "hardwareTargets": ["mothership-v1"],
      "size": 1176976,
      "sha256": "<64 lowercase hex characters>",
      "url": "https://release-host/.../mothership-esp32wroom.bin"
    },
    {
      "role": "node",
      "version": "2.4.0",
      "buildId": "git-def5678",
      "hardwareTargets": ["node-v2", "node-v3"],
      "sensorPackages": ["standard", "cwt-th-a"],
      "minimumMothershipVersion": "2.3.0",
      "size": 853456,
      "sha256": "<64 lowercase hex characters>",
      "url": "https://release-host/.../node-esp32wroom.bin"
    }
  ],
  "signatureAlgorithm": "ed25519",
  "signature": "<signature over canonical manifest content>"
}
```

Exact canonicalisation and signature encoding must be specified before implementation. A manifest that cannot be reproduced byte-for-byte for verification is not a stable signing contract.

**Implemented scheme (2026-07-17).** The signature is a **detached Ed25519 signature over the exact bytes of `manifest.json`**, shipped alongside it (e.g. `manifest.json.sig`, 64-byte signature as hex). The device verifies the signature over the raw bytes it downloaded and only then parses them, so there is no re-serialisation on device and the canonicalisation trap above does not apply. The host emits the manifest compact with sorted keys purely for reproducible builds. Tooling: `scripts/release_sign.py` (`keygen` / `make` / `sign` / `pubkey`, Python `cryptography`); the private key stays on the build machine, the 32-byte public key is embedded in firmware. Device side: `node/firmware/shared/firmware_manifest.h` — `manifestVerifySignature()` (rweather/Crypto Ed25519), `manifestParse()` (ArduinoJson), and `manifestCheckCompatibility()` (role / hardware target / anti-downgrade sequence → `FwReason`). Per-artifact SHA-256 is carried for the download integrity check (§8.6); recall the bench finding that `esp_ota_end` does not cover the padding, so this manifest hash is the authoritative image check.

### 5.3 Mandatory compatibility fields

- Device role: `mothership` or `node`.
- Semantic version and monotonically increasing release sequence.
- Build identity, preferably the source commit plus a clean/dirty marker.
- Hardware target/revision.
- Minimum compatible mothership version for node releases.
- Minimum OTA protocol version.
- Image size.
- SHA-256.
- Sensor packages supported by the node image.
- Signature and signature algorithm.

### 5.4 Version rules

- Reject a lower release sequence unless the operator explicitly enters a physical/service recovery mode.
- Do not use build date strings as the ordering mechanism.
- Permit the same semantic version only when the build ID and hash are identical.
- Store the installed release ID and build ID in NVS and expose them in status.
- Treat hardware revision incompatibility as a permanent block, not a retryable failure.

## 6. SD release store

### 6.1 Initial use of SD

Use SD for OTA artifacts only during the first OTA phases. Keep the existing data log and upload queue on LittleFS until SD logging has its own migration and power-loss testing.

Suggested layout:

```text
/fieldmesh/ota/
  active.json
  releases/
    fieldmesh-2026.08.0/
      manifest.json
      node-esp32wroom.bin
      node-esp32wroom.bin.part
      verification.json
  state/
    node-001.json
    node-002.json
```

### 6.2 Atomic storage rules

- Download to `.part`.
- Verify expected size and SHA-256 while streaming.
- Flush and close the file.
- Reopen and verify the stored image before publishing it.
- Rename `.part` to `.bin` only after verification.
- Write state to a temporary file and atomically rename it.
- Never delete the last known-good node artifact while any node still reports it as installed or rollback-relevant.
- Enforce free-space requirements before activating a release.
- Keep at most the configured number of releases, initially two, and garbage-collect only artifacts not referenced by an active rollout.

### 6.3 SD acceptance gate

Before node OTA work begins, prove on the actual mothership hardware:

- Mount after cold power-on.
- Write, close, reopen, and hash a file larger than the current node image.
- Survive removal or write failure without blocking normal LittleFS logging.
- Recover from power loss during `.part` creation.
- Reject a read-only, corrupt, missing, or full card with a clear status reason.
- Sustain simultaneous Wi-Fi HTTP reads without SPI errors or brownout.

## 7. Mothership OTA design

### 7.1 Local maintenance OTA first

Implement local mothership OTA before LTE OTA. Add a protected firmware page to the existing config UI that accepts a manifest and binary, performs all validation, and streams directly into the inactive application partition.

This establishes partition, verification, first-boot, and rollback behaviour without adding modem uncertainty.

Local workflow:

1. Operator enters service/config mode with USB or the physical config control.
2. UI shows current version, build, running partition, available slot size, and rollback support.
3. Operator uploads the signed manifest and matching mothership binary.
4. Firmware verifies role, hardware target, size, version policy, signature, and hash.
5. Firmware writes only the inactive app slot.
6. UI displays verification success and asks for explicit reboot confirmation.
7. New image boots, performs the first-boot health check, and confirms itself.
8. UI/cloud status records success or rollback.

Do not reuse the captive-portal not-found handler for binary upload errors. OTA endpoints must return explicit HTTP status and a machine-readable reason.

### 7.2 Remote LTE OTA

Remote OTA begins only after the mothership accepts a backend-queued `DEPLOY_RELEASE` instruction through the control check-in. The dashboard/backend supplies an immutable release ID and deployment policy; it does not stream the image to the device, directly invoke the modem, or address nodes.

The accepted command should identify a release already known to an approved release service. Do not permit an unrestricted dashboard-supplied URL. The mothership resolves the release ID against its configured HTTPS host, verifies the signed manifest, persists the rollout, and only then starts artifact acquisition.

Local `/firmware` actions must create the same internal `DEPLOY_RELEASE`, `PAUSE_DEPLOYMENT`, `CANCEL_DEPLOYMENT`, or `RETRY_TARGET` commands. Their source is recorded as local, but they use the same OTA coordinator and persisted rollout state as a dashboard-originated action.

Extend `ModemDriver` with a streaming GET abstraction rather than a method that accumulates a binary response body in `String`.

Suggested interface responsibilities:

- Parse HTTPS URL and open the existing CCH SSL transport.
- Send GET with `Connection: close` and optional authorization.
- Parse status and headers incrementally.
- Require or reconcile `Content-Length` with manifest size.
- Deliver body chunks to a sink callback.
- Apply bounded timeouts and feed the watchdog.
- Abort cleanly on short body, overrun, socket loss, or sink failure.
- Never fall back from HTTPS to plaintext for firmware artifacts or backend control traffic.

Two sinks are required:

- ESP OTA sink for mothership self-update.
- SD file sink for node artifacts.

### 7.3 Mothership self-update state machine

```text
IDLE
  -> MANIFEST_AVAILABLE
  -> PREFLIGHT
  -> DOWNLOADING
  -> VERIFYING
  -> READY_TO_REBOOT
  -> PENDING_BOOT_VALIDATION
  -> CONFIRMED
  -> FAILED or ROLLED_BACK
```

Persist the state transitions that matter across reboot. Do not persist high-frequency byte counters to NVS; use RAM during a session and coarse checkpoints on SD where necessary.

Associate the OTA state machine with the accepted `commandId`, mothership state revision, source, and release ID. After reboot, status reporting must reconnect the resumed/rolled-back operation to the same backend command rather than creating a second deployment.

### 7.4 Mothership preflight

Block self-update when any mandatory condition fails:

- Manifest signature invalid.
- Wrong role or hardware revision.
- Image too large for inactive slot.
- Installed version is already the target.
- Release is below allowed sequence.
- Battery is below the OTA threshold and USB/service power is absent.
- A sensor-schema release has unuploaded data that would be endangered by the new CSV header.
- RTC/rescue wake cannot be armed.
- Another update or critical sync/upload operation is active.

The OTA battery threshold should be distinct from ordinary dashboard battery attention thresholds and established by load testing. A conservative initial threshold is appropriate because LTE and flash writes overlap with a long powered session.

### 7.5 First-boot mothership validation

Confirm the new image only after a bounded test verifies:

- `PWR_HOLD` remains asserted.
- NVS opens and the node registry is readable.
- RTC time/status can be read and a rescue alarm can be armed.
- LittleFS mounts without formatting an existing valid filesystem.
- SD state is reported accurately but is not mandatory for mothership self-recovery.
- Wi-Fi initializes.
- Required firmware version and release identity are readable.
- Main task reaches a stable event loop without watchdog or brownout reset.

Modem registration should not be required to confirm the image; lack of field coverage must not cause a rollback loop. It can be a post-confirmation degraded-health condition.

## 8. Node OTA design

### 8.1 Update selection

The mothership must target nodes explicitly by stable node ID/MAC and verify:

- Reported OTA protocol capability.
- Hardware revision compatibility.
- Current/target firmware version.
- Sensor package selection.
- Last reported battery voltage and age.
- Node state is deployed or an explicitly allowed maintenance state.
- No unresolved update is already active.

Prefer one universal node image containing all supported sensor backends where flash capacity allows. Use the existing sensor mask/configuration to specify installed packages. Only split firmware variants when PCB pins, power sequencing, or incompatible hardware genuinely require it.

### 8.2 Additive ESP-NOW messages

Do not enlarge or reinterpret existing structs in place. Older flashed devices and the current exact-length dispatch logic require additive messages.

Suggested control messages:

| Message | Direction | Purpose |
|---|---|---|
| `FW_CAPS` | node -> mothership | OTA protocol version, firmware version, build, hardware revision, max image size, rollback support |
| `FW_OFFER` | mothership -> node | Release ID/hash prefix, target version, size, maintenance deadline |
| `FW_ACCEPT` | node -> mothership | Accept or defer with battery/storage/compatibility reason |
| `FW_START` | mothership -> node | Session ID, AP identity, channel, short-lived access token, expected hash/size |
| `FW_PROGRESS` | node -> mothership | Coarse download/verify state and bytes received |
| `FW_RESULT` | node -> mothership | Installed version, success/failure/rollback code, boot diagnostics |
| `FW_CANCEL` | mothership -> node | Cancel an offered or not-yet-activated release |

All messages must:

- Fit comfortably within the current ESP-NOW v1 payload ceiling.
- Include protocol version and session/release identity.
- Be idempotent.
- Reject messages from an unexpected mothership.
- Be queued from receive callbacks and processed in the node main loop.
- Use bounded retries and deadlines.

### 8.3 Control-plane authentication

Current FieldMesh ESP-NOW peers are configured without link encryption. Signed firmware prevents arbitrary code installation, but unauthenticated OTA control could still cause battery-drain or denial-of-service attempts.

The bootstrap design should therefore provide one of these, in priority order:

1. Enable encrypted unicast ESP-NOW peers with a per-node LMK established during trusted pairing.
2. Add an HMAC to OTA control messages using a per-node secret provisioned during bootstrap/pairing.

Broadcast sync markers can remain non-secret. `FW_START` and any access credentials must be authenticated unicast messages. Never place a reusable fleet Wi-Fi password in an unauthenticated broadcast.

### 8.4 Temporary OTA Wi-Fi network

- Run on `ESPNOW_CHANNEL` because an ESP32 radio cannot use independent Wi-Fi and ESP-NOW channels simultaneously.
- Use a dedicated SSID such as `FieldMesh-OTA-<mothership suffix>`.
- Use WPA2 with a per-session credential derived from a shared secret and random session nonce, or a randomly generated credential sent through authenticated unicast.
- Permit one updating node at a time initially.
- Expose only the firmware and health endpoints required for the active session.
- Require a short-lived per-node HTTP token in addition to WPA2.
- Disable captive-portal DNS and general config routes during a remote OTA session.
- Stop the AP immediately after completion, cancellation, or timeout.

Suggested endpoints:

```text
GET  /ota/v1/session/<sessionId>/manifest
GET  /ota/v1/session/<sessionId>/firmware
POST /ota/v1/session/<sessionId>/result
GET  /ota/v1/session/<sessionId>/health
```

The firmware response must include exact `Content-Length`, immutable ETag/hash identity, and no dynamic compression.

### 8.5 Node update state machine

```text
IDLE
  -> OFFERED
  -> ACCEPTED or DEFERRED
  -> WAITING_FOR_AP
  -> DOWNLOADING
  -> VERIFYING
  -> READY_TO_BOOT
  -> PENDING_BOOT_VALIDATION
  -> CONFIRMED
  -> FAILED or ROLLED_BACK
```

### 8.6 Node update sequence

1. Wake for the normal coordinated session and send `FW_CAPS` after `NODE_HELLO`.
2. Receive an authenticated `FW_OFFER`.
3. Check hardware, target, version, battery, free OTA slot, RTC, and current critical work.
4. Send `FW_ACCEPT` or a durable deferral reason.
5. Finish or safely checkpoint queued sensor delivery before maintenance begins.
6. Arm a rescue RTC alarm before extending the powered session.
7. Assert/retain `PWR_HOLD`; suppress normal final power release.
8. Receive `FW_START`, switch from ESP-NOW to Wi-Fi STA, and join the temporary AP.
9. Request the manifest and verify it independently.
10. Stream the binary sequentially into the inactive OTA partition while computing SHA-256.
11. Verify byte count, hash, image validity, release identity, and signature binding.
12. Set the boot partition only after all verification succeeds.
13. Persist `PENDING_BOOT_VALIDATION`, close Wi-Fi, and reboot.
14. Run the first-boot health test.
15. Confirm the image or trigger rollback.
16. At the next coordinated session, send `FW_RESULT` until acknowledged.

### 8.7 Interrupted download behaviour

First production version:

- Keep the node powered for the whole transfer.
- Retry a dropped HTTP connection from byte zero a bounded number of times.
- Never activate a partial partition.
- On session timeout, abandon the inactive slot, restore normal scheduling, and power down.

Later optimisation:

- Add HTTP `Range` and a durable page-aligned checkpoint.
- Resume with low-level partition writes only after power-loss tests prove erase/write boundaries and hash continuity.

Do not make cross-power-cycle resume a phase-one requirement. It adds considerable flash-state complexity and is unnecessary if Wi-Fi transfer is short and the power path is stable.

### 8.8 First-boot node validation

Run quickly, before ordinary long work:

- Confirm firmware role, version, release ID, and hardware target.
- Assert `PWR_HOLD` immediately.
- Open NVS and load node ID, pairing, schedule, sensor mask, and queue metadata.
- Read RTC status and arm a rescue alarm.
- Initialise Wi-Fi/ESP-NOW and reach a send-capable state.
- Mount/recover the local queue without erasing valid records.
- Initialise configured sensor backends sufficiently to catch fatal driver/pin conflicts.
- Feed watchdog throughout.

Do not require every physically optional sensor to be present before confirming firmware. Missing expected sensors should be reported as a sensor fault, not cause a firmware rollback. Roll back only for platform-critical failures.

## 9. Sensor-package release workflow

OTA distributes software; it does not remove the need to physically install a new sensor package. The recommended operator flow is:

1. Develop and validate the sensor driver and calibration path.
2. Allocate sensor IDs and update node registry/snapshot handling.
3. Update mothership decode, CSV/JSON mapping, and backend schema where required.
4. Build one coordinated signed release.
5. Upload or preserve all pending mothership data before a CSV-header-changing release.
6. Deploy and validate the mothership artifact first.
7. Install the physical sensor package on selected nodes.
8. Roll node firmware to one canary and wait for its new firmware identity and sensor capabilities.
9. Select the package through either the mothership UI or a backend-queued desired-state change.
10. Let the mothership accept and revision that desired state, then deliver it through the existing `NODE_CONFIG` flow at the next sync.
11. Confirm `CONFIG_ACK`, the expected sensor-present bit, and sane values.
12. Roll to remaining compatible nodes and apply their desired sensor configuration only after each node reports compatible firmware.

The existing `docs/ADDING_A_NEW_SENSOR_CHECKLIST.md` remains the source of truth for the end-to-end sensor data path. OTA release tooling should enforce its ordering requirement: mothership first, then nodes.

The existing sensor mask is sufficient for selecting sensor drivers already represented by protocol capability bits. New calibration, bus-address, pin, or package-specific parameters should use an additive capability-negotiated configuration extension; do not change the size or interpretation of the working `NODE_CONFIG` message for older nodes. Firmware identity should advertise the supported sensor configuration schema so the mothership can reject a dashboard request that the installed node firmware cannot understand.

### 9.1 Protecting queued data during schema changes

The current mothership CSV header-mismatch behaviour can recreate the data file. Before activating a release that changes the CSV schema:

- Block activation while the upload queue has pending rows, unless an explicit service override is used.
- Copy the current data log to dated SD archival storage.
- Record the old schema/version with the archive.
- Verify the archive before reboot.
- Report this as a distinct `DATA_BACKLOG_BLOCKED` update state.

This gate belongs in the release manifest (`dataSchemaVersion`) and mothership preflight, not in operator memory.

## 10. Rollback and recovery

### 10.1 Bootstrap bootloader

The initial USB bootstrap must install and verify a bootloader built with application rollback support. The running application must confirm a new image only after its self-test. If it crashes or resets before confirmation, the bootloader selects the previous valid slot.

Because bootloader and partition updates are materially riskier than application OTA, later routine OTA releases must modify app slots only.

**Bench-proven 2026-07-17 (spare Node V3, ESP32-D0WD).** A self-contained slot test (`bringup_ota_slots.cpp`, envs `esp32wroom-ota-slots` / `mothership-v1-ota-slots`) clones the running image into the inactive slot, switches boot, and inspects `esp_ota_get_state_partition`. On the current stock Arduino-ESP32 3.20017 (core 2.0.17) precompiled bootloader — identical for both roles:

- **Slot switch works.** Inactive-slot write, `esp_ota_end` verification, and reboot into the other slot all succeed.
- **App-driven rollback works.** `esp_ota_mark_app_invalid_rollback_and_reboot()` reverts to the previous slot.
- **Automatic rollback works — no custom bootloader required.** `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y` and `CONFIG_APP_ROLLBACK_ENABLE=y` are already set in the shipped framework SDK config. With deferred verification enabled, a freshly-switched image boots `PENDING_VERIFY`, and rebooting/crashing before it confirms causes the bootloader to roll back to the previous slot automatically (proven end-to-end on the bench).

The catch that hid this: Arduino's `initArduino()` (runs before `setup()`) auto-calls `esp_ota_mark_app_valid_cancel_rollback()` on a `PENDING_VERIFY` image by default, because the weak `verifyRollbackLater()` returns `false`. A naive image therefore self-confirms instantly and can never roll back. To arm real rollback protection the application must:

1. Override the weak symbol — `extern "C" bool verifyRollbackLater() { return true; }` — so the core does **not** auto-confirm.
2. Run its first-boot self-test (§7.5, §8.5), and call `esp_ota_mark_app_valid_cancel_rollback()` **only** after it passes.
3. On any crash/hang/reset before that call, the bootloader reverts to the prior slot with no operator action.

Consequences:

- The Phase 0 "verified rollback-capable bootloader" gate is satisfied by the stock bootloader plus the deferred-verify pattern above; no Arduino-as-IDF-component / custom bootloader build is needed. This removes the main Phase 0 blocker.
- First-boot validation (§7.5, §8.5) *is* the confirmation trigger: pass → `mark_app_valid`; fail → `mark_app_invalid_rollback_and_reboot`; crash-before-either → automatic bootloader rollback.
- NVS survives a slot switch (verified: a marker written before the switch is intact after it), so pairing/schedule/sensor-mask/queue are preserved across OTA — §16 requirement.
- The node partition layout is now pinned in `node/firmware/partitions_ota.csv` (mirrors the current esp32dev 4 MB geometry, two `0x140000` slots), satisfying the §14.2 pin requirement.

Integrity gates measured with the same test (negative cases `x`/`y`):

- A corrupt **header magic byte** is rejected immediately by `esp_ota_write` on the first chunk; boot slot never changes.
- **Mid-segment** corruption is rejected by `esp_ota_end` (ESP image checksum, `ESP_ERR_OTA_VALIDATE_FAILED`); boot slot never changes.
- Corruption in the **padding beyond the image segments is NOT caught** by the ESP layer. This confirms the manifest SHA-256 requirement (§5, §8.6, §16) is load-bearing: firmware must compute a full SHA-256 over the exact manifest-declared byte range and compare it before `esp_ota_set_boot_partition`. `esp_ota_end` is a backstop, not a substitute — a corrupt/truncated download can otherwise pass.

### 10.2 Recovery hierarchy

1. Retry a transient acquisition/download failure without changing boot slot.
2. Keep the current app when verification fails.
3. Roll back automatically when first-boot validation fails.
4. Retry the last known-good app and report the failed target.
5. Enter a bounded service/recovery mode after repeated rollback loops.
6. Preserve serial USB recovery as the final field procedure.

### 10.3 Failure reason vocabulary

Use stable reason codes, not only log strings:

```text
NONE
COMMAND_EXPIRED
COMMAND_DUPLICATE
REVISION_CONFLICT
SUPERSEDED
DEFERRED_LOW_BATTERY
DEFERRED_BUSY
LOCAL_SERVICE_LOCK
INCOMPATIBLE_ROLE
INCOMPATIBLE_HARDWARE
INCOMPATIBLE_PROTOCOL
DOWNGRADE_REJECTED
SD_MISSING
SD_FULL
MANIFEST_INVALID
SIGNATURE_INVALID
IMAGE_TOO_LARGE
DOWNLOAD_TIMEOUT
DOWNLOAD_TRUNCATED
HASH_MISMATCH
FLASH_WRITE_FAILED
BOOT_VALIDATION_FAILED
ROLLED_BACK
DATA_BACKLOG_BLOCKED
OPERATOR_CANCELLED
```

## 11. Security model

Minimum production requirements:

- Authenticated backend check-in using a unique per-mothership credential; do not depend on a shared credential compiled into all devices.
- A bounded, versioned command-response parser with target binding, command ID, monotonic cursor/sequence, expiry, type allowlist, and payload-size limits.
- Durable duplicate detection so a repeated backend response can only repeat an acknowledgement, not repeat the action.
- HTTPS for release acquisition; never plaintext fallback.
- Signed manifest with an offline/private release-signing key.
- Verification public key embedded in the bootstrap firmware.
- SHA-256 checked while streaming and after SD storage.
- Exact role and hardware target validation.
- Monotonic anti-downgrade release sequence.
- Authenticated OTA control messages.
- Temporary WPA2 AP and short-lived per-node HTTP token.
- No secret values in cloud status or serial logs.
- Operator confirmation before fleet-wide activation.
- Rate and attempt limits to prevent battery-drain loops.
- Local physical/service lockout that the backend cannot override.

TLS authenticates the download channel; it does not replace signed release identity. Firmware must remain rejectable even if a release host or signed URL is misconfigured.

Secure boot and flash encryption can be considered later, but eFuse provisioning is effectively irreversible and needs its own manufacturing/recovery plan.

## 12. Power, timing, and scheduling

### 12.1 Dedicated maintenance window

Do not extend every ordinary sync window for OTA. Add a distinct maintenance session that is entered only after offer/accept.

- Complete normal data collection first.
- Update one node at a time.
- Arm rescue alarms before extended work.
- Keep `PWR_HOLD` asserted explicitly.
- Feed or deliberately reconfigure watchdogs for known blocking operations.
- Set a hard session deadline.
- Restore the normal RTC schedule before every exit path.

### 12.2 Battery policy

Maintain separate thresholds for:

- ordinary operation/dashboard attention;
- mothership self-update;
- node update;
- update allowed while USB/service power is present.

The final values must come from current measurements during LTE download, Wi-Fi AP serving, Wi-Fi node download, and flash write. Low battery should defer, not fail, a release.

### 12.3 Fleet pacing

Initial default:

- one canary node;
- wait for confirmed first boot plus one successful data snapshot;
- then one node per maintenance session;
- configurable daily cap;
- stop the rollout on the first unexpected rollback or repeated transfer failure.

## 13. Status model and user interface

> **Implemented field contract (keep aligned):** this section is design intent. The concrete, source-grounded shapes actually built live in `docs/FIELDMESH_FIRMWARE_DASHBOARD_INTEGRATION_BRIEF.md` **Appendix A**. Key specifics that differ from the abstract examples below: node `targetState` is a **uint8 enum `0=UNPAIRED, 1=PAIRED, 2=DEPLOYED/ACTIVE, 3=STANDBY`** — so **pause = 3, resume = 2, undeploy = 0** (JSON envelope examples may use the string names, which map to these). `sensorMask` is a **uint16** with a `VALID` bit (0x8000) and a wind selector (bit 9). The LTE **check-in is the existing data upload** (one HTTPS POST; `responseBody` is already captured but not yet parsed for commands). `FW_CAPS` (per-node firmware version/hw/OTA capability) is **not built yet**. `commandId` is capped at **≤23 chars** (`CMD_ID_LEN=24`). When these change, update Appendix A first, then reconcile this section.

### 13.1 Per-device status

Record and expose:

- Mothership control-protocol version, authoritative state revision, last backend cursor, and last change source.
- Pending local-to-backend mirror state and last successful control check-in.
- Active/recent command IDs with acceptance, conflict, supersession, execution, and convergence results.
- Device role, hardware revision, current version, build ID, and release ID.
- OTA protocol and rollback capability.
- Target release.
- State and stable reason code.
- Bytes received and total bytes during an active session.
- Attempt count.
- First offered, last attempted, completed, and confirmed timestamps.
- Previous version and rollback result.
- Last reported battery voltage and age.

Avoid writing progress every packet to NVS or cloud. Update UI progress in RAM and persist coarse durable transitions.

### 13.2 Local mothership UI

Add a `/firmware` or `/updates` page with:

- Current mothership release and partition state.
- SD/release-store health.
- Upload/sideload release action in service mode.
- Check-for-update action.
- Release notes and compatibility summary.
- Mothership update preflight results.
- Node table with current/target version and state.
- Desired versus node-reported configuration version and convergence state.
- Last change source (`LOCAL_UI` or `DASHBOARD`) and mothership state revision.
- Backend mirror state and last successful check-in time.
- Received dashboard commands, conflicts and superseded operations using operator-readable reason text.
- Select canary, selected nodes, or compatible fleet.
- Pause, resume, cancel, and retry actions.
- Explicit warnings for low battery, missing SD, unuploaded data, unsigned artifact, and incompatible hardware.

All local mutation handlers must call the shared command dispatcher. The UI must not directly edit NVS/registry state after that refactor. A local change may be used immediately by the mothership, but the UI must label it as pending backend mirroring until a successful LTE check-in reports the new revision.

### 13.3 Backend mirror status

Extend the existing status payload additively:

```json
{
  "control": {
    "protocolVersion": 1,
    "stateRevision": 43,
    "lastCommandCursor": 129,
    "lastChangeSource": "DASHBOARD",
    "mirrorPending": false,
    "lastCheckInUnix": 1784217600,
    "commandResults": [
      {
        "commandId": "018f-example",
        "state": "WAITING_FOR_SYNC",
        "reason": "NONE",
        "acceptedRevision": 43
      }
    ]
  },
  "firmware": {
    "version": "2.3.0",
    "buildId": "git-abc1234",
    "releaseId": "fieldmesh-2026.08.0",
    "state": "CONFIRMED",
    "targetVersion": null,
    "lastResult": "success"
  },
  "nodes": [
    {
      "nodeId": "node-001",
      "firmwareVersion": "2.4.0",
      "hardwareRevision": "node-v3",
      "desiredConfigVersion": 12,
      "appliedConfigVersion": 11,
      "configConvergence": "WAITING_FOR_SYNC",
      "otaState": "CONFIRMED",
      "otaTargetVersion": null,
      "otaReason": "NONE"
    }
  ]
}
```

The backend must store this as reported state and must not overwrite it with a still-queued dashboard value. Secret values are excluded. The dashboard may render queued desired intent beside reported state, but only the reported mothership revision and node acknowledgement establish what has actually happened.

### 13.4 Backend instruction response

The authenticated status/upload response may add a small versioned control envelope:

```json
{
  "controlProtocolVersion": 1,
  "serverTimeUnix": 1784217600,
  "nextCursor": 130,
  "commands": [
    {
      "commandId": "018f-example",
      "sequence": 130,
      "type": "SET_NODE_CONFIG",
      "target": { "nodeId": "node-001" },
      "expectedStateRevision": 43,
      "issuedAtUnix": 1784217500,
      "expiresAtUnix": 1784303900,
      "payload": {
        "wakeIntervalMin": 10,
        "targetState": "DEPLOYED",
        "sensorMask": 37
      }
    }
  ]
}
```

Firmware parsing rules:

- Treat a missing `control` envelope as a successful legacy upload with no commands.
- Reject an unsupported protocol version or oversized response without affecting uploaded-data cursor handling.
- Validate the authenticated target, sequence, expiry, command type and complete payload before persistence.
- Persist the command before advancing `lastCommandCursor`.
- Re-delivery of the same `commandId` returns its stored result and never executes it again.
- Process only a bounded number of commands per check-in so the backend cannot extend the power session indefinitely.
- Prefer full desired configuration over ambiguous partial patches. If patches are later supported, apply them only inside the dispatcher against the validated expected revision.

Use lifecycle terms consistently across firmware and backend:

| State | Meaning |
|---|---|
| `QUEUED` | Backend has intent; mothership has not received it |
| `RECEIVED` | Mothership parsed and durably stored it |
| `ACCEPTED` | Preconditions passed and a mothership revision was assigned |
| `REJECTED` | Invalid, expired, unauthorised, incompatible, or revision conflict |
| `WAITING_FOR_SYNC` | Accepted node desired state awaits the next normal sync window |
| `SENT_TO_NODE` | `NODE_CONFIG` or OTA offer was transmitted; application is not yet confirmed |
| `CONVERGED` | Node acknowledgement/snapshot or OTA result confirms the requested state |
| `SUPERSEDED` | A newer accepted local or dashboard revision replaced it before convergence |
| `FAILED` / `CANCELLED` | Execution ended with a stable reason code |

Cloud-originated control must remain disabled until TLS-only transport, unique device authentication, bounded parsing, durable idempotency, and backend audit logging are available together. Local UI operation continues when the backend is absent.

## 14. Repository implementation map

Suggested new modules and touched areas:

### 14.1 Shared protocol

- `node/firmware/shared/protocol.h`
  - Add OTA protocol version and additive control structs.
  - Add stable update-state and reason-code enums.
  - Keep every message below the v1 ESP-NOW limit.
- Add focused encode/size/dispatch tests under `node/firmware/tests/` and mothership tests.

### 14.2 Node firmware

- `node/firmware/partitions_ota.csv`
  - Pin the verified two-slot layout.
- `node/firmware/platformio.ini`
  - Select the pinned layout and stable version/build flags.
- `node/firmware/src/ota/node_ota_manager.h/.cpp`
  - State machine, preflight, download, verification, boot activation.
- `node/firmware/src/ota/node_boot_validation.h/.cpp`
  - Pending-image health check and confirmation/rollback.
- `node/firmware/src/ota/firmware_identity.h/.cpp`
  - Role, semantic version, build, hardware revision, release ID.
- `node/firmware/src/message_dispatch.*`
  - Recognise additive OTA messages.
- `node/firmware/src/node_event_queue.*`
  - Queue OTA events outside callbacks.
- `node/firmware/src/main.cpp`
  - Maintenance state, `PWR_HOLD`, rescue alarm, status reporting.
- `node/firmware/src/storage/node_config_store.*`
  - Minimal durable OTA state and trust material, with versioned NVS migration.

### 14.3 Mothership firmware

- `mothership/firmware/v2/src/control/control_protocol.h/.cpp`
  - Bounded backend envelope parser, command/state/reason enums, schema-version checks and validation.
- `mothership/firmware/v2/src/control/command_dispatcher.h/.cpp`
  - Single entry point for local UI and dashboard instructions, compare-and-set validation, revision allocation, resource locking and supersession.
- `mothership/firmware/v2/src/control/command_store.h/.cpp`
  - Durable command IDs/cursor, current state revision, recent results and reboot-safe active operation; use NVS for critical compact state and LittleFS/SD for the bounded audit journal.
- `mothership/firmware/v2/src/control/cloud_control_client.h/.cpp`
  - Build reported control status, parse queued instructions from the authenticated backend response, and submit receipt/results.
- `mothership/firmware/v2/src/ota/firmware_manifest.h/.cpp`
  - Parse, canonicalise, verify, and apply compatibility policy.
- `mothership/firmware/v2/src/ota/release_store.h/.cpp`
  - Atomic SD artifact and rollout-state storage.
- `mothership/firmware/v2/src/ota/mothership_ota.h/.cpp`
  - Self-update sink and boot validation.
- `mothership/firmware/v2/src/ota/node_ota_coordinator.h/.cpp`
  - Targeting, ESP-NOW orchestration, retry policy, canary/fleet pacing.
- `mothership/firmware/v2/src/ota/ota_http_server.h/.cpp`
  - Restricted temporary AP server and image streaming.
- `mothership/firmware/v2/src/comms/modem_driver.*`
  - Streaming HTTPS GET, bounded small response handling, and no plaintext fallback for either artifacts or control traffic.
- `mothership/firmware/v2/src/storage/sd_logger.*` or a new SD storage abstraction
  - Release-store mount and health without forcing data-log migration.
- `mothership/firmware/v2/src/comms/espnow_sync.*`
  - Keep existing `NODE_CONFIG`/`CONFIG_ACK` reconciliation; add serialised OTA control sends without replacing working sync choreography.
- `mothership/firmware/v2/src/config/node_registry.*`
  - Firmware/hardware capability, desired/applied config, wider mothership control revision metadata and rollout fields.
- `mothership/firmware/v2/src/config/config_server.*`
  - Render the authoritative shared state; submit all local configuration/rollout actions to `command_dispatcher` instead of mutating desired state directly.
- `mothership/firmware/v2/src/config/transmission_settings.*`
  - Explicit `remoteManagementEnabled`, command-check cadence, and non-secret check-in diagnostics; remote management remains separately controllable from reading upload.
- `mothership/firmware/v2/src/storage/json_payload.*`
  - Additive reported control revision, command results, desired/applied node versions, source and mirror status.
- `mothership/firmware/v2/src/main.cpp`
  - Preserve node-sync-then-LTE order; ingest/persist backend commands after upload for the following sync, enforce bounded check-in time, and resume durable operations after reboot.

### 14.4 Release tooling

- Add a repeatable release script that builds both production images, records sizes, hashes artifacts, emits the canonical manifest, and signs it.
- Fail the build when an artifact exceeds a configured percentage of its slot, initially 90%.
- Build from a clean source revision for production releases.
- Retain unsigned development releases only for a physically enabled bench mode.
- Archive bootloader, partition table, app binaries, manifest, source commit, toolchain lock, and release notes together.

## 15. Implementation phases

### Phase 0 - decisions and bootstrap contract

Deliverables:

- Versioned backend command/status envelope and maximum response/command counts.
- Mothership-owned state revision, cursor, command lifecycle, conflict and supersession contract.
- Explicit list of remotely mutable, locally restricted and never-mirrored fields.
- Final manifest/signature specification. *(Done, 2026-07-17: detached Ed25519 signature over the exact `manifest.json` bytes — device verifies the downloaded bytes, no canonicalisation on device. Host tool `scripts/release_sign.py` (keygen/make/sign/pubkey, uses Python `cryptography`); device verify via `rweather/Crypto` Ed25519 + `bblanchon/ArduinoJson`. See §5 and `node/firmware/shared/firmware_manifest.h`. Proven end-to-end on bench: 15/15 checks incl. tamper + wrong-target rejection.)*
- Explicit hardware revision identifiers. *(Initial: `node-v3`, `mothership-v1` set via `FW_HW_TARGET` build flag; revisit if `node-v2` must be supported by the first release.)*
- Pinned node partition CSV. *(Done: `node/firmware/partitions_ota.csv`, 2026-07-17.)*
- Verified rollback-capable bootloader for both roles. *(Done, 2026-07-17: stock Arduino bootloader already has `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y`; automatic rollback proven end-to-end using the deferred-verify pattern (`verifyRollbackLater()` override + self-test-gated `mark_app_valid`). No custom bootloader needed. See §10.1.)*
- Firmware identity module and build version injection. *(Done, 2026-07-17: `node/firmware/shared/firmware_identity.h` (role/semver/buildId/hwTarget/protocol), git build id injected by `scripts/fw_version.py`, replacing stale `__DATE__/__TIME__`. Wired into node + mothership production builds; runtime print verified on hardware: `role=node ver=0.1.0 build=<git>-dirty hw=node-v3 proto=2`.)*
- Defined OTA NVS schema and failure codes. *(Partial: reason-code enum `FwReason` in `firmware_manifest.h` covers manifest/compat cases; NVS schema for installed release-id/sequence still to define.)*
- USB recovery image and written recovery procedure.

Exit gate:

- Backend/frontend and firmware reviewers agree on the store-and-forward/revision contract, and both device roles can boot either slot, preserve NVS, deliberately fail validation, and return to the previous image on bench hardware.

### Phase 1 - observability without updating

> **Implementation plan:** `MOTHERSHIP_STATUS_REPORTING_PLAN.md` breaks this into shippable tiers — Tier 1 (mothership firmware identity + control block into the cloud payload, no node change), Tier 2 (per-node desired-vs-applied, no node change), Tier 3 (`FW_CAPS` node→mothership firmware report). Tiers 1–2 can ship immediately.

Deliverables:

- Additive `FW_CAPS`/firmware status protocol.
- Mothership registry stores node firmware and hardware identity.
- Mothership reports its control revision plus desired/applied node configuration without accepting cloud changes.
- Local/cloud UI shows versions, OTA capability and the same reported state.
- Mixed old/new fleet compatibility tests.

Exit gate:

- No commands or update bytes are transferred, but every bootstrapped device is correctly inventoried, mirrored state can be compared, and old devices remain operational.

### Phase 2 - shared local/backend control and mirroring

Deliverables:

- Shared command dispatcher used by every local UI mutation. *(Core done — `command_dispatcher.{h,cpp}`. Wired into `config_server.cpp`: `dispatcherInit()` at config start, `handleNodeConfigSave` + `handleSetNodeSensors` record changes via `controlRecordNodeChange()` before `setDesiredConfig`, and `GET /api/control` exposes the revision + recent results. Compiles into the mothership; not yet bench-run on hardware. Remaining local mutations (global wake/sync-mode/sync-time) not yet routed.)*
- Persistent mothership state revision, backend cursor, command IDs and recent results. *(Done — monotonic revision + bounded result ring + node desired-config table, all NVS-persisted; bench-proven to survive reboot.)*
- TLS-only authenticated backend command ingestion from the existing scheduled LTE response. *(Not started — needs the backend contract §4.5.)*
- Integration against a backend queue that already enforces user authorization, target binding and audit logging. *(Not started — backend side.)*
- Safe command subset beginning with `REQUEST_STATUS` and then `SET_NODE_CONFIG`. *(Done — both implemented; `SET_NODE_CONFIG` carries wakeIntervalMin + targetState (pause) + sensorMask.)*
- Compare-and-set conflict rejection, supersession, duplicate delivery and reboot recovery. *(Done — bench-proven 18/18: stale expectedRevision → REVISION_CONFLICT, newer accepted change supersedes an unconverged one, repeated commandId returns stored result without re-executing, revision + idempotency survive reboot.)*
- Existing `NODE_CONFIG`/`CONFIG_ACK` delivery in the sync window after cloud acceptance. *(Not started — downstream of the dispatcher; `dispatcherMarkConverged()` is the hook.)*
- Local-to-backend and dashboard-to-local state mirroring with desired/applied distinction. *(Not started — needs backend.)*

Implementation note (2026-07-17): `node/firmware/shared/command_dispatcher.{h,cpp}` (placed in shared for bench-testability; conceptually mothership-owned). Bench env `esp32wroom-dispatcher`. Convergence tracking (`dispatcherMarkConverged`) is in place but not yet fed by real `CONFIG_ACK`.

Exit gate:

- A queued dashboard node change is accepted exactly once, survives a mothership reboot, is delivered through the existing protocol in the following sync window, converges only after node acknowledgement, and appears identically in local and backend reported state. A stale dashboard change cannot overwrite a newer local change. *(Dispatcher-core half — accepted-once, reboot-survival, stale-rejection — bench-proven; NODE_CONFIG delivery + backend mirroring still pending.)*

### Phase 3 - local mothership self-update

Deliverables:

- Signed manifest validation. *(Done — `firmware_manifest.h`, bench-proven.)*
- Config-mode upload UI. *(Done + bench-run on the real mothership 2026-07-17: `POST /firmware/manifest` (Ed25519 verify + compat) then `POST /firmware/image` (streamed install) drove a full self-update over the config-mode AP — `SW_CPU_RESET` into the new slot, `[OTA] first-boot self-test passed; image confirmed: ESP_OK`, SD absent → LittleFS fallback, config mode resumed. `/api/control` served the persisted dispatcher revision.)*
- Local rollout actions routed through the shared command dispatcher. *(Deferred to the Phase 2 dispatcher; self-update currently drives the installer directly.)*
- Inactive-slot write and verification. *(Done — `ota_installer.h` (streaming esp_ota + SHA-256 gate); bench-proven on node: happy path installs & boots, wrong SHA rejected with slot untouched.)*
- First-boot validation and rollback reporting. *(Done — `mothershipOtaFirstBootCheck()` + `verifyRollbackLater()` in `main.cpp`, confirms after PWR_HOLD/NVS/RTC/wake init; deferred-verify rollback bench-proven on node.)*

Implementation notes (2026-07-17): `mothership/firmware/v2/src/ota/mothership_selfupdate.{h,cpp}` chains verify→compat(role=mothership)→install. Embedded release public key is currently the **bench/test key** — replace with the production key before deployment. Anti-downgrade uses `installedReleaseSequence()==0` for now (no persisted OTA state yet), so the sequence gate is inert until the OTA NVS store lands.

Exit gate:

- Repeated local upgrades, corrupt uploads, interrupted uploads, deliberate bad boots, and rollbacks pass on mothership hardware. *(All proven on the real mothership over HTTP 2026-07-17, SD-free: happy-path upgrade + first-boot confirm; interrupted upload (curl killed mid-transfer) left the running firmware and boot slot untouched with no reboot; deliberate bad-boot image (`mothership-v1-badboot` env, `OTA_FORCE_BADBOOT`) installed, booted, refused to confirm, and the bootloader automatically rolled back to the previous good slot — `[OTA-TEST] BAD BOOT` appeared only on the trial boot, not after rollback. Wrong-SHA/corrupt-image cases proven on the node bench.)*

### Phase 4 - SD release store and local node OTA

Deliverables:

- SD acceptance and atomic release store.
- Local sideload of a node release.
- OTA ESP-NOW control messages.
- Temporary AP/HTTP data path.
- One-node state machine and first-boot validation.

Exit gate:

- One node can update repeatedly without USB, while power interruption and radio loss always retain a bootable image.

### Phase 5 - backend-queued remote LTE acquisition

Deliverables:

- Streaming HTTPS GET in modem driver.
- Backend-queued `DEPLOY_RELEASE` acceptance and approved release-ID resolution.
- Remote signed-manifest check after command acceptance.
- Direct mothership OTA sink.
- SD node-artifact sink.
- Retry/backoff integrated with power/session limits.
- Command-correlated progress, result, reboot resume and local-UI mirroring.

Exit gate:

- A dashboard deployment remains queued until check-in, is accepted once, and mothership/node artifacts can then be acquired over real LTE under weak-signal, disconnect, timeout, and server-error tests. Local and backend views retain the same command/release identity.

### Phase 6 - fleet orchestration and sensor-package releases

Deliverables:

- Canary then staged rollout.
- Pause/cancel/retry and daily caps.
- Backlog/schema safety gate.
- Sensor-package targeting.
- Local and cloud rollout status sourced from the same mothership coordinator.
- Firmware-capability gate before applying a new sensor configuration through existing `NODE_CONFIG` or an additive negotiated extension.

Exit gate:

- A mixed-version, mixed-hardware fleet completes a staged release without data loss or incompatible-node activation.

### Phase 7 - production hardening

Deliverables:

- Unique device-key provisioning/rotation/revocation procedure.
- Backend audit trail correlated with mothership command results.
- Long-duration soak and power-fault testing.
- Operator recovery guide.
- Production release checklist and audit trail.

Exit gate:

- All acceptance tests in section 16 pass on representative deployed hardware.

## 16. Verification plan

### 16.1 Build and static checks

- Both production environments build from clean state.
- Artifact size is below slot budget.
- Manifest size/hash/signature match the actual binary.
- Every OTA struct has a compile-time size assertion.
- Backend command parser rejects oversized, malformed and unknown-version envelopes with focused tests.
- Dispatcher tests cover compare-and-set, duplicate IDs, cursor persistence, supersession and reboot restoration.
- Existing protocol/dispatch/event-queue tests remain green.
- Old firmware ignores new messages safely.

### 16.2 Mothership bench matrix

- Legacy successful upload with no control envelope remains successful.
- Valid backend response is persisted before cursor advance and executed exactly once.
- Duplicate command after a lost acknowledgement returns the stored result without repeating the change.
- Power loss after command persistence but before acknowledgement resumes the same command ID.
- Expired, reordered, wrong-target, unknown-type and oversized commands are rejected without advancing data-upload cursors incorrectly.
- Local revision 43 rejects a delayed dashboard instruction based on revision 42 and mirrors the current state.
- Local revision 44 supersedes an accepted dashboard revision 43 before node convergence and the earlier command is not reported as applied.
- Accepted cloud node configuration waits for the following normal sync window; current sync and upload choreography remain unchanged.
- Remote management disabled results in no command execution while local UI and normal logging continue.
- TLS transport failure never retries control traffic or artifacts over plaintext.
- Valid local update.
- Valid LTE update.
- Wrong role/hardware/version.
- Invalid manifest signature.
- Valid signature with corrupt image.
- Content length too short/long.
- LTE loss at beginning, middle, and end.
- Power cut during inactive-slot write.
- Reset during first boot before confirmation.
- SD missing, read-only, full, corrupt, and removed mid-write.
- LittleFS with pending data and schema-changing release.
- Brownout/watchdog during download.
- Previous app retained after every pre-activation failure.

### 16.3 Node bench matrix

- Valid update for each supported PCB revision.
- Old node receives unknown `FW_OFFER` and continues normal sync.
- Low-battery defer.
- Wrong hardware target reject.
- AP unavailable and wrong credential.
- HTTP disconnect at 10%, 50%, and 99%.
- Corrupt byte/hash failure.
- Power cut during inactive-slot write.
- Reset on first boot before confirmation.
- Missing optional sensor after update does not cause platform rollback.
- Critical RTC/NVS/radio failure does cause rollback.
- Local queue remains intact across upgrade and rollback.
- Normal alarms and `PWR_HOLD` are restored on every abort path.

### 16.4 Fleet tests

- Dashboard node change follows `backend queued -> mothership accepted -> next sync NODE_CONFIG -> CONFIG_ACK -> backend converged`.
- A node absent for several sync windows retains the latest desired configuration and later converges without replaying superseded intermediate states.
- Local and dashboard views eventually show the same desired revision, applied version, firmware target and result after connectivity returns.
- One old node, one bootstrapped current node, and one target node in the same sync session.
- Canary success unlocks rollout.
- Canary rollback pauses rollout.
- Mothership reboot during pending rollout resumes state correctly.
- Node misses several windows and later updates.
- Duplicate offers/results are idempotent.
- Two nodes never receive each other's HTTP authorization.
- Normal sensor data continues from non-updating nodes.

### 16.5 Field acceptance

- Run on battery/solar power, not only USB.
- Measure command latency and energy use for backend queueing, LTE receipt, following sync delivery and acknowledgement reporting.
- Make a local change while LTE is unavailable, restore service, and verify the backend mirror converges without overwriting it.
- Test at representative radio range and obstruction.
- Record update duration, energy use, RSSI, attempts, and temperature.
- Observe at least one complete normal sample/sync cycle after confirmation.
- Retain physical access for the first field pilot.

## 17. Production acceptance criteria

- The dashboard has no direct mothership/node communication path; all remote intent is durably queued in the backend and pulled by the mothership.
- Local UI and dashboard instructions use one dispatcher and one mothership-owned state revision.
- A stale dashboard instruction cannot silently overwrite a newer local change, and a superseded command cannot be reported as node-applied.
- A backend command is persisted before cursor advance, executes at most once, and remains correlated across retries and reboots.
- Accepted node configuration uses the existing `NODE_CONFIG`/`CONFIG_ACK` path in the following sync window.
- Local changes appear in backend reported state after connectivity returns; accepted dashboard changes appear in the local UI after ingestion.
- Backend delivery, mothership acceptance, node transmission and node convergence remain visibly distinct.
- A failed or interrupted download never changes the active boot partition.
- A corrupt, unsigned, wrong-role, wrong-hardware, or downgraded image is rejected.
- A failed first boot returns automatically to the prior confirmed firmware.
- Pairing, node identity, schedules, calibration, sensor mask, and queued readings survive upgrade and rollback.
- Low battery results in a visible deferral.
- Missing SD disables node artifact distribution but does not disable normal collection or mothership self-recovery.
- The mothership updates before nodes when a release changes sensor IDs or storage mapping.
- Schema-changing releases protect or upload the existing data log before activation.
- One node can be canaried before fleet rollout.
- Update progress and final result are visible locally and in cloud status.
- Existing non-OTA nodes continue their current sync protocol throughout migration.
- USB recovery remains documented and tested.

## 18. Recommended first implementation slice

Use two focused vertical slices before combining backend-controlled fleet OTA.

### 18.1 Control and mirroring slice

1. Report mothership/node firmware identity, desired configuration, applied `configVersion`, and mothership state revision.
2. Add the persistent command store and shared dispatcher.
3. Route one existing local node-configuration action through it without changing the resulting `NODE_CONFIG` bytes or sync timing.
4. Parse a mocked/authenticated backend response containing `REQUEST_STATUS`, then a single `SET_NODE_CONFIG`.
5. Persist and accept the instruction after the current LTE phase.
6. Deliver it with the existing `NODE_CONFIG`/`CONFIG_ACK` path in the following sync window.
7. Mirror the accepted/converged state back and render it in the local UI.
8. Prove stale conflict, duplicate delivery, supersession, lost acknowledgement, reboot and backend-outage behaviour.

This slice establishes dashboard/local consistency without introducing firmware download or a new node configuration protocol.

### 18.2 OTA safety slice

1. Pin node partitions and create version/hardware identity.
2. Install and prove rollback-capable bootloaders by USB.
3. Add signed local mothership self-update through the shared dispatcher.
4. Bring up SD as a release store without changing LittleFS logging.
5. Sideload a node image to SD.
6. Update one bench node using ESP-NOW control plus Wi-Fi HTTP data.
7. Prove rollback and queue preservation.

Only after both slices are reliable should a backend-queued `DEPLOY_RELEASE` be allowed to initiate LTE acquisition and fleet rollout. This isolates state reconciliation, boot safety, SD reliability, radio control, Wi-Fi transfer, and modem behaviour before they are combined.

## 19. Rough effort

These are planning estimates for one engineer with access to representative hardware:

| Work | Estimate |
|---|---:|
| Bootstrap identity, partitions, bootloader and rollback proof | 4-7 days |
| Version/capability protocol and UI status | 3-5 days |
| Shared dispatcher, revisioned state and local-UI refactor | 4-7 days |
| Backend response ingestion, durable commands, mirroring and conflict tests | 5-9 days |
| Local mothership OTA | 3-5 days |
| SD release store and local node OTA | 8-15 days |
| LTE streaming acquisition | 5-10 days |
| Fleet orchestration, sensor-release gates and UI | 7-12 days |
| Power-fault, security and field hardening | 10-20 days |

A bench demonstration is realistic before the complete production system. A dependable unattended fleet release path should be treated as a multi-week reliability feature, not only an `Update.write()` integration.

## 20. Open decisions before implementation

- Will the existing ingest/status response carry the command envelope, or will the same LTE session perform a separate small control check-in? Firmware recommends the existing response first, with a dedicated endpoint only if backend response limits require it.
- What maximum dashboard-to-node latency is acceptable given that accepted commands are passed to nodes in the following sync window?
- Which settings are remotely mutable, which require a local physical/service lock, and which must never be mirrored?
- What bounded command count and response size can the backend guarantee per check-in?
- How long must command IDs/results and backend audit records be retained?
- Will the dashboard submit full desired node configuration as recommended, or versioned patches that require explicit merge semantics?
- Which backend/frontend labels and reason text map to `QUEUED`, `ACCEPTED`, `WAITING_FOR_SYNC`, `SENT_TO_NODE`, `CONVERGED`, `SUPERSEDED` and `REVISION_CONFLICT`?
- What separate remote-management cadence and battery policy is acceptable when no reading upload is due?
- Which exact node and mothership hardware revision identifiers must be supported by the first release?
- Will every future mothership have a working SD socket, and what minimum card capacity/class is supported?
- Which release host will serve immutable HTTPS artifacts?
- Is a local-only release workflow sufficient for the first field pilot after control mirroring is proven, or is backend-queued LTE acquisition required immediately?
- Which signing library and canonical manifest representation will be used?
- How will per-node OTA control secrets be provisioned and rotated?
- What measured battery thresholds and maximum maintenance duration are safe?
- Must cross-power-cycle node download resume be supported, or is restart-from-zero acceptable?
- Which platform-critical self-tests must pass before confirming each role?
- What cloud user role is authorised to schedule or cancel a fleet rollout?

These decisions should be recorded in this document before Phase 0 closes.
