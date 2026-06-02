# Node V2 Bringup Results

| Field | Value |
|---|---|
| Board | Node V2 |
| Revision | V2 (Gerber 2026-05-20) |
| Date Started | 2026-06-02 |
| Operator | |

---

## 1. Bringup Sequence

| Step | Test | PlatformIO env | V2-specific changes | Status |
|------|------|----------------|---------------------|--------|
| 1 | Serial/boot smoke | `esp32wroom-serial-counter` | No V2 changes | ⬜ |
| 2 | I2C bus scan | `esp32wroom-i2c-scan` | SDA=18, SCL=19 unchanged | ⬜ |
| 3 | I2C mux + SHTC3 | `esp32wroom-i2c-mux-shtc3` | MUX_ADDR=0x71 unchanged | ⬜ |
| 4 | I2C mux power probe | `esp32wroom-i2c-mux-power-probe` | TX_PWM=25 still used for 22V hold | ⬜ |
| 5 | Battery ADC | `esp32wroom-battery-io35` | V2 divider is 47k/47k, may differ from V1. Calibrate against DMM. | ⬜ |
| 6 | PWR_HOLD latch | `esp32wroom-pwrhold-gate` | PWR_HOLD=GPIO23 unchanged. V2 adds RUN/KILL switch gating SYS_GATE_CTRL. | ⬜ |
| 7 | TX PWM gate (V1-style) | `esp32wroom-txpwm-gate` | Partial only. V2 splits TX control. This only validates burst PWM path. | ⬜ |
| 8 | TX 22V enable (NEW) | `esp32wroom-tx-22v-enable` | NEW sketch needed. Toggles TX_22V_EN_N (GPIO5, active-low), verifies EN_22 inverter and boost control. | ⬜ |
| 9 | RX enable gate (NEW) | `esp32wroom-rx-enable-gate` | NEW sketch needed. Toggles RX_EN_N (GPIO4, active-low), verifies AND-gate path. | ⬜ |
| 10 | ADS1015 analog | `esp32wroom-ads1015-analog` | No V2 changes | ⬜ |
| 11 | ADS1015 soil | `esp32wroom-ads1015-soil` | No V2 changes | ⬜ |
| 12 | SHT40 + AS7343 via mux | `esp32wroom-sht40-as7343-mux` | No V2 changes | ⬜ |
| 13 | DS3231 RTC alarm | `esp32wroom-ds3231-alarm-10s` | No V2 changes | ⬜ |
| 14 | RUN/KILL switch (NEW) | `esp32wroom-run-kill-switch` | NEW sketch needed. Monitors VSYS while toggling RUN/KILL. | ⬜ |
| 15 | Off-state leakage (NEW) | `esp32wroom-off-current` | NEW sketch or DMM procedure. Measure quiescent current in KILL state. | ⬜ |
| 16 | Ultrasonic TOF pipeline | `esp32wroom-ultrasonic-first-test` | MUST UPDATE for V2 pins: add TX_22V_EN_N=5, change RX_EN to RX_EN_N=4 (active-low), add AND-gate awareness. | ⬜ |
| 17 | WiFi integrity | `esp32wroom-wifi-integrity` | No V2 changes | ⬜ |
| 18 | ESP-NOW range TX | `esp32wroom-espnow-range-tx` | No V2 changes | ⬜ |
| 19 | ESP-NOW range RX | `esp32wrover-espnow-range-rx` | No V2 changes | ⬜ |
| 20 | Mock mothership sync | `esp32s3-mock-mothership-sync` | No V2 changes | ⬜ |

---

## 2. Test Point Reference

