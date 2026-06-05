# Sensor & CSV Bring-Up Notes

Reference for remaining sensor validation, CSV schema finalisation, and queue optimisation.  
**Status:** In progress — blocked on sensor hardware delivery.

---

## 1. Sensor Bring-Up Status

| Sensor | Interface | Mux Ch | Status | Bringup Env |
|---|---|---|---|---|
| SHT40 (air temp/RH) | I2C @ 0x44 | PCA9548A ch 0 | ✅ PASS (2026-04-24) | `esp32wroom-sht40-as7343-mux` |
| AS7343 (spectral/PAR/NDVI) | I2C @ 0x39 | PCA9548A ch 1 | ❌ Physically damaged — replacement on order | `esp32wroom-sht40-as7343-mux` |
| ADS1015IDGSR (ADC) | I2C @ 0x48 | None (direct) | ⏳ IC confirmed present; soil sensor on order | `esp32wroom-ads1015-soil` |
| CWT TH-A (soil moisture + temp) | Analog 0–5V → ADS1015 | A0 (moisture), A2 (temp) | ⏳ Sensor on order | `esp32wroom-ads1015-soil` |

### 1.1 ADS1015 Input Voltage — Action Required Before Flashing

The CWT TH-A outputs 0–5 V. Before connecting to the ADS1015, confirm ADS VDD rail voltage:

- **If ADS VDD = 5 V:** Connect directly. Use `GAIN_TWOTHIRDS` (±6.144 V FS). No divider needed.
- **If ADS VDD = 3.3 V:** 5 V on ADS inputs exceeds abs max (VDD + 0.3 V = 3.6 V). A resistor divider (e.g. 10k/10k → multiply mV reading by 2) is required before connecting. Update `SOIL_ADC_INPUT_TO_SENSOR_VOLT_GAIN` in `sensors_soil_ads_calib.h` accordingly.

Check the schematic: ADS VDD pin net name and upstream regulator.

### 1.2 AS7343 Bringup Notes (for when replacement arrives)

- Library: SparkFun AS7343 from GitHub (`sfDevAS7343` base class)
- Correct API — **not** the generic Adafruit AS7341 API:
  - `getChannelData(sfe_as7343_channel_t channel)` — NOT `getChannel()`
  - `setAgain(AGAIN_16)` — NOT `setGain()` or `AS7343_GAIN_16X`
  - `setAutoSmux(AUTOSMUX_18_CHANNELS)`
- Channel enums: `CH_PURPLE_F1_405NM`, `CH_DARK_BLUE_F2_425NM`, `CH_BLUE_FZ_450NM`, `CH_LIGHT_BLUE_F3_475NM`, `CH_BLUE_F4_515NM`, `CH_GREEN_F5_550NM`, `CH_GREEN_FY_555NM`, `CH_ORANGE_FXL_600NM`, `CH_BROWN_F6_640NM`, `CH_RED_F7_690NM`, `CH_DARK_RED_F8_745NM`, `CH_NIR_855NM`
- PAR proxy = equal-weight sum of 10 channels 400–700 nm (F1+F2+FZ+F3+F4+F5+FY+FXL+F6+F7); requires calibration coefficient K vs reference sensor
- NDVI = `(NIR_855 - F7_690) / (NIR_855 + F7_690)`
- Bringup sketch (`bringup_sht40_as7343_mux.cpp`) already handles AS7343 init failure gracefully — `g_as7343_ok=false` skips reads; SHT40 path is fully independent

---

## 2. CSV Schema — Pending Finalisation

### 2.1 Current Schema (mothership espnow_manager.cpp)

```
timestamp,node_id,node_name,mac,event_type,sensor_type,value,meta
```

Where `meta` currently contains: `FW_ID=...;SENSOR_ID=...;SENSOR_TYPE=...;QF=0x...;NODE_TS=...`

### 2.2 Planned Schema

```
timestamp,node_id,node_name,mac,node_ts,sensor_type,value,unit,meta
```

Changes vs current:
- `event_type` removed — always `"SENSOR"`, redundant
- `node_ts` promoted from buried in `meta` to first-class column
- `unit` added — derived on mothership from `sensorType` (no protocol change, no node reflash)
- `meta` slimmed to: `QF=0x...;SENSOR_ID=...`

### 2.3 Unit Lookup Table (mothership-side derivation)

