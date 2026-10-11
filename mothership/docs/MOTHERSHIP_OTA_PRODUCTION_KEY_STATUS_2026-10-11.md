# Mothership OTA — production key and first cable flash (2026-10-11)

Firmware-side record of the first over-the-air test attempt on the production
release key. The backend and publishing half, including why the publish is
blocked, is in the FieldMeshDashboard repo at
`docs/operations/ota-first-test-status-2026-10-11.md`. The runbook is
`docs/operations/ota-first-test.md` there.

**Rule for this document: only record what was run.** Inferences are labelled.

OTA was **parked on purpose** at the end of the session. No firmware change is
needed to resume it; the blocker is on the upload path (see the dashboard
record).

## What changed in firmware

Branch `feat/fm2-provisioning-redemption`, commit `f7fb447`
("trust the production release key instead of the bench key"):
`kReleasePubKey` in `mothership/firmware/v2/src/ota/mothership_selfupdate.cpp`
now holds the production release public key
`c2e627e97494e4055aefed78367e62217023a4bfcddb9001ce5ede7b363752db`. The July
bench key's private half is no longer available, so a build with the old key
could never have been updated over the air.

**Checked:** the 32 bytes in the array equal that hex string, and equal the
`publicKey` field of `~/fieldmesh-keys/release_ed25519.json` (the private half
was not printed). The backend secret `OTA_SIGNING_PUBLIC_KEY_HEX` holds the same
key.

## Version A — built and flashed

- Environment `mothership-v1-main`, `FW_SEMVER` `0.1.0`
  ([platformio.ini:427](../firmware/v2/platformio.ini#L427)).
- Build: RAM 23.0 % (75,448 of 327,680 bytes), flash 90.2 % (1,419,117 of
  1,572,864 bytes). `firmware.bin` is 1,425,696 bytes. The app slot has little
  headroom, so watch this number when adding features.
- Cable flash to the bench hub that feeds the demo project, over a CH340 adapter
  (`1a86:7523`) at `/dev/ttyUSB0`:
  `pio run -e mothership-v1-main -t upload --upload-port /dev/ttyUSB0`.
  esptool wrote 1,425,696 bytes (856,403 compressed) at `0x10000`, reported
  "Hash of data verified", and reset the board through RTS.

### What was seen on serial afterwards, and what was not

Immediately after the flash the hub printed, every 2 seconds:

```
[WAIT] Board still alive — flick SW10 to cut USB power.
```

That is `loop()` in [main.cpp:2365-2377](../firmware/v2/src/main.cpp#L2365-L2377).
It runs once `setup()` has finished its wake handling and tried to release
`PIN_PWR_HOLD`, while USB keeps the board powered. It is the normal idle state on
USB, not a fault.

**Not seen, so not confirmed:** the boot and wake log of the new image (the
flash's own reset happened before the serial port was opened), the first
scheduled sync, and the first upload from this build. The runbook expects the
hub's connection key to survive a flash; no upload was observed to confirm it.
**Check:** the hub's last upload and firmware version on the dashboard's
FieldHubs page.

### Reading serial from a script

`pio device monitor` refuses to run without an interactive terminal. The system
Python has no `pyserial`; PlatformIO's does. Set DTR and RTS **before** opening
the port, as the existing bench notes say, because 0/0 resets the board:

```python
# ~/.platformio/penv/bin/python
import serial
s = serial.Serial(); s.port = "/dev/ttyUSB0"; s.baudrate = 115200; s.timeout = 0.5
s.dtr = True; s.rts = True
s.open()
```

To see a full boot, flick SW10 (cut and restore power) with the port open.

## Version B — built and signed, never flashed

`FW_SEMVER` `0.1.1`, same environment. The build is the same size as version A
(1,425,696 bytes) and the string `0.1.1` is present in the binary. It was
signed as release `fieldmesh-2026.10.1` (sequence 100); `image.bin` SHA-256
`e5d670d2c7f3ae5b5a32561bf82a5c1dcb0eb624b683ac54e38c491920c82487`. It was
**not published** (blocked upstream) and **never flashed**.

`FW_SEMVER` in `platformio.ini` was put back to `0.1.0` afterwards, so the
working tree matches what is on the hub. To resume, set it to `0.1.1` again and
rebuild without `-t upload`.

## Which hubs trust which key

- A hub only installs firmware whose manifest is signed by the key it was
  flashed with. Only builds from `f7fb447` onward carry the production key.
- Hubs flashed from any earlier build, **including the UNE solar farm hubs on
  older firmware**, do not trust it and need one cable flash before they can be
  updated over the air.
- *Inferred from the design, not tested:* the two existing catalog releases
  (`fieldmesh-bench-2026.07.0` and `…0b`) were signed with the bench key, so a
  hub on this build should refuse them.

## Open firmware-side items

- Confirm version A boots, syncs and uploads on the bench hub.
- Confirm the bench hub's connection key survived the flash.
- `feat/fm2-provisioning-redemption` is not merged to `main`.
- The UNE hubs need a cable flash with a production-key build before any OTA.
