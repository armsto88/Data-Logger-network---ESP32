# Ultrasonic Anemometer Work Plan

This plan is based on what has already been discovered during node bring-up, not on an idealized clean-sheet design.

## 1. Current Known State

### What is already true

- The node architecture already reserves a full ultrasonic path: transducers, direction control, gated TX power, analog RX chain, comparator, and MCU capture.
- In V1, the same control path effectively tied the burst activity and 22 V boost enable together, which made it hard to precharge the boost rail before transmit.
- The TX enable path fault was real and hardware-related: the diode in the TX gate to 22 V regulator enable path was reversed, so the regulator `EN` never followed the intended control signal.
- After rework, the 22 V regulator `EN` reached 3.3 V and boost output came up, but only to about 18 V rather than the intended 22 V.
- The previous RX clamp implementation was wrong enough that D8/D9 had to be removed during bring-up, which likely changed RX behavior substantially.
- In the ultrasonic route-finder test, all combinations returned `DET=24/24` in both OPEN and BLOCKED conditions, so the system was not measuring an acoustic path yet. It was dominated by feedthrough, over-sensitive thresholding, or other RX noise.

### V2 control change now in place

- V2 separates the ultrasonic burst signal from the 22 V boost enable.
- `TX_BURST_PWM` is now only the 40 kHz burst signal that drives the TC4427 transmit path.
- `TX_22V_EN_N` is now a dedicated active-low enable control for the 22 V boost path.
- `EN_22` is the MT3608 enable node.
- A single inverter (`SN74LVC1G04DRLR`, powered from `3V3_SYS`) is used so the ESP32 can command the boost cleanly with active-low logic:
  - `TX_22V_EN_N = HIGH` -> inverter output LOW -> `EN_22` LOW -> boost OFF
  - `TX_22V_EN_N = LOW` -> inverter output HIGH -> `EN_22` HIGH -> boost ON
- `GPIO5` is used for `TX_22V_EN_N` because GPIOs are scarce, so the control chain is biased for a safe boot default:
  - `GPIO5 / TX_22V_EN_N` -> `R170 2k` -> U49 input
  - U49 output -> `R173 1k` -> `EN_22`
  - `EN_22` -> `R171 100k` -> `GND`
  - `TX_22V_EN_N` -> `R172 100k` -> `3V3_SYS`
  - `C129 = 100nF` local decoupling for U49

### Why the GPIO5 choice is acceptable

- `GPIO5` is a strapping pin, so it must not be strongly pulled low.
- The weak `100k` pull-up keeps `TX_22V_EN_N` HIGH by default.
- Because the inverter makes HIGH mean boost OFF, boot and reset default to the safe state with the 22 V rail disabled.

### Why this matters

- The boost can now be enabled before the burst starts, so TX rail precharge can be tested explicitly.
- Burst timing and boost timing are now independently controllable.
- This removes one likely source of inconsistent TX amplitude and makes feedthrough debugging cleaner.

### V2 TX layout cleanup now in place

- During layout review, `22V_SYS` and `TX_PULSE` were initially implemented as larger copper or pour-style areas on an inner layer.
- Because `TX_PULSE` is a fast, high-voltage pulsed net, that broader copper behavior increased the risk of capacitive coupling into the RX analog section.
- V2 now treats `TX_PULSE` as a controlled trace rather than a broad pour.
- Current V2 routing direction is:
  - `TX_PULSE` = controlled `~0.5 mm` trace
  - short
  - direct
  - kept mostly within the TX / transducer region
  - not implemented as a large copper pour or broad copper area
- `22V_SYS` remains a local wider high-voltage supply routing shape near the TX / transducer section.
- `TX_PULSE` is treated more conservatively than `22V_SYS` because `TX_PULSE` is the higher-dV/dt aggressor.

### Why this TX layout change matters

- A large `TX_PULSE` copper area can behave like a capacitive plate.
- That can couple switching energy into:
  - `RX_IN`
  - `RX_COLD`
  - `ST1_IN`
  - `VREF`
  - `RX_AMP`
  - `COMP_RAW`
  - `TOF_EDGE`
