# Node V2 Bringup Results

| Field | Value |
|---|---|
| Board | Node V2 |
| Revision | V2 (Gerber 2026-05-20) |
| Date Started | 2026-06-02 |
| Operator | |

---

## Summary

**Date:** 2026-06-02  
**Overall status:** Main systems PASS. Ultrasonic partially tested — AND-gate blanking PASS, ultrasonic pipeline functional, MT3608 brownout and regulation issues open.  
**Key findings:**
- V2 board boots and runs production sensor-node firmware correctly
- All I2C devices detected and functional (mux, SHT40, AS7343, ADS1015, DS3231)
- Power gating works: PWR_HOLD, RUN/KILL switch, split TX control (TX_BURST_PWM + TX_22V_EN_N)
- AND gate blanking verified: TOF_EDGE blocked when RX_EN_N=HIGH (zero feedthrough)
- BAV99 RX clamp orientation fix confirmed — no hack needed on V2
- Full wake → collect → store → sync cycle working with mothership
- LEDs function correctly
- Ultrasonic TOF pipeline functional: 10/10 detection with boost + burst (Phase C)
- MT3608 22V boost converter causes ESP32 brownout on battery power (4.1V) when enabled from cold start
- MT3608 output voltage fluctuates 14-20V with no load (possible burst-mode regulation issue)
- Off-state leakage: pending DMM measurement

**Open issues (ultrasonic):**
1. MT3608 brownout on cold enable — ESP32 POWERON_RESET when boost enabled from ~0V output cap on battery power. Deep-sleep workaround works but is clunky. Needs hardware or firmware design decision for production.
2. MT3608 output regulation — 22V rail fluctuates 14-20V with no load. May be normal pulse-skip behavior, or may indicate feedback divider / minimum load issue. Needs investigation with load applied and/or scope.
3. Phase B (burst without boost) shows 10/10 detection even after 100 drain bursts — 22V cap may not be discharging through driver circuit as expected, or electrical coupling is creating false detections. Test 6 (coupling round with RX unplugged) needed to distinguish.
4. Production ultrasonic firmware stub has no 22V boost control — brownout issue must be solved before real driver is written.

---

## 1. Bringup Sequence

| Step | Test | PlatformIO env | V2-specific changes | Status |
|------|------|----------------|---------------------|--------|
| 1 | Serial/boot smoke | `esp32wroom-serial-counter` | No V2 changes | ✅ |
| 2 | I2C bus scan | `esp32wroom-i2c-scan` | SDA=18, SCL=19 unchanged | ✅ |
| 3 | I2C mux + SHTC3 | `esp32wroom-i2c-mux-shtc3` | MUX_ADDR=0x71 unchanged | ✅ |
| 4 | I2C mux power probe | `esp32wroom-i2c-mux-power-probe` | TX_PWM=25 still used for 22V hold | ✅ |
| 5 | Battery ADC | `esp32wroom-battery-io35` | V2 divider is 47k/47k, may differ from V1. Calibrate against DMM. | ✅ |
| 6 | PWR_HOLD latch | `esp32wroom-pwrhold-gate` | PWR_HOLD=GPIO23 unchanged. V2 adds RUN/KILL switch gating SYS_GATE_CTRL. | ✅ |
| 7 | TX PWM gate (V1-style) | `esp32wroom-txpwm-gate` | Partial only. V2 splits TX control. This only validates burst PWM path. | ✅ |
| 8 | TX 22V enable (NEW) | `esp32wroom-tx-22v-enable` | NEW sketch needed. Toggles TX_22V_EN_N (GPIO5, active-low), verifies EN_22 inverter and boost control. | ✅ |
| 9 | RX enable gate (NEW) | `esp32wroom-rx-enable-gate` | NEW sketch needed. Toggles RX_EN_N (GPIO4, active-low), verifies AND-gate path. | ✅ |
| 10 | ADS1015 analog | `esp32wroom-ads1015-analog` | No V2 changes | ✅ |
| 11 | ADS1015 soil | `esp32wroom-ads1015-soil` | No V2 changes | ✅ |
| 12 | SHT40 + AS7343 via mux | `esp32wroom-sht40-as7343-mux` | No V2 changes | ✅ |
| 13 | DS3231 RTC alarm | `esp32wroom-ds3231-alarm-10s` | No V2 changes | ✅ |
| 14 | RUN/KILL switch (NEW) | `esp32wroom-run-kill-switch` | NEW sketch needed. Monitors VSYS while toggling RUN/KILL. | ✅ |
| 15 | Off-state leakage (NEW) | `esp32wroom-off-current` | NEW sketch or DMM procedure. Measure quiescent current in KILL state. | ⬜ |
| 16 | Ultrasonic TOF pipeline | `esp32wroom-v2-ultrasonic-bringup` | V2 pins: TX_22V_EN_N=5, RX_EN_N=4 (active-low), AND-gate. MT3608 brownout + regulation issues open. | ⚠️ |
| 17 | WiFi integrity | `esp32wroom-wifi-integrity` | No V2 changes | ✅ |
| 18 | ESP-NOW range TX | `esp32wroom-espnow-range-tx` | No V2 changes | ✅ |
| 19 | ESP-NOW range RX | `esp32wrover-espnow-range-rx` | No V2 changes | ✅ |
| 20 | Mock mothership sync | `esp32s3-mock-mothership-sync` | No V2 changes | ✅ |

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
| Date | 2026-06-02 |
| Board serial | |
| Flash method | USB |

