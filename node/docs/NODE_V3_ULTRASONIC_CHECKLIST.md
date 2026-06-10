# Node V3 — Ultrasonic Anemometer Checklist

**Date:** 2026-06-10  
**Status:** Active — V3 ultrasonic bring-up and validation checklist  
**Scope:** Getting the ultrasonic anemometer subsystem working reliably on Node V3  
**Prerequisite:** V3 22 V boost circuit changes must be validated first (see `NODE_V3_22V_BOOST_UPDATE.md`)  
**See also:** `NODE_HARDWARE_V2_CHECKLIST.md`, `ultrasonic_circuit_v2_design_advice.md`, `ULTRASONIC_NOISE_ANALYSIS_2026-04-11.md`, `ULTRASONIC_ANEMOMETER_WORK_PLAN.md`

---

## Purpose

This checklist focuses on what's needed to get the ultrasonic anemometer working on Node V3. It covers the 22 V boost validation, the analog signal chain, and the acoustic measurement quality. Items are ordered by dependency: you can't test the analog chain until the 22 V rail is stable, and you can't validate acoustic discrimination until the analog chain is clean.

---

## Phase 0 — 22 V Boost Validation (Prerequisite)

These must pass before any ultrasonic testing can proceed. The V3 inductor upgrade and U49 decoupling are applied but unvalidated.

- [ ] **0.1** Cold-start 22 V boost test from battery (~4.1 V): ESP32 must not brown out or reset
- [ ] **0.2** VSYS remains above 2.7 V during boost enable
- [ ] **0.3** 3V3_SYS remains stable (±5%) during boost enable
- [ ] **0.4** U49 output transitions cleanly LOW→HIGH when GPIO5 goes LOW (no oscillation or ringing)
- [ ] **0.5** EN_22 goes HIGH and remains stable while boost is enabled (no wobble or dropout)
- [ ] **0.6** 22V_SYS regulates close to 22 V (not 14–20 V fluctuation) with no load
- [ ] **0.7** 22V_SYS remains stable under transducer burst load (12 cycles at 40 kHz)
- [ ] **0.8** No repeated MT3608 hiccuping or oscillation
- [ ] **0.9** MT3608 does not overheat during sustained measurement window (15–60 s)

**If Phase 0 fails:** See escalation path in `NODE_V3_22V_BOOST_UPDATE.md` — add EN_22 hold capacitor, Schottky isolation, or VSYS bulk capacitor as needed.

---

## Phase 1 — Electrical Baseline (Signal Chain Verification)

These tests verify the analog signal chain is electrically functional and quiet enough for acoustic measurements. They correspond to Phase 1.3 in the ultrasonic work plan.

### 1.1 TX Power Path

- [ ] **1.1.1** TX_BURST_PWM (GPIO25) outputs clean 40 kHz burst at expected amplitude
- [ ] **1.1.2** TC4427 driver output (PWM_5V) is a clean square wave at 5 V amplitude
- [ ] **1.1.3** TX_PULSE node reaches ~22 V amplitude during burst (verify on oscilloscope)
- [ ] **1.1.4** 22V_SYS remains within ±1 V of target during burst (no significant droop)
- [ ] **1.1.5** TX_PULSE decays cleanly after burst (220 kΩ bleed resistor working)

### 1.2 RX Path DC Levels

- [ ] **1.2.1** VREF measures ~1.65 V (47 kΩ/47 kΩ divider from 3V3_SYS)
- [ ] **1.2.2** VREF remains stable during TX burst (no significant dip or ripple on oscilloscope)
- [ ] **1.2.3** RX_IN DC bias is near VREF/2 when mux enabled (R174=1 MΩ bias working)
- [ ] **1.2.4** RX_AMP output DC level is near mid-rail when RX enabled
- [ ] **1.2.5** COMP_RAW output is stable (not constantly toggling) when RX is enabled and no burst is fired

### 1.3 RX-Disabled State

- [ ] **1.3.1** TOF_EDGE (GPIO34) shows zero edges when RX_EN_N=HIGH (AND gate blocks all edges)
- [ ] **1.3.2** COMP_RAW is quiet when RX is disabled (no constant toggling)
- [ ] **1.3.3** RX_IN is biased to VREF (not floating) when mux is disabled
- [ ] **1.3.4** RX_AMP output is near mid-rail when RX is disabled (not saturated or railing)

