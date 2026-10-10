# FieldHub v2 revision 5 bring-up plan

- **Plan date:** 2026-10-10
- **Hardware under test:** `revisions/2026-10-08-r5`
- **Initial batch:** five boards
- **Primary objective:** prove power integrity and every hardware interface before
  allowing unattended battery charging or field deployment.

This is a staged acceptance plan. A board advances only when the current stage
has a recorded pass. Stop at the first unexplained over-current, rail error,
over-temperature, unstable waveform, reset, or corrupted write. Do not work
around a failure by moving to a later subsystem.

## Revision-5 microSD design check

The revision-5 source project and fabrication test netlist agree with each
other and with the SHOU HAN `TF-CARD H1.8 SY` (`C7529391`) drawing.

| Socket contact | Card function | Revision-5 connection | Result |
|---|---|---|---|
| TF1.1 | DAT2 | 10 kOhm pull-up to `3V3_SYS`; no SPI data connection | Correct |
| TF1.2 | CD/DAT3 | `SD_CS` to ESP32 GPIO13 through R24, 22 Ohm; 10 kOhm pull-up | Correct |
| TF1.3 | CMD | `SD_MOSI` to ESP32 GPIO23 through R26, 22 Ohm; 10 kOhm pull-up | Correct |
| TF1.4 | VDD | `3V3_SYS`, with C130 100 nF and C131 10 uF local bypass | Correct |
| TF1.5 | CLK | `SD_SCK` to ESP32 GPIO18 through R27, 22 Ohm | Correct |
| TF1.6 | VSS | GND | Correct |
| TF1.7 | DAT0 | `SD_MISO` directly to ESP32 GPIO19; 10 kOhm pull-up | Correct |
| TF1.8 | DAT1 | 10 kOhm pull-up to `3V3_SYS`; no SPI data connection | Correct |
| TF1.CD | Mechanical card-detect switch | Not connected | Intentional limitation |
| TF1.10-13 | Socket shell/anchors | GND | Correct |

The previous board fault was GPIO23/MOSI connected to TF1.8/DAT1 instead of
TF1.3/CMD. Revision 5 routes GPIO23 to TF1.3 and leaves TF1.8 as a pull-up-only
unused SPI contact, so that specific fault is fixed.

The footprint also matches the connector drawing:

- signal pads are 0.70 x 1.60 mm on 1.10 mm pitch;
- the contact order is `CD, 8, 7, ... 1` when viewed from the component side;
- the two 1.00 mm locating holes are present as NPTH and are 8.00 mm apart; and
- the four shell anchors match the recommended land pattern.

This confirms the design files, not the quality of a future assembly. The
continuity checks and powered SD tests below remain mandatory.

Exact socket datasheet:
<https://datasheet.lcsc.com/datasheet/pdf/f91cb3ef17480eaaa64b684100a3fff4.pdf?productCode=C7529391>

## 1. Prepare before boards arrive

### Lock the build inputs

The repository's revision-5 directory currently contains the EasyEDA project
and Gerber ZIP only. It does not contain a revision-5 BOM, pick-and-place file,
assembly drawing, or schematic PDF. That is sufficient for a bare-PCB order,
but not an independently reproducible PCBA order.

Before assembly, export the BOM and pick-and-place data from the exact
revision-5 source, archive the assembler's final files, and verify the preview.
Do not silently combine revision-5 Gerbers with an older BOM/CPL. If the
October 8 BOM/CPL are intentionally reused, formally compare every reference,
value, footprint, side, coordinate, and rotation against revision 5 and record
the result. In particular, require TF1 to be `C7529391`, not a merely similar
socket with a different contact or anchor pattern.

### Equipment

- current-limited bench supply with current logging, initially set to 3.8 V;
- DMM with resistance, diode, continuity, current, and min/max modes;
- oscilloscope with x10 probes and short ground springs;
- electronic load and, for charger tests, a supply/load arrangement that can
  safely sink charge current;
- thermal camera or contact thermocouples;
- USB data cable and a Linux serial device such as `/dev/ttyUSB0`;
- one blank, known-good 8-32 GB FAT32 microSD card, then at least two different
  card makes/capacities for margin testing;
