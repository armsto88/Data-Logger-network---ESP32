# Multi-Node Validation Summary — 2026-05-05

This note summarises the sensor, firmware, and logging validation work completed on 5 May 2026. It is intended as a manuscript handoff document for downstream writing and synthesis.

---

## 1. Manuscript-Ready Summary

On 5 May 2026, the sensor-node firmware and mothership logging pipeline were validated first on a single fully instrumented node and then on a three-node deployment test. The validated node configuration comprised an SHT4x air temperature/relative humidity sensor on PCA9548A mux channel 0, an AS7341/AS734x-family spectral sensor on mux channel 1, a dual-probe soil moisture/temperature path via ADS1015, and a battery voltage input. Firmware integration was updated so that the node correctly selected mux channels before sensor access, emitted the eight expected spectral bands into the shared snapshot structure, and propagated these values through the mothership into the SD-card CSV logger.

Single-node testing showed that the end-to-end path from sensor readout to queued snapshot, sync-window flush, mothership reception, and SD-card logging was operational. Logged rows contained valid battery, air temperature, humidity, spectral, and soil values with the expected `sensor_present` mask (`0x0137`). Subsequent three-node testing demonstrated concurrent operation over repeated wake cycles and a shared sync flush. All three nodes produced valid battery, air temperature, humidity, and soil records, and the mothership successfully logged a multi-node backlog during the sync window without transport failure. One node initially showed missing spectral output during the fleet test; this was treated as a local wiring issue rather than a firmware or logging defect, and the spectral path was subsequently corrected in hardware. Overall, the tests support the conclusion that the node firmware, ESP-NOW transport, queueing behaviour, sync scheduling, and mothership CSV logging are functioning as intended for multi-node deployment.

---

## 2. Test Conditions and Scope

### 2.1 Firmware / hardware scope validated

- Node firmware environment: `esp32wroom`
- Mothership firmware environment: `esp32s3`
- Wake cadence during deployment test: 1 min
- Auto-derived sync cadence: 18 min
- Logged wide-format schema: battery, air, 8 spectral bands, wind placeholders, dual soil channels, AUX placeholders, `sensor_present`, `quality_flags`, and `seq_num`

### 2.2 Sensor configuration under test

- SHT4x air temperature / relative humidity on mux channel 0
- AS7341/AS734x-family spectral sensor on mux channel 1
- ADS1015 soil path on root I2C bus
- Soil probe wiring used in production firmware:
  - `A0 = soil1 temperature`
  - `A1 = soil1 moisture`
  - `A2 = soil2 moisture`
  - `A3 = soil2 temperature`

---

## 3. Key Findings

1. The single-node integrated deployment path functioned correctly after firmware alignment.
2. The mothership received queued snapshots and wrote them to SD-card CSV with correct timestamps and node metadata.
3. Three-node sync-window flushing worked without evidence of a system-level queue or receive-path collapse.
4. Two nodes produced complete sensor groups (`sensor_present = 0x0137`) throughout the fleet test.
5. One node initially produced `sensor_present = 0x0133`, indicating absent spectral bands only; this was treated as a node-local wiring problem rather than a transport or firmware limitation.
6. Soil temperature values were stable and plausible across the test period. Soil moisture values were operationally useful for bring-up, but still represent provisional scaling rather than final manuscript-grade calibration.

---

## 4. Outcome Table

| Test stage | Platform / nodes | Primary objective | Outcome | Interpretation |
|---|---|---|---|---|
| Single-node integrated deploy | 1 node (`ENV_945DC4`) | Verify sensor capture -> queue -> sync flush -> mothership CSV | PASS | End-to-end logging pipeline validated with complete sensor set present |
| Single-node CSV check | 1 node (`ENV_945DC4`) | Confirm wide-format CSV rows contain expected channels and timestamps | PASS | Battery, air, spectral, and soil values logged correctly; `sensor_present = 0x0137` |
| Three-node wake/sync run | 3 nodes (`ENV_94DF54`, `ENV_945DC4`, `ENV_94E38C`) | Verify concurrent wake cycles and shared sync-window backlog flush | PASS with one local hardware issue | Multi-node scheduling, queueing, transport, and logging validated |
| Spectral validation during fleet run | Node-local comparison across 3 nodes | Check that all nodes emit 8 spectral columns | PASS for 2/3 nodes; one local wiring issue | Missing spectral values on one node were attributable to wiring, not a software-path failure |