| Test Point | Expected Voltage / Signal | Condition | Notes |
|------------|---------------------------|-----------|-------|
| RAW_BAT | 3.0–4.2 V | Always | Direct battery input |
| VSYS | ~3.0–4.2 V | RUN state | 0 V in KILL state |
| 3V3_SYS | 3.3 V | When VSYS present | LDO output |
| 5V_SYS | 5.0 V | When VSYS present | Boost output |
| 22V_SYS | ~22 V | Only when TX_22V_EN_N is LOW (EN_22 HIGH) | MT3608 boost output |
| PWR_HOLD | HIGH when latched | GPIO23 | Latch control |
| TX_BURST_PWM | PWM burst | GPIO25 | Ultrasonic TX burst |
| TX_22V_EN_N | LOW = boost enabled | GPIO5, weak pull-up | Active-low enable for 22 V boost |
| EN_22 | Inverted from TX_22V_EN_N | MT3608 enable pin | Inverter output |
| RX_EN_N | LOW = mux enabled | GPIO4, weak pull-up | Active-low enable for RX path |
| RX_WINDOW_EN | Inverted from RX_EN_N | AND gate input | Inverter output |
| RX_IN | DC bias near VREF/2 | Hot-side bias via R174=1 MΩ | AC-coupled ultrasonic RX |
| RX_AMP | Amplified RX signal | TLV9062 output on 3V3_SYS | Op-amp output |
| VREF | ~1.65 V | 47 kΩ/47 kΩ from 3V3_SYS | Always when 3V3_SYS present |
| COMP_RAW | Comparator output | When RX_EN_N LOW | High-speed comparator |
| TOF_EDGE | COMP_RAW AND RX_WINDOW_EN | GPIO34 | AND-gated edge output |

---

## 3. Step-by-Step Results

### Step 1 — Serial/boot smoke

| Field | Value |
|---|---|
| Date | |
| Board serial | |
| Flash method | |

| Check | Expected | Actual | Pass? |
|-------|----------|--------|-------|
| Flash succeeds | Firmware flashes without error | | ⬜ |
| Boot completes | Serial output shows boot banner | | ⬜ |
| No boot loop | Device does not reboot repeatedly | | ⬜ |
| Baud 115200 readable | Console output is legible at 115200 baud | | ⬜ |

**Notes:**

---

### Step 2 — I2C bus scan

| Field | Value |
|---|---|
| Date | |
| Board serial | |
| Flash method | |

| Check | Expected | Actual | Pass? |
|-------|----------|--------|-------|
| I2C init | SDA=18, SCL=19 initialised | | ⬜ |
| Devices found | Expected addresses respond (0x44 SHTC3, 0x71 mux, 0x68 RTC, 0x49 ADS1015) | | ⬜ |
| No spurious addresses | Only expected devices appear | | ⬜ |

**Notes:**

---

### Step 3 — I2C mux + SHTC3

| Field | Value |
|---|---|
| Date | |
| Board serial | |
| Flash method | |

| Check | Expected | Actual | Pass? |
|-------|----------|--------|-------|
| Mux address | 0x71 responds | | ⬜ |
| Channel select | Mux channel can be selected | | ⬜ |
| SHTC3 read | Temperature and humidity values in reasonable range | | ⬜ |

**Notes:**

---

### Step 4 — I2C mux power probe

| Field | Value |
|---|---|
| Date | |
| Board serial | |
| Flash method | |

| Check | Expected | Actual | Pass? |
|-------|----------|--------|-------|
| TX_PWM=25 drives 22V | GPIO25 PWM enables boost path | | ⬜ |
| 22V_SYS measurable | ~22 V on 22V_SYS test point | | ⬜ |
| Mux power channel | ADS1015 reads boost voltage via mux | | ⬜ |

**Notes:**

---

### Step 5 — Battery ADC

| Field | Value |
|---|---|
| Date | |
| Board serial | |
| Flash method | |

| Check | Expected | Actual | Pass? |
|-------|----------|--------|-------|
| ADC reads non-zero | GPIO35 returns a voltage > 0 | | ⬜ |
| ADC tracks DMM | ADC-reported voltage matches DMM within ±5% | | ⬜ |
| Divider ratio correct | 47 kΩ/47 kΩ gives ~0.5 scaling | | ⬜ |

| Measurement | Value |
|-------------|-------|
| Measured RAW_BAT (DMM) | |
| ADC-reported voltage | |
| Calibrated BAT_DIVIDER_SCALE | |

**Notes:** V2 divider is 47 kΩ/47 kΩ. This may differ from V1. Calibrate against DMM and record the calibrated BAT_DIVIDER_SCALE above.

---

### Step 6 — PWR_HOLD latch

| Field | Value |
|---|---|
| Date | |
| Board serial | |
| Flash method | |