- a protected 1S test cell whose manufacturer permits 600 mA charging;
- panel simulator or current-limited 6 V-class source before using the real
  solar panel;
- LTE antenna and activated SIM; attach the antenna before enabling the modem;
- known-good FieldMesh node and known-good I2C/analogue sensor loads;
- ESD mat, eye protection, and a fire-safe area for charging work.

### Firmware readiness

The usable physical SD regression target is:

```sh
cd mothership/firmware/v2
PIO_BIN="${PIO_BIN:-$HOME/.platformio/penv/bin/pio}"
"$PIO_BIN" run -e mothership-v2-test-sd-logger
"$PIO_BIN" run -e mothership-v2-test-sd-logger -t upload --upload-port /dev/ttyUSB0
"$PIO_BIN" device monitor --port /dev/ttyUSB0 --baud 115200
```

The explicit `PIO_BIN` fallback is needed on the present development machine,
where PlatformIO is installed in its virtual environment but `pio` is not on
the shell `PATH`.

Do not use `mothership-v1-sd-card`: its configured source file is absent from
this repository. Several other older hardware bring-up environments have the
same problem. Before the boards arrive, port or create small, self-contained
tests for power hold/config latch, RTC/alarm, battery ADC, sensor rails/I2C/ADC,
ESP-NOW, and basic modem power. Do not make the full application the only
diagnostic tool.

The current SD code requests 40 MHz immediately. Add a diagnostic-only speed
sweep of 400 kHz, 4 MHz, 10 MHz, 20 MHz, and 40 MHz so signal-integrity margin
can be distinguished from a wiring fault. Production speed should be the
highest rate that passes all cards and temperature/load conditions with margin,
not automatically 40 MHz.

## 2. Batch and evidence control

Label the boards `HUB-R5-01` through `HUB-R5-05` before testing.

- Use `HUB-R5-01` for the complete engineering sequence, including fault,
  power-cut, maximum-load, and thermal tests.
- Repeat the full sequence on `HUB-R5-02` to distinguish a design result from
  a one-board result.
- Run the shortened production acceptance sequence on boards 03-05 only after
  boards 01 and 02 pass.
- Keep one log per board. Record supply voltage/current, rail readings,
  firmware commit, SD-card identity, instruments, ambient temperature, pass or
  fail, rework, and links to scope captures.
- Photograph both sides before power and after any rework.

Never silently reclassify a failure as a pass after rework. Record the original
failure, the change, and the complete repeated stage.

## 3. Stage A — assembly and mechanical inspection

Inspect at magnification before connecting USB, a battery, a panel, sensors,
SIM, or SD card.

- Compare every populated value and orientation with the assembly BOM and
  placement preview.
- Check pin 1 and polarity on U5, U21, U30, U33, U38, U45, U49, U50, U51,
  U52, U58, U59, U60-U62, all diodes, LEDs, MOSFETs, electrolytics, and the
  battery holder.
- Confirm the exact `C7529391` socket is fitted at TF1 and that all eight card
  contacts, the CD contact, and all shell anchors are soldered.
- Inspect USB-C, SIM, U.FL, JST, terminal block, switches, and manual/THT parts.
- Confirm all four 3.4 mm mounting holes are open NPTH holes with no visible
  copper annulus. Check screw-head and enclosure clearance before fitting metal
  hardware.
- Confirm the ESP32 antenna region and board-edge notch are free of solder,
  metal, wiring, battery, fasteners, and enclosure hardware.
- Confirm SJ4/SJ5 and their alternative zero-ohm options match the documented
  configuration; do not guess from the schematic.

**Pass gate:** no wrong/missing part, reversed polarity, solder bridge, lifted
pin, blocked hole, damaged connector, or unexplained assembly discrepancy.

## 4. Stage B — unpowered electrical checks

Remove the RTC coin cell, main cell, panel, USB, sensors, SIM, antenna, and SD
card.

### Rails

Measure resistance to GND on:

