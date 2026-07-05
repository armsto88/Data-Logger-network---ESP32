# AS7341 extended metadata null investigation

**Date:** 2026-07-04  
**Scope:** V2 node and mothership firmware only  
**Fields:** `spectral_clear`, `spectral_nir`, `spectral_gain`, `spectral_integration_ms`, `spectral_saturated`

## Result

The null conversion occurs when the mothership receives a V2 snapshot without sensor IDs 1109-1113:

```text
node V2 packet lacks IDs 1109-1113
  -> DecodedSnapshot.find(1109..1113) returns missing
  -> flash_logger writes nan in CSV columns 25..29
  -> json_payload converts each nan cell to JSON null
  -> Supabase faithfully stores explicit null values
```

The JSON mapping itself is correct. A production-function regression fixture now proves that finite ID/value pairs survive V2 decode, become a 30-column CSV row, and map to the five exact JSON field names.

## Confirmed node defect

The eight visible bands and the five extended values were not strictly bound to one exposure.

`read()` captured and cached the visible bands, but `getMetadata()` called `sampleIfNeeded()` again. The cache timestamp was recorded at the **start** of the potentially six-read auto-gain loop. By the time all registry sensors had been read and `buildReadingsArray()` requested metadata, more than 300 ms could have elapsed. This allowed `getMetadata()` to start a second AS7341 acquisition.

Consequences:

- if the second acquisition succeeded, Clear/NIR/gain/time could describe a different exposure from F1-F8;
- if it failed, `g_haveSample` became false, `md.valid` was false, IDs 1109-1113 were not appended, and all five values became `nan`/`null` downstream;
- the eight bands from the earlier successful exposure remained numeric, matching the observed database shape.

The fix:

- timestamps the cache when acquisition completes;
- makes `getMetadata()` read-only: it never starts an acquisition;
- makes `metadataAvailable()` require an existing successful sample;
- validates all four floating metadata values as finite;
- appends all five metadata entries atomically or none;
- logs the exact metadata and final snapshot capacity.

## Additional correctness fixes

### Auto-gain metadata

Previously, the final auto-gain attempt could change `g_gainIdx` and exit without acquiring at that new gain. Counts could therefore belong to the previous gain while metadata reported the next gain. Gain changes now occur only when another acquisition attempt remains, and failed gain writes invalidate the sample.

A failed `STATUS2` read now explicitly marks the exposure invalid and is logged. `spectral_saturated` remains numeric `0` or `1`.

### Capacity

The maximum registry has 20 readings. Capture adds at most one battery reading and five AS7341 metadata readings:

```text
20 + 1 + 5 = 26 <= MAX_READINGS_PER_SNAPSHOT (33)
```

A compile-time assertion enforces this. Runtime appending is all-or-nothing, so capacity pressure cannot create a misleading partial metadata set.

### Safe CSV schema migration

The previous `initFlash()` behavior deleted `/datalog.csv` whenever its header differed from the new header. That could silently erase unuploaded 25-column rows during a firmware upgrade.

The new policy is:

1. A current 30-column file is used normally.
2. A legacy 25-column file containing rows is preserved byte-for-byte.
3. New rows may append five trailing values; the first 25 positions remain unchanged.
4. Uploads are presented with the current 30-column superset header. Legacy rows simply lack the five trailing values; new rows contain them.
5. Once the cursor reaches EOF and all queued rows are drained, `purgeUploaded()` atomically replaces the empty file's legacy header with the 30-column header.
6. An unknown header is preserved and logging refuses to append an incompatible row instead of deleting data.

The flash logger and upload queue now share one header definition in `src/storage/csv_schema.h`.

## Boundary logging added

After flashing the new production builds, one successful exposure should emit:

```text
[FW] node build=<date time> protocol=2 spectral_metadata_ids=1109-1113
[PAR] ... CLR=<number> NIR=<number> GAIN=<number>x TINT=<number>ms SAT=<0|1> ...
[SENS-SPEC] appended ids=1109-1113 clear=<number> nir=<number> gain=<number> integration_ms=<number> saturated=<0|1> total=<n>/32
   [...] id=1109 value=<number>
   [...] id=1110 value=<number>
   [...] id=1111 value=<number>
   [...] id=1112 value=<number>
   [...] id=1113 value=<0|1>
```

The mothership should then emit:

```text
[FW] V2 snapshot decode; CSV schema=30; spectral metadata IDs=1109-1113
[SNAP-SPEC] ... extended=5/5 clear=<number> nir=<number> gain=<number> integration_ms=<number> saturated=<0|1>
[FLASH-SPEC] ... columns=30 extended_csv=number,number,number,number,number
[JSON-SPEC] csv_fields=30 spectral_clear=<number> spectral_nir=<number> spectral_gain=<number> spectral_integration_ms=<number> spectral_saturated=<0|1>
[JSON-SPEC-OBJECT] {..."spectral_clear":<number>,"spectral_nir":<number>,"spectral_gain":<number>,"spectral_integration_ms":<number>,"spectral_saturated":<0|1>}
```

