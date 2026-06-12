# Node V3 — 22 V Boost Circuit Update Note

**Date:** 2026-06-09  
**Status:** V3 design decision — retain U49, upgrade L4, improve U49 decoupling  
**Affected:** Node V3 PCB, MT3608 22 V boost enable path, firmware GPIO5 polarity  
**Supersedes:** R2 and R3 recommendations in `mothership/docs/MT3608_BROWNOUT_DESIGN_NOTE.md`  
**See also:** `node/docs/NODE_HARDWARE_V2_CHECKLIST.md`, `node/docs/NODE-PCB-OVERVIEW.md`

---

## Summary

The Node V3 22 V boost circuit has been updated with a more robust inductor and improved local decoupling around the U49 inverter used for the `EN_22` control path.

The decision for this revision is to **retain U49** rather than remove it. U49 provides useful boot-safe behaviour by keeping the 22 V boost disabled during ESP32 startup, because GPIO5 is normally HIGH during boot and the inverter drives `EN_22` LOW.

The current approach is to solve the suspected primary V2 issue — MT3608 inductor saturation — while hardening the existing enable path instead of changing the enable logic and firmware polarity at the same time.

---

## Background

In Node V2, the 22 V MT3608 boost converter caused ESP32 brownouts when enabled from a cold battery start. The suspected primary cause was saturation of the original 22 µH inductor during MT3608 startup/inrush.

When the inductor saturated, VSYS dipped, which then disturbed `3V3_SYS`. This disturbance could also affect the U49 inverter and the `EN_22` enable signal, potentially creating a feedback loop:

```
MT3608 startup current
→ VSYS dip
→ 3V3_SYS dip/ripple
→ U49 output disturbance
→ EN_22 disturbance
→ MT3608 hiccup / unstable startup
```

Originally, one possible V3 option was to remove U49 and drive `EN_22` directly through a Schottky-isolated path (recommendations R2 and R3 in the MT3608 brownout design note). However, after review, U49 is being retained for now because it provides important startup protection.

---

## Decision: Retain U49 Instead of Schottky-Isolated Direct Drive

### Why U49 Is Retained

| Factor | Schottky-isolated direct drive (R2+R3) | Retained U49 with decoupling |
|---|---|---|
| Boot safety | GPIO5 defaults HIGH → EN_22 HIGH → boost ON at boot (requires firmware to disable quickly) | GPIO5 defaults HIGH → U49 inverts → EN_22 LOW → boost OFF at boot (safe) |
| Firmware change | Requires GPIO5 polarity inversion in all sketches | No change — existing polarity preserved |
| Component count | −4 (remove U49, R170, R172, R173), +3 (add Schottky, 100kΩ, 10µF) | +1 (add 1µF decoupling on U49 VCC) |
| Feedback loop risk | Broken by Schottky isolation | Reduced by improved decoupling; primary cause (inductor saturation) addressed separately |
| Complexity | New circuit topology, new firmware behaviour | Minimal change from V2, proven boot-safe logic |
| Test burden | Must verify boot behaviour, timing, and brownout all change simultaneously | Only need to verify inductor fix and decoupling improvement |

The key insight is that the **primary root cause** of the V2 brownout was inductor saturation, not the U49 feedback loop. The feedback loop was a secondary effect that only manifested because VSYS was already collapsing from inductor saturation. By fixing the inductor first and adding U49 decoupling, the feedback loop should be eliminated or reduced to negligible levels.

### What This Means for R2 and R3

- **R2 (Schottky diode isolation on EN_22):** Not applied in V3. Retained as a future option if EN_22 instability persists after testing.
- **R3 (Remove U49 from EN_22 path):** Not applied in V3. U49 is retained with improved decoupling.
- **R6 (U49 decoupling):** Applied in V3 as a 1 µF ceramic capacitor on U49 VCC, plus repositioned decoupling closer to supply pins.

---

## Applied / Planned V3 Changes

### 1. L4 22 V Boost Inductor Updated — ✅ APPLIED

The 22 V boost inductor has been changed to:

| Property | Value |
|---|---|
| LCSC part number | C41406986 |
| Value | 22 µH |
| Saturation current | ≥ 2.5 A (replaces 1.5 A I_sat SMMS0420-220M) |
| Package | 5.6 × 5.2 mm molded |
| Location | L4 (MT3608 boost inductor) |

