# MT3608 22V Boost Converter — Brownout & Regulation Design Note

**Date:** 2026-06-02  
**Status:** Open — needs design decision before production ultrasonic firmware  
**Affected:** Node V2 PCB, all future boards using MT3608 for 22V ultrasonic TX rail

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

## Workarounds Tested

| Approach | Works? | Trade-off |
|---|---|---|
| Deep sleep during boost enable | ✅ | Adds 2-3s, serial disconnects, clunky UX |
| Accept brownout + RTC flag auto-resume | ✅ | Adds ~5s reboot, works reliably |
| Software soft-start pulses | ❌ | Brownout regardless of pulse width |
| Disable brownout detector | ❌ | Voltage drops below ESP32 minimum, not just BOD threshold |

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

---

## Recommendation

For V2 bringup: Use **Option 3** (accept brownout + keep boost on during measurement). This is the simplest path to get ultrasonic measurements working.

For V3 hardware: Evaluate **Option 4** (input bulk cap) or **Option 5** (series resistor + bypass FET) to eliminate the brownout entirely. Also investigate the regulation issue — if a minimum load resistor is needed, add it to the BOM.

---

## References

- Node V2 Bringup Results: `firmware/nodes/bringup_v2/NODE_V2_BRINGUP_RESULTS.md`
- Ultrasonic bringup sketch: `firmware/nodes/bringup_v2/bringup_v2_ultrasonic.cpp`
- 22V boost test sketch: `firmware/nodes/bringup_v2/bringup_22v_boost_only.cpp`
- V2 pin mapping: `docs/NODE-PCB-OVERVIEW.md`, `docs/NODE_HARDWARE_V2_CHECKLIST.md`
- MT3608 datasheet: typical application circuit, FB divider calculation