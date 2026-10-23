# Vendored SD library (patched)

This is the `SD` library from the Arduino-ESP32 core bundled with
`framework-arduinoespressif32` 3.20017.241212 (Arduino-ESP32 2.0.17, library
version 2.0.0). Only `src/` and `library.properties` were copied. PlatformIO's
library finder prefers a project `lib/` library over a framework library of the
same name, so every env in this project builds against this copy.

## The one change

`src/sd_diskio.cpp`, `ff_sd_initialize()`, SD v2 branch: the ACMD41
(`APP_OP_COND`) argument is now `0x40000000` instead of `0x40100000`.

In SPI mode, ACMD41's argument carries only HCS (bit 30). Bits 29:0 are
reserved and must be zero. Upstream also sets bit 20, the 3.2-3.3 V window bit,
which only has a meaning in SD bus mode. Most cards ignore it. The 16 GB SDHC
card used to bring up FieldHub `HUB-R6-01` on 2026-10-23 stays idle (R1 = 0x01)
for as long as that bit is set. So `SD.begin()` never mounts it, and production
`initSD()` reports no card.

Evidence (`hardware/FieldHub_v2/revisions/2026-10-10-r6/bringup/HUB-R6-01/`):

- `sd-raw` variant matrix, on the hardware SPI path at 400 kHz:
  - `0x40000000` initialises in 2 ms, with CRC on or off.
  - `0x40100000` stays idle for 1000 ms, with CRC on or off.
- Stock library: `sd-card` and `sd-sweep` fail with
  `APP_OP_COND failed: 1` / `f_mount failed: (3)`.
- With this patch: `sd-sweep` passes 7/7 (400 kHz to 40 MHz) and
  `mothership-v2-test-sd-logger` passes 25/25.

## Upgrading the framework

If the platform is moved to a newer Arduino-ESP32, re-copy that version's
`libraries/SD/src` and re-apply the same one-line change; do not keep this
2.0.17 copy against a newer core. Remove this directory once upstream sends a
spec-compliant ACMD41 argument.