| Check | Expected | Actual | Pass? |
|-------|----------|--------|-------|
| PWR_HOLD=GPIO23 latches | Setting GPIO23 HIGH holds VSYS on | | ⬜ |
| Release drops VSYS | Releasing GPIO23 allows VSYS to drop | | ⬜ |
| RUN/KILL switch gates SYS_GATE_CTRL | V2: RUN/KILL switch affects SYS_GATE_CTRL path | | ⬜ |

**Notes:** V2 adds RUN/KILL switch gating SYS_GATE_CTRL. Verify that the switch state interacts correctly with the PWR_HOLD latch.

---

### Step 7 — TX PWM gate (V1-style)

| Field | Value |
|---|---|
| Date | |
| Board serial | |
| Flash method | |

| Check | Expected | Actual | Pass? |
|-------|----------|--------|-------|
| TX_BURST_PWM active | GPIO25 outputs PWM burst | | ⬜ |
| Gate passes signal | PWM reaches ultrasonic TX transducer | | ⬜ |

**Notes:** Partial test only. V2 splits TX control between burst PWM and 22 V enable. This step only validates the burst PWM path.

---

### Step 8 — TX 22V enable (NEW)

| Field | Value |
|---|---|
| Date | |
| Board serial | |
| Flash method | |

| Check | Expected | Actual | Pass? |
|-------|----------|--------|-------|
| TX_22V_EN_N=GPIO5 toggles | GPIO5 can be driven LOW and HIGH | | ⬜ |
| EN_22 inverts correctly | EN_22 is HIGH when TX_22V_EN_N is LOW | | ⬜ |
| 22V_SYS enables | 22V_SYS rises to ~22 V when TX_22V_EN_N is LOW | | ⬜ |
| 22V_SYS disables | 22V_SYS drops when TX_22V_EN_N is HIGH | | ⬜ |

**Notes:** Sketch not yet created. This test toggles TX_22V_EN_N (GPIO5, active-low) and verifies the EN_22 inverter and boost converter control.

---

### Step 9 — RX enable gate (NEW)

| Field | Value |
|---|---|
| Date | |
| Board serial | |
| Flash method | |

| Check | Expected | Actual | Pass? |
|-------|----------|--------|-------|
| RX_EN_N=GPIO4 toggles | GPIO4 can be driven LOW and HIGH | | ⬜ |
| RX_WINDOW_EN inverts | RX_WINDOW_EN is HIGH when RX_EN_N is LOW | | ⬜ |
| AND-gate passes signal | TOF_EDGE (GPIO34) reflects COMP_RAW AND RX_WINDOW_EN | | ⬜ |
| AND-gate blocks when disabled | TOF_EDGE is LOW when RX_EN_N is HIGH (RX_WINDOW_EN LOW) | | ⬜ |

**Notes:** Sketch not yet created. This test toggles RX_EN_N (GPIO4, active-low) and verifies the AND-gate path to TOF_EDGE.

---

### Step 10 — ADS1015 analog

| Field | Value |
|---|---|
| Date | |
| Board serial | |
| Flash method | |

| Check | Expected | Actual | Pass? |
|-------|----------|--------|-------|
| ADS1015 responds | Address 0x49 on I2C | | ⬜ |
| Analog reads valid | ADC values in expected range | | ⬜ |

**Notes:**

---

### Step 11 — ADS1015 soil

| Field | Value |
|---|---|
| Date | |
| Board serial | |
| Flash method | |

| Check | Expected | Actual | Pass? |
|-------|----------|--------|-------|
| Soil probe reads | Frequency shift or capacitance change detectable | | ⬜ |
| Mux channel correct | ADS1015 accessible via correct mux channel | | ⬜ |

**Notes:**

---

### Step 12 — SHT40 + AS7343 via mux

| Field | Value |
|---|---|
| Date | |
| Board serial | |
| Flash method | |

| Check | Expected | Actual | Pass? |
|-------|----------|--------|-------|
| SHT40 reads | Temperature and humidity in reasonable range | | ⬜ |
| AS7343 reads | Spectral channels return data | | ⬜ |
| Mux routing correct | Both sensors accessible on their respective channels | | ⬜ |

**Notes:**

---

### Step 13 — DS3231 RTC alarm