- The narrower controlled trace reduces switching area and should reduce TX-to-RX feedthrough risk.

### V2 RX-side cleanup now in place

- The 74HC4052 mux enable net is now named `RX_EN_N` to reflect its active-low behavior.
- `RX_EN_N LOW` means mux enabled.
- `RX_EN_N HIGH` means mux disabled.
- A weak bias has been added from `RX_IN` to `VREF` using `R174 = 1M` so the hot-side protected RX input does not float when the mux is disabled.
- The hot-side RX path is now:
  - `RX_HOT` -> `R100 2.2k` -> `RX_IN`
  - `RX_IN` -> BAV99 clamp to `GND` / `3V3_SYS`
  - `RX_IN` -> `C103 220pF` -> `GND`
  - `RX_IN` -> `R174 1M` -> `VREF`
  - `RX_IN` -> `C102 1nF` -> `ST1_IN`
- The cold-side path remains unchanged for now because it already has a defined bias path:
  - `RX_COLD` -> `R152 2.2k` -> protected cold node
  - protected cold node -> `R107 100k // C109 1nF` -> `VREF`
- The `VREF` divider has been strengthened from `100k / 100k` to `47k / 47k` using `R175` and `R176`.
- `VREF` remains about `1.65 V`, with existing filtering retained: `C100 = 1uF` and `C99 = 100nF`.

### V2 comparator blanking now in place

- V2 now adds a hardware blanking path so comparator edges do not reach the ESP32 when the RX mux is disabled.
- The raw comparator output is now named `COMP_RAW`.
- The final ESP32 capture-side signal remains `TOF_EDGE`.
- The design reuses `RX_EN_N` rather than consuming another ESP32 GPIO.
- A single inverter `U50 = SN74LVC1G04DCKR` creates `RX_WINDOW_EN = NOT RX_EN_N`.
- Logic behavior is now:
  - `RX_EN_N HIGH` -> `RX_WINDOW_EN LOW` -> comparator blocked
  - `RX_EN_N LOW` -> `RX_WINDOW_EN HIGH` -> comparator allowed
- A single AND gate `U51 = SN74LVC1G08DCKR` gates the raw comparator output:
  - `COMP_RAW` -> U51 input A
  - `RX_WINDOW_EN` -> U51 input B
  - U51 output -> `R178 0R` -> `TOF_EDGE`
- Default V2 mode is gated comparator output:
  - `R178 = 0R` fitted
  - `COMP_RAW` to `TOF_EDGE` bypass jumper open
  - result: `TOF_EDGE = COMP_RAW AND RX_WINDOW_EN`
- Fallback mode is direct comparator output:
  - `R178` removed
  - `COMP_RAW` to `TOF_EDGE` bypass jumper closed
  - result: `TOF_EDGE = COMP_RAW`
- Important rule: do not fit `R178` and close the `COMP_RAW` to `TOF_EDGE` bypass jumper at the same time.
- Because `RX_EN_N` has a pull-up on the MCU side, the safe default state at boot is:
  - `RX_EN_N HIGH`
  - mux disabled
  - `RX_WINDOW_EN LOW`
  - `TOF_EDGE` blocked

### V2 comparator hysteresis tuning now in place

- V2 keeps the existing comparator feedback resistor `R109 = 1M` as the default hysteresis path.
- An optional parallel hysteresis path has been added using `R179 = 1M` enabled through a solder-jumper-selected path.
- Default mode leaves the jumper open, so only `R109` is active.
- Closing the jumper brings `R179` in parallel with `R109`, giving about `500k` effective feedback resistance.
- This is intended to strengthen hysteresis if comparator chatter remains a problem during bring-up.
- Important rule: the jumper enables the extra resistor path; it does not short the comparator feedback node directly.

### Why this RX change matters

- The immediate goal is not sophisticated mute circuitry yet; it is to stop `RX_IN` from floating and make the analog reference stiffer.
- This should reduce random pickup and undefined comparator triggering when the mux is disabled.
- It also gives a cleaner baseline before adding more aggressive analog mute circuitry.
- The new digital gating means firmware blanking is no longer the only line of defense against early comparator activity.