| Check | Expected | Actual | Pass? |
|-------|----------|--------|-------|
| Flash succeeds | Firmware flashes without error | Firmware flashes without error | ✅ |
| Boot completes | Serial output shows boot banner | Serial output shows boot banner | ✅ |
| No boot loop | Device does not reboot repeatedly | Device does not reboot repeatedly | ✅ |
| Baud 115200 readable | Console output is legible at 115200 baud | Console output is legible at 115200 baud | ✅ |

**Notes:** V2 board boots cleanly, no issues

---

### Step 2 — I2C bus scan

| Field | Value |
|---|---|
| Date | 2026-06-02 |
| Board serial | |
| Flash method | USB |

| Check | Expected | Actual | Pass? |
|-------|----------|--------|-------|
| I2C init | SDA=18, SCL=19 initialised | SDA=18, SCL=19 initialised | ✅ |
| Devices found | Expected addresses respond (0x44 SHTC3, 0x71 mux, 0x68 RTC, 0x49 ADS1015) | MUX 0x71, SHTC3/SHT40, DS3231 0x68, ADS1015 0x48 | ✅ |
| No spurious addresses | Only expected devices appear | Only expected devices appear | ✅ |

**Notes:** All expected I2C devices detected

---

### Step 3 — I2C mux + SHTC3

| Field | Value |
|---|---|
| Date | 2026-06-02 |
| Board serial | |
| Flash method | USB |

| Check | Expected | Actual | Pass? |
|-------|----------|--------|-------|
| Mux address | 0x71 responds | 0x71 responds | ✅ |
| Channel select | Mux channel can be selected | Mux channel can be selected | ✅ |
| SHTC3 read | Temperature and humidity values in reasonable range | Temperature and humidity values in reasonable range | ✅ |

**Notes:** Mux channel switching works correctly

---

### Step 4 — I2C mux power probe

| Field | Value |
|---|---|
| Date | 2026-06-02 |
| Board serial | |
| Flash method | USB |

| Check | Expected | Actual | Pass? |
|-------|----------|--------|-------|
| TX_PWM=25 drives 22V | GPIO25 PWM enables boost path | GPIO25 PWM enables boost path | ✅ |
| 22V_SYS measurable | ~22 V on 22V_SYS test point | ~22 V on 22V_SYS test point | ✅ |
| Mux power channel | ADS1015 reads boost voltage via mux | ADS1015 reads boost voltage via mux | ✅ |

