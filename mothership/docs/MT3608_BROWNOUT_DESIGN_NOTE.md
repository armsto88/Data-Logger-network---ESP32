# MT3608 22V Boost Converter — Brownout & Regulation Design Note

**Date:** 2026-06-02  
**Updated:** 2026-06-06 — inductor and input cap replaced  
**Status:** In progress — hardware fix applied, re-test pending  
**Affected:** Node V2/V3 PCB, all future boards using MT3608 for 22V ultrasonic TX rail

---

## Problem

The MT3608 boost converter on Node V2 causes the ESP32 to brown out (POWERON_RESET) when enabled from a cold start on battery power at 4.1V.

### Root Cause

The MT3608 draws 1-2A inrush current when charging the 22V output capacitor (100µF) from ~0V. Combined with the ESP32's ~100mA operating current, this exceeds the battery's current capability, causing VSYS to drop below the ESP32's minimum operating voltage (~2.7V).

### Measured Behaviour

| Condition | Result |
|---|---|
| Battery 4.1V, boost cold enable | ESP32 POWERON_RESET (complete power loss) |
| Battery 4.1V, boost re-enable after partial charge | Survives (cap partially charged, lower inrush) |
| Battery 4.1V, deep sleep during boost enable | Survives (ESP32 draws ~10µA, MT3608 gets all battery current) |
| Software soft-start (100µs-1ms pulses) | Fails — even 100µs pulses cause brownout after ~40 pulses |
| 22V_SYS voltage (no load, boost on) | Fluctuates 14-20V (possible pulse-skip mode) |

### Why Soft-Start Failed

The MT3608's internal soft-start resets every time the enable pin is toggled. Short pulses don't allow the converter to complete a soft-start cycle — they just repeatedly hit the inrush. As the output cap charges higher, each pulse draws more current (higher duty cycle needed), eventually exceeding the battery's capability.

---

## Root Cause Analysis (Updated 2026-06-03)

### Primary Root Cause: Inductor Saturation

The **SMMS0420-220M** inductor (22µH, 1.5A saturation current) is the primary root cause of ALL observed issues:

- **Brownout on cold enable:** The MT3608's switch current limit is 2A. When the inductor saturates at 1.5A, it loses inductance and becomes essentially a wire. Current spikes uncontrollably, VSYS collapses, and the ESP32 browns out.
- **22V regulation instability:** A saturated inductor cannot maintain proper boost regulation. The output fluctuates 14-20V instead of regulating at 22V.
- **3V3_SYS ripple:** VSYS dips from inductor saturation propagate through the LDO to 3V3_SYS, causing the entire 3.3V rail to ripple.
- **EN_22 oscillation:** 3V3_SYS ripple feeds back to EN_22 through two paths: (1) U49 inverter power supply → U49 output oscillation → EN_22, and (2) GPIO5 output level tracking 3V3_SYS → EN_22.
- **MT3608 overheating:** Rapid on/off cycling from EN_22 oscillation causes excessive switching losses.

### Why V1 Did Not Have This Problem

V1 used the same MT3608 circuit but had a **reversed diode** in the TX enable path. The diode was installed backwards, so EN_22 never received a valid enable signal and the boost converter was never properly turned on. After a rework (pads bridged, bypassing the diode), the boost came up but only to ~18V, suggesting marginal operation even then. The brownout and regulation issues are fundamentally MT3608 inrush and inductor problems that V1 never exposed because the enable path was broken.

### Secondary Issue: EN_22 Feedback Loop

Even after bypassing the U49 inverter (R173 removed, GPIO5→EN_22 direct), the feedback loop persists through a different path:

```
MT3608 switches → VSYS dips → LDO output (3V3_SYS) dips → GPIO5 output dips → EN_22 dips → MT3608 hiccups → repeat
```

GPIO5's output HIGH level tracks 3V3_SYS. When 3V3_SYS dips, GPIO5 dips, and EN_22 dips with it. The 10µF cap on EN_22 cannot hold it up because the dips are sustained, not transient.

