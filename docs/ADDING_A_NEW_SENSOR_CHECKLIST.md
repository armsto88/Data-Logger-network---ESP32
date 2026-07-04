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

`MAX_SENSORS` in [node/firmware/v2/shared/sensors.h](../node/firmware/v2/shared/sensors.h)
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
2. **Sensor IDs** ([protocol.h](../node/firmware/v2/shared/protocol.h)) — add
   `SENSOR_ID_*` constants. **IDs are grouped by range** (1100=spectral, 2000=soil, etc.).
3. **`resolveSensorId()`** ([sensors.cpp](../node/firmware/v2/src/sensors/sensors.cpp)) —
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
- [ ] Flash order planned (mothership first) and CSV-wipe backlog handled?

---

## The one-line lesson

> **Metadata is not a sensor.** When you catch yourself calling `commitSensorSlot()` for a
> value that describes *how* a measurement was taken rather than *what* was measured, stop —
> it belongs in the snapshot as an extra, not in the registry. And whenever you add channels,
> check `MAX_SENSORS` math, because the registry fails **silently** and takes down whichever
> sensor is unlucky enough to be registered next.