This replaces the previous lower-margin inductor and directly addresses the suspected V2 failure mode of inductor saturation during MT3608 cold-start.

Also applied: MT3608 input capacitance upgraded on the VSYS side of the 22 V boost regulator:
- 100 µF ceramic (1210, 6.3 V X5R) from VSYS to GND — bulk energy reserve for cold-start inrush
- 100 nF ceramic from VSYS to GND — high-frequency switching decoupling

These are placed on the input side of the 22 V boost regulator to support cold-start inrush current and reduce high-frequency switching disturbance on VSYS. This complements the upgraded L4 inductor and is intended to reduce VSYS collapse during 22 V boost startup.

### 2. U49 Retained — ✅ DECISION MADE

U49 remains in the design:

| Property | Value |
|---|---|
| Designator | U49 |
| LCSC part number | C19829625 |
| Function | GPIO5 inverter for EN_22 control |
| Package | SOT-353 (SN74LVC1G04DRLR) |

Current logic remains:

```
GPIO5 HIGH → U49 output LOW → EN_22 LOW → 22 V boost OFF
GPIO5 LOW  → U49 output HIGH → EN_22 HIGH → 22 V boost ON
```

This preserves boot-safe behaviour and avoids enabling the 22 V boost during ESP32 startup.

### 3. U49 Local Decoupling Improved — ✅ APPLIED

A local 1 µF ceramic capacitor has been added at the 3.3 V power input of U49.

The decoupling capacitors have also been moved closer to the U49 supply pins to reduce the loop area and improve local rail stability.

Purpose:

- Reduce fast 3V3_SYS disturbance reaching U49
- Provide local energy storage during short rail dips
- Reduce chance of U49 output instability coupling into EN_22

| Property | Value |
|---|---|
| Component | 1 µF ceramic capacitor |
| Voltage rating | ≥ 6.3 V |
| Package | 0402 or 0603 |
| Placement | As close as possible to U49 VCC pin |
| Net | U49 VCC to GND |

### 4. EN_22 Test Point Present — ✅ RETAINED

A test point for `EN_22` is already present and will be retained.

This is important for bring-up because `EN_22` must be probed directly during cold 22 V boost enable testing.

### 5. EN_22 Hold Capacitor Not Added — ❌ DEFERRED

A dedicated `EN_22` hold/filter capacitor has **not** been added in this revision.

Reason:

- Avoid adding another enable-timing variable before re-testing the upgraded boost circuit.
- The design will first be tested with:
  - upgraded L4,
  - retained U49,
  - improved U49 local decoupling,
  - existing EN_22 routing.

If `EN_22` still glitches during testing, an EN_22 hold/filter capacitor can be reconsidered later.

### 6. 22V_SYS Bleed / Minimum Load Resistor Not Added — ❌ NOT ADDED

A permanent 22V_SYS bleed/minimum-load resistor (R5 in the MT3608 design note) has **not** been added to V3.

Reason:

- The 22 V rail is only required during ultrasonic measurement windows. Adding a permanent bleed load would waste power whenever the 22 V boost is enabled.
- The upgraded L4 inductor and improved MT3608 input capacitance should be tested first before adding a minimum-load component.
- If light-load regulation remains unstable, a temporary 22 kΩ or 10 kΩ resistor can be bodged from 22V_SYS to GND during bring-up to test whether a minimum load improves stability.

**Important:** The ultrasonic transducer should not be used as a direct DC load on 22V_SYS. It should remain driven only through the intended TX driver/burst circuit. The transducer is primarily a piezo/capacitive load and only loads the 22 V rail meaningfully during TX bursts, not as a steady idle load.

Status:

- R_22V_BLEED not added for V3.
- Reconsider only if bring-up shows 22V_SYS instability at idle or during measurement startup.

### 7. Comparator Supply Filtering Applied — ✅ APPLIED

The comparator 3.3 V supply has been locally filtered to reduce TX-induced supply noise reaching the comparator.

**Applied change:**
- Added 4.7 Ω series resistor between 3V3_SYS and the comparator VCC supply node
- Added local 100 nF decoupling capacitor on the comparator side of the resistor
- Added local 1 µF ceramic capacitor on the comparator side of the resistor
- The filtered comparator supply node should be treated as a local clean supply island: **3V3_COMP**

