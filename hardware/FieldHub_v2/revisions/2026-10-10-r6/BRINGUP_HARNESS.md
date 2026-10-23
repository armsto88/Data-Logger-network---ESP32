# FieldHub v2 revision 6 bring-up harness

- **Date:** 2026-10-23
- **Hardware under test:** this directory's printed five-board batch, labelled
  `HUB-R6-01` to `HUB-R6-05`
- **Acceptance criteria:** [`BRINGUP_PLAN_R5.md`](../../BRINGUP_PLAN_R5.md),
  stages A-L, unchanged. Revision 6 changed only the F2 I2C pull-ups, the R29-R32
  ADC series resistors and the U52 land pattern. The outline, SD path and GPIO
  map are the same as revision 5.

This document covers the tooling that runs those stages, and the revision-6
facts it is built on. Where it differs from the R5 plan, that is because the
plan assumed something the r6 netlist does not provide. Each difference is
listed under [Findings](#findings-that-change-how-the-r5-plan-is-run).

## Contents of the harness

| Piece | Path |
|---|---|
| Runner: flash, capture, judge, record | `scripts/hub_bringup.py` |
| New r6 sketches | `mothership/firmware/v2/tests/bringup_hub_*.cpp`, `hub_bringup_report.h` |
| V1 subsystem sketches, restored from `f5e836f^` | `mothership/firmware/v2/tests/bringup_{pwr_hold,config_latch,rtc_alarm,battery_adc,wake_reason,espnow_basic,sd_card,modem_*}.cpp`, `modem_at_helper.h` |
| Evidence per board | `bringup/<BOARD>/` in this directory: one log per run plus `results.csv` |

The restored V1 sketches compile unchanged against the v2 tree, because r6 keeps
the V1 GPIO map in `src/system/pins.h`. One sketch was changed: `bringup_battery_adc.cpp`
now uses `analogReadMilliVolts()`, matching production `power.cpp` since
`08bc191`. It also prints the old uncalibrated figure for comparison.

Every env behind `scripts/hub_bringup.py list` builds at the time of writing.
`mothership-v1-sim-test` (`modem-https`) needed `http_response_parser.cpp` added
to its link list; `modem_driver.cpp` had come to depend on it. These envs still
point at sources that were never restored, so do not use them: `nvs-anchor`, `alarm-verify`, `rescue-alarm`, `rtc-validity`,
`espnow-queue`, `modem-sim-diag`, `modem-https`, `upload-cursor`, `flash-purge`,
`flash-write-check`, `atomic-purge`, `upload-chunk` and `config-wake-test`
(all `mothership-v1-*`).

## Revision-6 facts traced from the netlist

Traced from `HUB_ProPrj_MotherShip_v2_2026-10-10_v2_#6.epro2` (the PCB document's
pad nets) and the r6 BOM.

| Function | Connection | Covered by |
|---|---|---|
| Root I2C | GPIO21 `SDA1`, GPIO22 `SCL1`, F2 4.7 kOhm pull-ups to `3V3_SYS` | `i2c-mux` |
| RTC | U49 DS3231M at `0x68` | `i2c-mux`, `rtc-alarm` |
| Mux | U33 TCA9546A at `0x71`: A0 high via R160 0 Ohm, A1 low via R150 0 Ohm, A2 GND | `i2c-mux` |
| Mux channel to port | ch0 = CN19, ch1 = CN20, ch2 = CN17, ch3 = CN18 (33 Ohm F4/F8/F6/F7, USBLC6 D5/D8/D7/D9, 4.7 kOhm to `3V3_SEN` per branch) | `i2c-mux` |
| I2C port pinout | 1 `3V3_SEN`, 2 GND, 3 SDAx, 4 SCLx | harness continuity (Stage B) |
| ADC | U30 ADS1015 at `0x48` on `SDA2/SCL2`, the 5 V side of PCA9306 U29; VDD = `5V_SYS` through R28 10 Ohm | `ads1015` |
| ADC inputs | port, USBLC6, 1 kOhm (R29-R32), then AINx with 10 nF and a 1 MOhm pull-down. AIN0 = CN15.4, AIN1 = CN15.3, AIN2 = CN16.3, AIN3 = CN16.4; port pins 1/2 = `5V_SEN`/GND | `ads1015` |
| Reed | Connector (BOM reference `edaimagecopyoccupy`): pin 2 `REED_SIG`, pins 1/3/4 GND. D15 TVS, R8 1 kOhm to **GPIO36**, R9 10 kOhm pull-up, C2 100 nF | `reed` |
| Sensor rails | U3 TPS2553 to `3V3_SEN` (ILIM R11 133 kOhm), U4 TPS2553 to `5V_SEN` (R13 88.7 kOhm). **EN tied to IN, FAULT unconnected** | bench only |
| 5 V boost | U21 MT3608, **EN tied to VSYS** | bench only |
| SD | Identical to the R5 matrix: CS GPIO13/R24, MOSI GPIO23/R26, SCK GPIO18/R27, MISO GPIO19 direct, 10 kOhm on MISO/MOSI/DAT1/DAT2, CD not routed | `sd-card`, `sd-sweep`, `sd-logger` |
| Battery ADC | `RAW_BAT` to R157 220 kOhm / R158 100 kOhm, then R159 2.2 kOhm to GPIO34 (ADC1) | `battery-adc` |
| USB auto-program | Q15: DTR drives the base, RTS the emitter, collector on `PROG_EN`. Q14 is the mirror image on `PROG_BOOT` | runner reset logic |

## Findings that change how the R5 plan is run

1. **SJ4 and SJ5 must be open: bridging either one shorts `3V3_SYS` to GND.**
   They are alternative address straps wired in parallel with the fitted 0 Ohm
   links. SJ5 connects mux A0 to GND, while R160 ties A0 to `3V3_SYS`. SJ4
   connects A1 to `3V3_SYS`, while R150 ties A1 to GND. Add to Stage A
   inspection and Stage B: both jumpers read open, U33.1 reads about 0 Ohm to
   `3V3_SYS`, and U33.2 reads about 0 Ohm to GND.
2. **Firmware can neither switch nor monitor the sensor rails.** `3V3_SEN`,
   `5V_SEN` and `5V_SYS` are on whenever the hub is awake, and no fault output
   reaches the ESP32. R5 Stage G step 2, "enable `3V3_SEN` and `5V_SEN`
   separately", cannot be done in firmware. Instead, load each rail with the
   hub awake: first no sensor, then a dummy load, then the real harness. Steps
   3-4 (current limit and auto-retry) need a current probe and a scope. A
   faulted sensor port will be invisible to firmware in the field.
3. **Mux channel order is not port-label order.** It runs ch0-3 = CN19, CN20,
   CN17, CN18. Local-sensing firmware (`docs/HUB_LOCAL_SENSING_PLAN.md`, Phase 4)
   must use this mapping. That plan's open question 4 is now answered: reed is
   on GPIO36, the mux is at `0x71`, and there are no rail enables.
4. **GPIO36 erratum.** The reed input is GPIO36, which can see a short false low
   when ADC1 or the Wi-Fi front end powers up. The battery ADC (GPIO34) is also
   on ADC1. `reed` measures this directly (`REED_ADC1_IMMUNITY`,
   `REED_WIFI_IMMUNITY`). A failure there means false wind counts, and has to
   be handled in firmware: no battery reads or radio changes while counting.
5. **Serial monitor DTR/RTS.** The R5 plan's `pio device monitor` command
   inherited `monitor_dtr = 0` / `monitor_rts = 0`. On this board that resets the
   ESP32 and drops the rail. The shared `[env]` now sets both to 1. The runner
   opens the port with both lines asserted, independent of the ini.

## Findings from HUB-R6-01 (2026-10-23)

Evidence is in `bringup/HUB-R6-01/`. These are behaviours of the design or the
firmware stack, not one-board defects, so they apply to every board.

1. **The stock SD library cannot mount some SDHC cards; the SD hardware is
   good.** Arduino-ESP32 2.0.17 sends ACMD41 with `0x40100000`. The 16 GB SDHC
   reference card stays idle for as long as bit 20 is set, so `SD.begin()`
   fails with `APP_OP_COND failed: 1` / `f_mount failed: (3)`. Bit-banged and
   hardware-SPI probes (`sd-raw`) show that CMD0, CMD8, ACMD41 with
   `0x40000000`, and CMD58 all work. The project now vendors the library with
   that one change (`mothership/firmware/v2/lib/SD/FIELDMESH_PATCH.md`). With
   it: `sd-sweep` passes 7/7 up to 40 MHz, `sd-logger` passes 25/25,
   `sd-logger --repeat 100` passes 100/100, and `sd-nocard --repeat 25` passes
   25/25 (worst failed mount 628 ms).
2. **An RTC alarm flag left set keeps the hub powered indefinitely, and the
   ESP32 cannot see it.** Q3 holds the VSYS switch on while `INT_RTC` is
   asserted, i.e. while A1F/A2F is set with its interrupt enabled. No ESP32 pin
   reads `INT_RTC`; only the DS3231 status register over I2C shows it. After
   `rtc-alarm`, A1F stayed set with A1IE on. That held VSYS for over an hour,
   and made the first `pwr-hold` run look as though PWR_HOLD could not cut
   power. `pwr-hold` now reports the flags and clears them first. Production
   shutdown must confirm both flags are clear (or the next alarm is armed)
   before releasing `PWR_HOLD`; otherwise a flag left set drains the battery.
3. **The config button's set overrides clear while it is held.** U52's preset
   (`CONFIG_SET_N`, the button) dominates `CONFIG_CLEAR`, so a clear issued
   while the button is held does not take. Shutdown code should re-read
   `CONFIG_WAKE` after the clear pulse and retry until it reads HIGH. This is
   the "latch bounce unreliable" behaviour noted on V1. The latch output only
   feeds the `CONFIG_WAKE` readback; it does not hold power.
4. **The last log line before a power cut is lost**, because it is still
   leaving the UART when the rail collapses (the rail drops within about 30 ms
   of PWR_HOLD going low). Call `Serial.flush()` before releasing `PWR_HOLD`
   wherever that line matters.
5. **A card reset mid-transaction can keep driving DO.** It stays driven until
   the card sees clocks with CS high. This only affects bring-up pin checks
   (MISO is not shared), and `sd-raw`/`sd-sweep` now clock it out before
   reading.
6. **Open-input ADC noise is 5 LSB (15 mV) peak-to-peak, with a 14 mV offset,
   identical on all four channels**, against the harness's 4 LSB limit. The
   next check is whether it survives shorting an input to GND at the port.
7. **The battery ADC reads 1.2 % high** (3.775 V against a DMM's 3.73 V), so
   a 3.5 V firmware guard trips at about 3.46 V true.
8. **The modem passes everything up to authentication.** HTTPS to the ingest
   endpoint returns 401 "Invalid or revoked device credential", which is
   correct for a hub without FM2 provisioning.

## Host setup

```sh
scripts/hub_bringup.py list                 # the ladder, in stage order
scripts/hub_bringup.py run HUB-R6-01 i2c-mux
scripts/hub_bringup.py status HUB-R6-01     # latest verdict per test, and what is not yet run
```

- The script re-runs itself under `~/.platformio/penv/bin/python`, which has
  pyserial, and uses `~/.platformio/penv/bin/pio` (override with `PIO_BIN`).
- Port: `HUB_PORT`, else the single `/dev/serial/by-id/usb-1a86_USB_Serial*`
  device, else `/dev/ttyUSB0`. The by-id path survives renumbering after a
  power cut.
- `run` builds and flashes the env, opens the port, pulses EN so the capture
  starts at boot, answers the sketch's "Send Enter to start", and tees every
  line to a timestamped log. Typed lines go to the board. The run is judged
  from its `RESULT|SUMMARY` / `OVERALL` line, or by an operator prompt for the
  V1 sketches.
- If the port disappears (the board powered itself off), the runner logs it
  and waits for it to come back. RTC and config-wake tests stay in one log.
- `--repeat N` resets and re-runs an auto-judged test N times; use it for the
  R5 plan's reboot soaks. `--stay` keeps the console open after the summary,
  for interactive commands. `--no-flash`, `--note` and `--verdict` behave as
  their names say.
- `record BOARD ITEM pass|fail|info --note "..."` logs bench measurements
  (rails, currents, inspection) into the same `results.csv`, so each board has
  one record. Each row carries the git commit and whether the firmware tree
  was dirty.

### Switch positions

- **SW9** stays at RUN throughout (KILL disables every wake path).
- **SW10 service position + USB** holds the latch from VBUS. Flashing needs it,
  and so does every test with `SW10 = service` in `list`: the runner's EN pulse
  resets the ESP32, which releases `PWR_HOLD`.
- **SW10 run position** is required for `pwr-hold` and the off-wake parts of
  `wake-reason`, because the board has to be able to cut its own power. Flash
  in service, then slide SW10 to run while the sketch is holding `PWR_HOLD`.
- USB does not power or charge the hub. The bench supply or cell at `BATT`
  powers it in every position.

## Day-one sequence for HUB-R6-01

A board advances only on a recorded pass (R5 plan section 2). Stop at the first
unexplained over-current, rail error, reset or bad write.

1. **Stage A: inspection.** Include finding 1: SJ4/SJ5 open, and R150/R160
   fitted.
   `record HUB-R6-01 A-inspection pass --note "..."`
2. **Stage B: unpowered checks.** Rail resistances, the full SD continuity
   matrix (unchanged from R5), the mux-strap readings from finding 1, and
   harness continuity using the port pinouts above. Record each one with
   `record`.
3. **Stage C: first power.** 3.8 V at 100 mA into `BATT`, with no card, SIM,
   sensors or panel. Record the current in each state and the
   `VSYS`/`3V3_SYS`/`5V_SYS` readings. All three rails come up together,
   because nothing on the board gates `5V_SYS` or the sensor rails.
4. **Stage D:** `config-latch`, `battery-adc` (compare with a DMM at `BATT`),
   `espnow-basic`, then `pwr-hold` (SW10 at run). Then the 25 wake/hold/shutdown
   cycles.
5. **Stage E:** `rtc-alarm`, then `wake-reason` (SW10 at run; the runner follows
   the board through each power cycle). `rtc-alarm` leaves Alarm 1 armed with
   its flag set, which holds VSYS on (finding 2). `pwr-hold` clears it, and so
   does the first `wake-reason` boot. `wake-reason` re-arms a 15 s alarm on
   every wake, so leaving it running gives the 25 autonomous off-wake cycles
   directly.
6. **Stage F (SD):** with the card out, run `sd-nocard --repeat 25`: the R5
   plan's 25 boots without media. Then insert the reference card and run
   `sd-card`, `sd-sweep`, `sd-logger`, and `sd-logger --repeat 100` for the
   100-reboot soak. Scope at 400 kHz and at the highest passing rate with
   `sd-sweep --stay`, then `p 400` / `p 40000`.
7. **Stage G (rails):** bench only. Apply finding 2.
8. **Stage H (sensors):** `i2c-mux` first with nothing plugged in, then once per
   harness, and `w` (hot-plug watch) while plugging each port. `ads1015` with
   inputs open, then reference points with `--stay` and `p <ch> <mV>` at 0,
   1000, 2500 and 4500 mV on each channel. `reed` with a pushbutton or the
   anemometer on the reed connector; `n 20` gives an exact-count check.
9. **Stage J (modem):** antenna fitted before anything enables the modem. Run
   `modem-power`, `modem-uart`, `modem-identify`, `modem-sim`, `modem-network`,
   `modem-https`, then `modem-powercycle`.
10. **Stage K:** `production` flashes `mothership-v1-main` for the integrated
    tests. Stage I (charger) and Stage L remain bench work under the R5 plan.

## New sketches: what PASS means

**`sd-sweep`** (`bringup_hub_sd_sweep.cpp`). `SD_MISO` idles high, and the card
mounts at 400 kHz. Then, at each of 400 kHz and 4/10/20/40 MHz: three mounts,
and 128 KiB of a pattern seeded per speed, written, read back and compared with
zero bad bytes. It uses VSPI, as production does. A 40 MHz failure is a FAIL,
because production `initSD()` mounts at 40 MHz. `SD_MARGIN` reports the
highest contiguous pass. `sd-nocard` is the same sketch, started with `n`: no
speed may mount, and each failed attempt must return within 3 s. Commands:
`r` re-runs the sweep; `n` runs the no-card check; `p <kHz>` is a probe loop
for scope captures.

**`sd-raw`** (`bringup_hub_sd_raw.cpp`), for use when a mount fails. It first
checks each ESP32-to-pull-up path. It then runs CMD0, CMD8, CMD55/ACMD41 and
CMD58 twice: bit-banged at about 50 kHz, then on VSPI at 400 kHz. Finally it
runs a matrix of CRC on/off against ACMD41 `0x40000000` / `0x40100000`. The
`0x40000000` variants are checks; the `0x40100000` variants are WARN, because
they describe the card, not the board. `s` holds static levels on the pins for
DMM probing.

**`i2c-mux`** (`bringup_hub_i2c_mux.cpp`). SDA1/SCL1 idle high and hold high
against the internal pull-down, which proves F2 is fitted. The root bus holds
exactly `0x48`, `0x68` and `0x71`, with no bus errors. An ack at `0x70`, `0x72`
or `0x73` points at a strap fault. The RTC registers read sensibly; a set OSF
flag is a WARN on a new board. Each mux channel reads back from its control
register, and deselect restores the root set. Every channel scans with no bus
errors and is reported by port name. An SHT4x at `0x44`/`0x45` must pass its
serial-number CRC and give a plausible measurement. `0x39` (AS734x) is reported
when it acks but is not register-checked. Empty ports are WARN. A 400 kHz
rescan must give identical device sets. `w` is the hot-plug watch.

**`ads1015`** (`bringup_hub_ads1015.cpp`). The ADC acks; two config patterns
read back exactly, PGA bits included; all four channels convert without
timing out and without railing. Noise must be at most 4 LSB (12 mV)
peak-to-peak over 64 samples; the 5 V boost sits next to the ADC, so this is
the measurement the reviews asked for. An open input reading more than
15 mV is a WARN. `p <ch> <mV>` is a reference-point check with a tolerance
of 1 % + 2 LSB; it refuses anything above 5.3 V. `c` streams all channels.

**`reed`** (`bringup_hub_reed.cpp`). GPIO36 idles high. No false edges appear
during 4000 ADC1 reads or across 5 Wi-Fi on/off cycles. Then the operator
steps: a close and a release are each detected within 30 s, and a 10 s spin
counts at least 5 debounced pulses. The spin result prints Hz, m/s at
0.6667 m/s per Hz, raw edges against debounced edges (the bounce), and the
minimum raw gap. `a` runs only the automatic checks; `n <N>` is the
exact-count check; `c` prints counts every second.

## Limits of the harness

- The restored V1 sketches report in free text. The runner asks for the verdict
  on `pwr-hold`, `config-latch`, `battery-adc`, `espnow-basic`, `rtc-alarm`,
  `wake-reason`, `modem-power`, `modem-uart`, `modem-https` and `production`.
- No firmware can read rail voltages other than `BATT`. Stages B, C, G, I and L
  are DMM, scope, load and thermal work, recorded with `record`.
- AS734x parts are confirmed only by an address ack. A spectral read needs the
  node sensor module, which the hub firmware does not yet link
  (`HUB_LOCAL_SENSING_PLAN.md`).
- `espnow-basic` proves only that ESP-NOW initialises and receives on channel 11
  without an AP. Pairing and data transfer are Stage K, run with production
  firmware and a known-good node.
