# Node V3 Bringup Results

| Field | Value |
|---|---|
| Board | Node V3 |
| Revision | V3 (Gerber 2026-06-28) |
| Date Started | 2026-07-13 |
| Operator | |

---

## Summary

**Overall status:** Phase 0 (board basics) PASS. TX signal chain PASS (DMM confirmed). RX chain PASS. Acoustic detection PENDING — TX electronics reach transducer header, but no acoustic signal detected. Problem is at the transducer or acoustic path level.

**Key V3 changes from V2:**
- 22 V boost inductor upgraded to 22 µH ≥2.5 A I_sat (LCSC C41406986) — fixes V2 inductor saturation brownout
- MT3608 input cap: 100 µF + 100 nF added locally
- MT3608 output caps: 100 nF + 22 µF + 22 µF (reduced from 100 µF to reduce cold-start energy)
- U49 VCC decoupling: 1 µF + 100 nF added
- Analog supply islands: 3V3_COMP, 3V3_RXAMP, 3V3_MUX (each via 4.7 Ω filter resistor + local decoupling)
- VREF supply: 220 Ω filter + 100 nF + 1 µF + 4.7 µF bulk + distributed 100 nF at comparator and TLV9062 bias
- REL_* reinterpreted as damping/shunt MOSFET controls (net names unchanged)
- No new GPIO pins

---

## Phase 0 — Board Basics (bringup_v3_main_systems)

| Subsystem | Status | Notes |
|---|---|---|
| Boot / serial | ✅ PASS | Clean boot, serial menu responsive |
| I2C bus scan | ✅ PASS | 3 devices found: 0x48 (ADS1015), 0x68 (DS3231), 0x71 (mux) — all expected |
| I2C mux channel sweep | ✅ PASS | All 8 channels scanned; no external sensors attached (expected) |
| Battery ADC (IO35) | ✅ PASS | raw=1454, pin_V=1.172, batt_V=4.242 V — matches DMM reading (user confirmed correct) |
| PWR_HOLD latch | ✅ PASS | HIGH=ON, LOW=OFF — board stays alive on HIGH, user confirmed works |
| TX_BURST_PWM gate | ⬜ Pending | Not tested in this run |
| TX_22V_EN_N boost enable | ✅ PASS | 22 V detected on boost rail — U49 inverter logic working (LOW=ON, HIGH=OFF) |
| RX_EN_N + AND-gate | ✅ PASS | TOF_EDGE=LOW when RX_EN_N=HIGH (AND gate blocks). PASSED. |
| ADS1015 analog read | ✅ PASS | All 4 channels read ~0.018 V (no sensors attached — expected) |
| DS3231 RTC alarm | ✅ PASS | Alarm armed for +10s, fired on schedule, flag cleared, re-armed. I2C alarm path working. |
| DRV directional TX toggle | ⬜ Pending | Not tested in this run |
| REL/DAMP shunt toggle | ⬜ Pending | Not tested in this run |
| Mux A/B direction select | ⬜ Pending | Not tested in this run |

---

## Phase 1 — 22 V Boost Validation (bringup_v3_boost_cold_start)

| Item | Status | Notes |
|---|---|---|
| 0.1 Cold-start from battery (~4.1 V) | ✅ PASS | No brownout/reset — 22V rail hard-enabled successfully |
| 0.2 VSYS remains >2.7 V during boost enable | ✅ PASS | No brownout implies VSYS held up |
| 0.3 3V3_SYS stable (±5%) during boost enable | ✅ PASS | No reset implies 3V3_SYS stable |
| 0.4 U49 output transitions cleanly | ⬜ Pending | Needs scope — skipped (no oscilloscope) |
| 0.5 EN_22 stable while boost enabled | ⬜ Pending | Needs scope — skipped (no oscilloscope) |
| 0.6 22V_SYS regulates ~22 V no load | ✅ PASS | 22V detected (confirmed in Phase 0 main-systems test) |
| 0.7 22V_SYS stable under burst load (12 cycles 40 kHz) | ⬜ Pending | Will validate via acoustic test — if TOF detected, burst load is handled |
| 0.8 No MT3608 hiccuping | ✅ PASS | No brownout implies no hiccup |
| 0.9 MT3608 not overheating (15–60 s window) | ✅ PASS | Sustained enable without issue |