**Proposed fix:** Schottky diode (BAT54 or 1N5817) in series between GPIO5 and EN_22 (anode on GPIO5, cathode on EN_22), with 100kΩ pulldown on EN_22 and 10µF cap on EN_22. When 3V3_SYS dips, the diode becomes reverse-biased and the cap holds EN_22 HIGH, breaking the feedback loop.

---

## Workarounds Tested

| Approach | Works? | Trade-off |
|---|---|---|
| Deep sleep during boost enable | ✅ | Adds 2-3s, serial disconnects, clunky UX |
| Accept brownout + RTC flag auto-resume | ✅ | Adds ~5s reboot, works reliably |
| Software soft-start pulses | ❌ | Brownout regardless of pulse width |
| Disable brownout detector | ❌ | Voltage drops below ESP32 minimum, not just BOD threshold |
| U49 inverter bypass (GPIO5→EN_22 direct) | ❌ | Same feedback loop through GPIO5 output level tracking 3V3_SYS |

---

## Production Design Options

### Option 1: Accept brownout + auto-resume (firmware-only)

Enable boost → brownout → reboot → RTC flag → re-enable (cap partially charged). Total overhead: ~5s per wake cycle.

**Pros:** No hardware changes, reliable  
**Cons:** Adds 5s to every wake cycle, serial output lost during reboot

### Option 2: Deep sleep during boost enable (firmware-only)

Enable boost → deep sleep 2-3s → wake → boost ready. Total overhead: 2-3s per wake cycle.

**Pros:** No hardware changes, faster than option 1  
**Cons:** Serial disconnects during sleep, adds complexity

### Option 3: Never disable boost during measurement cycle (firmware-only)

Enable boost once (using option 1 or 2), keep on for entire measurement cycle (~1-2s for 4-axis wind), disable after. Only one brownout event per wake cycle.

**Pros:** Minimises brownout events, matches expected usage pattern  
**Cons:** 22V rail powered for longer, slightly higher power consumption

### Option 4: Add input bulk capacitor (hardware)

Add 1000µF+ electrolytic across VSYS/GND to absorb inrush current.

**Pros:** Eliminates brownout entirely, simplest firmware  
**Cons:** BOM cost, board space, ESR considerations

### Option 5: Add series resistor + bypass FET (hardware)

Limit inrush with small series resistor (1-2Ω), bypass with MOSFET after cap charged.

**Pros:** Controlled inrush, no brownout  
**Cons:** Extra components, control complexity, FET gate drive needed

### Option 6: Replace MT3608 with soft-start capable converter (hardware)

Use a boost converter with configurable soft-start (e.g., TPS61088 with SS pin).

**Pros:** Proper soft-start, no brownout  
**Cons:** Different pinout, may need layout change, higher cost

### Option 7: Replace inductor with higher saturation current (hardware)

Replace SMMS0420-220M (22µH, 1.5A Isat) with a 22µH inductor rated ≥2.5A saturation current.

**Candidate parts:**
| Part | Isat | DCR | Size | Notes |
|---|---|---|---|---|
| Würth 7440455220 | 2.8A | 68mΩ | 5×5mm | Good availability |
| TDK SLC7649 series | 3A | 45mΩ | 7×7mm | Larger footprint |
| Sunlord SWPA4018S series | 2.6A | ~80mΩ | 4×4mm | Closest to current footprint |

**Pros:** Eliminates root cause of brownout, regulation instability, and EN_22 oscillation  
**Cons:** May need footprint change, slightly higher cost

**Recommended:** Combine with Schottky diode isolation on EN_22 (defense in depth).

---

## 22V Regulation Issue

With no load on the 22V rail, the MT3608 output fluctuates between 14-20V (measured on DMM). This is likely pulse-skip / burst mode behaviour — the converter can't regulate well with no minimum load.

### Investigation Needed

1. Add 10kΩ minimum load across 22V_SYS (~2mA) — does voltage stabilise at ~22V?
2. Check feedback divider values (R1/R2 on MT3608 FB pin) against schematic
3. Measure with oscilloscope to distinguish switching ripple from burst-mode oscillation
4. Test regulation under actual transducer load (during burst firing)