- `BATT1` / `RAW_BAT`;
- `VSYS1` / `VSYS`;
- `AON2` / `KEEP_ALIVE`;
- `3V4` / `3V3_SYS` (the test-point name says 3V4, but the net is 3.3 V);
- `5V` / `5V_SYS`;
- `4V` / `M_VBAT`;
- `1V8` / `M_1V8`;
- `3V3_SEN` and `5V_SEN` at their connector pins.

Capacitors can make an ohmmeter reading rise. The reject condition is a stable,
low-resistance short or a value materially different from the other untouched
boards, not an arbitrary single resistance threshold.

### SD continuity matrix

With no card inserted, verify:

- TF1.4 to `3V3_SYS`: near zero Ohm;
- TF1.6 and shell pads 10-13 to GND: near zero Ohm;
- ESP32 GPIO13 to TF1.2: approximately 22 Ohm;
- ESP32 GPIO23 to TF1.3: approximately 22 Ohm;
- ESP32 GPIO18 to TF1.5: approximately 22 Ohm;
- ESP32 GPIO19 to TF1.7: near zero Ohm;
- TF1.1, 2, 3, 7, and 8 to `3V3_SYS`: each has its intended 10 kOhm pull-up
  path (allow for parallel paths/in-circuit meter effects);
- GPIO23 is not connected to TF1.8; and
- adjacent signal pads are not shorted.

TF1.CD is deliberately not routed to the ESP32. Its mechanical switching can
be characterised at the socket, but production firmware cannot use it.

### Connector harnesses

Continuity-test every cable before connecting a sensor. Record the view used
(PCB mating face, cable mating face, or solder face). The PCB assignments are:

| Connector | Pin 1 | Pin 2 | Pin 3 | Pin 4 |
|---|---|---|---|---|
| CN17-CN20 | `3V3_SEN` | GND | SDAx | SCLx |
| CN15 | `5V_SEN` | GND | A1 | A0 |
| CN16 | `5V_SEN` | GND | A2 | A3 |

**Pass gate:** no short, all required point-to-point paths agree with the
netlist, and all harnesses have a signed continuity record.

## 5. Stage C — first current-limited power

Do this without an SD card, SIM, sensors, panel, or production battery.

1. Set the bench supply to 3.8 V and 100 mA and connect it with correct polarity
   at the battery input. Keep a finger or thermal camera on the board.
2. If the board reaches current limit, disconnect immediately. Locate the load
   before raising the limit.
3. Exercise the intended wake/config action. Once there is no smoke-test fault,
   raise the limit only as required for an ESP32 boot, initially no more than
   500 mA with modem and sensor rails disabled.
4. Record input current in off, boot, idle/config, Wi-Fi-active, and shutdown
   states.
5. Verify `KEEP_ALIVE`, `VSYS`, `3V3_SYS`, and `5V_SYS`. Expected active values
   are approximately 3.3 V and 4.99 V; investigate any sustained value outside
   3.20-3.40 V or 4.85-5.15 V.
6. Check startup overshoot and ripple at U50 output and `5V_SYS` with the scope.
7. Repeat using 3.2 V, 3.8 V, and 4.2 V input after nominal operation passes.
8. Apply reverse polarity only with a very low current limit. Confirm Q37
   prevents downstream power and that current stays at the leakage/background
   level established by the circuit, not a rising or heating load.

The earlier desktop review raised questions about the 5 V converter geometry.
Treat those as a measurement target, not as a diagnosed failure. Probe U21,
L4, D6, C3/C75/C76, and the feedback network under load. Decide from ripple,
ringing, temperature, regulation, and ADC-noise results.

**Pass gate:** no unexplained current limit, overheating, rail overshoot,
brownout/reset loop, or rail outside its acceptance band across 3.2-4.2 V.

## 6. Stage D — USB, programming, ESP32, and hard-power control

1. With the bench supply still current limited, connect USB and confirm CH340C
   enumeration and a stable serial port.
2. Check that USB does not back-feed the battery or solar connector to an unsafe
   voltage/current.
3. Flash a minimal serial build, reset repeatedly, then flash the production
   build.
4. Verify EN/BOOT behaviour and that programming does not depend on manually
   forcing an undocumented signal.
5. Verify config-button wake, `PWR_HOLD` assertion, config-clear operation, web
   shutdown, and a clean physical power cut after releasing hold.
