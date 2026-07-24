# Cloud OTA — bench runbook (mock backend)

Prove the mothership's cloud-OTA path end-to-end over real LTE **before** the
real backend/dashboard exist, by serving a throwaway mock that plays both the
check-in endpoint and the release host. Mirrors how the local-AP OTA and the
modem HTTPS POST were originally bench-proven.

What this exercises that the unit/component tests can't: the live modem
streaming GET of a real ~1 MB image, the full `PREFLIGHT → DOWNLOADING →
VERIFYING → READY_TO_REBOOT` chain, the cold-boot into the new slot,
`mothershipOtaFirstBootCheck()` confirming it, and the failure legs
(interrupt/stall/budget/bad-boot rollback/anti-downgrade).

> Everything here is a **throwaway test harness**. No production keys, no real
> endpoints. The mock has no auth and no durability by design.

---

## 0. Prerequisites
- Mothership with a **powered A7670G modem, SIM with data, antenna**.
- `cryptography` Python package (for signing + the mock's self-signed cert):
  `py -3 -m pip install --user cryptography`.
- A **public HTTPS tunnel** to your PC: [ngrok](https://ngrok.com) or
  `cloudflared`. The modem reaches the mock over the public internet, so a
  purely-local server will not work. The tunnel terminates TLS with a valid
  public cert (the modem does not verify certs, but a tunnel is the simplest way
  to be reachable).

## 1. Generate a bench signing key + point the firmware at it
The firmware verifies the manifest against the embedded `kReleasePubKey` in
`mothership/firmware/v2/src/ota/mothership_selfupdate.cpp`. For the bench, use a
key you control:

```bash
py -3 scripts/release_sign.py keygen --out-dir keys
py -3 scripts/release_sign.py pubkey --key keys/release_ed25519
```
The `pubkey` command prints a `static const uint8_t kReleasePubKey[32] = {...};`
line. **Temporarily** paste it over the one in `mothership_selfupdate.cpp` for
the bench build (revert when done — `keys/` and `release/` are git-ignored).
> If you still have the original bench private key that matches the committed
> `kReleasePubKey`, you can skip this and sign with that instead.

## 2. Build the "new" firmware image to install
This is the image the mothership will download and boot into. Build it against
the pinned OTA partition table and bump `FW_SEMVER` so you can see the version
change after install:

```bash
cd mothership/firmware/v2
# Edit platformio.ini [env:mothership-v1-main] FW_SEMVER to e.g. "0.2.0", then:
pio run -e mothership-v1-main
# the image is .pio/build/mothership-v1-main/firmware.bin
```
Copy it to `release/image.bin` at the repo root (or pass `--image` to the mock).

## 3. Make + sign the manifest
```bash
py -3 scripts/release_sign.py make \
    --key keys/release_ed25519 \
    --release-id fieldmesh-2026.08.0 \
    --sequence 1 \
    --artifact role=mothership,version=0.2.0,hw=mothership-v1,proto=2,min-mothership=0.1.0,bin=release/image.bin \
    --out-dir release
```
This writes `release/manifest.json` + `release/manifest.json.sig`.
> **Anti-downgrade note:** `--sequence` must be **>** the sequence of whatever is
> already installed-and-confirmed on the device (a fresh device reads 0, so
> `--sequence 1` installs; to test `DOWNGRADE_REJECTED`, later serve `--sequence 0`).

## 4. Start the mock + tunnel
```bash
py -3 scripts/mock_ota_server.py \
    --release-dir release --image release/image.bin \
    --release-id fieldmesh-2026.08.0 --role mothership \
    --ingest-path /ingest --command-id mock-ota-0001 --port 8443
# in another shell:
ngrok http https://localhost:8443        # -> https://<random>.ngrok-free.app
```
Note the tunnel host, e.g. `abc123.ngrok-free.app`.

## 5. Build + flash the mothership for the bench
The firmware derives release URLs from `FM_APPROVED_ENDPOINT_HOST` and only
uploads to / fetches from that approved host. Point both at the tunnel:

```bash
cd mothership/firmware/v2
pio run -e mothership-v1-main \
    -t upload --upload-port COM4 \
    --build-flag '-D FM_APPROVED_ENDPOINT_HOST=\"abc123.ngrok-free.app\"'
```
> This is the mothership's *running* firmware for the test — it must have the
> bench `kReleasePubKey` from step 1. (The image it *downloads* in step 2 can
> keep the same key.)

Then in the mothership's **config-mode UI**, set the upload endpoint to
`https://abc123.ngrok-free.app/ingest` and a non-empty API key (any value — the
mock ignores auth but the firmware uses the Supabase header path when a key is
set, sending the URL unmodified).

## 6. Trigger a check-in and watch
Let the mothership take a sync/upload wake (or force one). On `pio device
monitor -p COM4` you should see, in order:

1. `[CONTROL] ... commands=1 ... deploy-release durable ... releaseId=fieldmesh-2026.08.0`
2. `[OTA] pending release fieldmesh-2026.08.0 — starting cloud fetch`
3. `=== ModemDriver::httpsGetStream() ===` (manifest, then sig, then image)
4. `[Modem] GET stream: status=200 declared=<size> delivered=<size> complete=1`
5. `[OTA] cloud fetch result: state=READY_TO_REBOOT reason=NONE ... rebootPending=1`
6. `[OTA] image armed — next RTC-alarm wake will boot the new slot`

On the mock console you'll see the DEPLOY_RELEASE served, then the three GETs.

## 7. Confirm the install took
Let the mothership power down and take its **next** wake (it cold-boots into the
armed slot). Watch for:
- `[OTA] first-boot self-test passed; image confirmed: ESP_OK`
- `[OTA] release promoted to installed: fieldmesh-2026.08.0 (seq 1)`
- `[fwid] ... ver=0.2.0 ...` — the new version is now running.
- On the next check-in, `status.firmware{}` reports `releaseId:"fieldmesh-2026.08.0"`,
  `armedReleaseId:null`, and the active slot flipped.

## 8. Failure legs (run each after the happy path)
- **Interrupted transfer:** restart the mock with `--truncate-image 200000`
  (declares the full length but sends only 200 KB then closes). Expect the
  running firmware/slot untouched, `pendingReleaseId` retained, and the **next**
  wake retries and succeeds with no new command. (`DOWNLOAD_TRUNCATED`,
  transient.) Killing the mock (Ctrl-C) mid-image works too.
- **Stall:** restart the mock with `--stall-image-sec 30` (serves image headers
  then goes quiet). Expect `DOWNLOAD_TIMEOUT` within ~20 s (the idle timeout),
  not the full 5-minute session budget.
- **Bad-boot rollback:** build the step-2 image with the existing
  `-D OTA_FORCE_BADBOOT` flag, re-sign, serve it. Expect it to install, boot
  `PENDING_VERIFY`, fail to confirm, and the bootloader auto-reverts. Next
  check-in shows the *old* `releaseId` running while `armedReleaseId` was the new
  one → dashboard renders `ROLLED_BACK`.
- **Anti-downgrade:** after a confirmed install of `--sequence 1`, serve a fresh
  command (new `--command-id`) for a manifest with `--sequence 0`. Expect
  `state=FAILED reason=DOWNGRADE_REJECTED`, no partition change.
- **Low battery / low budget:** these `DEFERRED_*` legs are already unit-proven
  (`test_ota_cloud_fetch`); to see one live, run the check-in with a low battery
  or near the end of the session budget.

## 9. Clean up
- Revert the temporary `kReleasePubKey` edit and the `FW_SEMVER` bump.
- Delete `keys/` and `release/` (git-ignored).
- Rebuild + reflash production firmware (with the real approved host) before
  returning the mothership to the field.