### Impact on Ultrasonic Measurements

If the 22V rail is at 14-20V instead of 22V, the TX burst amplitude will be lower and more variable. This could affect:
- Detection range and reliability
- TOF measurement consistency
- Wind speed calculation accuracy

The transducer only draws current during bursts (~50mA for ~300µs), so the rail may stabilise under load even if it fluctuates at no-load.

**Update (2026-06-03):** The 14-20V fluctuation persists even with U49 inverter bypassed (GPIO5→EN_22 direct). EN_22 and 3V3_SYS both fluctuate. Root cause is inductor saturation causing VSYS collapse, which propagates through the entire power chain. Replacing the inductor with a ≥2.5A saturation part is expected to resolve both the brownout and regulation issues.

---

## V3 Hardware Recommendations

Based on V2 bringup findings, the following changes are recommended for the next PCB revision. Each change is classified as **Required** (must-fix for reliable operation), **Recommended** (defense-in-depth, strongly advised), or **Optional** (nice-to-have for robustness).

### R1. Replace 22V Boost Inductor — **Required** ✅ **APPLIED**

**Problem:** SMMS0420-220M (22µH, 1.5A Isat) saturates at the MT3608's 2A switch current limit, causing VSYS collapse, 3V3_SYS ripple, EN_22 oscillation, brownout, and 22V regulation failure.

**Change applied (2026-06-06):** Replaced L4 with a 22µH 2.5A I_sat 5.6×5.2mm molded inductor. Also upgraded MT3608 input cap from 22µF to 1210 100µF 6.3V X5R ceramic for improved cold-start energy reserve.

**Status:** Hardware change applied. Re-test needed to confirm brownout is resolved and 22V rail is stable.

| Candidate | Isat | DCR | Size | LCSC | Notes |
|---|---|---|---|---|---|
| Würth 7440455220 | 2.8A | 68mΩ | 5×5mm | — | Good availability, proven brand |
| TDK SLC7649 series | 3A | 45mΩ | 7×7mm | — | Lowest DCR, larger footprint |
| Sunlord SWPA4018S series | 2.6A | ~80mΩ | 4×4mm | C2929402 | Closest to current footprint |

**Footprint note:** The current SMMS0420 uses a 4.2×4.2mm footprint. Check that the replacement inductor's footprint is compatible or add a new land pattern. The TDK SLC7649 (7×7mm) will require a layout change; the Sunlord SWPA4018S (4×4mm) is closest in size.

**Also apply to L7** (5V boost inductor) if it uses the same part, though the 5V boost has not exhibited issues at its lower output voltage and current.

---

### R2. Schottky Diode Isolation on EN_22 — **Required**

**Problem:** Even with U49 inverter bypassed, 3V3_SYS ripple couples through GPIO5's output level to EN_22, creating a feedback loop that destabilises the MT3608.

**Change:** Add a Schottky diode (BAT54 or 1N5817) in series between the enable source and EN_22, with a 100kΩ pulldown on EN_22 and a 10µF ceramic cap on EN_22 to GND.

```
Enable source ──►|── EN_22 ── 10µF ── GND
(Schottky, anode)    │
                  100kΩ ── GND
```

**How it works:**
- Enable HIGH: Diode conducts, EN_22 ≈ 3.0V (3.3V − 0.3V Schottky Vf). MT3608 enabled.
- 3V3_SYS dips: Diode becomes reverse-biased. 10µF cap holds EN_22 HIGH. MT3608 stays enabled.
- Enable LOW: Diode reverse-biased. 100kΩ pulldown discharges EN_22 cap. MT3608 disabled.

**Turn-off time:** 10µF × 0.7V / (100kΩ × ln(3)) ≈ 25ms. This is acceptable — the boost converter is disabled for seconds at a time between measurement cycles.

**BOM impact:** +1 Schottky diode, +1 100kΩ resistor, +1 10µF ceramic cap. The 10µF cap is already on the V2 board as a bodge; make it a proper footprint.

---

### R3. Remove U49 Inverter from EN_22 Path — **Required**