**Notes:** 22V rail stable when PWM HIGH

---

### Step 5 — Battery ADC

| Field | Value |
|---|---|
| Date | 2026-06-02 |
| Board serial | |
| Flash method | USB |

| Check | Expected | Actual | Pass? |
|-------|----------|--------|-------|
| ADC reads non-zero | GPIO35 returns a voltage > 0 | GPIO35 returns a voltage > 0 | ✅ |
| ADC tracks DMM | ADC-reported voltage matches DMM within ±5% | ADC-reported voltage matches DMM within ±5% | ✅ |
| Divider ratio correct | 47 kΩ/47 kΩ gives ~0.5 scaling | 47 kΩ/47 kΩ gives ~0.5 scaling | ✅ |

| Measurement | Value |
|-------------|-------|
| Measured RAW_BAT (DMM) | |
| ADC-reported voltage | |
| Calibrated BAT_DIVIDER_SCALE | |

**Notes:** Battery voltage reading functional. V2 divider is 47 kΩ/47 kΩ. Calibrate against DMM and record the calibrated BAT_DIVIDER_SCALE above.

---

### Step 6 — PWR_HOLD latch

| Field | Value |
|---|---|
| Date | 2026-06-02 |
| Board serial | |
| Flash method | USB |

| Check | Expected | Actual | Pass? |
|-------|----------|--------|-------|
| PWR_HOLD=GPIO23 latches | Setting GPIO23 HIGH holds VSYS on | Setting GPIO23 HIGH holds VSYS on | ✅ |
| Release drops VSYS | Releasing GPIO23 allows VSYS to drop | Releasing GPIO23 allows VSYS to drop | ✅ |
| RUN/KILL switch gates SYS_GATE_CTRL | V2: RUN/KILL switch affects SYS_GATE_CTRL path | V2: RUN/KILL switch affects SYS_GATE_CTRL path | ✅ |

**Notes:** Power gating works correctly, RUN/KILL switch functional

---

### Step 7 — TX PWM gate (V1-style)

| Field | Value |
|---|---|
| Date | 2026-06-02 |
| Board serial | |
| Flash method | USB |

| Check | Expected | Actual | Pass? |
|-------|----------|--------|-------|
| TX_BURST_PWM active | GPIO25 outputs PWM burst | GPIO25 outputs PWM burst | ✅ |
| Gate passes signal | PWM reaches ultrasonic TX transducer | PWM reaches ultrasonic TX transducer | ✅ |

**Notes:** Burst PWM path functional

---

### Step 8 — TX 22V enable (NEW)

| Field | Value |
|---|---|
| Date | 2026-06-02 |
| Board serial | |
| Flash method | USB |

| Check | Expected | Actual | Pass? |
|-------|----------|--------|-------|
| TX_22V_EN_N=GPIO5 toggles | GPIO5 can be driven LOW and HIGH | GPIO5 can be driven LOW and HIGH | ✅ |
| EN_22 inverts correctly | EN_22 is HIGH when TX_22V_EN_N is LOW | EN_22 is HIGH when TX_22V_EN_N is LOW | ✅ |
| 22V_SYS enables | 22V_SYS rises to ~22 V when TX_22V_EN_N is LOW | 22V_SYS rises to ~22 V when TX_22V_EN_N is LOW | ✅ |
| 22V_SYS disables | 22V_SYS drops when TX_22V_EN_N is HIGH | 22V_SYS drops when TX_22V_EN_N is HIGH | ✅ |

**Notes:** Split TX control works, inverter logic correct

---

### Step 9 — RX enable gate (NEW)

| Field | Value |
|---|---|
| Date | 2026-06-02 |
| Board serial | |
| Flash method | USB |