6. Perform 25 wake/hold/shutdown cycles while logging input current and reset
   reasons.
7. Verify Wi-Fi AP creation, dashboard access, and reliable USB serial operation
   with at least two cables/host ports.

**Pass gate:** 25 clean cycles, repeatable flashing, no unexpected back-feed,
and no boot strap or latch state that leaves the unit unpredictably on or off.

## 7. Stage E — RTC, coin cell, and autonomous wake

1. Confirm U49 responds at I2C address `0x68` before installing the coin cell.
2. Install a known-good CR1220 with correct polarity; set UTC time and remove
   main power for at least ten minutes.
3. Restore main power and verify retained time and oscillator status.
4. Exercise Alarm 1 and Alarm 2 separately, then together. Confirm the `ALM2`
   / `INT_RTC` line asserts, clears, and re-arms without disturbing the other
   alarm.
5. Verify scheduled wake from fully off, config wake priority, service/USB wake,
   and shutdown only after the next required alarm is confirmed armed.
6. Run at least 25 autonomous off-wake-work-off cycles.

**Pass gate:** retained UTC time, correct wake classification, no stuck-low
interrupt, and 25 consecutive autonomous cycles.

## 8. Stage F — microSD hardware and archive path

Use a blank card for destructive and power-cut tests.

### Static power

1. With no card fitted, confirm TF1.4 is 3.20-3.40 V and TF1.6/shell are GND.
2. Insert the card with power off. Confirm the 3.3 V rail does not collapse or
   show an abnormal resistance/current change.
3. During initialization and writes, scope 3.3 V at TF1.4 relative to TF1.6.
   Record minimum voltage, ripple, and transient droop.

### SPI proof

Run the diagnostic speed sweep before the full regression suite.

- At 400 kHz, verify clock on TF1.5, command traffic on TF1.3, CS on TF1.2,
  and a response on TF1.7.
- A toggling TF1.8 with no TF1.3 traffic would indicate the old fault; that is
  not what the revision-5 netlist specifies.
- Repeat at 4, 10, 20, and 40 MHz. Save scope captures of CLK, MOSI, MISO, CS,
  and socket VDD at the highest passing rate.
- Check for overshoot/undershoot, double-clocking, slow edges, and rail droop.
  If 40 MHz is marginal, lower production speed instead of accepting a
  card-brand-dependent design.

### Functional and abuse tests

1. Run `mothership-v2-test-sd-logger`; require `OVERALL:PASS` and inspect the
   card on a PC afterward.
2. Repeat with at least three cards. Record make, model, capacity, formatting,
   and speed result.
3. With production firmware, receive real node rows and confirm identical
   canonical records in the intended LittleFS/SD paths.
4. Reboot 100 times with a card present and 25 times without one. Missing media
   must not crash or block the hub.
5. Remove the card when idle and during a controlled write on the sacrificial
   board. The write must fail visibly and LittleFS operation must continue.
6. Cut main power during repeated writes at several timing offsets. After every
   cut, check the filesystem on a PC, headers, prior rows, and the ability to
   append after reboot.
7. Test near-full media and write-protected/read-only failure where practical;
   require a visible error and intact LittleFS fallback.
8. Run a four-hour continuous write/read/verify loop, then repeat at the cold
   and hot bench temperature limits chosen for the product.

Current limitations to record honestly:

- TF1 card detect is not connected, so insertion is inferred only by an SD
  mount attempt.
- `initSD()` is called at startup; there is no demonstrated automatic hot-card
  remount loop. Reinsertion may require a reboot unless that feature is added.
- Socket-side ESD protection is not present. If the card is user-accessible in
  the final enclosure, add an enclosure/ESD acceptance test or revise the
  protection strategy.

**Pass gate:** all three cards initialize and pass file verification; no crash
without media; no silent write loss; fallback remains available; and the chosen
SPI rate has waveform and multi-card margin.

## 9. Stage G — 5 V converter and protected sensor rails

1. Load `5V_SYS` from zero to the intended maximum at 3.2, 3.8, and 4.2 V
   battery input. Record regulation, input current, efficiency, startup,
   switch-node waveform, ripple, and temperatures at U21/L4/D6/C76.