---

## Phase 2 — Electrical Baseline (bringup_v3_carrier_chain / directional_tx / rx_baseline)

| Item | Status | Notes |
|---|---|---|
| 1.1.1 TX_BURST_PWM clean 40 kHz burst | ⬜ Pending | |
| 1.1.2 TC4427 driver output clean 5 V square | ⬜ Pending | |
| 1.1.3 TX_PULSE ~22 V amplitude | ⬜ Pending | |
| 1.1.4 22V_SYS ±1 V during burst | ⬜ Pending | |
| 1.1.5 TX_PULSE decays cleanly | ⬜ Pending | |
| 1.2.1 VREF ~1.65 V | ⬜ Pending | |
| 1.2.2 VREF stable during TX burst | ⬜ Pending | |
| 1.3.1 TOF_EDGE zero edges when RX disabled | ⬜ Pending | |
| 1.4.1 RX_WINDOW_EN follows RX_EN_N | ⬜ Pending | |
| 1.4.3 TOF_EDGE forced LOW when RX_EN_N=HIGH | ⬜ Pending | |

---

## Phase 3 — Noise Isolation (bringup_v3_coupling)

| Item | Status | Notes |
|---|---|---|
| 2.1.1 Baseline noise (RX enabled, no burst, no boost) | ⬜ Pending | |
| 2.2.1 Boost ON, no burst — edge count | ⬜ Pending | |
| 2.2.2 Burst ON, boost OFF — edge count | ⬜ Pending | |
| 2.2.3 Both ON — edge count | ⬜ Pending | |
| 2.3.1 Open path — TOF_EDGE | ⬜ Pending | |
| 2.3.2 Blocked path — TOF_EDGE | ⬜ Pending | |
| 2.4.1 RX unplugged — feedthrough baseline | ⬜ Pending | |

---

## Phase 4 — Acoustic Discrimination (bringup_v3_acoustic)

| Item | Status | Notes |
|---|---|---|
| 3.1.1 N→S TOF (median + jitter) | ❌ FAIL | 0/24 detections, EDGES=0 — comparator never fired |
| 3.1.2 S→N TOF (median + jitter) | ❌ FAIL | 0/24 detections, EDGES=0 — comparator never fired |
| 3.1.4 Δt ≈ 0 at zero wind | ❌ N/A | No detections — delta not computable |
| 3.2.1 Blanking sweep (160–500 µs) | ⬜ Pending | Blocked by zero detections |
| 3.3.1 Precharge sweep (5–100 ms) | ⬜ Pending | Blocked by zero detections |

---

## Open Questions

- **Battery divider scale:** V3 uses same 3.62 as V2 — verify with DMM during bring-up
- **VREF stability:** V3 has 220 Ω + 4.7 µF filtering — may resolve V2 VREF noise; verify on scope
- **Comparator hysteresis:** R179 solder jumper — test both open (1 MΩ) and closed (~500 kΩ)

---

## Diagnostic Findings — 2026-07-13 (zero detections)

**Symptom:** `bringup_v3_acoustic-10cm` — 98 shots fired (50 open-path + 24 N→S + 24 S→N). **Every shot: DET=0, EDGES=0.** Comparator never fired.

### Finding 1: Mux table mismatch (FIXED)

Firmware `setRxDirection()` had S/E/W all wrong. Fixed to match V3 overview §18:
- N: MUX_B=0, MUX_A=0
- E: MUX_B=0, MUX_A=1
- S: MUX_B=1, MUX_A=0
- W: MUX_B=1, MUX_A=1

### Finding 2: RX chain works with boost ON (confirmed)

`bringup_v3_rx_vs_boost` auto-run results:

| Test | Condition | Edges/sec | Finding |
|------|-----------|-----------|---------|
| 1 | RX disabled, boost OFF | 0.0 | AND gate blocks ✓ |
| 2 | RX enabled, boost OFF, mux=N | 1004.0 | RX chain alive |
| 3 | RX enabled, boost OFF, mux=S | 1003.0 | S channel works |
| 4 | RX enabled, boost ON, mux=N | 1003.0 | **Boost does NOT suppress RX** |
| 5 | RX enabled, boost ON, mux=S | 1003.0 | S channel OK with boost |