### Practical interpretation

The ultrasonic problem is not "write the wind algorithm". The immediate problem is that the hardware does not yet produce an acoustically discriminative first-arrival signal.

That means the shortest path to success is:

1. make TX amplitude and timing trustworthy
2. make RX quiet enough to distinguish real arrivals from feedthrough
3. only then tune capture windows and solve reciprocal TOF robustly

## 2. Success Criteria

The ultrasonic subsystem should only be considered working when all of these are true:

- A transmitted burst produces a measurable RX response that changes between OPEN and BLOCKED states.
- RX output is not saturated or constantly tripping the comparator.
- Comparator events occur inside a repeatable and physically sensible time window.
- Forward and reverse TOF values are repeatable on a static bench setup.
- Reciprocal timing changes directionally when airflow is introduced.
- The subsystem integrates into the node without breaking power, sleep, or the other sensors.

## 2.1 Current V2 Position

Based on the current V2 board review, the structural board changes are largely in place:

- split TX burst versus 22 V enable control
- safe-default active-low 22 V enable chain on `GPIO5`
- RX input bias and stronger `VREF`
- hardware comparator blanking
- optional stronger comparator hysteresis path
- `TX_PULSE` routing cleanup
- debug header and labelled test-point coverage

That means the active remaining ultrasonic design work is no longer the obvious V1 correction list. The current hardware focus is the Phase 3 style cleanup needed to make the subsystem acoustically discriminative on the real board:

- finalize RX protection so TX feedthrough does not either destroy the front end or flatten useful arrivals
- verify gain staging and comparator thresholding are sensitive without being saturation-prone
- keep TX/high-dV/dt routing and return currents away from `RX_IN`, `RX_COLD`, `ST1_IN`, `VREF`, `RX_AMP`, `COMP_RAW`, and `TOF_EDGE`
- lock the transducer/head connector and cable-routing approach so servicing does not force rework and wiring does not contaminate the acoustic volume
- keep the `R178` versus bypass selection explicit so only one `TOF_EDGE` path is ever populated

Important constraint: this does not mean Phases 1 and 2 can be skipped. It means the board-level design conversation has progressed to the remaining Phase 3 hardware risks, while the Phase 1 and Phase 2 bench checks are still the proof gate that must confirm those decisions.

## 3. Phase 1: Re-establish a Clean Electrical Baseline

Objective: prove that the TX and RX electrical paths are behaving sensibly before trying to compute wind.

### 3.1 TX power path

- Verify the corrected D21 orientation on the actual V2 board.
- Verify the new inverter-controlled enable path on the actual V2 board: `TX_22V_EN_N` -> inverter -> `EN_22`.
- Measure regulator input, `TX_22V_EN_N`, `EN_22`, and boost output during precharge and TX windows.
- Determine why boost output is about 18 V instead of the intended 22 V.
- Confirm that the boost can be turned on before the burst and held stable during the burst.
- Confirm whether 18 V is acceptable for first bench validation or whether TX amplitude must be restored before continuing.

### 3.2 RX protection and threshold path

- Build the RX clamp back into V2 with the corrected BAV99 orientation.
- Verify the renamed mux control net behaves as intended: `RX_EN_N LOW` = enabled, `RX_EN_N HIGH` = disabled.
- Verify `RX_IN` sits near `VREF` when the mux is disabled.
- Verify the new `R174 = 1M` bias is strong enough to prevent floating but weak enough not to corrupt real signal content.
- Verify the strengthened `VREF` divider (`47k / 47k`) remains quiet during mux switching, TX burst activity, and comparator transitions.
- Verify `COMP_RAW` toggles only when expected and that `TOF_EDGE` is blocked whenever `RX_EN_N HIGH`.
- Verify the U50/U51 logic chain behaves as intended: `RX_EN_N` -> `RX_WINDOW_EN` -> gated `TOF_EDGE`.
- Verify the default boot state leaves `TOF_EDGE` blocked.
- Add the schematic note near `R178` and the bypass jumper: populate only one path.
- Verify the clamp does not create rail loading and does not over-limit the signal.
- Confirm comparator reference level and hysteresis values on the actual board.
- Add labelled test pads for:
  - boost output
  - `TX_22V_EN_N`
  - `EN_22`
  - `TX_BURST_PWM`
  - `RX_EN_N`
  - `RX_IN`
  - `RX_AMP`
  - `VREF`
  - `COMP_RAW`
  - local analog GND
  - mux output
  - post-amplifier RX node
  - `TOF_EDGE`