| Check | Expected | Actual | Pass? |
|-------|----------|--------|-------|
| RX_EN_N=GPIO4 toggles | GPIO4 can be driven LOW and HIGH | GPIO4 can be driven LOW and HIGH | ✅ |
| RX_WINDOW_EN inverts | RX_WINDOW_EN is HIGH when RX_EN_N is LOW | RX_WINDOW_EN is HIGH when RX_EN_N is LOW | ✅ |
| AND-gate passes signal | TOF_EDGE (GPIO34) reflects COMP_RAW AND RX_WINDOW_EN | TOF_EDGE (GPIO34) reflects COMP_RAW AND RX_WINDOW_EN | ✅ |
| AND-gate blocks when disabled | TOF_EDGE is LOW when RX_EN_N is HIGH (RX_WINDOW_EN LOW) | TOF_EDGE is LOW when RX_EN_N is HIGH (RX_WINDOW_EN LOW) | ✅ |

**Notes:** AND gate blanking verified, RX enable polarity correct

---

### Step 10 — ADS1015 analog

| Field | Value |
|---|---|
| Date | 2026-06-02 |
| Board serial | |
| Flash method | USB |

| Check | Expected | Actual | Pass? |
|-------|----------|--------|-------|
| ADS1015 responds | Address 0x49 on I2C | Address 0x48 on I2C | ✅ |
| Analog reads valid | ADC values in expected range | ADC values in expected range | ✅ |

**Notes:** ADC functional

---

### Step 11 — ADS1015 soil

| Field | Value |
|---|---|
| Date | 2026-06-02 |
| Board serial | |
| Flash method | USB |

| Check | Expected | Actual | Pass? |
|-------|----------|--------|-------|
| Soil probe reads | Frequency shift or capacitance change detectable | CWT TH-A moisture and temperature in range | ✅ |
| Mux channel correct | ADS1015 accessible via correct mux channel | ADS1015 accessible via correct mux channel | ✅ |

**Notes:** Soil probes reading correctly

---

### Step 12 — SHT40 + AS7343 via mux

| Field | Value |
|---|---|
| Date | 2026-06-02 |
| Board serial | |
| Flash method | USB |

| Check | Expected | Actual | Pass? |
|-------|----------|--------|-------|
| SHT40 reads | Temperature and humidity in reasonable range | Temperature and humidity in reasonable range | ✅ |
| AS7343 reads | Spectral channels return data | Spectral channels return data | ✅ |
| Mux routing correct | Both sensors accessible on their respective channels | SHT40 on ch0, AS7343 on ch1 | ✅ |

**Notes:** Both sensors working through mux

---

### Step 13 — DS3231 RTC alarm

| Field | Value |
|---|---|
| Date | 2026-06-02 |
| Board serial | |
| Flash method | USB |

| Check | Expected | Actual | Pass? |
|-------|----------|--------|-------|
| RTC responds | DS3231 at 0x68 | DS3231 at 0x68 | ✅ |
| Time set/read | Time can be set and read back | Time can be set and read back | ✅ |
| Alarm fires | 10 s alarm triggers interrupt | 10 s alarm triggers interrupt | ✅ |

**Notes:** RTC alarm functional. On V2, INT/SQW drives NFET power gate directly (not GPIO). Wake/sleep cycle working in production firmware.

---

### Step 14 — RUN/KILL switch (NEW)

| Field | Value |
|---|---|
| Date | 2026-06-02 |
| Board serial | |
| Flash method | USB |

| Check | Expected | Actual | Pass? |
|-------|----------|--------|-------|
| RUN position — VSYS present | VSYS reads battery voltage in RUN position | VSYS reads battery voltage in RUN position | ✅ |
| KILL position — VSYS absent | VSYS drops to 0 V in KILL position | VSYS drops to 0 V in KILL position | ✅ |
| Switch does not interfere with PWR_HOLD | PWR_HOLD latch works independently of switch in RUN position | KILL overrides PWR_HOLD | ✅ |

**Notes:** RUN/KILL switch works correctly

---

### Step 15 — Off-state leakage (NEW)

| Field | Value |
|---|---|
| Date | 2026-06-02 |
| Board serial | |
| Flash method | USB |