**Purpose:**
- Reduce fast 3V3_SYS disturbances caused by TX burst / 22 V boost activity
- Improve comparator supply stability
- Reduce false COMP_RAW transitions caused by supply noise

| Property | Value |
|---|---|
| Series resistor | 4.7 Ω (0402 or 0603) |
| Decoupling cap (HF) | 100 nF ceramic ≥6.3 V (0402 or 0603) |
| Decoupling cap (bulk) | 1 µF ceramic ≥6.3 V (0402 or 0603) |
| Filtered supply net | 3V3_COMP |
| Source supply | 3V3_SYS through 4.7 Ω resistor |

**Layout requirements:**
- 100 nF and 1 µF capacitors must be placed close to comparator VCC/GND pins
- The capacitors must be on the comparator side of the 4.7 Ω resistor
- No noisy digital loads should be connected to the filtered comparator supply node (3V3_COMP)

**Bring-up check:**
Probe 3V3_SYS, 3V3_COMP, COMP_RAW, and TOF_EDGE during TX burst and RX blanking tests.

### 8. TLV9062 RX Amplifier Supply Filtering Applied — ✅ APPLIED

Added local supply filtering for the TLV9062 RX amplifier.

**Applied change:**
- Added 4.7 Ω series resistor between 3V3_SYS and TLV9062 VCC
- Created local filtered node: **3V3_RXAMP**
- Added local 100 nF and 1 µF decoupling capacitors from 3V3_RXAMP to GND
- Capacitors are placed on the TLV9062 side of the resistor and close to the IC supply pins

**Purpose:**
- Reduce TX/boost-induced 3V3_SYS disturbance reaching the RX amplifier
- Reduce analog feedthrough, amplifier recovery artifacts, and false downstream comparator triggering
- Create a small local analog supply island without needing a full separate 3V3_A rail

| Property | Value |
|---|---|
| Series resistor | 4.7 Ω (0402 or 0603) |
| Decoupling cap (HF) | 100 nF ceramic ≥6.3 V (0402 or 0603) |
| Decoupling cap (bulk) | 1 µF ceramic ≥6.3 V (0402 or 0603) |
| Filtered supply net | 3V3_RXAMP |
| Source supply | 3V3_SYS through 4.7 Ω resistor |

**Layout requirements:**
- 100 nF and 1 µF capacitors must be placed close to TLV9062 VCC/GND pins
- The capacitors must be on the TLV9062 side of the 4.7 Ω resistor
- No noisy digital loads should be connected to the filtered amplifier supply node (3V3_RXAMP)

**Bring-up check:**
Probe 3V3_SYS, 3V3_RXAMP, RX_AMP, COMP_RAW, and TOF_EDGE during TX burst and receive-window tests.

### 9. VREF Filtering Applied — ✅ APPLIED

The ultrasonic mid-rail reference has been improved.

**Applied change:**
- Added 220 Ω series resistor from 3V3_SYS into the VREF divider supply path
- Retained 47 kΩ / 47 kΩ divider
- Main VREF node has 100 nF + 1 µF + 4.7 µF to GND
- Added 100 nF from VREF to GND near the comparator reference input
- Added local 100 nF VREF decoupling near the TLV9062 preamp/bandpass bias networks feeding R101/R103 and R104/R105

**Result:**
- VREF remains approximately mid-rail at ~1.65 V
- The added capacitance improves reference stability during TX burst and boost switching events
- The 220 Ω resistor provides an additional supply-side isolation/tuning point
- Distributed local decoupling reduces noise pickup on the longer VREF route
- Comparator threshold is stabilised locally at the comparator reference input
- Mid-rail bias used by the RX amplifier/filter stages is stabilised locally

**Implementation notes:**
- These capacitors connect only from VREF to GND
- No extra capacitance has been added to ultrasonic signal nodes such as ST1_IN, ST1_OUT, ST2_IN, RX_AMP, COMP_RAW
- Because total VREF capacitance is now several µF, firmware should allow VREF to settle before ultrasonic measurement. A 500 ms to 1 s delay after analog power-up is recommended before firing the first ultrasonic burst