### 1.4 Blanking and Gating

- [ ] **1.4.1** RX_WINDOW_EN goes HIGH when RX_EN_N goes LOW (U50 inverter working)
- [ ] **1.4.2** TOF_EDGE = COMP_RAW AND RX_WINDOW_EN (U51 AND gate working)
- [ ] **1.4.3** TOF_EDGE is forced LOW when RX_EN_N=HIGH regardless of COMP_RAW state
- [ ] **1.4.4** Blanking window (BLANKING_US) suppresses early edges after burst start

### 1.5 Comparator and Hysteresis

- [ ] **1.5.1** Confirm comparator part number on V3 board (expected: TLV9062IDR or equivalent)
- [ ] **1.5.1a** Probe 3V3_COMP during TX burst — should be cleaner than 3V3_SYS (4.7 Ω filter + local decoupling working)
- [ ] **1.5.1b** Probe 3V3_RXAMP during TX burst — should be cleaner than 3V3_SYS (4.7 Ω filter + local decoupling working)
- [ ] **1.5.2** Measure actual hysteresis at comparator decision point (target: tens of mV, not just a few mV)
- [ ] **1.5.3** Test with R179 solder jumper open (1 MΩ hysteresis only) — record baseline noise edge count
- [ ] **1.5.4** Test with R179 solder jumper closed (~500 kΩ effective hysteresis) — record noise edge count
- [ ] **1.5.5** Choose hysteresis configuration that gives clean switching on real acoustic events while rejecting small disturbances

### 1.6 VREF Quality

- [ ] **1.6.1** Measure VREF with oscilloscope during TX burst — check for dip or ripple (V3 now has 220 Ω supply filter + 4.7 µF bulk; VREF should be significantly more stable than V2)
- [ ] **1.6.2** If VREF still moves during burst despite 220 Ω + 4.7 µF filtering, consider buffering VREF with an op-amp (V3 future change)
- [ ] **1.6.3** If VREF is still noisy, consider buffering with an op-amp (V3 future change, not immediate)

---

## Phase 2 — Noise Isolation (Coupling and Feedthrough)

These tests determine whether the analog chain is clean enough for acoustic discrimination. They correspond to Phase 2 in the ultrasonic work plan.

### 2.1 Baseline Noise

- [ ] **2.1.1** Measure TOF_EDGE edge count with RX enabled, no burst, no boost (ambient noise baseline)
- [ ] **2.1.2** Measure COMP_RAW edge count under same conditions (before AND gate)
- [ ] **2.1.3** Record baseline for 1 s, 5 s, and 10 s intervals

### 2.2 TX Coupling

- [ ] **2.2.1** Measure TOF_EDGE edges with boost ON but no burst (MT3608 switching noise)
- [ ] **2.2.2** Measure TOF_EDGE edges with burst ON but boost OFF (electrical coupling from burst PWM)
- [ ] **2.2.3** Measure TOF_EDGE edges with both boost and burst ON (normal operating condition)
- [ ] **2.2.4** Compare edge counts: if boost noise dominates, investigate 3V3_SYS ripple coupling into analog chain
- [ ] **2.2.4a** Probe 3V3_SYS vs 3V3_COMP during boost enable and TX burst — confirm comparator supply filtering is effective
- [ ] **2.2.4b** Probe 3V3_SYS vs 3V3_RXAMP during boost enable and TX burst — confirm RX amplifier supply filtering is effective

### 2.3 Open vs Blocked Path

- [ ] **2.3.1** Measure TOF_EDGE with acoustic path OPEN (no obstruction)
- [ ] **2.3.2** Measure TOF_EDGE with acoustic path BLOCKED (foam or physical obstruction)
- [ ] **2.3.3** If open and blocked give similar results, the system is still feedthrough-dominated — not yet acoustically discriminative
- [ ] **2.3.4** If open shows consistent detections and blocked shows significantly fewer, the system is acoustically discriminative

### 2.4 RX Unplugged Test

- [ ] **2.4.1** Disconnect RX transducer from the board
- [ ] **2.4.2** Fire burst and measure TOF_EDGE edges
- [ ] **2.4.3** Any edges detected are purely electrical coupling (not acoustic)
- [ ] **2.4.4** Record feedthrough baseline — this is the minimum noise floor

