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

---

## V3 BOM Change Summary

| Action | Component | Value | Package | Notes |
|--------|-----------|-------|---------|-------|
| Change | L4 | 22 µH ≥2.5 A I_sat | 5.6×5.2 mm molded | LCSC C41406986, replaces SMMS0420-220M |
| Change | MT3608 input cap (bulk) | 1210 100 µF 6.3 V X5R | 1210 | Upgraded from 22 µF, VSYS input |
| Add | MT3608 input cap (HF decoupling) | 100 nF ceramic ≥6.3 V | 0402/0603 | VSYS input high-frequency decoupling |
| Add | C_new | 1 µF ceramic ≥6.3 V | 0402/0603 | U49 VCC decoupling, placed close to U49 |
| Retain | U49 | SN74LVC1G04DRLR | SOT-353 | LCSC C19829625, NOT removed |
| Retain | R170 | 2 kΩ | 0402/0603 | U49 output → EN_22 |
| Retain | R172 | 100 kΩ | 0402/0603 | EN_22 pulldown |
| Retain | R173 | 1 kΩ | 0402/0603 | GPIO5 → U49 input |

**Net BOM impact:** +2 components (1 µF U49 decoupling cap, 100 nF VSYS input decoupling cap), 2 value/package changes (L4, input bulk cap). No components removed.

---

## Relationship to Other V3 Recommendations

| Original Recommendation (MT3608 Design Note) | V3 Status | Notes |
|---|---|---|
| R1: Replace L4 inductor | ✅ Applied | LCSC C41406986, 22 µH ≥2.5 A I_sat |
| R2: Schottky diode isolation on EN_22 | ❌ Deferred | Not needed if U49 decoupling + inductor fix resolves instability |
| R3: Remove U49 from EN_22 path | ❌ Not applied | U49 retained for boot-safe behaviour |
| R4: Add VSYS bulk capacitor | ⬜ Optional | 470–1000 µF footprint can be added as DNP if board space allows; current 100 µF + 100 nF local input capacitance is the main required change |
| R5: Add 10 kΩ minimum load on 22V_SYS | ⬜ Optional | Can be added if 22 V regulation is still unstable at light load |
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
5. **Retain EN_22 test access.** The existing test point is kept for bring-up probing.
6. **Avoid adding an EN_22 hold capacitor until testing proves it is needed.** Minimise simultaneous variables changed between V2 and V3.

This reduces the number of simultaneous variables changed between V2 and V3 while still addressing the most likely root cause of the V2 brownout problem.