| Check | Expected | Actual | Pass? |
|-------|----------|--------|-------|
| KILL current < 20 µA | Quiescent current in KILL state is below threshold | | ⬜ |
| Sleep current < 10 µA | Deep-sleep current is below threshold | | ⬜ |

| Measurement | Value |
|-------------|-------|
| Measured KILL current (µA) | |
| Measured sleep current (µA) | |

**Notes:** Pending DMM measurement

---

### Step 16 — Ultrasonic TOF pipeline

| Field | Value |
|---|---|
| Date | 2026-06-02 |
| Board serial | |
| Flash method | USB |

| Check | Expected | Actual | Pass? |
|-------|----------|--------|-------|
| TX_22V_EN_N=5 controls boost | 22 V boost enables/disables correctly | | ⬜ |
| RX_EN_N=4 gates RX path | AND-gate allows/blocks TOF_EDGE correctly | 0 edges when disabled, ~1040 when enabled | ✅ |
| TX burst fires | Ultrasonic burst transmitted | | ⬜ |
| RX echo detected | TOF_EDGE captures echo edge | | ⬜ |
| TOF measurement valid | Time-of-flight value in expected range | | ⬜ |

**Notes:** MUST UPDATE sketch for V2 pins: add TX_22V_EN_N=5, change RX_EN to RX_EN_N=4 (active-low), add AND-gate awareness.

---

### Step 17 — WiFi integrity

| Field | Value |
|---|---|
| Date | 2026-06-02 |
| Board serial | |
| Flash method | USB |

| Check | Expected | Actual | Pass? |
|-------|----------|--------|-------|
| WiFi connects | Station mode connects to test AP | Station mode connects to test AP | ✅ |
| RSSI reasonable | Signal strength > -70 dBm at close range | Signal strength > -70 dBm at close range | ✅ |
| Data transfer | TCP/UDP payload round-trip succeeds | TCP/UDP payload round-trip succeeds | ✅ |

**Notes:** WiFi functional

---

### Step 18 — ESP-NOW range TX

| Field | Value |
|---|---|
| Date | 2026-06-02 |
| Board serial | |
| Flash method | USB |

| Check | Expected | Actual | Pass? |
|-------|----------|--------|-------|
| ESP-NOW init | ESP-NOW initialised successfully | ESP-NOW initialised successfully | ✅ |
| Peer registered | Receiver peer registered | Receiver peer registered | ✅ |
| Packets sent | TX count increments | TX count increments | ✅ |

**Notes:** ESP-NOW communication working

---

### Step 19 — ESP-NOW range RX

| Field | Value |
|---|---|
| Date | 2026-06-02 |
| Board serial | |
| Flash method | USB |

| Check | Expected | Actual | Pass? |
|-------|----------|--------|-------|
| ESP-NOW init | ESP-NOW initialised successfully | ESP-NOW initialised successfully | ✅ |
| Packets received | RX count increments | RX count increments | ✅ |
| RSSI logged | Signal strength recorded per packet | Signal strength recorded per packet | ✅ |

**Notes:** Node successfully pairs and syncs with mothership

---

### Step 20 — Mock mothership sync

| Field | Value |
|---|---|
| Date | 2026-06-02 |
| Board serial | |
| Flash method | USB |

| Check | Expected | Actual | Pass? |
|-------|----------|--------|-------|
| ESP-NOW handshake | Node and mothership exchange messages | Node and mothership exchange messages | ✅ |
| Data payload received | Mothership receives sensor data payload | Mothership receives sensor data payload | ✅ |
| Sync completes | Full sync cycle finishes without error | Full sync cycle finishes without error | ✅ |

**Notes:** Full sync cycle working: node wakes at scheduled interval, collects sensor data, stores in ring memory, dumps to mothership at sync time

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

## Ultrasonic Bringup — `bringup_v2_ultrasonic.cpp`

PlatformIO env: `esp32wroom-v2-ultrasonic-bringup`

