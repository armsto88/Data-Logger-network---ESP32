# Mothership V2 LTE Backhaul Concept

This note documents a proposed LTE backhaul subsystem for mothership V2.

It is a planning and PCB-review document, not a claim that the modem, power rail, antenna path, or firmware upload workflow has already been validated on hardware.

## 1. Purpose

The current mothership workflow is:

- sensor nodes -> ESP-NOW -> mothership -> SD card CSV logging

The proposed LTE workflow extends that path to:

- sensor nodes -> ESP-NOW -> mothership -> SD card CSV logging -> scheduled LTE upload

Important boundary:

- LTE backhaul does not replace ESP-NOW.
- ESP-NOW remains the local node-to-mothership telemetry and command path.
- The LTE modem is only a mothership-side backhaul subsystem for remote upload, status reporting, and possible future alert delivery.

## 2. Current Proposal Status

Confirmed project direction for this note:

- treat LTE as an optional proposed mothership V2 subsystem
- keep SD card logging as the primary local record
- separate confirmed current workflow from unvalidated LTE design work

Open proposal items:

- final modem footprint and module variant selection
- final power rail architecture for the modem
- final upload protocol choice (`MQTT`, `HTTPS POST`, file upload, or another transport)
- final power schedule and retry policy
- final enclosure antenna implementation

## 3. Preferred Modem Candidate

Current preferred modem:

- `SIMCom A7670G`

Current rationale:

- global LTE Cat-1 class option
- includes important LTE bands for Australia, especially `B28`
- suitable for low-to-moderate telemetry and logger backhaul
- controllable by the ESP32 using UART `AT` commands
- compatible with workflows such as `MQTT`, `HTTPS POST`, and periodic queued data upload

This note treats the modem as a separate PCB subsystem, not as an extension of the ESP-NOW radio path.

## 4. System-Level Workflow Context

### 4.1 Current mothership workflow

1. Mothership receives ESP-NOW packets from deployed nodes.
2. Mothership writes structured records to SD card as CSV.
3. Mothership maintains local deployment state, node state, and schedule metadata.
4. Browser UI is used for configuration, deployment, and local monitoring.

### 4.2 Proposed LTE-extended workflow

1. Continue receiving node data over ESP-NOW.
2. Continue saving all data to SD card as the primary record.
3. At scheduled intervals, power on the LTE modem.
4. Wait for modem boot and network registration.
5. Upload selected data, summaries, or queued CSV records.
6. Confirm successful upload.
7. Record upload status to SD or other local metadata.
8. Gracefully shut down the modem.
9. Disable the modem regulator or load switch if appropriate.

Design intent:

- LTE should be additive to the existing logger workflow, not a replacement for local persistence.
- If LTE upload fails, the primary data record must still exist locally on SD.

## 5. Proposed Hardware Integration

Document the LTE subsystem as a distinct mothership V2 hardware block:

`ESP32 mothership -> UART2 -> 3.3 V <-> 1.8 V level shifting -> SIMCom A7670G -> SIM holder + LTE antenna`

Key design rules:

- do not use the ESP32 programming UART for the modem
- prefer a second UART, likely `UART2`
- treat the modem UART/control interface as a `1.8 V` domain requiring level shifting
- do not power the modem from the ESP32 `3.3 V` rail
- add a dedicated modem supply rail, currently expected to be around `4.0 V`

### 5.1 Recommended subsystem split

- ESP32 host/control domain
- modem logic interface domain (`1.8 V` side of translator)
- dedicated modem power rail (`3.8-4.0 V` high-current rail)
- SIM connector domain
- RF path from modem to external antenna connector

### 5.2 Candidate modem-related signals

At minimum, document these signals in the mothership schematic plan:

- `MODEM_TX`
- `MODEM_RX`
- `MODEM_PWRKEY`
- `MODEM_STATUS`
- `MODEM_RESET`
- `MODEM_DTR`
- `MODEM_RI`

Recommended control notes:

- `PWRKEY` should be driven through a transistor or open-drain pull-down, not directly as a raw push-pull GPIO abstraction.
- `RESET` should also be transistor-controlled and used primarily for recovery.
- `STATUS` should be readable by the ESP32 so firmware can confirm the modem on/off state.
- `DTR` may later be useful for modem sleep and wake handling.
- `RI` may later be useful for modem event or wake behavior.
- USB test pads for the modem should be considered for debugging or firmware recovery.

## 6. Proposed Power Architecture

The modem should have a dedicated high-current rail.

Current target:

- dedicated `3.8-4.0 V` modem rail

Candidate parts currently being considered:

- regulator: `TPS63020DSJR`, `LCSC C15483`
- inductor: `JIERR PCR0420-1R5-M`, `LCSC C53896855`
- output target: `4.0 V` modem rail

Current power-rail requirements to document:

- at least `2 A` peak current capability
- preferably `3 A` design margin
- `330-470 uF` bulk capacitance near modem `VBAT`
- `10 uF`, `1 uF`, and `100 nF` ceramic capacitors close to modem `VBAT` pins
- wide modem power routing
- optional `TVS` protection on modem `VBAT`

Important design rule:

- the modem power path should be treated as a burst-current subsystem with its own layout and decoupling needs, not as a small accessory load hanging off the ESP32 rail.

### 6.1 Modem power-control options to evaluate

Open control options:

- regulator enable only
- load switch only
- `PWRKEY` only
- combination of regulator enable plus `PWRKEY`

Current preferred direction for investigation:

- use a hardware power-disconnect method such as regulator enable or load switch for true standby removal
- use `PWRKEY` for the modem's intended boot and graceful shutdown sequence while the rail is present

This combined approach is likely easier to reason about than relying on `PWRKEY` alone while leaving the modem rail energized indefinitely.

## 7. Antenna And RF Path Concept

Current RF concept:

- `A7670G RF_ANT -> pi matching network -> u.FL/IPEX connector -> external LTE antenna`

Preferred antenna approach:

- external waterproof LTE antenna
- `50 ohm` antenna system
- wideband LTE coverage, approximately `700-2700 MHz`
- `u.FL/IPEX` to `SMA` bulkhead pigtail for enclosure mounting

Important PCB notes:

- keep the RF path short and controlled
- reserve the matching network footprint even if first-pass tuning is not complete yet
- keep the modem RF section physically away from noisy digital and switched-power regions as much as the enclosure and board outline allow
- verify that enclosure wall material, bulkhead position, and pigtail routing do not create an awkward or lossy antenna install

## 8. Firmware Workflow Integration Feedback

The cleanest firmware integration is to keep LTE upload downstream of the current storage contract rather than tightly coupling it to live ESP-NOW packet handling.

Recommended architecture direction:

- ESP-NOW ingest remains responsible for receiving node data and updating current mothership state
- SD logging remains responsible for durable local persistence
- LTE upload logic operates on persisted local records or upload queues after they have been committed locally

This has several advantages:

- LTE failures do not block or complicate local ingest
- upload retry can be based on durable state rather than transient RAM-only queues
- the modem can remain fully off most of the time
- the browser UI can continue to reflect local system state even if LTE is unavailable

### 8.1 Recommended firmware block split

- node ingest and ESP-NOW handling
- local record persistence and CSV management
- upload queue or upload cursor management
- modem power-state and AT-command manager
- transport client (`MQTT`, `HTTPS`, or another chosen protocol)
- upload result logging and retry scheduling

### 8.2 Suggested upload record strategy

Prefer one of these models:

- append every record locally, then upload unsent records using a durable cursor
- append every record locally, then periodically upload summaries plus a retained backlog when bandwidth allows

Avoid this first-pass architecture:

- treating LTE upload success as a prerequisite for considering the data stored

The local SD record should remain authoritative.

## 9. Scheduling And Power Questions

This subsystem introduces a new scheduling problem that should remain explicitly open until the mothership power model is settled.

Questions to investigate:

- should LTE upload happen after every logging interval, once per day, or only after enough data has accumulated?
- should the modem only be powered during upload windows?
- should failed uploads be queued and retried later?
- should LTE uploads be skipped below a battery-voltage threshold?
- should the mothership maintain separate schedules for node collection, SD logging, LTE upload, and local configuration mode?
- should LTE upload run immediately after a successful ESP-NOW collection cycle, or as a separate scheduled event?
- should modem control rely on regulator enable, load switch, `PWRKEY`, or a combined method?