| Field | Value |
|---|---|
| Date | |
| Board serial | |
| Flash method | |

| Check | Expected | Actual | Pass? |
|-------|----------|--------|-------|
| RTC responds | DS3231 at 0x68 | | ⬜ |
| Time set/read | Time can be set and read back | | ⬜ |
| Alarm fires | 10 s alarm triggers interrupt | | ⬜ |

**Notes:**

---

### Step 14 — RUN/KILL switch (NEW)

| Field | Value |
|---|---|
| Date | |
| Board serial | |
| Flash method | |

| Check | Expected | Actual | Pass? |
|-------|----------|--------|-------|
| RUN position — VSYS present | VSYS reads battery voltage in RUN position | | ⬜ |
| KILL position — VSYS absent | VSYS drops to 0 V in KILL position | | ⬜ |
| Switch does not interfere with PWR_HOLD | PWR_HOLD latch works independently of switch in RUN position | | ⬜ |

**Notes:** Sketch not yet created. This test monitors VSYS while toggling the RUN/KILL switch.

---

### Step 15 — Off-state leakage (NEW)

| Field | Value |
|---|---|
| Date | |
| Board serial | |
| Flash method | |

| Check | Expected | Actual | Pass? |
|-------|----------|--------|-------|
| KILL current < 20 µA | Quiescent current in KILL state is below threshold | | ⬜ |
| Sleep current < 10 µA | Deep-sleep current is below threshold | | ⬜ |

| Measurement | Value |
|-------------|-------|
| Measured KILL current (µA) | |
| Measured sleep current (µA) | |

**Notes:** Sketch not yet created. Minimum is a DMM procedure measuring quiescent current in KILL state. Record both KILL-state and deep-sleep currents above.

---

### Step 16 — Ultrasonic TOF pipeline

| Field | Value |
|---|---|
| Date | |
| Board serial | |
| Flash method | |

| Check | Expected | Actual | Pass? |
|-------|----------|--------|-------|
| TX_22V_EN_N=5 controls boost | 22 V boost enables/disables correctly | | ⬜ |
| RX_EN_N=4 gates RX path | AND-gate allows/blocks TOF_EDGE correctly | | ⬜ |
| TX burst fires | Ultrasonic burst transmitted | | ⬜ |
| RX echo detected | TOF_EDGE captures echo edge | | ⬜ |
| TOF measurement valid | Time-of-flight value in expected range | | ⬜ |

**Notes:** MUST UPDATE sketch for V2 pins: add TX_22V_EN_N=5, change RX_EN to RX_EN_N=4 (active-low), add AND-gate awareness.

---

### Step 17 — WiFi integrity

| Field | Value |
|---|---|
| Date | |
| Board serial | |
| Flash method | |

| Check | Expected | Actual | Pass? |
|-------|----------|--------|-------|
| WiFi connects | Station mode connects to test AP | | ⬜ |
| RSSI reasonable | Signal strength > -70 dBm at close range | | ⬜ |
| Data transfer | TCP/UDP payload round-trip succeeds | | ⬜ |

**Notes:**

---

### Step 18 — ESP-NOW range TX

| Field | Value |
|---|---|
| Date | |
| Board serial | |
| Flash method | |

| Check | Expected | Actual | Pass? |
|-------|----------|--------|-------|
| ESP-NOW init | ESP-NOW initialised successfully | | ⬜ |
| Peer registered | Receiver peer registered | | ⬜ |
| Packets sent | TX count increments | | ⬜ |

**Notes:**

---

### Step 19 — ESP-NOW range RX

| Field | Value |
|---|---|
| Date | |
| Board serial | |
| Flash method | |

| Check | Expected | Actual | Pass? |
|-------|----------|--------|-------|
| ESP-NOW init | ESP-NOW initialised successfully | | ⬜ |
| Packets received | RX count increments | | ⬜ |
| RSSI logged | Signal strength recorded per packet | | ⬜ |

**Notes:**

---

### Step 20 — Mock mothership sync

| Field | Value |
|---|---|
| Date | |
| Board serial | |
| Flash method | |

| Check | Expected | Actual | Pass? |
|-------|----------|--------|-------|
| ESP-NOW handshake | Node and mothership exchange messages | | ⬜ |
| Data payload received | Mothership receives sensor data payload | | ⬜ |
| Sync completes | Full sync cycle finishes without error | | ⬜ |