If the first node log exists but `[SENS-SPEC] appended` does not, the new diagnostic states whether metadata was unavailable, non-finite, or lacked packet capacity. If the mothership reports `extended=0/5`, the values were already absent from the received node packet.

## Deployment identity

Repository HEAD at investigation start:

```text
5734e5d07cf1949f2640828d1ecb83aa53a67030
```

That commit introduced the extended metadata implementation. The prior commit cannot send IDs 1109-1113.

No USB ESP32 serial device was attached during the initial investigation, so the exact deployed images could not initially be identified. The mothership was subsequently attached and verified as described below; node verification is still pending.

New locally built production artifacts:

```text
node/firmware/v2/.pio/build/esp32wroom/firmware.bin
SHA-256 9A34761BFB7616E3697938EE01CD9739EF2646EAFE6D41CC8FA233056A6F1B12

mothership/firmware/v2/.pio/build/mothership-v1-main/firmware.bin
SHA-256 1D7F9E705F9713D1A1C5216DA678B405A4AD9A9B76158C2DB133C6875EF932FA
```

Build timestamps are also printed at boot. Capture those lines from each attached board before and after flashing. The mothership already sends its `firmwareBuild` in upload metadata; compare that value with its boot line.

### Mothership hardware follow-up, 2026-07-05

The mothership appeared as CH340 `COM4`. Its pre-flash boot identity was:

```text
Build: Jul  4 2026 13:53:43
```

It was flashed successfully with `mothership-v1-main`. The upload wrote the bootloader, partition table, OTA metadata, and application image; it did not write or erase the LittleFS/SPIFFS data partition at `0x310000`. Post-flash serial evidence was:

```text
Build: Jul  4 2026 19:30:12
[FW] V2 snapshot decode; CSV schema=30; spectral metadata IDs=1109-1113
```

This confirms the current 30-column decoder/logger/migration image is running on the mothership. A normal USB service wake does not receive a node snapshot, so the `[SNAP-SPEC]`, CSV, and JSON boundary evidence still requires the updated node and a data/sync wake.

### Node 1 hardware follow-up, 2026-07-05

Node 1 appeared as CH340 `COM4` with identity `ENV_6C0AA0`. Before flashing it detected the AS7341 and registered visible IDs 1101-1108, but its boot output did not contain the new `[FW] ... spectral_metadata_ids=1109-1113` marker. Its queue state before flashing was empty with `nextSeq=1174`.

The verified `esp32wroom` production image was uploaded successfully without erasing NVS. During the next scheduled data wake the node released `PWR_HOLD` and its CH340 disappeared, interrupting the host serial reader before the buffered exposure lines could be returned. A second physical wake is required to capture the post-flash build marker and inspect whether the queue advanced to sequence 1175. This is not yet counted as runtime proof of the five metadata values.

### Node 1 hardware follow-up, 2026-07-05

Node 1 appeared as CH340 `COM4` and identified itself as `ENV_6C0AA0`. Before flashing it detected the AS7341 and registered F1-F8, but its boot output did not contain the new marker:

```text
[FW] node build=<date time> protocol=2 spectral_metadata_ids=1109-1113
```

That proves the previously running application was not the final instrumented image. `esp32wroom` was flashed successfully twice at the operator's request. Esptool verified every written segment. The application upload did not erase the NVS partition, so node identity, deployment configuration, and its local queue were retained.

The board was disconnected before a post-flash data alarm completed, so a physical `[PAR]`/`[SENS-SPEC]` exposure and transfer to the mothership remain the next acceptance step.

## Verification performed

Successful compile targets:

```text
node/firmware/v2:        platformio run -e esp32wroom
mothership/firmware/v2:  platformio run -e mothership-v1-main
mothership/firmware/v2:  platformio run -e mothership-v2-test-spectral-pipeline
```

The focused regression firmware uses the production `decodeV2()`, CSV formatter, and `buildJsonUpload()` with:

```json
{
  "spectral_clear": 12000,
  "spectral_nir": 6800,
  "spectral_gain": 4,
  "spectral_integration_ms": 50.04,
  "spectral_saturated": 0
}
```

It asserts 13 decoded spectral readings, a 30-column row, correct values at positions 25-29, and five numeric JSON fields. The test image compiled successfully but was not executed because no ESP32 serial device was connected.

## Remaining hardware acceptance

The following require the physical node/mothership and cannot be fabricated from source inspection:

1. Record the pre-flash node `[FW]` build and mothership `Build:` line.
2. Flash both new production images.
3. Capture one exposure using the boundary logs above.
4. Download `/datalog.csv` from the mothership and verify the new row has 30 cells and numeric cells 25-29.
5. Trigger one JSON upload and retain `[JSON-SPEC-OBJECT]`.
6. Query the uploaded Supabase row by node ID and sequence number and confirm all five values are numeric.

Do not erase LittleFS to perform the upgrade. The migration is specifically designed to drain existing queued rows safely.