**Problem:** U49 (SN74LVC1G04DRLR) oscillates when 3V3_SYS has ripple from MT3608 switching noise. Even with R1 (inductor replacement), the inverter remains a noise coupling path.

**Change:** Remove U49 and R173 from the EN_22 path. Drive EN_22 directly from GPIO5 through the Schottky diode (R2 above). This eliminates the inverter as a noise coupling path entirely.

**Boot safety:** GPIO5 is a strapping pin that defaults HIGH at boot. With the Schottky diode + 10µF cap, EN_22 will be HIGH at boot, meaning the boost converter will start immediately. This is acceptable because:
- With R1 (higher Isat inductor), the inrush current will not cause VSYS collapse
- The deep-sleep enable sequence can still be used if needed
- The 10µF cap on EN_22 provides a soft turn-on (EN_22 rises as the cap charges through the Schottky)

**Alternative:** If boot-safe disable is still required, replace U49 with a P-FET high-side switch on the MT3608's VIN, controlled by an active-low GPIO. This isolates the enable path from 3V3_SYS entirely. However, this adds complexity and the Schottky diode approach (R2) should be sufficient.

**BOM impact:** −1 inverter (U49), −1 2kΩ resistor (R170), −1 100kΩ resistor (R172), −1 1kΩ resistor (R173). Net BOM reduction.

---

### R4. Add Input Bulk Capacitance on VSYS — **Recommended** ✅ **PARTIALLY APPLIED**

**Problem:** The MT3608 draws 1-2A inrush when charging the 22V output cap from 0V. Even with a higher-Isat inductor, the battery's internal resistance can cause VSYS to dip under heavy transient loads.

**Change applied (2026-06-06):** MT3608 input cap upgraded from 22µF to 1210 100µF 6.3V X5R ceramic. This provides ~5× more local energy storage at the IC pins.

**Remaining recommendation:** Consider adding a footprint for a 470µF–1000µF electrolytic or polymer capacitor across VSYS/GND as additional bulk energy storage. The 100µF ceramic is a significant improvement but may still benefit from additional bulk capacitance for cold-start scenarios.

**BOM impact:** +1 electrolytic/polymer cap (470µF, 6.3V or 10V). May need a small layout area increase.

---

### R5. Add Minimum Load Resistor on 22V_SYS — **Recommended** ❌ **NOT ADDED IN V3**

**Problem:** With no load, the MT3608 enters pulse-skip/burst mode, causing 22V_SYS to fluctuate 14-20V. Even with inductor replacement (R1), light-load regulation may still be marginal.

**Change:** Add a 10kΩ 1/4W resistor from 22V_SYS to GND as a minimum bleed load (~2.2mA at 22V, ~48mW dissipation). This forces the MT3608 into continuous PWM mode and stabilises the output voltage.

**Power budget impact:** 2.2mA × 22V = 48mW continuous when boost is enabled. During the ~15-62s measurement window, this adds ~0.75-3.1mJ per wake cycle. Negligible compared to the transducer burst current.

**Alternative:** Use a 22kΩ resistor (~1mA) and verify stability. If 1mA is sufficient, the power waste is halved.

**BOM impact:** +1 10kΩ 1/4W resistor.

**V3 decision (2026-06-10):** Not added for V3. The 22 V rail is only required during ultrasonic measurement windows, and a permanent bleed load wastes power whenever the boost is enabled. The upgraded L4 inductor and improved MT3608 input capacitance should be tested first. If light-load regulation remains unstable, a temporary 22 kΩ or 10 kΩ resistor can be bodged from 22V_SYS to GND during bring-up to test whether a minimum load improves stability. The ultrasonic transducer should not be used as a direct DC load on 22V_SYS — it is a piezo/capacitive load that only loads the rail meaningfully during TX bursts. Reconsider only if bring-up shows 22V_SYS instability at idle or during measurement startup. See `node/docs/NODE_V3_22V_BOOST_UPDATE.md`.

---

### R6. Add Decoupling Cap Footprint on U49 Power Pins — **Optional**

**Problem:** If U49 is retained for any reason (e.g., boot-safe disable), it needs better power supply decoupling to prevent oscillation from 3V3_SYS ripple.