### 3.3 Immediate electrical pass/fail checks

- TX burst visible and repeatable.
- With TX off and `RX_EN_N HIGH`, `RX_IN` should sit quietly near `VREF`.
- With TX off and `RX_EN_N HIGH`, `RX_AMP` should be quiet.
- With TX off and `RX_EN_N HIGH`, `TOF_EDGE` should be blocked and should not chatter even if `COMP_RAW` is noisy.
- With TX off and `RX_EN_N LOW`, the RX path should remain mostly quiet with no constant comparator triggering.
- RX analog node not stuck at rail.
- `COMP_RAW` not constantly toggling with no acoustic event.
- OPEN versus BLOCKED shows some difference somewhere in the analog chain.

## 4. Phase 2: Isolate the Noise Source

Objective: determine whether the false detections are mainly electrical feedthrough, comparator thresholding, or true acoustic ring-down.

Run the following tests in this order.

### 4.1 Baseline comparator noise with TX disabled

- Count comparator edges in a fixed listen window with TX fully disabled.
- Repeat with `RX_EN_N HIGH` and `RX_EN_N LOW`.
- Watch `RX_IN`, `RX_AMP`, `VREF`, `COMP_RAW`, and `TOF_EDGE` during this test.
- Outcome wanted: quiet baseline with very low edge count.

If this fails, the RX chain or thresholding is too noisy even before acoustics are involved.

### 4.2 TX-only coupling with receive transducer disconnected

- Fire TX bursts with the receive transducer disconnected or otherwise isolated.
- Run this both with the boost precharged before burst and with the shortest practical enable-to-burst delay.
- Also run it with `RX_EN_N HIGH` so mux-disabled behavior can be compared directly against mux-enabled behavior.
- Compare comparator activity against the fully connected case.
- Outcome wanted: large drop in detections if true acoustic coupling is the main path.

If detections remain high, the dominant mechanism is electrical feedthrough, not air-path arrival.

### 4.3 OPEN versus BLOCKED test after threshold hardening

- Re-run the same route-finder test after RX clamp restoration and comparator threshold review.
- Compare counts and timing distributions, not just a yes/no trigger.
- Outcome wanted: OPEN and BLOCKED become measurably different.

### 4.4 Ring-down blanking window sweep

- Add a configurable blanking window after TX.
- Sweep blanking time and watch when false detections collapse versus when real arrivals disappear.
- Outcome wanted: identify a listen window that excludes immediate feedthrough/ring-down but retains plausible first arrivals.

## 5. Phase 3: Make the Hardware Acoustically Discriminative

Objective: if Phase 2 shows the main issue is hardware, fix the analog path before doing more firmware sophistication.

### 5.1 RX front-end cleanup

- Review protection topology so it survives TX feedthrough without flattening useful arrivals.
- Review amplifier gain staging so the chain is sensitive but not saturated.
- Review comparator threshold strategy; allow either adjustable threshold or at least a justified fixed threshold.
- Keep analog return paths and comparator reference routing away from TX switching currents.

### 5.2 Layout and grounding

- Separate the 22 V pulsed TX current path from comparator and RX analog return paths.
- Minimize coupling from `TX_BURST_PWM`, driver lines, `TX_22V_EN_N`/`EN_22`, and boost switching nodes into the RX chain.
- Route `TX_PULSE` as a short, direct, controlled trace rather than a broad pour.
- Keep `22V_SYS` local to the TX / transducer region and avoid letting it spread toward the RX analog section.
- Do not run `TX_PULSE` as a long parallel segment near `RX_IN`, `RX_COLD`, `ST1_IN`, `VREF`, `RX_AMP`, `COMP_RAW`, or `TOF_EDGE`.
- Avoid routing `TX_PULSE`, `PWM_5V`, or `TX_BURST_PWM` under or alongside the RX / `VREF` / comparator section.
- Keep the mux and RX analog path physically short and shielded from the switching power region.