**Notes:**

---

## 4. New Sketches Needed

| Sketch | Purpose | Pins | Priority |
|--------|---------|------|----------|
| `bringup_tx_22v_enable.cpp` | TX_22V_EN_N toggle, verify EN_22 inverter | TX_22V_EN_N=5, EN_22 | High |
| `bringup_rx_enable_gate.cpp` | RX_EN_N toggle, verify AND-gate | RX_EN_N=4, RX_WINDOW_EN, TOF_EDGE=34 | High |
| `bringup_run_kill_switch.cpp` | RUN/KILL toggle, monitor VSYS | PWR_HOLD=23, VSYS | High |
| `bringup_off_current.cpp` | Quiescent current in KILL state | BAT_ADC=35, VSYS | Medium |
| `bringup_analog_chain_dc.cpp` | VREF, RX_AMP, COMP_RAW DC levels | VREF, RX_AMP, COMP_RAW | Medium |
| `bringup_reed_wind.cpp` | Reed-switch edge count on GPIO4 (shares RX_EN_N via solder jumper) | REED_SIG=GPIO4 | Low |

---

## 5. V2 Pin Mapping Summary

| Pin / Constant | V1 | V2 | Change | Notes |
|----------------|----|----|--------|-------|
| SDA | 18 | 18 | Unchanged | |
| SCL | 19 | 19 | Unchanged | |
| PWR_HOLD | 23 | 23 | Unchanged | |
| BAT_ADC | 35 | 35 | Unchanged | |
| TX_BURST_PWM | 25 | 25 | Renamed from TX_PWM | |
| TX_22V_EN_N | — | 5 | NEW | Active-low, weak pull-up, inverter → EN_22 |
| RX_EN_N | — | 4 | NEW (was RX_EN active-high) | Active-low, weak pull-up, inverter → RX_WINDOW_EN |
| TOF_EDGE | 34 | 34 | Unchanged | Now AND-gated with RX_WINDOW_EN |
| MUX_ADDR | 0x71 | 0x71 | Unchanged | Conflicts with protocol.h default 0x70 |
| RTC_INT | 4 (GPIO) | NFET → power gate | **Changed** | V2: RTC INT/SQW drives NFET gate directly, not a GPIO. No ESP32 pin used. |
| BAT_DIVIDER_SCALE | 2.0 (bringup) / 3.58 (prod) | TBD | Calibrate | V2 uses 47 kΩ/47 kΩ divider |

---

## 6. Open Issues

1. **RTC_INT is hardware-only on V2** — The DS3231 INT/SQW pin drives an NFET gate controlling the main power gate directly. It is NOT connected to any ESP32 GPIO. The `RTC_INT_PIN=4` define in `protocol.h` and `sensor-node/platformio.ini` is a V1 remnant that needs updating for V2. The DS3231 alarm bringup (Step 13) should verify the NFET/power-gate path instead of polling a GPIO interrupt.

2. **BAT_DIVIDER_SCALE** — V2 uses a 47 kΩ/47 kΩ divider (ratio ≈ 0.5). The bringup sketch uses 2.0 and production firmware uses 3.58. Calibrate against DMM measurement and record the correct scale factor in Step 5.

3. **MUX_ADDR** — `protocol.h` defaults to 0x70, but all bringup sketches and the V2 schematic use 0x71. Confirm the V2 schematic address and update `protocol.h` or bringup sketches as needed.

4. **Ultrasonic bringup pin updates** — `bringup_ultrasonic_first_test.cpp` needs V2 pin mapping: add TX_22V_EN_N=5, change RX_EN to RX_EN_N=4 (active-low), add AND-gate awareness for RX_WINDOW_EN.

5. **Off-state leakage** — No bringup sketch exists yet. A DMM procedure is the minimum requirement. A sketch that logs BAT_ADC while entering deep sleep would be useful but is not blocking.

6. **V2 bring-up sheet** — Checklist §7 calls for a formal test-point-to-expected-voltage mapping. This document partially addresses it with the Test Point Reference table in Section 2. A complete mapping should be validated against the V2 schematic.