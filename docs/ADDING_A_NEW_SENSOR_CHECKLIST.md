# Adding a New Sensor — Considerations & Checklist

**Purpose:** A pre-flight checklist so adding or extending a sensor doesn't silently
break other sensors or lose data. Written after the 2026-07-04 spectral/soil incident,
where adding 5 spectral metadata "channels" overflowed the sensor registry and silently
starved **soil probe 2 and wind** on deployed nodes.

Read this **before** touching a sensor backend. The failure mode is nasty: everything
compiles, the sensor you added works, and a *different* sensor quietly stops reporting.

---

## The #1 Rule: A physical sensor is not its channels, and its channels are not its metadata

Three distinct concepts that are easy to conflate:

| Concept | Example (AS7341 spectral) | Where it lives |
|---------|---------------------------|----------------|
| **Physical sensor** | The one AS7341 chip | One backend, one I2C address |
| **Channels** | 8 visible bands (415–680 nm) | Registry slots (one each) |
| **Metadata** | Clear, NIR, gain, integration time, saturation | **NOT registry slots** — packed as snapshot extras |

**What went wrong:** We registered all 13 (8 bands + 5 metadata) as separate registry
entries. That's the trap. Metadata describes *the measurement*, not independent sensors.

**Rule of thumb:** If a value would be nonsensical to "select" on its own in the
mothership UI (e.g. "gain multiplier"), it is **metadata**, not a channel. Metadata rides
the snapshot via a `getMetadata()`-style accessor (see `sensors_par_as7343.cpp`), it does
**not** call `commitSensorSlot()`.

---

## Trap 1: MAX_SENSORS registry overflow (the silent killer)

`MAX_SENSORS` in [node/firmware/shared/sensors.h](../node/firmware/shared/sensors.h)
caps the registry. When it's full, `commitSensorSlot()` **returns false silently** — no
crash, no error, the sensor just never registers. Backends registered *later* in
`initSensors()` are the ones that get starved.

**Registration order in `initSensors()` matters.** Current order:
`SHT41 → spectral → soil → ultrasonic wind → reed wind → aux`.
If spectral eats too many slots, **soil and wind (registered after it) disappear first.**

**Before adding channels, do the math:**
```
air(2) + spectral bands(8) + soil(4) + wind(1) + aux(2) = 17 slots
MAX_SENSORS must be >= that, with headroom (currently 20).
```

**Symptom in logs:** `[SENS] ✅ Total registered sensors: N` where N is lower than
expected, and a backend's `[SENS] Slot X -> ...` lines are simply missing (no gated
message either — gating logs `[SENS] gated`, overflow logs *nothing*).