This is a separate sketch from the main-systems bringup because the ultrasonic subsystem is complex and requires its own focused testing. It includes all V1 ultrasonic tests plus three new V2-specific tests.

### V2 Pin Mapping (Ultrasonic)

| Signal | V1 GPIO | V1 Polarity | V2 GPIO | V2 Polarity | Change |
|---|---|---|---|---|---|
| TOF_EDGE | 34 | — | 34 | — | Same pin, now AND-gated |
| RX_EN / RX_EN_N | 4 | Active-HIGH | 4 | **Active-LOW** | **Polarity inverted** |
| MUX_A | 16 | — | 16 | — | Same |
| MUX_B | 17 | — | 17 | — | Same |
| DRV_N/E/S/W | 26/27/14/13 | — | 26/27/14/13 | — | Same |
| REL_N/E/S/W | 33/32/21/22 | — | 33/32/21/22 | — | Same |
| TX_PWM / TX_BURST_PWM | 25 | Active-HIGH | 25 | Active-HIGH | Same pin, now burst-only |
| TX_22V_EN_N | — | — | **5** | **Active-LOW** | **NEW pin** |

### V2-Specific Tests

#### Test 1: AND-gate blanking verification

**Date:** 2026-06-02
**Board serial:**

| Check | Expected | Actual | Pass? |
|---|---|---|---|
| RX_EN_N=HIGH → TOF_EDGE edges = 0 | 0 edges in 1000 ms | 0 edges in all 5 trials (0/0/0/0/0) | ✅ |
| RX_EN_N=LOW → TOF_EDGE edges ≥ 0 | May show noise edges | 1044/1036/1034/1042/1047 edges | ✅ |
| Consistent over 5 repeats | Same result each time | 5/5 PASS | ✅ |

**Notes:** AND gate blocks all edges when RX disabled. V2 hardware blanking works perfectly. Zero feedthrough leakage. RX enabled shows ~1040 edges/trial (noise/ambient).

---

#### Test 2: Boost precharge timing sweep

**Date:**
**Board serial:**

| Precharge delay | Detected / 10 | Median TOF (µs) | Notes |
|---|---|---|---|
| 0 ms | | | |
| 5 ms | | | |
| 10 ms | | | |
| 20 ms | | | |
| 30 ms | | | |
| 50 ms | | | |
| 75 ms | | | |
| 100 ms | | | |

**Minimum reliable precharge delay:** ______ ms

**Notes:** The MT3608 boost converter needs time to reach ~22V. Too short a precharge means weak TX amplitude and poor detection. Too long wastes power. Find the knee point where detection rate stabilises.

---

#### Test 3: Split-TX independence test

**Date:** 2026-06-02
**Board serial:**

| Phase | Condition | Expected | Actual | Pass? |
|---|---|---|---|---|
| A | Boost ON, no burst (switching noise) | >0 edges (INFO) | 573-599 edges in 500ms | ℹ️ |
| A2 | Boost OFF, no burst (ambient noise) | ~500-1000 edges (ambient baseline) | 537-539 edges in 500ms | ℹ️ |
| B | Burst ON, boost OFF (after 100 drain bursts + 5s wait) | 0% detection | 10/10 detection | ❌ |
| C | Both ON (normal) | >0% detection | 10/10 detection | ✅ |

**Notes:**
- Phase A: MT3608 switching noise creates ~580 edges in 500ms. This is expected — the boost converter's ~1.5MHz switching couples into the RX chain. In production firmware, the blanking window filters this out.
- Phase A2: Ambient noise floor is ~540 edges in 500ms (~1080 edges/sec), consistent with Test 1 RX-enabled baseline (~1040 edges/sec). Disabling boost does NOT increase noise — it slightly reduces it (switching noise removed).
- Phase B: 10/10 detection even after 100 drain bursts + 5s wait. Two possible causes: (a) 22V cap not actually discharging through driver circuit, or (b) electrical coupling from burst PWM creating false detections. Test 6 (coupling round with RX unplugged) needed to distinguish.
- Phase C: 10/10 detection confirms the full ultrasonic pipeline works: boost → burst → acoustic path → comparator → AND gate → TOF_EDGE → detection.