| sensorType | unit |
|---|---|
| `AIR_TEMP` | `C` |
| `HUMIDITY` | `%` |
| `SOIL_VWC` | `%` |
| `SOIL_TEMP` | `C` |
| `PAR` | `umol/m2/s` |
| `NDVI` | `` (dimensionless) |
| `WIND_SPEED` | `m/s` |
| `WIND_DIR` | `deg` |
| `AUX` | `` |

### 2.4 Rationale

- `unit` is not in the `sensor_data_message_t` protocol struct. Adding it to the struct requires a matched reflash of all nodes + mothership (binary struct compatibility). Deriving it on the mothership from `sensorType` avoids this entirely.
- `timestamp` (mothership wall-clock) retained alongside `node_ts` — if the node clock is wrong, mothership time is the only absolute time reference.
- Old test data is disposable; clean cut-over when schema change is implemented.

**Do this change after all sensors are confirmed** — avoids a second schema break when soil/AS7343 channels are added.

---

## 3. Queue and Record Optimisation — Deferred

**Trigger:** Implement after all sensor channels are confirmed and validated.

### 3.1 Current State

- Queue: 24 records, flat NVS-backed FIFO, DROP_OLDEST, `QF_DROPPED=0x0001`
- One `sensor_data_message_t` record per sensor channel per wake cycle
- Struct size includes char arrays (`nodeId[16]`, `sensorType[16]`, `sensorLabel[24]`) — high overhead per reading

### 3.2 Optimisation Options (evaluate when sensor set is fixed)

**Option A — Pack all channels per wake cycle into one queue record**
- Replace N records per wake (one per sensor) with a single struct containing all channel values for that cycle
- Multiplies effective queue depth by N (number of sensors per node)
- Requires new queue record struct and matched changes to flush + CSV row builder

**Option B — Replace char arrays with `uint16_t sensorId` + mothership lookup**
- `sensorType[16]` and `sensorLabel[24]` → single `uint16_t sensorId`
- Mothership maps `sensorId` to type, label, and unit string
- Saves 38 bytes per record; enables ~30% more records in same NVS budget
- Stable `SENSOR_ID_*` constants already defined in `protocol.h`

**Option C — Scaled integer values for bounded-range channels**
- `float value` (4 bytes) → `int16_t value` (2 bytes) with fixed scale factor per channel
- RH (%): multiply by 100, store as `int16_t` (0–10000 → 0.00–100.00%)
- Soil VWC (%): same
- Air temp (°C): multiply by 100, store as `int16_t` (−3276.8 to +3276.7 °C range, more than sufficient)
- PAR: scale to fit `uint16_t` (0–65535 µmol/m²/s covers full sunlight range)
- Saves 2 bytes per reading; meaningful when combined with Option B

**Recommended approach:** A + B first. C only if NVS budget is still tight after A+B.

### 3.3 NVS Budget Reference

ESP32 NVS partition default: 24576 bytes usable (6 pages × 4096 bytes, minus overhead).  
Current record size (`sensor_data_message_t`): ~70 bytes + NVS key overhead ≈ 90 bytes/record.  
At 24 records × N sensors: 24 records × 5 sensors × 90 bytes = ~10.8 kB — approaching limit.  
With Option A+B (single packed record ~30 bytes + overhead): ~24 × 50 bytes = 1.2 kB — comfortable headroom.

---

## 4. Sensor Order Checklist

When hardware arrives, complete in order:

- [ ] Confirm ADS1015 VDD rail voltage (3.3 V or 5 V) — determines whether voltage divider needed on CWT TH-A outputs
- [ ] Flash `esp32wroom-ads1015-soil`, connect CWT TH-A, confirm moisture and temperature readings
- [ ] Replace AS7343, flash `esp32wroom-sht40-as7343-mux`, confirm all spectral channels and PAR proxy
- [ ] Log representative readings for each channel — basis for unit lookup table and scaling factors
- [ ] Implement CSV schema change (Section 2.2)
- [ ] Implement queue/record optimisation (Section 3.2)
- [ ] Integrate all sensors into main `sensors.cpp` / `soil_moist_temp.cpp` / `sensors_par_as7343.cpp` backends
- [ ] End-to-end test: deploy node → data wake → sync → verify all channels appear in SD CSV with correct units