| Property | Value |
|---|---|
| Series resistor | 220 Ω (0402 or 0603) |
| Divider | 47 kΩ / 47 kΩ from filtered supply to GND |
| Filtering | 100 nF + 1 µF + 4.7 µF ceramic from VREF to GND |
| VREF target | ~1.65 V (mid-rail) |

**Optional future improvement:**
- Add 100 nF from the node between the 220 Ω resistor and the upper 47 kΩ divider resistor to GND if space allows

**Bring-up check:**
Probe VREF at the main generator, comparator input, and TLV9062 bias area during TX burst. VREF should remain stable and free of TX-correlated spikes or steps at all three probe points.

### 10. 74HC4052 Mux Supply Filtering Applied — ✅ APPLIED

Added local supply filtering for the ultrasonic 74HC4052 mux.

**Applied change:**
- Added 4.7 Ω series resistor between 3V3_SYS and U42 VCC
- Created a local filtered mux supply node: **3V3_MUX**
- Existing C101 100 nF decoupling remains on the mux side of the resistor and should be placed close to U42 VCC/GND

**Purpose:**
- Reduce 3V3_SYS switching disturbance reaching the analog mux
- Reduce TX/boost-induced noise coupling through the RX channel selection path
- Improve RX-disabled and receive-window analog stability

| Property | Value |
|---|---|
| Series resistor | 4.7 Ω (0402 or 0603) |
| Existing decoupling | C101 100 nF ceramic (on mux side of resistor) |
| Filtered supply net | 3V3_MUX |
| Source supply | 3V3_SYS through 4.7 Ω resistor |

**Important implementation notes:**
- The node after the 4.7 Ω resistor must not also be labelled 3V3_SYS, otherwise the resistor will be bypassed
- RX clamp diodes D8/D9 should remain tied to normal 3V3_SYS, not 3V3_MUX, so clamp transient current is not injected into the filtered mux supply
- U50/U51 digital blanking gates remain on 3V3_SYS

**Bring-up check:**
Probe 3V3_SYS, 3V3_MUX, RX_IN, COMP_RAW, and TOF_EDGE during TX burst and receive-window tests.

---

## Firmware Impact

No firmware polarity change is required for the current retained-U49 option.

Current firmware behaviour remains:

```
GPIO5 LOW  = 22 V boost ON  (TX_22V_EN_N active-low)
GPIO5 HIGH = 22 V boost OFF (default safe state at boot)
```

This is unchanged from Node V2.

**Important:** If U49 is removed in a future revision and replaced by a direct Schottky-isolated drive, the GPIO5 polarity would need to be inverted in firmware:

```
GPIO5 HIGH = 22 V boost ON   (direct drive, no inverter)
GPIO5 LOW  = 22 V boost OFF
```

All PlatformIO build flags referencing `PIN_TX_22V_EN_N` and `TX_22V_EN_N` remain valid. The active-low naming convention matches the retained-U49 logic.

**VREF settling delay:** Because total VREF capacitance is now several µF, firmware should allow VREF to settle before ultrasonic measurement. A 500 ms to 1 s delay after analog power-up is recommended before firing the first ultrasonic burst. This delay should be added to the ultrasonic bring-up sketches and production firmware.

---

## Bring-Up Test Requirements

After applying these changes, perform a cold-start 22 V boost test from battery power.

### Recommended Probe Points

| Probe | Expected Behaviour |
|---|---|
| VSYS | Should remain above 2.7 V during boost enable |
| 3V3_SYS | Should remain stable (±5%) during boost enable |
| U49 VCC | Should remain clean with local 1 µF decoupling |
| GPIO5 | Should be clean digital level |
| U49 output | Should transition cleanly LOW→HIGH when GPIO5 goes LOW |
| EN_22 | Should go HIGH and remain stable when boost is enabled |
| 22V_SYS | Should regulate close to 22 V (not 14-20 V fluctuation) |
| ESP32 reset/boot log | No POWERON_RESET or brownout during boost enable |

### Test Condition

```
Battery around 4.1 V
22 V boost disabled at boot
Enable boost from firmware
Observe startup behaviour
```

### Pass Criteria

- ESP32 does not brown out or reset
- VSYS does not collapse
- 3V3_SYS remains stable enough for ESP32 operation
- U49 output remains clean (no oscillation or ringing)
- EN_22 remains stable HIGH while boost is enabled
- 22V_SYS regulates close to target voltage
- No repeated MT3608 hiccuping/oscillation

