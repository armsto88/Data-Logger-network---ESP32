# FieldHub v2 — Next-Revision Notes

**Started:** 2026-10-23, during HUB-R6-01 bring-up

A running list of hardware changes for the revision after r6
(`revisions/2026-10-10-r6`). Entries are requirements, observations and open
questions, not a design. Requested items, facts confirmed from the r6 netlist,
and proposals are labelled separately in each entry.

## 1. A higher sensor supply for the soil probes on CN15/CN16

**Requested (Tom, 2026-10-23).** The soil moisture probes need a higher voltage
on the ADC connectors than r6 provides.

**Confirmed from the r6 netlist:**

- CN15/CN16 pin 1 is `5V_SEN`. It comes from U4 (TPS2553, about 300 mA limit),
  which is fed by `5V_SYS` from the MT3608. U4's EN is tied on, and its FAULT
  output is not connected.
- The signals are A0 = CN15.4, A1 = CN15.3, A2 = CN16.3 and A3 = CN16.4. Each
  input runs through a USBLC6 (D10/D11, whose VBUS is `5V_SEN`), then 1 kOhm
  (R29-R32), then AINx with 10 nF and a 1 MOhm pull-down.
- The ADS1015 (U30) VDD is `5V_SYS` through R28 (10 Ohm). Its absolute maximum
  input is VDD + 0.3 V, about 5.3 V.
- Node V3 already had a higher supply here. Its CN15/CN16 pin 1 is `5V_SYS`
  through an SS14, diode-ORed with a switchable `22V_SYS` branch (Q18/Q20
  AO3407A, D15-D18). The connector pinout is otherwise the same as the hub's.
- Review F-05 (`revisions/2026-10-08/REVIEW.md`) reports the Comwintop TH-V5
  probes as specified for a 10-30 V supply. 5 V is outside that range.

**Design points (proposed, nothing decided):**

1. **Switched rail:** a boost converter to the probe supply, enabled by a GPIO
   and on only during the sample window, with firmware allowing the probe's
   warm-up time. V3's 22 V branch is the reference circuit. 12 V would clear
   the reported 10 V minimum with margin.
2. **Per-port current limit with a visible fault:** r6 has neither, because
   U3/U4 FAULT is unrouted.
3. **Input protection rated for the probe supply.** On r6 the USBLC6 clamps
   each ADC input to `5V_SEN`. With 10-30 V on pin 1 of the same 4-pin
   connector, a miswire or damaged cable puts that voltage on an ADC input, and
   the clamp then dumps the fault current into `5V_SEN`/`5V_SYS`. The series
   resistance and clamp (or TVS) must survive the full probe supply indefinitely.
4. **Input range follows the probe's output variant**, not its supply. A 0-5 V
   output can stay 1:1 on the +/-6.144 V PGA, but with no headroom (5.3 V
   absolute maximum). A 0-10 V output needs a divider. A 4-20 mA output needs a
   burden resistor (for example 250 Ohm gives 1-5 V). The shared node sensor
   module scales by `SOIL_ADC_INPUT_TO_SENSOR_VOLT_GAIN`, which is currently 1.0
   (`node/firmware/src/sensors/sensors_soil_ads_calib.h`).
5. **Keep a 5 V option** for 5 V probes, either per port (jumper or diode-OR as
   on V3) or as a shared selection.
6. **Battery budget and sequencing:** a 1S cell feeding a 12-24 V boost, during
   the same wake as LTE bursts. Schedule soil sampling away from modem
   transmission.

**Open questions:**

- What are the exact probe part and output variant (0-5 V, 0-10 V, 4-20 mA,
  RS485)? The F-05 action to keep its datasheet with the release is still open.
- Which supply voltage, how much current per probe, and how long a warm-up?
- One switched rail shared by both ports, or one per port?

## 2. A second reed (pulse) input

**Requested (Tom, 2026-10-23).** Add a second reed input.

**Confirmed from the r6 netlist:**

