# Node Hardware V2 Checklist

Working checklist for the next node PCB revision. This combines known V1 bring-up failures, field-service gaps, and likely V2 feature additions.

## 1. Must-Fix Issues From V1

- [x] Correct BAV99 RX clamp orientation: pin 1/A = `GND`, pin 2/K = `3V3_SYS`, pin 3 = RX signal.
- [x] Correct D21 22 V regulator enable diode orientation: cathode/bar faces `EN_22` / regulator `EN` pin.
- [x] Split V1 combined TX control into separate V2 signals: `TX_BURST_PWM` for the TC4427 transmit burst path, `TX_22V_EN_N` for boost enable control, and `EN_22` as the MT3608 enable node.
- [x] Add inverter-based 22 V enable control using `SN74LVC1G04DRLR` so the boost can be precharged before burst.
- [x] Use `GPIO5` for `TX_22V_EN_N` with weak pull-up and inverter logic so the 22 V rail stays OFF through boot/reset despite the strapping-pin constraint.
- [x] Correct LED polarity: LED anode toward the positive rail, LED cathode toward the GND side.
- [x] Audit the remaining polarity-sensitive parts on the node schematic and PCB: diodes, PMOS/NMOS parts, regulators, charger ICs, TVS parts, battery connectors.
- [x] Add labelled test points for `RAW_BAT`, `VSYS`, `3V3_SYS`, regulator `EN`, `PWR_HOLD`, 5 V rail, and 22 V ultrasonic TX rail.
- [x] Add labelled test points for `TX_BURST_PWM`, `TX_22V_EN_N`, `EN_22`, and 22 V boost output.
- [x] Keep ESP32 reset, boot, and auto-programming circuitry aligned with the standard Espressif reference design.
- [x] Lock the EN/programming delay capacitor to `1 uF`, since that value worked in bring-up.
- [x] Remove EN/programming capacitor selection pads; fixed `1 uF` retained because flashing and boot behaviour were stable during V1 bring-up.
- [x] Re-check ESP32 strapping pins so none can float or be driven into invalid states at boot.
- [x] Keep an emergency programming/debug header on the board: `GND`, `3V3`, `TX`, `RX`, `EN`, `BOOT`.
- [ ] Review decoupling and local bulk capacitance placement around the ESP32, charger, regulators, RTC, mux, ADC, and ultrasonic power stage. **MT3608 input cap upgraded to 1210 100µF 6.3V + 100 nF ceramic HF decoupling on VSYS; U49 VCC decoupling upgraded with 1 µF ceramic; comparator VCC supply filtered with 4.7 Ω + 100 nF + 1 µF (3V3_COMP island); TLV9062 RX amplifier VCC supply filtered with 4.7 Ω + 100 nF + 1 µF (3V3_RXAMP island); VREF supply filtered with 220 Ω + 4.7 µF bulk decoupling added; 74HC4052 mux VCC supply filtered with 4.7 Ω + C101 100 nF (3V3_MUX island); remaining decoupling review still needed. See `NODE_V3_22V_BOOST_UPDATE.md`.**

## 2. Power Architecture