### Fail Indicators

- ESP32 POWERON_RESET
- VSYS dip large enough to disturb 3V3_SYS
- U49 output oscillation
- EN_22 wobble/dropout
- 22V_SYS unstable or pulsing heavily
- MT3608 overheating

### If Tests Fail

If the inductor upgrade and U49 decoupling do not fully resolve the brownout or EN_22 instability:

1. **Add EN_22 hold capacitor** — 10 µF ceramic from EN_22 to GND, as originally recommended in R2 of the MT3608 brownout design note.
2. **Add Schottky diode isolation** — BAT54S from GPIO5 to EN_22 with 100 kΩ pulldown, as per R2. This can be applied with U49 still in circuit (Schottky between U49 output and EN_22).
3. **Remove U49 and switch to direct Schottky drive** — As per R2+R3, but requires firmware polarity change.
4. **Add VSYS bulk capacitor** — 470–1000 µF electrolytic/polymer across VSYS/GND, as per R4.
5. **Add 22V_SYS minimum load resistor** — 10 kΩ or 22 kΩ from 22V_SYS to GND, as per R5. Test with a bodge first; only add to the PCB if the minimum load is confirmed necessary.

---

## V3 BOM Change Summary

| Action | Component | Value | Package | Notes |
|--------|-----------|-------|---------|-------|
| Change | L4 | 22 µH ≥2.5 A I_sat | 5.6×5.2 mm molded | LCSC C41406986, replaces SMMS0420-220M |
| Change | MT3608 input cap (bulk) | 1210 100 µF 6.3 V X5R | 1210 | Upgraded from 22 µF, VSYS input |
| Add | MT3608 input cap (HF decoupling) | 100 nF ceramic ≥6.3 V | 0402/0603 | VSYS input high-frequency decoupling |
| Add | C_new | 1 µF ceramic ≥6.3 V | 0402/0603 | U49 VCC decoupling, placed close to U49 |
| Add | R_comp_filt | 4.7 Ω | 0402/0603 | Comparator VCC supply filter resistor (3V3_SYS → 3V3_COMP) |
| Add | C_comp_hf | 100 nF ceramic ≥6.3 V | 0402/0603 | Comparator VCC HF decoupling on 3V3_COMP |
| Add | C_comp_bulk | 1 µF ceramic ≥6.3 V | 0402/0603 | Comparator VCC bulk decoupling on 3V3_COMP |
| Add | R_rxamp_filt | 4.7 Ω | 0402/0603 | TLV9062 VCC supply filter resistor (3V3_SYS → 3V3_RXAMP) |
| Add | C_rxamp_hf | 100 nF ceramic ≥6.3 V | 0402/0603 | TLV9062 VCC HF decoupling on 3V3_RXAMP |
| Add | C_rxamp_bulk | 1 µF ceramic ≥6.3 V | 0402/0603 | TLV9062 VCC bulk decoupling on 3V3_RXAMP |
| Add | R_vref_filt | 220 Ω | 0402/0603 | VREF divider supply filter resistor (3V3_SYS → VREF divider) |
| Add | C_vref_bulk | 4.7 µF ceramic ≥6.3 V | 0805/1206 | VREF bulk decoupling to GND |
| Add | C_vref_comp | 100 nF ceramic ≥6.3 V | 0402/0603 | Local VREF decoupling near comparator reference input |
| Add | C_vref_rxamp | 100 nF ceramic ≥6.3 V | 0402/0603 | Local VREF decoupling near TLV9062 preamp/bandpass bias (R101/R103, R104/R105) |
| Add | R_mux_filt | 4.7 Ω | 0402/0603 | 74HC4052 mux VCC supply filter resistor (3V3_SYS → 3V3_MUX) |
| Retain | U49 | SN74LVC1G04DRLR | SOT-353 | LCSC C19829625, NOT removed |
| Retain | R170 | 2 kΩ | 0402/0603 | U49 output → EN_22 |
| Retain | R172 | 100 kΩ | 0402/0603 | EN_22 pulldown |
| Retain | R173 | 1 kΩ | 0402/0603 | GPIO5 → U49 input |