### 2.5 Aggressor Matrix

- [ ] **2.5.1** Test each digital switching event individually: RX_EN_N toggle, mux switch, DRV/REL switch, TX burst, boost enable
- [ ] **2.5.2** Record which events create noise on COMP_RAW and TOF_EDGE
- [ ] **2.5.3** Identify the dominant aggressor(s) and their coupling path(s)

---

## Phase 3 — Acoustic Discrimination (Wind Measurement Validation)

These tests verify the system can actually measure wind. They correspond to Phase 3–4 in the ultrasonic work plan.

### 3.1 Paired-Axis Measurement

- [ ] **3.1.1** Measure N→S TOF (multiple shots, record median and jitter)
- [ ] **3.1.2** Measure S→N TOF (multiple shots, record median and jitter)
- [ ] **3.1.3** Calculate Δt = t(N→S) − t(S→N)
- [ ] **3.1.4** At zero wind, Δt should be ≈ 0 µs (within measurement jitter)
- [ ] **3.1.5** At known wind speed, Δt should be proportional to wind speed along the axis

### 3.2 Blanking and Guard Window Sweep

- [ ] **3.2.1** Sweep BLANKING_US from 160 to 500 µs — find optimal value that suppresses feedthrough but doesn't miss early acoustic arrivals
- [ ] **3.2.2** Sweep MIN_VALID_TOF_US from 160 to 300 µs — find optimal minimum valid TOF threshold
- [ ] **3.2.3** Record detection rate and median TOF for each setting

### 3.3 Precharge Timing

- [ ] **3.3.1** Sweep BOOST_PRECHARGE_MS from 5 to 100 ms — find minimum reliable precharge time
- [ ] **3.3.2** Verify 22V_SYS reaches target voltage before burst fires
- [ ] **3.3.3** Record detection rate vs precharge time

---

## Phase 4 — V3 Hardware Changes (If Needed After Testing)

These are hardware changes that may be needed based on Phase 1–3 results. They are NOT applied yet — they are contingent on test results.

### 4.1 If VREF Is Noisy During Burst

- [ ] **4.1.1** ~~Add 4.7 µF ceramic near the analog section (VREF bulk decoupling)~~ — ✅ Already applied in V3
- [ ] **4.1.2** If still noisy despite 220 Ω + 4.7 µF, add op-amp buffer on VREF (requires PCB change for V3+)
- [ ] **4.1.3** Consider 3V3_SYS → ferrite bead → 3V3_A filtered analog rail (requires PCB change) — note: V3 already has separate 3V3_COMP, 3V3_RXAMP, and VREF supply filtering; a full 3V3_A rail would be a different topology

### 4.2 If Comparator False-Triggers

- [ ] **4.2.1** Close R179 solder jumper to increase hysteresis to ~500 kΩ effective
- [ ] **4.2.2** If still false-triggering, increase hysteresis further (requires resistor value change)
- [ ] **4.2.3** If hysteresis increase reduces sensitivity too much, investigate analog mute during TX

### 4.3 If TX Coupling Dominates

- [ ] **4.3.1** Verify TX_PULSE trace is not running near RX analog nodes (layout audit)
- [ ] **4.3.1a** If comparator supply filtering is insufficient, consider increasing R_comp_filt from 4.7 Ω to 10–22 Ω or adding ferrite bead in place of resistor
- [ ] **4.3.1b** If RX amplifier supply filtering is insufficient, consider increasing R_rxamp_filt from 4.7 Ω to 10–22 Ω or adding ferrite bead in place of resistor
- [ ] **4.3.2** Add analog mute/clamp on RX input during TX (requires PCB change)
- [ ] **4.3.3** Add 3V3_A filtered analog rail for RX amplifier and comparator (requires PCB change) — note: V3 already has separate 3V3_COMP and 3V3_RXAMP islands; a full 3V3_A rail would be a single filtered supply feeding both, which is a different topology

### 4.4 If 22 V Rail Is Unstable at Light Load