- [ ] Decide whether the node should keep the current full power-cut architecture as the normal sleep model. Current answer: yes, unless V2 moves to a different low-power strategy.
- [ ] Add a true battery isolation method for storage, shipping, and safe servicing.
- [ ] Treat the isolation switch as a service or storage disconnect, not as the normal user on/off control.
- [x] Keep the existing main rail structure for the V2 prototype: `3V3_SYS`, `5V_SYS`, and `22V_SYS` all reviewed as acceptable for bring-up.
- [x] ~~Keep the compact `22 uH` inductor in the 22 V stage for V2 prototype space reasons, with 22 V performance treated as a bring-up validation item.~~ **V3 update:** Replaced SMMS0420-220M (1.5A I_sat) with 22µH ≥2.5A I_sat 5.6×5.2mm molded inductor (LCSC C41406986) to fix saturation brownout. See `NODE_V3_22V_BOOST_UPDATE.md`.
- [x] Upgrade MT3608 input cap from 22µF to 1210 100µF 6.3V X5R ceramic for improved cold-start energy reserve.
- [x] Use a RUN/KILL latch-control disconnect slide switch as the current V2 hard-kill approach.
- [x] Keep the USB service-force path, but place it under RUN/KILL authority so it only works when the node is in RUN.
- [x] Standardize all four PCB slide switches to the same lower-cost `MST-12D18G2`-style SMD SPDT slide switch family.
- [x] Replace the more expensive `JS102011SAQN` logic-switch positions where only low-current logic/control switching is required.
- [x] Confirm the slide switches are only used for low-current logic/control functions, not main battery/load current.
- [x] Final-check the exact footprint/datasheet so pin 2 is definitely the common/wiper in every switch position.
- [x] Final-check physical slide direction versus silkscreen labels before ordering.
- [x] Confirm unused throws remain NC where the switch is used in SPST-style logic applications.
- [ ] Add reverse-polarity protection or verify the current input path already covers real-world battery wiring mistakes.
- [ ] Re-check solar, USB, and any dual-source charging/power-path behaviour under all expected field states.
- [x] Remove the earlier `Q12/Q13` PMOS charger-isolation approach and tie CN3163 BAT plus TP5100 BAT directly to `BAT_BUS`.
- [x] Reduce TP5100 USB charge current to about `1 A` by changing `U25` from `50 mOhm` to `100 mOhm`.
- [x] Reduce CN3163 solar charge current to about `0.5 A` by changing `R81` from `1.5 kOhm` to `3.6 kOhm`.
- [ ] Verify leakage current in the fully off state, including charger, divider networks, LEDs, pull-ups, RTC backup path, and protection parts.
- [ ] Review whether the battery divider should be switchable so it does not create unnecessary standby drain.
- [ ] Ensure `PWR_HOLD` defaults to a safe state on cold boot and cannot latch incorrectly because of pull-up/down mistakes.
- [x] For the RUN/KILL switch, confirm only the RUN position connects `SYS_GATE_Q2` to `LOGIC / SYS_GATE_CTRL`.
- [x] For the RUN/KILL switch, confirm the KILL position disconnects `SYS_GATE_Q2` from `LOGIC` so `R10` pulls the Q2 gate high and turns Q2 off.

## 3. User Controls And Recovery

- [ ] Add a dedicated reset button connected to ESP32 `EN`.
- [ ] Decide whether to also add a dedicated boot/program button on `GPIO0`, or rely only on the auto-reset circuit plus header access.
- [x] Do not add a dedicated firmware service-button GPIO on V2; RUN/KILL, force-on, and USB service force are the chosen recovery path.
- [x] No separate user power button for normal operation; the current V2 direction relies on RUN/KILL plus the existing force/service paths instead.
- [ ] If a power switch is added, decide whether it must be externally reachable without opening the enclosure.
- [ ] Make sure any button exposed to the user is debounced, ESD-protected, and still usable with gloves or in wet conditions.

## 4. LEDs And Visual Feedback

- [x] Keep LEDs that indicate when the main power rails are on.
- [ ] Decide whether additional LEDs are useful in the field: charging, boot/status, sensor fault, radio activity, deployment state.
- [ ] Avoid always-on LEDs that waste power during deployment.
- [ ] Put LEDs under MCU control where possible so they can be disabled in field mode.
- [ ] Consider a very low-duty diagnostic blink pattern rather than continuous illumination.
- [ ] Decide whether LEDs should be visible only during servicing or through the sealed enclosure in normal use.
- [ ] If brightness is an issue, add resistor values or a solder-jumper option for dimming/disabling non-critical LEDs.

## 5. Ultrasonic Anemometer Integration

