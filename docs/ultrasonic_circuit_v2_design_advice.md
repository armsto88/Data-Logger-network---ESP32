# Ultrasonic Circuit V2 Design Advice

## Purpose

This document captures recommended design changes for **Ultrasonic Circuit V2** based on the current custom ESP32 ultrasonic anemometer board, bring-up observations, and the ultrasonic noise/coupling test results.

The goal of V2 should be to move from:

- electrically active but false-trigger dominated behavior

to:

- stable, acoustically discriminative first-arrival detection suitable for real time-of-flight measurement.

---

## Current V1 conclusion

The present ultrasonic subsystem is active and repeatable at an electrical level, but the timing output is not yet acoustically trustworthy.

The strongest evidence for this is:

- baseline comparator edges occur even without intended acoustic events
- `TX_PWM` is the dominant aggressor
- measured timing shifts strongly with blanking instead of remaining tied to a fixed physical path
- route selection combinations all appear valid under current feedthrough-dominated conditions
- RX-disabled baseline still shows edge activity, indicating the issue is not only acoustic feedthrough through the selected transducer path

So V2 should not focus primarily on firmware tuning. It should focus on **hardening the RX path, isolating the analog path, and enforcing real hardware blanking**.

---

## Main design goals for V2

The V2 ultrasonic section should be redesigned around these goals:

1. robust and unambiguous RX input protection
2. proper hardware blanking during TX and recovery
3. a much stiffer and cleaner analog reference (`VREF`)
4. deliberate comparator hysteresis
5. much better TX-to-RX isolation in both schematic and layout
6. a quiet and well-defined RX-disabled state
7. better bring-up visibility through test points and tuning footprints

---

## 1. Redesign the receive clamp / protection stage

This is the first priority.

In V1, the original clamp implementation around D8/D9 became a bring-up fault and had to be removed. Once removed, the RX front end became more vulnerable to overdrive, feedthrough, and false triggering.

### Recommendation for V2

Use a clear and explicit clamp structure per receive input node:

- signal node -> series resistor -> clamp point
- clamp point -> diode to `3V3_A`
- clamp point -> diode to `GND_A`

### Practical advice

- use low-capacitance small-signal diodes or a properly selected protection device
- avoid ambiguous dual-diode symbol/footprint combinations unless the mapping is extremely well controlled
- place the clamps physically close to the point where the signal enters the analog chain
- keep the series resistor before the clamp so fault/transient current is limited

### Design objective

The receive chain should survive TX transients without:

- saturating badly
- injecting large disturbances into downstream amplifiers
- causing the comparator to trigger on overload recovery

---

## 2. Add real hardware blanking

This is the most important system-level change for V2.

The current results indicate that firmware blanking alone is not enough. Even when software tries to ignore early events, the comparator/timing path still reacts to TX-associated disturbances.

### Recommendation for V2

Add at least one hardware blanking mechanism.

Preferred options:

- blank the **digital comparator output** during TX and blanking
- optionally also mute the **analog receive path** during TX

### Good implementation options

#### Option A — blank `TOF_EDGE`
Use a transistor or logic gate to force `TOF_EDGE` low during TX and blanking.

Advantages:
- simple
- easy to verify
- does not require altering analog gain stages

#### Option B — analog mute
Use an analog switch or clamp so the RX chain input is held in a quiet state during TX.

Advantages:
- prevents overload from propagating into the analog chain
- may reduce recovery artifacts

#### Option C — comparator disable
If comparator choice allows, disable or mute it during TX and blanking.

### Recommended direction

For V2, the best approach is:

- **digital output blanking as standard**
- optional **analog mute/clamp** if space allows

---

## 3. Redesign `VREF` as a proper analog reference

The current `VREF` approach is likely too weak for a noisy mixed-signal ultrasonic system.

A high-impedance divider is easy to disturb with:

- TX switching currents
- comparator input bias effects
- op-amp stage recovery
- ground bounce

### Recommendation for V2

At minimum:

- reduce divider impedance substantially
- increase local decoupling

Better:

- buffer `VREF`

### Preferred V2 implementations

#### Minimum acceptable
- divider around **10k / 10k**
- local `100 nF + 1 µF` minimum
- preferably extra `4.7 µF` near the analog section

#### Better
- divider -> op-amp buffer -> distributed `VREF`
- route buffered `VREF` only within the analog section

### Layout objective

Treat `VREF` as a sensitive analog reference net, not as a casual bias node.

Keep it:
- short
- isolated from TX copper
- locally decoupled
- referenced to quiet analog ground

---

## 4. Increase comparator hysteresis deliberately

The current comparator stage appears too easy to trigger from small disturbances.

### Recommendation for V2

Design in real hysteresis, not token hysteresis.

### Goal

Target hysteresis in the **tens of millivolts** at the comparator decision point, not just a few millivolts.

### Practical advice

- make comparator hysteresis an intentional design parameter
- avoid a very weak positive-feedback path into a low-impedance input structure
- consider placing hysteresis on the reference side if that gives cleaner control
- add tuning footprints so hysteresis can be adjusted during bring-up