- r6 has one reed input. Its connector is the JST-PH B2B-PH-SM4 with BOM
  reference `edaimagecopyoccupy` (still not renamed, review 2026-10-01 F-11):
  pin 2 `REED_SIG`, pins 1/3/4 GND. The signal runs through D15 (ESDS311), then
  R8 (1 kOhm), to GPIO36, with R9 (10 kOhm pull-up to `3V3_SYS`) and C2 (100 nF).
- Node V3 uses the same connector and pinout (signal to GPIO4 through SJ8), so
  a V3 anemometer harness fits the hub.
- The unrouted ESP32 pins on r6 (U45) are GPIO39, GPIO12, GPIO15, GPIO2 and
  GPIO5.

**GPIO choice (proposed):**

| GPIO | For a pull-up reed input | Reason |
|---|---|---|
| GPIO5 | Suitable | Strapping pin, high by default. A contact closed at boot only changes SDIO-slave timing, which doesn't affect SPI-flash boot |
| GPIO15 | Suitable | Strapping pin, high by default. Low at boot only silences the ROM boot log |
| GPIO39 | Avoid | Input-only, no internal pull. It shares GPIO36's false-low limitation with ADC1/Wi-Fi activity, which `reed` confirmed on GPIO36 on r6: 2-4 false edges per 5 Wi-Fi on/off cycles, and 1 per 4000 ADC1 reads |
| GPIO2 | Avoid | A pull-up interferes with serial download mode, which the auto-program circuit uses |
| GPIO12 | Do not use | MTDI strapping: high at boot selects 1.8 V flash, and the WROOM-32E N4 then fails to boot |

**Other design points (proposed):**

- **Move the first reed input off GPIO36 too.** HUB-R6-01 bring-up showed false
  closures on GPIO36 with the contact open (see `BRINGUP_HARNESS.md`, finding
  9). Putting both reed inputs on ordinary GPIOs (GPIO5 and GPIO15) avoids the
  problem for both, and frees GPIO36 as a spare input-only pin.
- Copy the existing input network (TVS, then 1 kOhm series, 10 kOhm pull-up,
  100 nF) and the same connector and pinout, so harnesses are interchangeable.
- Firmware has a single reed backend (`sensors_reed_wind.cpp`, `REED_WIND_PIN`),
  so it needs a second instance, plus its own sensor ID and capability-mask bit.

**What the input is for decides the circuit.** The hub's power is fully cut
between wakes.

- A second anemometer is sampled for a window during each wake, so a GPIO is
  enough.
- A tipping-bucket rain gauge, or anything that counts cumulative events,
  would lose every pulse while the hub is off. It needs an always-on counter on
  `KEEP_ALIVE`, or a wake-on-pulse path into the power latch.

**Open questions:**

- What sensor goes on the second input: a second anemometer, a rain gauge, or
  something else?
- If it counts events, how many pulses per day, and is it acceptable for the
  hub to wake on each one?

## 3. Candidates raised by r6 bring-up (not requested; for review)

These came out of `revisions/2026-10-10-r6/BRINGUP_HARNESS.md` and the
HUB-R6-01 evidence. Nothing here is decided.

- **Route `INT_RTC` to an ESP32 input.** An alarm flag left set holds VSYS on
  through Q3, and today only an I2C register read reveals it (bring-up
  finding 2).
- **Remove or redesign SJ4/SJ5.** With R150/R160 fitted, bridging either one
  shorts `3V3_SYS` to GND.
- **Bring sensor-rail EN and FAULT to GPIOs** (U3/U4), so firmware can power
  the sensor ports down and see a faulted port.
- **Make mux channel order match the port labels, or match Node V3.** On r6,
  ch0-3 = CN19, CN20, CN17, CN18; on V3, ch0 = CN18 and ch1 = CN17. Node
  firmware hard-codes the SHT40 on ch0 and the AS7343 on ch1.
- **Route TF1 card-detect to a GPIO**, so a card inserted while the hub is
  running can be seen without a reboot.
- **ADC noise needs no layout change.** With inputs shorted to GND the noise is
  1-2 LSB peak-to-peak on all four channels. The 4-5 LSB seen on open inputs is
  pickup on the high-impedance node. Open inputs also read a 14 mV mean, which
  a low-impedance probe output will override.
- **Rename `edaimagecopyoccupy`** to a `CNxx` designator (review F-11).