- [x] Keep ultrasonic support on the main node PCB as part of the single V2 node design.
- [ ] Review the full ultrasonic signal chain: TX burst generation, 22 V drive stage, analog mux path, protection network, amplifier stages, comparator thresholding, timer capture routing.
- [x] Confirm final ultrasonic-related GPIO assignments in the reviewed V2 prototype: `RX_EN_N` on `GPIO4`, `TOF_EDGE` on `GPIO34`, and `TX_22V_EN_N` on `GPIO5`.
- [x] Rename the 74HC4052 inhibit control net from `RX_EN` to `RX_EN_N` to reflect active-low behavior.
- [x] Add weak hot-side bias from `RX_IN` to `VREF` using `R174 = 1M` so the RX input does not float when the mux is disabled.
- [x] Strengthen the `VREF` divider from `100k / 100k` to `47k / 47k` using `R175` and `R176`.
- [x] Keep `VREF` filtering at `1uF + 100nF`.
- [x] Leave the cold-side `R107/C109` bias path unchanged for now.
- [x] Move the `TLV9062IDR` RX amplifier supply from `5V_SYS` to `3V3_SYS` so the RX amplifier, comparator, and ESP32 capture path share the same voltage domain.
- [x] Keep `R108 = 1k` and do not add an extra comparator-input BAV99 clamp in the current V2 prototype.
- [x] Rename the raw comparator output to `COMP_RAW`, keeping `TOF_EDGE` as the final ESP32 capture signal.
- [x] Add inverter `U50 = SN74LVC1G04DCKR` to derive `RX_WINDOW_EN = NOT RX_EN_N`.
- [x] Add AND gate `U51 = SN74LVC1G08DCKR` so `TOF_EDGE = COMP_RAW AND RX_WINDOW_EN` in default V2 mode.
- [x] Add `R178 = 0R` gated-path selector and a `COMP_RAW`-to-`TOF_EDGE` bypass jumper for fallback direct mode.
- [x] Add optional comparator hysteresis strengthening using `R179 = 1M` in parallel with `R109 = 1M` through a solder-jumper-selected path.
- [x] Keep `RX_EN_N` pull-up so the safe default boot state is mux disabled and `TOF_EDGE` blocked.
- [x] Replace the broad `TX_PULSE` pour with a controlled `~0.5 mm` trace.
- [x] Keep `22V_SYS` as a local wider supply routing shape near the TX / transducer section.
- [x] Prioritize reduced capacitive coupling over maximum copper area for `TX_PULSE`.
- [x] Add a `220 kOhm` bleed resistor from `TX_PULSE` to `GND` so the TX pulse node does not float after Q16 turns off.
- [ ] Add cleaner protection around the receive path so future clamp or overdrive issues are easier to diagnose and less destructive.
- [ ] Verify analog and power-domain separation around the ultrasonic front end so switching noise does not dominate the receive chain.
- [x] Add test pads for key ultrasonic nodes: `RX_EN_N`, `RX_WINDOW_EN`, `RX_IN`, `RX_AMP`, `VREF`, `COMP_RAW`, `TOF_EDGE`, local analog GND, mux output, TX enable, boost enable, and boost output.
- [ ] Add a schematic note near `R178` and the bypass jumper: populate only one path, never both.
- [ ] Confirm connector strategy for transducers and head assembly wiring so servicing does not require rework on the main PCB.
- [ ] Confirm mechanical placement keeps acoustic wiring outside the measurement volume and away from noisy switching nodes.
- [ ] Keep `TX_PULSE`, `22V_SYS`, `PWM_5V`, and `TX_BURST_PWM` away from `RX_IN`, `RX_COLD`, `ST1_IN`, `VREF`, `RX_AMP`, `COMP_RAW`, and `TOF_EDGE`.
- [x] Ultrasonic is not an optional-population subsystem for this V2 direction.

## 6. Sensor And Connector Strategy

- [ ] Reconfirm all sensor connector pinouts against the actual field wiring that worked in bring-up.
- [ ] Clearly key and label all external connectors so soil probes, battery, PAR, ultrasonic, and auxiliary ports cannot be mixed up easily.
- [x] Expose a connector for a reed-switch style anemometer input using `J52 / AUX WIND`.
- [ ] Expose a 1-Wire-capable connector.
- [x] Decide that the reed-switch anemometer input is a dedicated fallback connector implemented separately from the 1-Wire path.
- [x] Reuse the existing `RX_EN_N` pull-up as the reed input pull-up by routing `REED_SIG` to `GPIO4` through a normally-open solder jumper.
- [x] Keep the reed input isolated from the ultrasonic control path by default; jumper open = ultrasonic mode, jumper closed = reed fallback mode.
- [ ] Review whether additional input protection or debounce components are still needed beyond the existing pull-up and physical jumper isolation.
- [ ] For the 1-Wire connector, define power pinout, pull-up strategy, cable length expectations, and ESD protection.
- [ ] Add ESD protection on external sensor lines and any connector exposed to long cables.
- [ ] Review whether sensor power rails should be switchable per port to reduce idle current and support warm-up sequencing.
- [ ] Check I2C bus pull-up strategy across onboard devices and off-board sensor cables.
- [x] Keep the ADS1015 section, 22 V switched output section, PCA9306 translator section, and manual sensor power switching unchanged for V2 because they worked acceptably in V1.
- [ ] Keep the mux and ADC address plan explicit on the schematic and silkscreen/debug notes.
- [ ] Decide whether V2 needs more expansion ports or whether the current connector count is already enough.