2. Enable `3V3_SEN` and `5V_SEN` separately with no sensor, dummy load, and real
   harness load.
3. Confirm the approximate intended current limits: 200 mA for `3V3_SEN` and
   300 mA for `5V_SEN`. Use a controlled electronic load, not a wire short, for
   initial characterization.
4. After controlled overload behaviour is understood, briefly short each
   sensor rail on `HUB-R5-01`. Confirm auto-retry, recovery after fault removal,
   acceptable device temperature, and that the other domain and ESP32 remain
   operational.
5. Hot-plug every final harness and verify connector-end voltage and ground
   offset at maximum cable length.

**Pass gate:** regulation across input/load range, repeatable auto-recovery,
fault containment, and safe temperatures.

## 10. Stage H — I2C mux, ADC, and reed input

Use dedicated test firmware; the current production firmware is not sufficient
evidence for every local sensor path.

1. Verify the board I2C bus and expected devices individually. The RTC is at
   `0x68`; the intended TCA9546A mux address is `0x71`; confirm the actual
   ADS1015 address from the populated strap state.
2. Select every TCA9546A channel and scan with the final harness. Verify only
   the expected downstream devices appear and no channel holds SDA/SCL low.
3. Measure SDA/SCL rise time, low level, crosstalk, and noise at minimum and
   maximum cable/load combinations.
4. For each ADS1015 channel, apply 0 V and several traceable values up to the
   allowed input/rail limit. Verify gain selection, scale, channel order,
   monotonicity, and over-range handling. Do not assume the +/-6.144 V PGA
   setting permits an input above the ADC supply.
5. Test the chosen soil probes across their full expected output and
   environmental range. The reported TH-V5 family is specified for a higher
   supply than 5 V, so apparent operation at 5 V is not acceptance evidence;
   qualify the exact part or change the interface.
6. Exercise the reed input slowly and at maximum expected pulse rate. Measure
   threshold, leakage, contact bounce, debounce behaviour, missed/extra counts,
   long-cable noise, hot-plug, and ESD response.

**Pass gate:** correct channel identity and scaling, robust I2C timing with the
final harness, no rail coupling, and no missed/extra reed counts in the defined
operating range.

## 11. Stage I — solar charger and battery system

Do not leave this test unattended. The PCB does not measure cell temperature,
so the system needs an external operating rule or additional design control to
prevent charging outside the cell manufacturer's limits.

1. First use a battery simulator or protected test cell with a documented
   600 mA charge rating. A normal bench supply may be damaged if it cannot sink
   charger current.
2. Verify correct-polarity and reverse-polarity battery behaviour.
3. Sweep the panel input with a current limit. Confirm D14 polarity, C89 voltage
   margin, charger startup, and a charger-side MPPT point near 5.60 V. Record
   the corresponding panel-side voltage; approximately 6.0-6.2 V is expected
   after D14 under load, but use measured diode drop.
4. Verify approximately 600 mA current limit, 4.2 V regulation, taper,
   termination, and recharge against the exact cell datasheet.
5. Repeat while the hub is off, awake, writing SD, powering sensors, using
   Wi-Fi, and later transmitting over LTE. System load must not create false
   termination or uncontrolled recharge cycling.
6. With a charged cell and a dark/disconnected panel, measure reverse current
   out of the solar input.
7. Test actual panel cold open-circuit voltage and hot/low-light operation;
   confirm all voltage and temperature margins.
8. Scope the CN3791 switch node and both sides of R25 with short ground springs.
   Use measured differential noise/current-regulation behaviour to decide
   whether sense routing needs revision; do not infer failure from a screenshot.

**Pass gate:** controlled charge current/voltage, reliable termination/recharge
under system load, safe temperatures, acceptable reverse current, and a written
battery/temperature operating envelope.

## 12. Stage J — LTE modem, SIM, and RF

Attach the correct LTE antenna before enabling U59/U58. Never hot-plug U.FL.

1. With the modem disabled, confirm `4V`/`M_VBAT` is off and there is no unsafe
   back-feed.
2. Enable the TPS63020 rail using the established soft-start. Record rail rise,
   `ESP_PG`, peak input current, `M_VBAT` ripple, and temperature.