**Fix options:**
- Prefer: keep metadata out of the registry (see Rule #1).
- If you genuinely need more channels: raise `MAX_SENSORS` **and** update its comment math.

> **Improvement idea (not yet done):** make `commitSensorSlot()` log a loud
> `[SENS] REGISTRY FULL — dropped id=...` when it returns false on overflow. That one log
> line would have saved hours here.

---

## Trap 2: The full pipeline — a new channel touches ~8 files

A sensor value has to survive a long journey. Miss a link and it silently becomes `nan`
or `null` downstream. Touch **all** of these:

### Node side
1. **Driver backend** (`sensors_<name>.cpp/.h`) — read the hardware, expose
   `count()/label()/type()/read()`. Metadata goes through a separate accessor.
2. **Sensor IDs** ([protocol.h](../node/firmware/shared/protocol.h)) — add
   `SENSOR_ID_*` constants. **IDs are grouped by range** (1100=spectral, 2000=soil, etc.).
3. **`resolveSensorId()`** ([sensors.cpp](../node/firmware/src/sensors/sensors.cpp)) —
   map the new label string → sensor ID. Miss this and the slot resolves to
   `SENSOR_ID_UNKNOWN (0)` and is skipped in the snapshot.
4. **`snapPresentBitForSensorId()`** (protocol.h) — map the ID → its `SNAP_PRESENT_*`
   capability bit, so mask gating and mothership fault detection agree.
5. **Registration loop** (sensors.cpp `initSensors()`) — the backend gets its
   `for` loop; usually nothing to change unless it's a brand-new backend.
6. **Snapshot builder** (sensors.cpp `buildReadingsArray()`) — channels go automatically
   via the registry loop; **metadata must be appended explicitly** after the loop.

### Mothership side
7. **`decodeV2()`** ([flash_logger.cpp](../mothership/firmware/v2/src/storage/flash_logger.cpp)) —
   add the new ID to the `switch` that rebuilds `sensorPresent`, or the SPECTRAL/etc. bit
   won't set and the mothership flags a false fault.
8. **CSV writers (THREE of them!)** in flash_logger.cpp — `logDecodedSnapshot()` (V2),
   `logSnapshotRow()` (V1), `logSnapshotBatch()` (V1). Plus **`kCSVHeader`**. Keep the
   column count identical across all four or the CSV↔JSON mapping shifts.
   - **Append new columns at the END** (after `aux2`) so existing column indices don't move.
   - V1 writers emit `nan` for fields the V1 struct doesn't have.
9. **JSON payload mapping** ([json_payload.cpp](../mothership/firmware/v2/src/storage/json_payload.cpp)) —
   bump `kNumCsvColumns` and add the `{index, "key", CELL_NUM_NULLABLE}` rows.

### Backend / Frontend
10. **Supabase schema** — add nullable columns; update the ingest key whitelist if any.
11. **Frontend** — usually no change if wire format (sensorId+value) is unchanged.

**Wire format note:** the V2 snapshot is `{sensorId, value}` pairs. The mothership maps by
**ID, not order**, so channel ordering on the node doesn't matter to the backend. But the
**CSV/JSON column mapping IS positional** — that's why appending (not inserting) matters.

---

## Trap 3: Sensor IDs are NOT sequential by channel

Example that bit us: soil IDs are interleaved by *type*, not by probe:
```
SENSOR_ID_SOIL1_VWC  = 2001
SENSOR_ID_SOIL2_VWC  = 2002
SENSOR_ID_SOIL1_TEMP = 2003
SENSOR_ID_SOIL2_TEMP = 2004
```
So a boot log showing `Slot 11 -> id=2003 label='SOIL1_TEMP'` is **correct**, even though
2003 looks "out of order." Verify the **label ↔ ID** pairing, not the numeric sequence.

Also: the backend's `label(index)`, `type(index)`, and `read(index)` must all agree on
what each index means. We had `read()` returning the wrong cached value for an index
because the label order was changed without updating `read()`. Keep the three switch
statements in lockstep.

---

## Trap 4: Build cache & flashing gotchas

- **`pio run -t upload` can flash a stale binary** if the build cache is confused. If a
  code change "didn't take," do a clean rebuild:
  ```
  pio run -e esp32wroom --target cleanall
  pio run -e esp32wroom -t upload --upload-port COMx
  ```
  Confirm the recompile actually happened (a real build is ~30–40s, not <2s) and check the
  `firmware.bin` timestamp.
- **Verify by boot log, not by assumption.** Always read the `[SENS] Slot ...` lines after
  flashing to confirm every expected sensor registered.

---

## Trap 6: Two mothership receive paths — testing one does not prove the other

The mothership has **two separate ESP-NOW receive callbacks**, and it's easy to verify
only one of them:

| Path | File | When active | Used by |
|------|------|-------------|---------|
| Config-mode | `espnow_config.cpp` | WiFi AP up (discovery, pairing, bench tests) | Manual testing, `mothership-v2-test-spectral-pipeline` |
| Sync-mode | `espnow_sync.cpp` | Every real fleet sync window | **The only path that matters in the field** |

**What went wrong (2026-07-05):** adding the 5 AS7341 metadata fields was verified with a
bench regression test and a live bring-up capture — both went through **config-mode**,
which decodes straight into the flexible `DecodedSnapshot` (ID/value pairs) and looked
perfect. But every field-deployed reading still landed `null` in the backend.

The cause: `espnow_sync.cpp`'s receive callback decoded the packet correctly, then called
`decodedToV1(decoded, slot.snap)` to downgrade it into `node_snapshot_t` — a **legacy,
fixed-field struct** predating the ID/value-pair protocol, with named fields only for
what existed when V1 was written (`spectral[8]`, `batVoltage`, `soil1Vwc`, ...). Anything
without a dedicated field there — the 5 new metadata values, or any future sensor without
a hand-added V1 field — is silently discarded at that single conversion line, before ever
reaching flash or JSON. The queue (`EspNowSnapSlot`) was left carrying `node_snapshot_t`
as a leftover compatibility shim from before the flexible protocol existed, and was never
upgraded when V2 landed.

**The fix:** `EspNowSnapSlot` now carries a `DecodedSnapshot` directly (same flexible
shape used everywhere else). `onEspNowRecv()` in `espnow_sync.cpp` decodes straight into
the slot for both V1 and V2 wire packets — no downgrade step. `processSnapshot()` in
`main.cpp` is called with the decoded snapshot directly; the old `node_snapshot_t*`
compatibility wrapper was deleted since nothing needs it anymore.

**Symptom in logs:** the giveaway is a mismatch between what the *node* built and what
the *mothership* decoded for the identical `seq`. Node-side: `sensorCount=20` with all
expected IDs listed. Mothership-side, during a **real sync** (not a bench test):
`[SNAP-SPEC] ... readings=15 extended=0/5 clear=MISSING ...`. If a bench/config-mode test
shows the field working but the field data is still null, **this is the first thing to
check** — don't assume decodeV2() itself is broken; check whether the sync-window path
still round-trips through a fixed-field struct.

**Because of the fix, this class of bug cannot recur:** there is now exactly one snapshot
shape (`DecodedSnapshot`, ID/value pairs, capacity `MAX_READINGS_PER_SNAPSHOT`) from radio
receive through to flash/JSON, on both mothership receive paths. Adding a sensor never
requires touching `espnow_sync.cpp`, `espnow_config.cpp`, or the snapshot queue again —
only the steps in Trap 2 above.

**One ceiling this does NOT remove:** `MAX_READINGS_PER_SNAPSHOT` (protocol.h) caps the
*total* readings (channels + metadata) a single node snapshot can carry — currently **33**,
derived from the ESP-NOW single-packet limit: `(250 − 48 byte header) / 6 bytes per
reading = 33`. This is a **different constant from `MAX_SENSORS`** (Trap 1, the node's
registry-slot cap) — `MAX_READINGS_PER_SNAPSHOT` also has to fit metadata that never
touches the registry. Current usage is ~20–26 depending on node sensor mix (a
compile-time `static_assert` guards this — e.g. `20 + 1 + 5 = 26 <= 33`). If future
sensors push a single node's total past 33, that needs a real design change (multi-packet
snapshots), not a constant bump — raising `MAX_READINGS_PER_SNAPSHOT` alone would exceed
one ESP-NOW packet's hard 250-byte payload limit.