**Change:** Add a 100nF ceramic cap directly across U49's VCC and GND pins, placed as close as possible to the IC. Also add a 1µF ceramic cap on the same supply.

**Note:** This is only needed if U49 is retained. If R3 is implemented (remove U49), this change is unnecessary.

**BOM impact:** +1 100nF cap, +1 1µF cap (only if U49 is retained).

---

### R7. Add EN_22 Test Point — **Recommended**

**Problem:** EN_22 is a critical debug node that currently requires probing a component pad (R173 or U49 pin).

**Change:** Add a labelled test point for EN_22 on the PCB. This makes it easy to probe the MT3608 enable signal during bringup and production testing.

**BOM impact:** +1 test point (zero cost).

---

### R8. Review 5V Boost Inductor (L7) — **Optional**

**Problem:** L7 (5V boost inductor) uses the same SMMS0420-220M part. While the 5V boost has not exhibited issues (lower output voltage, lower inrush), it has the same saturation current limitation.

**Change:** If L4 is being replaced, evaluate whether L7 should also be upgraded to a higher-Isat part for consistency and margin. The 5V boost draws less current, so this is lower priority.

**BOM impact:** Same as R1 if applied.

---

## V2 Bringup Path (No Hardware Changes)

For V2 bringup with the current hardware, the recommended path is:

1. **Replace L4 inductor** with a ≥2.5A Isat part (bodge/rework on existing board)
2. **Add Schottky diode** on EN_22 (bodge on existing board)
3. **Keep U49 bypass** (R173 removed, GPIO5→EN_22 via Schottky)
4. **Use deep-sleep enable** for initial boost turn-on (firmware workaround)
5. **Test 22V_SYS stability** after inductor replacement — if stable, proceed with ultrasonic bringup

If inductor replacement alone resolves the brownout and regulation issues, the Schottky diode is still recommended as defense in depth.

## V3 Design Decision (2026-06-09)

**See also:** `node/docs/NODE_V3_22V_BOOST_UPDATE.md` — full V3 22 V boost circuit update note.

After review, the V3 design decision is to **retain U49** rather than remove it. This changes the status of R2 and R3:

- **R2 (Schottky diode isolation on EN_22):** ❌ **Not applied in V3.** Deferred. If EN_22 instability persists after inductor upgrade and U49 decoupling, this can be reconsidered.
- **R3 (Remove U49 from EN_22 path):** ❌ **Not applied in V3.** U49 is retained because it provides boot-safe behaviour (GPIO5 defaults HIGH at boot → U49 inverts → EN_22 LOW → boost OFF). Removing U49 would require a firmware polarity change and creates a boot-time risk where the boost converter starts immediately.
- **R6 (U49 decoupling):** ✅ **Applied in V3.** A 1 µF ceramic capacitor has been added on U49 VCC, placed close to the supply pins. This replaces the original R6 recommendation of 100 nF + 1 µF.

The rationale is that the **primary root cause** was inductor saturation (R1), not the U49 feedback loop. The feedback loop was a secondary effect that only manifested because VSYS was already collapsing. By fixing the inductor and adding U49 decoupling, the feedback loop should be eliminated or reduced to negligible levels, without changing the enable logic or firmware polarity.

This approach minimises the number of simultaneous variables changed between V2 and V3.

## V3 BOM Change Summary

| Change | Action | BOM Delta | Priority |
|---|---|---|---|
| R1 | Replace L4 with ≥2.5A Isat inductor | L4 changed | Required |
| R2 | Add Schottky diode + 100kΩ + 10µF on EN_22 | +3 components | Required |
| R3 | Remove U49, R170, R172, R173 from EN_22 path | −4 components | Required |
| R4 | Add 470µF input cap footprint on VSYS | +1 component | Recommended |
| R5 | Add 10kΩ bleed resistor on 22V_SYS | +1 component | Recommended |
| R6 | Add 100nF + 1µF on U49 VCC (if retained) | +2 components | Optional |
| R7 | Add EN_22 test point | +1 test point | Recommended |
| R8 | Evaluate L7 replacement | L7 possibly changed | Optional |

