# Node V4 Ultrasonic Redesign — Shared Bidirectional Transducers

**Date:** 2026-07-13  
**Board:** Node V4 (proposed — not a full board restart, a focused ultrasonic sheet redesign)  
**Status:** Design direction confirmed — open questions remain on component values and GPIO count  
**Supersedes:** Node V3 ultrasonic connector and transducer topology (see `NODE_V3_OVERVIEW.md` §16–§18)

---

## 1. Background — Why V3 Cannot Do Ultrasonic Anemometry

Node V3 bring-up confirmed that all analog and power electronics work:

- 22 V boost enables without brownout ✅
- TX carrier chain reaches `TX_PULSE` at ~15 V average (continuous 40 kHz) ✅
- Directional P-MOS routes `TX_PULSE` to `TD_N_A` ✅
- RX analog chain produces ~1000 edges/sec noise floor ✅
- Boost ON does not suppress the RX chain (V3 supply filtering works) ✅
- AND-gate blanking blocks `TOF_EDGE` when `RX_EN_N=HIGH` ✅

But **zero acoustic detections** were observed across 98 shots. Root cause:

> The V3 board uses **separate TX and RX transducer nets** per direction (`TD_*` for TX, `RX_*` for RX). There are only 4 physical transducer connectors, all wired to the `TD_*` (TX) nets. The RX mux (`RX_*` nets) has no transducer connected. The transmitting transducer fires correctly, but the receiving transducer is on the wrong net — the RX mux listens on empty nets.

This is a **hardware design error**, not a firmware bug. The V3 board cannot perform reciprocal ultrasonic wind measurement without a redesign of the transducer connector and RX input topology.

**Confirmed decisions:**
- V3 board is not fixable for ultrasonic anemometry without PCB rework
- A focused redesign (V4 ultrasonic sheet) is needed — not a full board restart

---

## 2. Target Architecture — Four Shared Bidirectional Transducers

### Physical connectors

```
J_US_N   (2-pin)  →  US_N_A / US_N_B
J_US_E   (2-pin)  →  US_E_A / US_E_B
J_US_S   (2-pin)  →  US_S_A / US_S_B
J_US_W   (2-pin)  →  US_W_A / US_W_B
```

**4 transducers total.** Each transducer does both TX and RX through the same 2-pin connector.

### Measurement concept

```
N transducer transmits → S transducer receives (N→S)
S transducer transmits → N transducer receives (S→N)
E transducer transmits → W transducer receives (E→W)
W transducer transmits → E transducer receives (W→E)
```

Reciprocal TOF pairs give wind speed along each axis.

---

## 3. Core Redesign Problem

The 74HC4052 RX mux input pins are physical IC pins. Even when a mux channel is not selected, the unselected input pins still see whatever voltage is on those nets.

If a transducer net is driven with a 22 V TX pulse, and that same net is connected to a mux input pin, the mux input sees 22 V — even if that channel isn't selected.

**Requirement:** Every RX mux input pin must have protection from TX voltage.

The V3 post-mux protection (2.2 kΩ series + BAV99 clamps after the mux) only protects the amplifier side — it does NOT protect the mux input pins.

---

## 4. Per-Transducer Block (New)

For each direction X (N, E, S, W):

### 4.1 TX drive path

```
TX_PULSE
  → directional P-MOS (existing V3 design, keep)
  → US_X_A
  → transducer
  → US_X_B
  → switched TX return N-MOS to GND (NEW — replaces permanent 1 kΩ)
```