3. Verify the 1.8 V rail, PWRKEY pulse, STATUS transition, GPIO level shifting,
   and UART at 115200 baud.
4. Run AT, identity/IMEI, SIM presence, and three graceful power cycles before
   network registration.
5. Register on the network and record operator, RAT, RSRP/RSRQ/SINR or available
   equivalents, time to register, and supply droop during transmit bursts.
6. Complete HTTPS dry-run upload, real ingest, and graceful detach. Confirm SD
   and ESP32 operation during worst-case modem bursts.
7. Repeat at minimum battery voltage and in the final enclosure/antenna
   position, including weak-signal operation.

**Pass gate:** no brownout or rail violation, reliable AT/SIM/network operation,
successful HTTPS, graceful shutdown, and acceptable enclosure RF results.

## 13. Stage K — integrated FieldMesh operation

1. Provision the hub and one known-good node.
2. Verify discovery, pairing, deployment, RTC synchronization, and a complete
   node snapshot.
3. Confirm the same real snapshot is retained in LittleFS and the SD archive,
   uploaded once, and remains downloadable after reboot.
4. Exercise config wake, scheduled wake, sync-window broadcasts, early
   shutdown, alarm re-arm, and physical power-off.
5. Expand to the intended node count. Test simultaneous queues, missing nodes,
   duplicate frames, delayed nodes, full SD, absent SD, absent network, and
   recovery after power interruption.
6. Run at least 100 automated wake/sync/write/upload/shutdown cycles, including
   a 24-hour soak.
7. Confirm no duplicate/lost rows across LittleFS, SD, and cloud using node ID,
   sequence number, deployment epoch, and event ID.

**Pass gate:** 100 consecutive cycles with reconciled records, no unexpected
reset, no stuck-powered state, and correct degraded behaviour when SD or LTE is
unavailable.

## 14. Stage L — thermal, enclosure, and final acceptance

Run worst credible simultaneous load: charging, maximum qualified sensor load,
continuous SD activity, Wi-Fi/ESP-NOW, and LTE bursts.

- Log every rail, input current, reset reason, SD error, and temperature.
- Check U5, U21, U3/U4, U50/U51, U59, U45, U58, inductors, diodes, current-sense
  resistor, connectors, and battery lead.
- Repeat at the declared ambient limits and minimum/maximum battery voltage.
- Verify mounting hardware cannot contact copper and cannot compromise the
  ESP32/LTE antenna regions.
- Perform cable, card-slot, USB, and enclosure ESD tests appropriate to actual
  user access. Record the method and acceptance level.
- Repeat critical USB, SD, Wi-Fi, ESP-NOW, LTE, and sensor tests in the final
  enclosure.

## 15. Short production acceptance for boards 03-05

After boards 01 and 02 complete all engineering gates, each remaining board
must at minimum pass:

1. visual/polarity inspection and complete unpowered rail/SD continuity matrix;
2. current-limited first boot at 3.8 V and rail measurements;
3. USB enumeration, flash, config wake, PWR_HOLD shutdown, and RTC wake;
4. SD regression on the reference card plus one verified real snapshot;
5. 5 V and both sensor rails at representative load;
6. I2C scan, one ADC point per channel, and reed pulse check;
7. modem AT/SIM/network/HTTPS with antenna attached;
8. one-node end-to-end snapshot, SD archive, upload, reboot, and download; and
9. a minimum one-hour integrated thermal/functional soak.

## 16. Release decision

The five-board batch is bring-up complete only when:

- boards 01 and 02 pass the full plan independently;
- boards 03-05 pass production acceptance;
- every failure and rework has a closed record;
- the selected battery, panel, sensors, SIM, SD cards, antenna, harnesses, and
  enclosure are identified by exact part/revision;
- SD data reconcile across card, LittleFS, and cloud;
- no unresolved over-current, over-temperature, reset, signal-integrity, RF,
  or data-integrity failure remains; and
- firmware is tagged with the exact build used for acceptance.

Bring-up proves functional readiness of this prototype. It does not replace
formal electrical safety, EMC/ESD, radio, environmental, battery, or metrology
qualification.