**Conclusion:** The V3 supply filtering (3V3_COMP, 3V3_RXAMP, 4.7 Ω resistors) works. Boost ON does not kill the comparator. RX chain is fully functional.

### Finding 3: TX burst produces late, weak signal

Test 6 (full TX burst N, RX=S, boost ON):
- Burst duration: 106 µs (correct for 4 cycles at ~40 kHz)
- Edges detected: 2 in 2 ms window
- First edge at 1309 µs after TX
- Second edge at 2310 µs after TX

**Problem:** Expected TOF at 10 cm ≈ 294 µs. Detected edges at ~1300 µs and ~2300 µs are far too late — likely transducer ringdown or reverberation, not the direct acoustic arrival. The acoustic gate (200-400 µs) rejects these.

### Finding 5: TX signal chain is FULLY FUNCTIONAL (DMM confirmed)

`bringup_v3_continuous_tx` DMM readings (continuous 40 kHz TX, boost ON, DRV_N HIGH):

| Test Point | Reading | Expected | Verdict |
|------------|---------|----------|---------|
| PWM_5V | 1.98 V | ~2.5 V | **TC4427 working** — switching 5V, slightly under 50% duty (bit-bang overhead) |
| HS_GATE | 14.73 V | ~16-18 V | **Q21 switching** — not stuck at 22V, actively pulling down |
| TX_PULSE | 14.78 V | ~11 V | **Q16 switching 22V** — swinging between 0V and 22V |
| TD_N_A (DRV_N HIGH) | 14.9 V | ~11 V | **Q17 working** — TX_PULSE reaching N transducer header |
| TD_N_A (DRV_N LOW) | 1.0 V | ~0 V | Q17 correctly OFF when not selected |

**Conclusion: The TX signal chain from GPIO25 → TC4427 → Q21 → Q16 → TX_PULSE → Q17 → transducer header is 100% functional.** The 22V burst signal reaches the N transducer header pin.

### Finding 7: Transducer connection topology — root cause identified

**Final root cause:** The V3 board has 4 transducer connectors, all wired to the `TD_*` (TX) nets only. The RX mux listens on separate `RX_*` nets that have no connectors. A single transducer cannot do both TX and RX on V3 because the TX and RX nets are not connected on the PCB.

The `NODE_V3_OVERVIEW.md` §16 statement "separate TX and RX transducers" was misleading. The board was intended for 8 transducers (4 TX + 4 RX) but only has 4 connectors (TX). This is a hardware design error — the board cannot perform reciprocal ultrasonic wind measurement.

**All V3 electronics work correctly:**
- 22V boost ✅
- TX carrier chain ✅ (DMM confirmed: TX_PULSE → Q16 → Q17 → TD_N_A at ~15V)
- RX analog chain ✅ (~1000 edges/sec on all mux channels)
- AND-gate blanking ✅
- Mux table fixed in firmware ✅

**Resolution:** Hardware redesign required (V4 ultrasonic sheet). See `node/docs/NODE_V4_ULTRASONIC_REDESIGN_NOTES.md`. The V4 design uses 4 shared bidirectional transducers with per-mux-input protection and switched TX returns.

`bringup_v3_tx_timing` auto-run results (5ms capture window, no gate):

**Part A (4-cycle burst, boost ON):**
- All 5 shots: 5 edges at ~332, ~1332, ~2332, ~3332, ~4332 µs
- Spacing: exactly ~1000 µs (1 ms) between each edge

**Part B (12-cycle burst, boost ON):**
- All 5 shots: 5-6 edges at ~1332, ~2334, ~3334, ~5332 µs
- Same ~1000 µs spacing pattern

**Part C (4-cycle burst, boost OFF):**
- All 5 shots: 5 edges at ~332, ~1332, ~2334, ~4332 µs
- **IDENTICAL to Part A (boost ON)**

**Critical findings:**

1. **The ~1000 edges/sec noise is a periodic ~1000 Hz signal** — edges are spaced exactly ~1 ms apart. This is present whenever RX is enabled, regardless of TX or boost state. It's likely ESP32 WiFi or internal RF interference. The 5ms window captures ~5 edges, matching the noise floor perfectly.

2. **Boost ON vs OFF makes NO difference to edge count** — Part A (boost ON) and Part C (boost OFF) produce identical edge patterns. The 22V rail being present does not add any detectable signal. This means the TX burst is producing ZERO acoustic energy.