- [ ] **4.4.1** Bodge a 22 kΩ or 10 kΩ resistor from 22V_SYS to GND as minimum load test
- [ ] **4.4.2** If stability improves, add permanent bleed resistor to PCB (R5 from MT3608 design note)
- [ ] **4.4.3** If still unstable, add VSYS bulk capacitor footprint (R4 from MT3608 design note)

### 4.5 If EN_22 Glitches

- [ ] **4.5.1** Add 10 µF ceramic from EN_22 to GND (hold capacitor)
- [ ] **4.5.2** If still glitching, add Schottky diode isolation on EN_22 (R2 from MT3608 design note)
- [ ] **4.5.3** If still glitching, remove U49 and switch to direct Schottky drive (R2+R3, requires firmware polarity change)

---

## V3 Ultrasonic BOM Summary (Applied Changes)

| Change | Component | Value | Status | Notes |
|--------|-----------|-------|--------|-------|
| L4 inductor upgrade | L4 | 22 µH ≥2.5 A I_sat | ✅ Applied | LCSC C41406986, fixes saturation brownout |
| MT3608 input cap (bulk) | C_input | 1210 100 µF 6.3 V X5R | ✅ Applied | VSYS input energy reserve |
| MT3608 input cap (HF) | C_input_hf | 100 nF ceramic | ✅ Applied | VSYS input HF decoupling |
| U49 VCC decoupling | C_u49 | 1 µF ceramic | ✅ Applied | Reduces 3V3_SYS ripple coupling |
| Comparator supply filter resistor | R_comp_filt | 4.7 Ω | ✅ Applied | 3V3_SYS → 3V3_COMP series filter |
| Comparator VCC HF decoupling | C_comp_hf | 100 nF ceramic | ✅ Applied | On 3V3_COMP side, close to comparator |
| Comparator VCC bulk decoupling | C_comp_bulk | 1 µF ceramic | ✅ Applied | On 3V3_COMP side, close to comparator |
| 3V3_COMP supply island | — | Filtered 3.3 V | ✅ Applied | Clean comparator supply via 4.7 Ω + caps |
| RX amplifier supply filter resistor | R_rxamp_filt | 4.7 Ω | ✅ Applied | 3V3_SYS → 3V3_RXAMP series filter |
| RX amplifier VCC HF decoupling | C_rxamp_hf | 100 nF ceramic | ✅ Applied | On 3V3_RXAMP side, close to TLV9062 |
| RX amplifier VCC bulk decoupling | C_rxamp_bulk | 1 µF ceramic | ✅ Applied | On 3V3_RXAMP side, close to TLV9062 |
| 3V3_RXAMP supply island | — | Filtered 3.3 V | ✅ Applied | Clean RX amp supply via 4.7 Ω + caps |
| VREF divider supply filter resistor | R_vref_filt | 220 Ω | ✅ Applied | 3V3_SYS → VREF divider supply isolation |
| VREF bulk decoupling | C_vref_bulk | 4.7 µF ceramic | ✅ Applied | VREF to GND, improves stability during TX burst |
| VREF filtering (total) | — | 100 nF + 1 µF + 4.7 µF | ✅ Applied | VREF decoupling stack with 220 Ω supply filter |
| U49 retained | U49 | SN74LVC1G04DRLR | ✅ Retained | Boot-safe EN_22 logic preserved |
| EN_22 test point | TP | EN_22 | ✅ Present | For bring-up probing |
| 22V_SYS bleed resistor | R_bleed | — | ❌ Not added | Reconsider if 22V unstable at idle |
| EN_22 hold capacitor | C_en22 | — | ❌ Not added | Reconsider if EN_22 glitches |
| VREF buffer | — | — | ❌ Not added | Reconsider if VREF noisy during burst |
| 3V3_A filtered rail | — | — | ❌ Not added | Reconsider if TX coupling dominates |
| Analog mute during TX | — | — | ❌ Not added | Reconsider if RX saturates during burst |

---

## V2 Ultrasonic Items Already Completed (Carried Forward)

These were done in V2 and are retained in V3:

- [x] Split TX control: TX_BURST_PWM (GPIO25) separate from TX_22V_EN_N (GPIO5)
- [x] AND-gate hardware blanking: TOF_EDGE = COMP_RAW AND RX_WINDOW_EN
- [x] BAV99 RX clamp orientation corrected
- [x] RX_IN bias: R174=1 MΩ from RX_IN to VREF (prevents floating when mux disabled)
- [x] VREF divider strengthened: 47 kΩ/47 kΩ (from 100 kΩ/100 kΩ)
- [x] VREF filtering: 1 µF + 100 nF retained
- [x] TLV9062IDR RX amplifier moved to 3V3_SYS domain
- [x] Comparator supply filtered: 3V3_SYS → 4.7 Ω → 3V3_COMP with 100 nF + 1 µF local decoupling
- [x] TLV9062 RX amplifier supply filtered: 3V3_SYS → 4.7 Ω → 3V3_RXAMP with 100 nF + 1 µF local decoupling
- [x] VREF supply filtered: 3V3_SYS → 220 Ω → 47 kΩ/47 kΩ divider, with 100 nF + 1 µF + 4.7 µF decoupling to GND
- [x] R108=1 kΩ retained (no extra comparator-input BAV99)
- [x] COMP_RAW / TOF_EDGE naming convention
- [x] U50 inverter for RX_WINDOW_EN = NOT RX_EN_N
- [x] U51 AND gate for TOF_EDGE = COMP_RAW AND RX_WINDOW_EN
- [x] R178=0 Ω gated path selector + bypass jumper
- [x] R179=1 MΩ optional hysteresis via solder jumper
- [x] RX_EN_N pull-up for safe boot default (mux disabled, TOF_EDGE blocked)
- [x] TX_PULSE as controlled ~0.5 mm trace (not broad pour)
- [x] 22V_SYS kept local to TX section
- [x] 220 kΩ bleed resistor on TX_PULSE
- [x] Test pads for key ultrasonic nodes
- [x] Ultrasonic is mandatory on-board (not optional population)

---

## V3 Ultrasonic Items Still Open (From V2 Checklist §5)

These are unchecked items from the V2 hardware checklist that directly affect ultrasonic operation:

- [ ] **Full ultrasonic signal chain review** — TX burst generation, 22 V drive, analog mux, protection, amplification, comparator, timer capture
- [ ] **Add cleaner RX protection** — explicit clamp per RX input node (signal → series R → clamp point → diode to 3V3_A → diode to GND_A)
- [ ] **Verify analog and power-domain separation** around ultrasonic front end
- [ ] **Add schematic note near R178 and bypass jumper** — populate only one path, never both
- [ ] **Confirm connector strategy** for transducers and head assembly wiring
- [ ] **Confirm mechanical placement** keeps acoustic wiring outside measurement volume and away from noisy switching nodes
- [ ] **Keep TX_PULSE, 22V_SYS, PWM_5V, TX_BURST_PWM away from RX_IN, RX_COLD, ST1_IN, VREF, RX_AMP, COMP_RAW, TOF_EDGE** — layout audit
- [ ] **Decoupling review** around ultrasonic power stage (MT3608 input cap and U49 decoupling done; remaining decoupling still needed)

---

## Key References

| Document | Location | Relevance |
|----------|----------|-----------|
| V3 22 V boost update | `node/docs/NODE_V3_22V_BOOST_UPDATE.md` | Inductor, decoupling, U49 decisions |
| V2 hardware checklist | `node/docs/NODE_HARDWARE_V2_CHECKLIST.md` | Full checklist including ultrasonic §5 |
| V2 bringup results | `node/firmware/tests/NODE_V2_BRINGUP_RESULTS.md` | Test results, MT3608 brownout details |
| PCB overview | `node/docs/NODE-PCB-OVERVIEW.md` | Signal chain architecture, pin mapping |
| Ultrasonic design advice | `node/docs/ultrasonic_circuit_v2_design_advice.md` | 7 design goals for V2/V3 |
| Noise analysis | `node/docs/ULTRASONIC_NOISE_ANALYSIS_2026-04-11.md` | V1 noise findings, acceptance criteria |
| Work plan | `node/docs/ULTRASONIC_ANEMOMETER_WORK_PLAN.md` | Phased approach to acoustic validation |
| MT3608 brownout design note | `mothership/docs/MT3608_BROWNOUT_DESIGN_NOTE.md` | Full root cause analysis, R1–R8 recommendations |
| Function note | `node/docs/ultrasonic_anemometer_function_note.md` | Hardware/firmware function reference |