**Key change:** The permanent 1 kΩ from `US_X_B` to GND (V3's `R121`) must be removed. A permanent low-value resistor loads the receiver too heavily. Replace with a **switched TX return** N-MOS that is ON only during TX for that direction.

| State | TX P-MOS | TX return N-MOS | Damping MOSFET | Meaning |
|-------|----------|-----------------|----------------|---------|
| RX / idle | OFF | OFF | OFF | Transducer high-impedance into RX path |
| Transmit | ON | ON | OFF | 22 V carrier drives transducer |
| Damping | OFF | OFF | ON | Transducer shunted to kill ringdown |
| **Forbidden** | ON | ON | ON | TX fights damping — never allow |

### 4.2 Damping path

```
US_X_A → damping MOSFET → US_X_B
```

- `REL_X LOW` = damping OFF
- `REL_X HIGH` = transducer shunted/damped
- Active only briefly after burst (10–50 µs)

### 4.3 RX protection path (NEW — per mux input)

```
US_X_A
  → R_RX_XA (22 kΩ, start)
  → RX_X_A_PROT
  → BAV99 clamp to GND and 3V3
  → 74HC4052 input (Y channel)

US_X_B
  → R_RX_XB (22 kΩ, start)
  → RX_X_B_PROT
  → BAV99 clamp to GND and 3V3
  → 74HC4052 input (Y channel)
```

**Every 74HC4052 input gets its own series resistor and clamp.** No mux input can ever see raw 22 V.

**Resistor footprint values to test:**

| Value | Clamp current (22V → ~4V) | RX attenuation | Notes |
|-------|---------------------------|---------------|-------|
| 10 kΩ | ~1.8 mA | Low | Stronger RX signal, higher clamp current |
| 22 kΩ | ~0.8 mA | Moderate | **Starting value** — balanced |
| 47 kΩ | ~0.38 mA | Higher | Lower clamp current, weaker RX |

Start with **22 kΩ**. Tunable after scope testing.

---

## 5. Revised Signal Naming

### Physical connector nets

```
US_N_A / US_N_B
US_E_A / US_E_B
US_S_A / US_S_B
US_W_A / US_W_B
```

### TX-side derived nets

```
TX_N_A  (P-MOS output → US_N_A)
TX_E_A  (P-MOS output → US_E_A)
TX_S_A  (P-MOS output → US_S_A)
TX_W_A  (P-MOS output → US_W_A)
```

### RX protected nets

```
RX_N_A_PROT  /  RX_N_B_PROT
RX_E_A_PROT  /  RX_E_B_PROT
RX_S_A_PROT  /  RX_S_B_PROT
RX_W_A_PROT  /  RX_W_B_PROT
```

**The connector should never be called only `TD_*` or only `RX_*`.** That naming was the trap that caused the V3 error.

---

## 6. What Stays from V3

The following blocks can largely stay unchanged:

- 22 V MT3608 boost converter (inductor upgrade, input/output caps)
- U49 active-low enable inverter
- TC4427 gate driver
- Q16 global TX_PULSE high-side switch
- R112 / R5 / R6 / R182 / D10 gate network changes
- TLV9062 two-stage RX amplifier
- VREF network (220 Ω filter + 4.7 µF bulk + distributed 100 nF)
- MCP6561 comparator
- R109 / R179 hysteresis option
- U50 / U51 hardware blanking gate
- TOF_EDGE into GPIO34
- MUX_A / MUX_B / RX_EN_N logic
- Directional P-MOS TX blocks (output now goes to `US_X_A` instead of `TD_X_A`)

---

## 7. What Changes from V3

| Item | V3 | V4 | Reason |
|------|----|----|--------|
| Transducer connectors | 4× TX-only (`TD_*`) | 4× shared (`US_*`) | Single transducer does TX+RX |
| RX connectors | Separate (`RX_*`) | None (shared with TX) | One connector per transducer |
| TX return path | Permanent 1 kΩ to GND | Switched N-MOS to GND | Avoid loading receiver |
| RX mux input protection | Post-mux only (2.2 kΩ + BAV99) | Per-input (22 kΩ + BAV99) | Protect mux pins from 22 V |
| Transducer count | 8 (4 TX + 4 RX) | 4 (shared) | Reciprocal measurement |
| Net naming | `TD_*` / `RX_*` separate | `US_*` shared | Prevent the V3 mistake |

---

## 8. GPIO Impact

### Current V3 GPIO usage

| GPIO | V3 function | V4 function |
|------|-------------|-------------|
| GPIO25 | TX_BURST_PWM | TX_BURST_PWM (same) |
| GPIO5 | TX_22V_EN_N | TX_22V_EN_N (same) |
| GPIO26 | DRV_N | DRV_N (same) |
| GPIO27 | DRV_E | DRV_E (same) |
| GPIO14 | DRV_S | DRV_S (same) |
| GPIO13 | DRV_W | DRV_W (same) |
| GPIO33 | REL_N (damping) | REL_N (damping, same) |
| GPIO32 | REL_E (damping) | REL_E (damping, same) |
| GPIO21 | REL_S (damping) | REL_S (damping, same) |
| GPIO22 | REL_W (damping) | REL_W (damping, same) |
| GPIO16 | MUX_A | MUX_A (same) |
| GPIO17 | MUX_B | MUX_B (same) |
| GPIO4 | RX_EN_N | RX_EN_N (same) |
| GPIO34 | TOF_EDGE | TOF_EDGE (same) |
| GPIO35 | VOLT_ESP | VOLT_ESP (same) |
| GPIO23 | PWR_HOLD | PWR_HOLD (same) |

### New GPIO requirement: switched TX return

V4 needs 4 additional GPIOs for the TX return N-MOS gates (one per direction):

| New GPIO | V4 function | Purpose |
|----------|-------------|---------|
| GPIO? | TX_RET_N | Switched TX return for North |
| GPIO? | TX_RET_E | Switched TX return for East |
| GPIO? | TX_RET_S | Switched TX return for South |
| GPIO? | TX_RET_W | Switched TX return for West |

**Open question:** Does V3 have 4 spare GPIOs available? The current V3 GPIO map uses 16 GPIOs. The ESP32-WROOM-32 has GPIOs 0, 2, 12, 15 potentially available (GPIO0 is boot, GPIO2 is boot strapping, GPIO12 is boot strapping, GPIO15 is available). This needs review — may require a GPIO multiplexer or I2C expander if not enough pins are free.

---

## 9. Firmware Control Sequence (V4)

For transmit from direction X, receive at opposite direction Y:

```
1. RX_EN_N HIGH            (disable RX)
2. set MUX_A/MUX_B for Y   (select receiving transducer)
3. TX_RET_X ON              (switched TX return for transmitting transducer)
4. DRV_X ON                 (directional P-MOS for transmitting transducer)
5. emit TX_PWM burst        (40 kHz, 6 cycles)
6. TX_PWM LOW
7. DRV_X OFF
8. TX_RET_X OFF
9. short non-overlap delay (2 µs)
10. DAMP_X ON               (damping shunt on transmitting transducer)
11. wait 10–50 µs
12. DAMP_X OFF
13. RX_EN_N LOW             (enable RX — receiving transducer is high-Z)
14. wait 10–20 µs            (mux/gate settle, discard false edge)
15. capture TOF_EDGE         (valid gate window)
16. RX_EN_N HIGH             (disable RX)
```

For the receiving transducer (direction Y), ensure:
- `DRV_Y` OFF (TX P-MOS off — transducer not driven)
- `TX_RET_Y` OFF (TX return off — transducer not loaded)
- `DAMP_Y` OFF (damping off — transducer not shunted)

---

## 10. Redesign Checklist

Before starting layout work, create a new ultrasonic schematic sheet with exactly this structure:

- [ ] Four physical 2-pin connectors only: `J_US_N`, `J_US_E`, `J_US_S`, `J_US_W`
- [ ] Each connector has two nets: `US_X_A` and `US_X_B`
- [ ] Each `US_X_A` has a TX P-MOS path from `TX_PULSE`
- [ ] Each `US_X_B` has a switched TX return N-MOS to GND (not permanent resistor)
- [ ] Each transducer has a damping MOSFET across `US_X_A` / `US_X_B`
- [ ] Each `US_X_A` has a protected RX path (22 kΩ + BAV99) into the mux
- [ ] Each `US_X_B` has a protected RX path (22 kΩ + BAV99) into the mux
- [ ] Every 74HC4052 input has its own series resistor and clamp
- [ ] No mux input can ever see raw 22 V
- [ ] No permanent low-value resistor loads the receive transducer
- [ ] Post-mux amplifier/protection (TLV9062, VREF, MCP6561, blanking) remains
- [ ] GPIO availability for 4× TX return N-MOS gates confirmed

---

## 11. What NOT to Do

- Do **not** add 8 connectors
- Do **not** tie `TD_*` and `RX_*` together on the current V3 schematic without per-input protection
- Do **not** use a permanent 1 kΩ return if the same transducer is also a receiver
- Do **not** rely on post-mux clamps alone to protect U42 (74HC4052)

---

## 12. References

| Document | Relevance |
|----------|-----------|
| `node/docs/NODE_V3_OVERVIEW.md` §16–§18 | V3 topology (to be superseded) |
| `node/docs/NODE_V3_ULTRASONIC_CHANGES_BRINGUP_FIRMWARE_GUIDE_2026-06-12.md` | V3 changes (TX chain stays) |
| `node/docs/ultrasonic_anemometer_function_note.md` | Function reference |
| `node/firmware/tests/NODE_V3_BRINGUP_RESULTS.md` | V3 bringup results (confirms V3 can't do anemometry) |
| `node/docs/ultrasonic_circuit_v2_design_advice.md` | V2 design advice |
| `node/docs/NODE_V3_ULTRASONIC_CHECKLIST.md` | V3 checklist |

---

## 13. Open Questions

1. **GPIO availability:** Are there 4 spare GPIOs on the ESP32-WROOM-32 for the TX return N-MOS gates? If not, consider an I2C GPIO expander (e.g., PCF8574) or reusing strapping pins with care.

2. **TX return N-MOS part:** What N-MOS to use? Needs low Rds(on), fast switching, logic-level gate. Suggested: 2N7002K or AO3400.

3. **RX protection resistor value:** Start with 22 kΩ. Scope testing needed to confirm. Footprints for 10 kΩ / 22 kΩ / 47 kΩ should be available for tuning.

4. **Damping MOSFET:** Keep the CJ2310 or equivalent? Verify it can handle the switched TX return current.

5. **Mux part:** Keep 74HC4052? The per-input protection means the mux never sees 22 V, so the 74HC4052 should be fine. Verify signal range with 22 kΩ + BAV99 clamps.

6. **Board revision:** Is this V3.1 (minor rework) or V4 (new board)? Depends on the extent of the connector area rework.