**Net BOM impact:** +13 components (1 µF U49 decoupling cap, 100 nF VSYS input decoupling cap, 4.7 Ω comparator filter resistor, 100 nF comparator HF decoupling, 1 µF comparator bulk decoupling, 4.7 Ω RX amplifier filter resistor, 100 nF RX amplifier HF decoupling, 1 µF RX amplifier bulk decoupling, 220 Ω VREF filter resistor, 4.7 µF VREF bulk decoupling, 100 nF VREF decoupling near comparator, 100 nF VREF decoupling near TLV9062 bias, 4.7 Ω mux supply filter resistor), 2 value/package changes (L4, input bulk cap). No components removed. C101 (100 nF mux decoupling) is repositioned to the mux side of the 4.7 Ω resistor but is not a new component.

---

## Relationship to Other V3 Recommendations

| Original Recommendation (MT3608 Design Note) | V3 Status | Notes |
|---|---|---|
| R1: Replace L4 inductor | ✅ Applied | LCSC C41406986, 22 µH ≥2.5 A I_sat |
| R2: Schottky diode isolation on EN_22 | ❌ Deferred | Not needed if U49 decoupling + inductor fix resolves instability |
| R3: Remove U49 from EN_22 path | ❌ Not applied | U49 retained for boot-safe behaviour |
| R4: Add VSYS bulk capacitor | ⬜ Optional | 470–1000 µF footprint can be added as DNP if board space allows; current 100 µF + 100 nF local input capacitance is the main required change |
| R5: Add 10 kΩ minimum load on 22V_SYS | ❌ Not added | Not added for V3; reconsider only if bring-up shows 22V_SYS instability at idle or during measurement startup |
| R6: Add U49 decoupling | ✅ Applied | 1 µF ceramic on U49 VCC, moved closer to pins |
| R7: Add EN_22 test point | ✅ Already present | Retained from V2 |
| R8: Evaluate L7 (5 V boost inductor) | ⬜ Optional | Lower priority, 5 V boost has not exhibited issues |

---

## Current Decision

For Node V3, the 22 V boost enable circuit will remain based on U49.

The design strategy is:

1. **Keep the proven boot-safe enable logic.** U49 inverts GPIO5 so the 22 V boost is OFF at boot.
2. **Fix the suspected primary cause by upgrading L4.** The 1.5 A I_sat inductor was saturating at the MT3608's 2 A switch current limit.
3. **Upgrade MT3608 input decoupling.** 100 µF + 100 nF ceramic on VSYS input side to support cold-start inrush and reduce high-frequency switching disturbance on VSYS.
4. **Improve U49 supply decoupling.** Add 1 µF ceramic close to U49 VCC to reduce 3V3_SYS ripple coupling into the inverter.
5. **Add comparator supply filtering.** 4.7 Ω series resistor + 100 nF + 1 µF local decoupling creates a filtered 3V3_COMP supply island, reducing TX-induced supply noise reaching the comparator.
6. **Add RX amplifier supply filtering.** 4.7 Ω series resistor + 100 nF + 1 µF local decoupling creates a filtered 3V3_RXAMP supply island, reducing TX-induced supply noise reaching the RX amplifier and downstream comparator.
7. **Improve VREF filtering.** Add 220 Ω series resistor in VREF divider supply path + 4.7 µF bulk capacitance on VREF to GND. Improves reference stability during TX burst and boost switching events.
8. **Add mux supply filtering.** 4.7 Ω series resistor between 3V3_SYS and U42 VCC creates a filtered 3V3_MUX supply island, reducing TX/boost-induced noise coupling through the RX channel selection path. RX clamp diodes (D8/D9) remain on 3V3_SYS; U50/U51 digital gates remain on 3V3_SYS.
9. **Retain EN_22 test access.** The existing test point is kept for bring-up probing.
10. **Avoid adding an EN_22 hold capacitor until testing proves it is needed.** Minimise simultaneous variables changed between V2 and V3.
11. **Do not add a 22V_SYS bleed resistor.** The 22 V rail is only active during measurement windows; a permanent bleed wastes power. Test with upgraded inductor and input capacitance first. Bodge a temporary resistor during bring-up if needed.

This reduces the number of simultaneous variables changed between V2 and V3 while still addressing the most likely root cause of the V2 brownout problem.