### V2 objective

The comparator should:
- ignore small rail or reference disturbances
- ignore small recovery artifacts
- switch cleanly on the first real amplified ultrasonic event

---

## 5. Improve power and ground isolation between TX and RX

The current data strongly suggests that the TX burst path is the dominant electrical aggressor.

That typically means some mixture of:
- ground bounce
- analog rail disturbance
- electric-field coupling
- switching loop radiation
- shared return contamination

### Recommendation for V2

Deliberately partition the board into:

#### Quiet analog zone
- receive mux
- preamp
- band-pass filter
- comparator
- `VREF`
- analog decoupling

#### Noisy TX zone
- 22 V boost
- TX pulse switch
- TX driver
- high-current burst loop
- transducer TX routing

### Power strategy

Feed the RX analog section from a filtered analog rail:

- `3V3_SYS` -> ferrite bead or small resistor -> `3V3_A`
- local decoupling on `3V3_A`

Suggested local decoupling:
- `100 nF`
- `1 µF`
- `4.7–10 µF` bulk near op-amp/comparator zone

### Layout priorities

- keep TX pulse loops compact
- keep TX copper away from comparator input and `VREF`
- keep analog return currents away from TX return currents
- avoid running `TOF_EDGE` near TX burst nodes
- keep comparator output and reference routing short and clean

---

## 6. Make the RX-disabled state truly quiet

One of the most important findings from current testing is that “RX disabled” still does not produce a quiet system.

That suggests the disabled state leaves the analog path floating or otherwise vulnerable to internal disturbances.

### Recommendation for V2

When RX is disabled, the front end should be forced into a defined quiet state.

Possible approaches:

- disconnect the transducer with an analog switch and clamp downstream input toward `VREF`
- short the first receive node weakly toward `VREF`
- mute the comparator input during TX/blanking
- force comparator output blank during RX-disabled state

### Objective

“RX off” should mean:
- no meaningful analog amplification of external signal
- no floating node behavior
- no digital timing activity from the comparator output

---

## 7. Add dedicated test points for bring-up

V2 should be designed for debugging.

### Recommended probe points

Add labeled test points for:

- `TX_PWM`
- post-driver burst node / `PWM_5V`
- `TX_PULSE`
- `22V_SYS`
- `3V3_A`
- `VREF`
- `RX_HOT`
- `RX_IN`
- `RX_AMP`
- comparator threshold node if accessible
- `TOF_EDGE`

### Why this matters

The current board needed a lot of inference from behavior. V2 should let you directly inspect:

- where TX aggression enters
- whether `VREF` moves during burst
- whether the analog receive path saturates
- whether the comparator is producing false early edges

---

## 8. Add tuning footprints

Do not try to force V2 to be perfect on first spin. Give yourself controlled tuning options.

### Recommended optional footprints

- alternative comparator hysteresis resistor values
- optional RC filtering at comparator input or output
- optional TX snubber footprint
- optional series damping resistor in TX pulse path
- optional analog mute / blanking transistor footprint
- optional extra clamp / protection footprints

### Objective

V2 should be easier to trim during bring-up rather than requiring major bodges.

---

## 9. Suggested V2 receive architecture

A strong V2 receive path would look like:

```text
Transducer
-> series resistor
-> clamp-to-rails
-> AC coupling / bias to buffered VREF
-> first gain stage
-> band-pass stage
-> comparator with real hysteresis
-> hardware-blanked digital output
-> ESP32 timing input
```

This structure would be much more robust than the current effective state of V1.

---

## 10. Suggested V2 system architecture priorities

If choosing priorities, the order should be:

1. correct RX protection/clamp implementation
2. hardware blanking
3. stronger `VREF`
4. more robust comparator hysteresis
5. analog/TX power and layout isolation
6. testability and tuning footprints

---

## 11. What should count as V2 success

Before V2 is considered acoustically ready, it should show:

- very low baseline edge activity in listen-only mode
- RX-disabled mode that is nearly quiet
- coupling tests that are no longer saturated
- timing that does not simply follow blanking interval changes
- stable direction-dependent TOF under fixed geometry
- clear difference between open and blocked path conditions
- opposite-direction TOFs that can be used for real wind estimation

---

## 12. Additional information that would help refine V2 values

The overall V2 architecture is already clear, but exact value recommendations would be easier with:

- a clean full RX schematic with component values
- transducer part number / frequency / capacitance
- exact transducer spacing
- desired priority:
  - easiest bring-up
  - lowest cost
  - highest measurement quality

---

## Bottom line

The current V1 board has already revealed the important lessons for V2.

The biggest design priorities for V2 are:

- restore and simplify RX input protection
- add hardware blanking
- stiffen or buffer `VREF`
- increase comparator hysteresis
- isolate the analog section from TX burst activity
- make the RX-disabled state truly quiet

These changes should convert the ultrasonic subsystem from:

- electrically responsive but false-trigger dominated

to:

- stable, acoustically discriminative, and suitable for real time-of-flight measurement.