### 9.1 Current recommended scheduling direction

For first-pass architecture work, the most defensible default is:

- keep node collection and SD logging on their existing schedule path
- keep LTE upload as a separate scheduled event or batch window
- power the modem only during intended upload windows
- skip upload or shorten retry behavior when battery or energy conditions are poor

Reasoning:

- LTE registration and upload consume enough energy and time that they should not automatically happen after every small node event unless the deployment genuinely requires near-real-time backhaul.
- decoupling collection from upload keeps the power model easier to analyze.

## 10. Recommended Modem Upload Power Sequence

Document the following proposed upload cycle:

1. Enable modem regulator or load switch.
2. Wait for modem `VBAT` to stabilize.
3. Pull `PWRKEY` low using an ESP32-controlled transistor.
4. Wait for `STATUS` to indicate modem on.
5. Wait for UART readiness.
6. Send `AT` commands to check `SIM`, signal, and network registration.
7. Open `MQTT` or `HTTPS` session.
8. Upload queued data.
9. Save upload result locally.
10. Gracefully shut down the modem using an `AT` command or `PWRKEY` shutdown sequence.
11. Wait for `STATUS` off.
12. Disable the modem regulator or load switch to remove standby drain.

Recommended implementation note:

- firmware should time-bound each stage and classify failures clearly, for example: rail enable failure, modem not booting, SIM absent, registration timeout, transport connect failure, upload reject, or clean shutdown failure.

## 11. Open Questions And Risks

### 11.1 Power-system risk

- modem current bursts may upset the rest of the system if rail sizing, grounding, and bulk capacitance are inadequate

### 11.2 Sleep-state ambiguity

- if the modem is not truly power-removed between sessions, standby drain may be much worse than expected

### 11.3 Firmware complexity risk

- adding LTE can create a fragile coupled system if ingest, local storage, modem control, and upload retry are not separated cleanly

### 11.4 RF and enclosure risk

- bulkhead placement, antenna quality, cable losses, and enclosure geometry may dominate real-world link quality more than the modem choice itself

### 11.5 Carrier and regional risk

- even with `B28` support, actual deployment suitability still depends on local carrier behavior, antenna selection, and regional certification assumptions

### 11.6 Recovery and service risk

- if modem debug access is omitted, bring-up and field diagnosis may be unnecessarily slow

## 12. Recommended Next Steps Before PCB Finalisation

1. Confirm the exact `A7670G` module variant, footprint, and required power-on timing from the modem datasheet.
2. Confirm which ESP32 UART pins are available and defensible for a dedicated modem port without colliding with SD, RTC, boot, or service functions.
3. Decide whether the modem rail will use regulator enable, load switch, or both.
4. Validate the candidate `4.0 V` rail design against modem burst current expectations and capacitor ESR behavior.
5. Reserve schematic and layout space for the RF matching network, `u.FL/IPEX`, SIM holder, and modem USB test pads.
6. Decide the first upload protocol to support in firmware so the control-state design is not completely transport-agnostic forever.
7. Define a first-pass upload queue model and local metadata needed to mark records as pending, uploaded, failed, or deferred.
8. Decide the initial battery-threshold policy for skipping LTE upload when energy is limited.
9. Review the mothership power-state model to determine whether LTE upload should be a dedicated scheduled wake reason.
10. Bench-validate modem start-up, registration time, and burst-current behavior before locking PCB placement or copper width assumptions.

## 13. Bottom Line

The `SIMCom A7670G` is a plausible mothership V2 LTE backhaul candidate for scheduled remote upload, but it should be treated as a distinct, high-current, still-unvalidated subsystem.

The safest architectural direction is:

- keep `ESP-NOW` as the local telemetry path
- keep `SD` as the primary durable record
- use LTE only as a scheduled backhaul path on top of local persistence
- power the modem only when needed
- keep the hardware and firmware boundaries explicit so LTE complexity does not destabilize the rest of the mothership design