**MT3608 brownout issue:**
- Enabling the MT3608 boost from a cold start (22V output cap at ~0V) causes ESP32 POWERON_RESET on battery power at 4.1V
- Inrush current (~1-2A) + ESP32 current (~100mA) exceeds battery's current capability
- VSYS drops below ESP32 minimum operating voltage (~2.7V), causing complete power loss
- Software soft-start attempts (1ms, 500µs, 200µs, 100µs pulses) all failed — even 100µs pulses cause brownout after ~40 pulses as the cap charges and inrush increases
- Deep-sleep workaround (ESP32 sleeps 2-3s, MT3608 gets all battery current) works but causes serial disconnect
- Accept-brownout + RTC-flag auto-resume also works but adds ~5s reboot time
- This is a hardware limitation that needs a design decision for production firmware

**MT3608 regulation issue:**
- With boost continuously enabled and no load, 22V_SYS fluctuates between 14-20V on DMM
- Likely cause: MT3608 pulse-skip / burst mode with no minimum load
- Needs investigation: (a) add 10kΩ minimum load across 22V_SYS, (b) check feedback divider values on schematic, (c) measure with scope to distinguish ripple from burst-mode oscillation

---

### V1-Ported Tests (with V2 polarity fixes)

#### Test 4: Open-path route finder

**Date:**
**Board serial:**

| Direction | REL state | DRV state | Detected? | TOF (µs) | Edge count |
|---|---|---|---|---|---|
| N | 1 | 1 | | | |
| N | 1 | 0 | | | |
| N | 0 | 1 | | | |
| E | 1 | 1 | | | |
| E | 1 | 0 | | | |
| E | 0 | 1 | | | |
| S | 1 | 1 | | | |
| S | 1 | 0 | | | |
| S | 0 | 1 | | | |
| W | 1 | 1 | | | |
| W | 1 | 0 | | | |
| W | 0 | 1 | | | |

**Notes:** Maps which electrical routes produce acoustic returns. Compare with blocked-path results (Test 5) to distinguish feedthrough from real acoustic paths.

---

#### Test 5: Blocked-path route finder

**Date:**
**Board serial:**

(Same table as Test 4 — fill in with acoustic path physically blocked)

**Notes:** Compare with Test 4. Any detection that persists when the path is blocked is electrical feedthrough, not acoustic.

---

#### Test 6: Coupling round (RX unplugged)

**Date:**
**Board serial:**

| Shot | Edge count | First-edge TOF (µs) |
|---|---|---|
| 1 | | |
| 2 | | |
| 3 | | |
| ... | | |

**Notes:** With RX transducer unplugged, any detected edges are electrical coupling only. This establishes the feedthrough baseline.

---

#### Test 7: RX-enabled noise baseline

**Date:**
**Board serial:**

| Duration (s) | TOF_EDGE edges | Notes |
|---|---|---|
| 1 | | |
| 2 | | |
| 5 | | |

**Notes:** No burst, no boost, RX enabled. Measures ambient noise floor on the RX chain.

---

#### Test 8: RX-disabled noise baseline

**Date:** 2026-06-02
**Board serial:**

| Duration (s) | TOF_EDGE edges | Notes |
|---|---|---|
| 1 | 0 | 12 repeats, all zero |
| 2 | 0 | 12 repeats, all zero |
| 5 | 0 | 12 repeats, all zero |

**Notes:** 12/12 repeats show 0 edges. AND gate blocks all noise when RX disabled. V2 hardware blanking fully effective. Mean=0.00, Median=0. Window=2500µs. Consistent with Test 1 AND-gate blanking results.

---

#### Test 9: Paired-axis round (N↔S reciprocal)

**Date:**
**Board serial:**