## 7. Serviceability And Bring-Up

- [ ] Put reference designators and signal names on the silkscreen wherever probe access matters.
- [ ] Add clear polarity markings for battery, solar, diodes, LEDs, and any bodge-prone parts.
- [ ] Keep enough probe space around regulators, RTC, charger, UART header, and ultrasonic analog nodes.
- [ ] Consider small jumper links or zero-ohm options in risky paths so subsystems can be isolated during bring-up.
- [ ] Add a board revision ID and, if practical, a per-board serial/asset label area.
- [ ] Create a formal V2 bring-up sheet that maps each test point to expected voltages and pass/fail checks.

## 8. Environmental And Mechanical Items

- [ ] Check whether any new buttons, switches, LEDs, or connectors compromise enclosure sealing.
- [ ] Confirm cable gland and connector placement still avoids disturbing the ultrasonic measurement geometry.
- [ ] Review strain relief for battery, transducer, and external sensor wiring.
- [ ] Decide whether conformal coating is needed on parts of the node PCB.
- [ ] Confirm RTC backup battery access and replacement method.
- [ ] Verify mounting holes, board edges, and connector heights against the current belly enclosure geometry.

## 9. Decisions To Make Early

- [x] V2 is the updated revision of a single node design, not a split standard-versus-ultrasonic hardware family.
- [x] The ultrasonic system is mandatory on-board and needs to function properly without requiring an off-board option.
- [x] No dedicated MCU service-button GPIO is planned for V2; RUN/KILL plus force-on and USB service force are sufficient.
- [x] A separate user power button is not needed for normal deployment.
- [x] Current V2 direction uses a RUN/KILL latch-control slide switch for hard kill rather than a richer GPIO-backed service button.
- [x] Keep LEDs that show when the main power rails are on.

### 9.1 Isolation Switch vs Power Button

- [ ] `Power button`: a normal user control for turning the node on or off during regular operation.
- [ ] `Isolation switch`: a hardware disconnect used for storage, transport, servicing, safety, or long-term battery isolation.
- [ ] For V2, the current direction is: no regular user power button, but likely yes to some form of service or shipping isolation disconnect.

## 10. Recommended V2 Minimum Scope

If you want to keep V2 disciplined, the minimum high-value changes are:

- [x] Fix all diode and polarity issues from V1.
- [x] Add test points and a proper debug/programming header.
- [ ] Add a reset button.
- [x] Keep recovery architecture based on RUN/KILL, force-on, and USB service force instead of adding an MCU service button.
- [ ] Add a battery isolation method for storage and servicing.
- [ ] Finish the remaining external connector work: keep `J52 / AUX WIND` as the reed-switch fallback input and add the 1-Wire connector.
- [ ] Rework the ultrasonic interface so it is robust, diagnosable, and fully integrated on-board.
- [ ] Review LED behaviour so indicators are useful but not a battery drain.
- [ ] Add connector labeling, ESD protection, and clearer service markings.

### 10.1 AUX WIND Fallback Mode Note

- `J52 / AUX WIND` is now part of the reviewed V2 connector set.
- Pinout: `3V3_SYS`, `REED_SIG`, `GND`.
- `REED_SIG` reaches `GPIO4 / RX_EN_N` only when the dedicated solder jumper is closed.
- Default state is jumper open so ultrasonic timing and mux control stay isolated from any attached reed switch.
- Reed mode is a fallback operating mode, not a simultaneous second wind sensor path.

## 11. Nice-To-Have Items

- [ ] Per-port sensor power switching.
- [ ] Load switch for the battery measurement divider.
- [ ] Optional solder jumpers to disable LEDs.
- [ ] Dedicated current-sense point for off-state current measurement.
- [ ] Formal design-for-test pads for automated or semi-structured bench validation.