**Net BOM change:** −4 components removed (U49, R170, R172, R173), +5-7 components added. Net +1-3 components. Cost impact approximately neutral (removed inverter offsets added diode and caps).

---

## Use-Case Alignment Analysis

### Actual Measurement Cycle Timing

The ultrasonic anemometer's production measurement cycle is:

1. Node wakes from deep sleep (RTC alarm)
2. Enable 22V boost (TX_22V_EN_N = LOW)
3. Wait for boost warm-up (~5-20 ms)
4. For each axis pair (N↔S, E↔W):
   a. Select transducer direction
   b. Generate 40 kHz burst (~300 µs for 12 cycles)
   c. Apply RX blanking (200-800 µs)
   d. Capture first comparator edge (TOF ~427 µs at 20°C)
   e. Repeat in opposite direction
5. Collect 16-64 samples per direction, median-filter
6. Disable 22V boost (TX_22V_EN_N = HIGH)
7. Node goes back to deep sleep

**Total measurement window: ~15-62 seconds** (4 axis pairs × 2 directions × 16-64 shots × ~121.5 ms per shot)

**The 22V boost is enabled ONCE and held ON for the entire measurement window, then disabled. There is no on/off cycling during a single wake cycle.**

### Critical Brownout Scenario

The **only** brownout scenario that matters for production is:

```
Deep sleep wake → enable 22V boost → brownout?
```

After this initial enable, boost stays on for 15-62 seconds. There is no second enable event. The brownout issue discovered during bringup (cycling boost on/off in Test 3) does **not** represent the production use case.

### Regulation Scenario

The 22V rail must:
- Reach a stable voltage within 5-20 ms of enable
- Sustain that voltage for the full 15-62 second measurement window
- The no-load fluctuation (14-20V on DMM) may not matter if the rail stabilises under the intermittent transducer load during bursts

### Test Realignment Recommendations

| Current Test | Use-Case Alignment | Recommendation |
|---|---|---|
| Test 1: AND-gate blanking | ✅ Directly relevant | Keep as-is |
| Test 2: Precharge sweep | ✅ Directly relevant | Keep as-is — find minimum precharge for stable detection |
| Test 3: Split-TX independence | ⚠️ Partially relevant | Redesign: Phase A should test boost noise AFTER precharge (not during switching). Phase B (burst without boost) is a failure-mode test, not normal operation. Phase C is the real test. |
| Test 3 brownout testing | ❌ Misaligned | Stop testing boost on/off cycling. Instead, test the single-enable-from-deep-sleep scenario. |
| Test 8: RX-disabled baseline | ✅ Directly relevant | Keep as-is |
| Phase A2: Ambient noise with boost off | ⚠️ Marginal | In use, boost is ON during measurement. Test noise with boost ON and precharged instead. |

### New Priority Test: Single-Enable from Deep Sleep

This test directly matches the production use case:

1. Node enters deep sleep (simulating RTC alarm wake)
2. On wake, immediately enable 22V boost
3. Wait for precharge (5-50 ms)
4. Fire a single burst and check for detection
5. Hold boost on for 60 seconds, periodically checking 22V_SYS voltage
6. Disable boost

**Pass criteria:**
- No brownout on the single enable after deep-sleep wake
- 22V_SYS reaches stable voltage within precharge window
- Detection works during the measurement window
- 22V_SYS remains stable for the full 60-second hold

This test replaces the complex soft-start and cycling tests that were consuming bringup time without matching the real use case.

---

## References

- Node V2 Bringup Results: `firmware/nodes/bringup_v2/NODE_V2_BRINGUP_RESULTS.md`
- Ultrasonic bringup sketch: `firmware/nodes/bringup_v2/bringup_v2_ultrasonic.cpp`
- 22V boost test sketch: `firmware/nodes/bringup_v2/bringup_22v_boost_only.cpp`
- V2 pin mapping: `docs/NODE-PCB-OVERVIEW.md`, `docs/NODE_HARDWARE_V2_CHECKLIST.md`
- MT3608 datasheet: typical application circuit, FB divider calculation