---

## Trap 5: Flash order & the CSV wipe

- **Flash the mothership before the nodes** when you add new sensor IDs. The mothership
  must understand the new IDs before nodes start sending them, or it flags false faults /
  drops them.
- **Changing `kCSVHeader` wipes the mothership's local `datalog.csv`** on next boot (the
  header-mismatch check recreates the file). **Upload any pending backlog first**, or
  un-uploaded rows are lost.
- **NVS survives a normal flash.** Pairing, deployment, RTC, sensor mask all persist. Only
  a full chip erase wipes NVS.

---

## Pre-merge checklist

Copy this into the PR description and tick each box:

- [ ] Decided per value: **channel** (registry slot) vs **metadata** (snapshot extra)?
- [ ] `MAX_SENSORS` still ≥ total registry slots, with headroom? Comment math updated?
- [ ] `MAX_READINGS_PER_SNAPSHOT` (33) still covers channels + metadata for the busiest
      node? (Different cap from `MAX_SENSORS` — see Trap 6.)
- [ ] `SENSOR_ID_*` added in the correct range block (protocol.h)?
- [ ] `resolveSensorId()` maps every new label?
- [ ] `snapPresentBitForSensorId()` maps every new ID to its capability bit?
- [ ] Snapshot builder emits it (auto for channels; **explicit append for metadata**)?
- [ ] `decodeV2()` switch sets the right `sensorPresent` bit?
- [ ] **All 3 CSV writers + kCSVHeader** updated, columns **appended** at the end?
- [ ] `kNumCsvColumns` bumped and JSON mapping rows added (json_payload.cpp)?
- [ ] Backend schema columns added (nullable)?
- [ ] Node build clean (`cleanall` if in doubt)?
- [ ] Mothership build clean?
- [ ] Boot log verified: every expected `[SENS] Slot` line present, total count correct?
- [ ] Sync log verified: `present=0x....` mask includes the new capability bit, no false
      `sensor fault mask` lines?
- [ ] Verified on a **real sync window**, not just a config-mode/bench test — the two
      mothership receive paths are separate code (Trap 6). Check mothership
      `[SNAP-SPEC] ... extended=N/N` (or equivalent) during an actual RTC-alarm sync, not
      only during discovery/pairing/manual upload.
- [ ] Flash order planned (mothership first) and CSV-wipe backlog handled?

---

## The one-line lesson

> **Metadata is not a sensor.** When you catch yourself calling `commitSensorSlot()` for a
> value that describes *how* a measurement was taken rather than *what* was measured, stop —
> it belongs in the snapshot as an extra, not in the registry. And whenever you add channels,
> check `MAX_SENSORS` math, because the registry fails **silently** and takes down whichever
> sensor is unlucky enough to be registered next.