### 5.3 Mechanical/wiring discipline

- Keep transducer wiring and internal cable routing out of the primary acoustic volume.
- Ensure transducer cable routing does not sit adjacent to high-dV/dt TX nodes.
- Verify transducer polarity and channel mapping explicitly.

## 6. Phase 4: Bring Firmware Back In Carefully

Objective: once the bench signal is real, make timing extraction robust.

### 6.1 Bench firmware tools

- Keep a dedicated ultrasonic bring-up firmware separate from production node firmware.
- Log raw comparator edge timing, edge count, mux state, direction, blanking value, and burst parameters.
- Add serial modes for:
  - TX off baseline
  - single-direction burst
  - repeated burst histogram
  - OPEN versus BLOCKED comparison

### 6.2 First valid TOF capture

- Capture first-arrival times over many repeated shots in a fixed geometry.
- Reject impossible early events.
- Look for a stable cluster near the expected no-wind TOF rather than isolated trigger spikes.

### 6.3 Reciprocal timing

- Measure forward and reverse paths along one axis first.
- Confirm both directions produce stable arrival clusters.
- Only then compute reciprocal differences for wind.

## 7. Phase 5: Expand from Bench Proof to Real Wind Measurement

Objective: move from "I can detect something" to "I can trust wind output".

### 7.1 Static geometry validation

- Confirm measured no-wind TOF is close to the expected geometry-based value.
- Re-check transducer spacing, pod tilt, and reflector geometry if timing is consistently off.

### 7.2 Controlled airflow validation

- Introduce repeatable airflow from one direction.
- Verify sign and magnitude of TOF difference change as expected.
- Repeat at multiple flow strengths.

### 7.3 Environmental robustness

- Re-check operation with the full node assembled.
- Confirm no regression from battery operation, RTC wake, radio activity, or other sensor subsystems.

## 8. Recommended Near-Term Execution Order

This is the order I would actually run the work:

1. Build V2 with the corrected D21 and corrected BAV99 RX clamp.
2. Verify the new split TX control path: `TX_BURST_PWM` separate from `TX_22V_EN_N` / `EN_22`.
3. Add test pads for boost, RX analog, comparator, mux output, and the split TX control nodes.
4. Verify whether TX really reaches the intended drive level, including precharge-before-burst behavior.
5. Run comparator-noise baseline with TX disabled.
6. Run TX-only coupling test with RX transducer disconnected.
7. Sweep blanking window and comparator threshold.
8. Repeat OPEN versus BLOCKED and require a real difference before proceeding.
9. Once discriminative, collect timing histograms on one axis only.
10. Then move to reciprocal TOF and wind calculation.

## 9. What Not To Do Yet

- Do not spend time on full wind-vector maths until the RX chain can separate OPEN and BLOCKED states.
- Do not merge ultrasonic into the production node sampling path until bench detection is stable.
- Do not assume the current 18 V TX level is good enough without checking the SNR impact.
- Do not treat repeated comparator triggers as proof of acoustic detection.

## 10. Concrete Deliverables

The next useful outputs should be:

- a corrected V2 ultrasonic schematic review
- a dedicated bring-up test sheet with expected probe points and voltages
- a dedicated ultrasonic debug firmware mode for timing histograms
- one bench report showing whether the system is feedthrough-limited or acoustically discriminative

## 11. Bottom Line

The fastest credible route is not to "finish the anemometer" in one pass. It is to force the subsystem through three gates:

1. TX path electrically correct
2. RX path quiet and discriminative
3. timing extraction stable enough to support reciprocal TOF

Right now the project is between gates 1 and 2. The V2 hardware corrections you have already made are exactly the right kind of corrections; the next task is to prove the RX chain can distinguish real arrivals from self-generated noise.