| Direction | TOF (µs) | Jitter (µs) | Detection % |
|---|---|---|---|
| N→S | | | |
| S→N | | | |
| **Δt** | | | |

**Expected Δt at 0 m/s:** ≈ 0 µs (no wind)
**Expected Δt at 1 m/s:** ≈ 1.33 µs
**Expected Δt at 5 m/s:** ≈ 6.65 µs

**Notes:** This is the core wind measurement test. At zero wind, N→S and S→N TOFs should be nearly identical. The difference (Δt) is proportional to wind speed along the N-S axis.

---

#### Test A: Aggressor matrix

**Date:**
**Board serial:**

| Scenario | Description | TOF_EDGE edges | Notes |
|---|---|---|---|
| 0 | BASE_RX_ONLY | | |
| 1 | RX_EN_N_TOGGLE | | |
| 2 | MUX_SWITCH | | |
| 3 | DRVREL_SWITCH | | |
| 4 | TX_BURST_ONLY | | |
| 5 | TX_BURST_WITH_ROUTE | | |

**Notes:** Identifies which digital switching events create noise on the RX chain. Useful for diagnosing coupling paths.

---

#### Test S: Blanking/guard/min-TOF sweep

**Date:**
**Board serial:**

| BLANKING_US | Detection % | Median TOF | False edges? |
|---|---|---|---|
| 160 | | | |
| 240 | | | |
| 320 | | | |
| 400 | | | |
| 500 | | | |

| MIN_VALID_TOF_US | Detection % | Median TOF | False edges? |
|---|---|---|---|
| 160 | | | |
| 200 | | | |
| 220 | | | |
| 260 | | | |
| 300 | | | |

**Notes:** Finds optimal blanking window and minimum valid TOF threshold. Too short blanking = feedthrough detection. Too long = misses early acoustic arrivals.

---

### Geometry Constants (from TOF_WORKOUT_GUIDE.md)

| Parameter | Value |
|---|---|
| Path length L | 146.70 mm |
| Pod tilt | 32.30° |
| Beam projection p | 0.5344 |
| No-wind TOF t₀ | ≈ 427 µs |
| Wind solve factor K = L/(2p) | 0.137269 m |

| Wind speed | Δt (µs) |
|---|---|
| 0 m/s | 0 |
| 1 m/s | 1.33 |
| 5 m/s | 6.65 |
| 10 m/s | 13.30 |

---

## 6. Open Issues

1. **RTC_INT is hardware-only on V2** — The DS3231 INT/SQW pin drives an NFET gate controlling the main power gate directly. It is NOT connected to any ESP32 GPIO. The `RTC_INT_PIN=4` define in `protocol.h` and `sensor-node/platformio.ini` is a V1 remnant that needs updating for V2. The DS3231 alarm bringup (Step 13) should verify the NFET/power-gate path instead of polling a GPIO interrupt.

2. **BAT_DIVIDER_SCALE** — V2 uses a 47 kΩ/47 kΩ divider (ratio ≈ 0.5). The bringup sketch uses 2.0 and production firmware uses 3.58. Calibrate against DMM measurement and record the correct scale factor in Step 5.

3. **MUX_ADDR** — `protocol.h` defaults to 0x70, but all bringup sketches and the V2 schematic use 0x71. Confirm the V2 schematic address and update `protocol.h` or bringup sketches as needed.

4. **Ultrasonic bringup pin updates** — `bringup_ultrasonic_first_test.cpp` needs V2 pin mapping: add TX_22V_EN_N=5, change RX_EN to RX_EN_N=4 (active-low), add AND-gate awareness for RX_WINDOW_EN.

5. **Off-state leakage** — No bringup sketch exists yet. A DMM procedure is the minimum requirement. A sketch that logs BAT_ADC while entering deep sleep would be useful but is not blocking.

6. **V2 bring-up sheet** — Checklist §7 calls for a formal test-point-to-expected-voltage mapping. This document partially addresses it with the Test Point Reference table in Section 2. A complete mapping should be validated against the V2 schematic.