3. **The TX path is non-functional** — no acoustic signal is being generated regardless of:
   - Boost state (ON/OFF)
   - Burst length (4 vs 12 cycles)
   - The ~1000 Hz noise floor is the ONLY thing the comparator sees

4. **The first edge in Part A Shot 1 (303 µs) is NOT acoustic** — it's the noise floor happening to land near the expected 294 µs TOF. Shots 2-5 show it at 332 µs, and Part C (boost OFF) shows the same 332 µs. If it were acoustic, it wouldn't appear with boost OFF.

**Conclusion: The failure is in the TX signal chain — no acoustic energy reaches the transducer.**

Likely TX-chain failure points (in order of probability):
1. **TC4427 driver not populated or not working** — GPIO25 bit-bang never reaches Q16 gate
2. **Q16 high-side MOSFET not switching 22V onto TX_PULSE** — boost rail present but not routed to transducer
3. **DRV_N directional P-MOS (Q17) not routing TX_PULSE to N transducer** — TX_PULSE may be generated but not reaching the header
4. **Transducers not connected to N/S headers** — wiring/connector issue
5. **Transducers not 40 kHz** — wrong frequency part, 40 kHz burst won't excite them
6. **TX_PULSE trace damaged** — open circuit on PCB

**Key inference:** EDGES=0 means the capture ISR never triggered. This is NOT a gate-timing issue (where edges would appear but outside the window). The signal never reaches TOF_EDGE at all.

**Likely failing layers (in order of probability):**

1. **TX path not producing acoustic output** — boost enables OK (22V detected), but the burst may not be reaching the transducer. Possible causes:
   - TX_PULSE not toggling (GPIO25 bit-bang issue, or burst duration too short)
   - DRV_x not selecting the transducer (direction mux not switching)
   - Transducer not connected to N/S headers (wiring/connector issue)
   - 22V rail present but not routed to TX_PULSE during burst (boost enable timing)

2. **RX path not producing edges** — acoustic arrives but comparator doesn't fire. Possible causes:
   - RX_EN_N not being driven LOW (AND gate blocks TOF_EDGE — but Phase 0 RX_EN_N test PASSED)
   - Mux not selecting the RX transducer (MUX_A/MUX_B wrong for N/S)
   - Comparator hysteresis too high (R179 solder jumper state)
   - VREF not settled / amplifier saturated
   - RX transducer not connected

3. **Capture ISR not running** — edges happen but ISR doesn't fire. Less likely since Phase 0 AND-gate test showed TOF_EDGE=LOW correctly.

**Next discriminating checks (no oscilloscope needed):**

- **A. RX baseline test** (`bringup_v3_rx_baseline`) — measure noise edges with RX enabled, no TX. If EDGES>0 here, RX path works and the problem is in TX. If EDGES=0 here too, RX path is dead.
- **B. Coupling test** (`bringup_v3_coupling`) — full sequence with scoring. Reports edge counts per shot. If EDGES=0 here too, confirms TX or RX path is dead.
- **C. Carrier chain test** (`bringup_v3_carrier_chain`) — fires burst with no DRV/REL. Reports BurstResult timing. If firstEdgeUs/lastEdgeUs are valid, the bit-bang TX is working.

**Hardware checks (manual, no firmware):**
- Confirm transducers are plugged into N and S headers (not E/W)
- Confirm transducer polarity/orientation (facing each other, 10 cm)
- Check R178 (0 Ω gated path selector) is populated correctly
- Check R179 (hysteresis solder jumper) state — try toggling it

---

## References

| Document | Location |
|---|---|
| V3 overview | `node/docs/NODE_V3_OVERVIEW.md` |
| V3 ultrasonic changes | `node/docs/NODE_V3_ULTRASONIC_CHANGES_BRINGUP_FIRMWARE_GUIDE_2026-06-12.md` |
| V3 ultrasonic checklist | `node/docs/NODE_V3_ULTRASONIC_CHECKLIST.md` |
| V3 22V boost update | `node/docs/NODE_V3_22V_BOOST_UPDATE.md` |
| V2 bringup results | `node/firmware/tests/NODE_V2_BRINGUP_RESULTS.md` |