---

## 5. Data Produced Table

The table below condenses the representative CSV output from the 5 May 2026 tests.

| Node | MAC | FW ID | Representative period | `sensor_present` | Air temp (deg C) | RH (%) | Spectral status | Soil1 moisture* | Soil1 temp (deg C) | Soil2 moisture* | Soil2 temp (deg C) | Summary |
|---|---|---|---|---|---:|---:|---|---:|---:|---:|---:|---|
| NODE 1 | `D4:E9:F4:94:DF:54` | `ENV_94DF54` | 21:46-22:04 | `0x0133` | 19.32-19.95 | 62.75-65.51 | Spectral absent during fleet run (`NaN` in all 8 columns); later attributed to wiring error | 0.24-2.76 | 19.33-19.62 | 2.76-3.36 | 19.23-19.71 | Air and soil channels valid; spectral path initially missing due to local hardware wiring |
| NODE 2 | `D4:E9:F4:94:5D:C4` | `ENV_945DC4` | 21:46-22:04 | `0x0137` | 19.14-19.39 | 64.66-65.49 | 8 spectral bands present; low but nonzero counts under test lighting | 2.28-2.68 | 19.04-19.42 | 2.40-2.76 | 18.90-19.23 | Complete sensor set logged correctly through repeated wake/sync cycles |
| NODE 3 | `D4:E9:F4:94:E3:8C` | `ENV_94E38C` | 21:47-22:04 | `0x0137` | 19.45-19.91 | 62.58-64.93 | 8 spectral bands present; low but nonzero counts under test lighting | 0.12-0.24 | 18.99-19.42 | 0.08-0.20 | 18.94-19.23 | Complete sensor set logged correctly through repeated wake/sync cycles |

\* Soil moisture columns should currently be treated as provisional operational outputs from the TH-A path rather than final calibrated volumetric water content.

---

## 6. Representative CSV Interpretation

- `ms_datetime` / `ms_sync_unix` represent mothership logging time, i.e. when the backlog was received and written to SD.
- `node_datetime` / `node_unix` represent the actual sampling time on the node.
- Repeated rows sharing the same `ms_datetime` but differing `node_datetime` are expected during sync-window backlog flushes.
- `0x0137` indicates the presence of battery, air temperature, air humidity, spectral group, soil1 group, and soil2 group.
- `0x0133` indicates the same set except the spectral group, which matched the node-local wiring issue observed on one unit.

---

## 7. Recommended Manuscript Framing

The tests on 5 May 2026 support three main claims suitable for manuscript text:

1. The integrated node firmware was able to acquire, queue, and transmit multi-sensor observations spanning air, spectral, and soil channels.
2. The mothership successfully synchronised with multiple nodes and consolidated queued records into a structured SD-card CSV log.
3. A temporary loss of spectral output on one node was traced to a wiring fault rather than a software or communications limitation, and therefore does not alter the overall conclusion that the architecture supports multi-node environmental sensing.

---

## 8. Cautions for Manuscript Use

- Soil moisture values should not yet be presented as final calibrated volumetric water content without additional scaling / calibration work.
- Wind and AUX channels were not active in these tests and should be described as placeholders or not-yet-validated channels.
- The fleet test validates operational behaviour over repeated cycles, but it should still be described as a controlled deployment-style bench/short-run validation rather than a long-duration field campaign.

---

## 9. Suggested In-Text Citations and APA References

If the manuscript needs explicit citations for the PCB design and fabrication platforms, the following wording can be used.

### 9.1 Example in-text citation wording

- Parenthetical style: PCB schematics and layouts were prepared in EasyEDA (EasyEDA, n.d.), and prototype boards were fabricated using JLCPCB's rapid PCB manufacturing service (JLCPCB, n.d.).
- Narrative style: PCB schematics and layouts were prepared in EasyEDA (n.d.), and the prototype boards were fabricated by JLCPCB (n.d.).

### 9.2 Reference list entries (APA 7th style)

EasyEDA. (n.d.). *Easy-to-use & free PCB design software*. Retrieved May 6, 2026, from https://easyeda.com/

JLCPCB. (n.d.). *JLCPCB*. Retrieved May 6, 2026, from https://jlcpcb.com/
