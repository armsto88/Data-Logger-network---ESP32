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

- final schematic and footprint review for the selected modem implementation
- final power rail architecture for the modem
- final long-term transport mix beyond the first file-upload pass
- final power schedule and retry policy
- final command/settings schema shared across web UI and later BLE/native surfaces
- final enclosure antenna implementation

## 3. Preferred Modem Candidate

Current preferred modem:

- `SIMCom A7670G`

Current rationale:

- global LTE Cat-1 modem suited to Australia, Europe, and wider multi-region deployments
- includes useful LTE bands for Australia, Europe, and general global use, with `B28` particularly important for Australian regional coverage
- suitable for low-to-moderate telemetry, queued status reporting, and scheduled CSV upload workloads
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

`ESP32-WROOM mothership -> UART2 through level shifters -> A7670G modem -> SIM holder -> u.FL/IPEX antenna connector -> internal flexible LTE antenna or external antenna option`

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

### 5.2 Current assigned ESP32 modem pinout

| Function | ESP32 pin | Notes |
| --- | --- | --- |
| UART RX | `GPIO16 / RX2` | receives modem `TXD` through level shifting |
| UART TX | `GPIO17 / TX2` | drives modem `RXD` through level shifting |
| Rail enable | `GPIO33` | drives `4V_EN` / `TPS63020 EN` |
| Rail power-good | `GPIO35` | reads `LTE_REG_PGOOD` / `TPS63020 PG` |
| `PWRKEY` control | `GPIO14` | drives NMOS pull-down stage |
| `STATUS` input | `GPIO4` | reads modem `STATUS` through level shifting |

Pin allocation note: this mapping follows the current working allocation in `MOTHERSHIP_V2_POWER_AND_WAKE_DESIGN_NOTE.md`, where `GPIO34` remains reserved for battery ADC sense (`BATTERY_VSENSE`) and is not reused for modem regulator power-good.

`GPIO4` is acceptable for modem `STATUS` because the modem rail is intended to default `OFF` during ESP32 boot, so the status source should remain inactive until the rail is explicitly enabled.

### 5.3 Current modem-related signals and control notes

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
- `RESET` is not currently assigned because of ESP32 pin limits and should at least be exposed as a test pad for recovery.
- `STATUS` should be readable by the ESP32 so firmware can confirm the modem on/off state.
- `DTR` may later be useful for modem sleep and wake handling.
- `RI` may later be useful for modem event or wake behavior.
- USB test pads for the modem should be considered for debugging or firmware recovery.

### 5.4 `PWRKEY` pull-down implementation

Current preferred `PWRKEY` control is an NMOS pull-down stage using `2N7002`:

- `GPIO14 -> 100 ohm` gate resistor -> `2N7002` gate
- `100k` gate pull-down to `GND`
- source to `GND`
- drain to modem `PWRKEY`

Behavioral intent:

- `GPIO14 HIGH` pulls `PWRKEY` low
- `GPIO14 LOW` releases `PWRKEY`
- `PWRKEY` is not level-shifted because the NMOS stage acts as the required pull-down interface

### 5.5 `1.8 V` logic-domain translation

Use `SN74LVC1T45` translators for the modem logic-domain crossings:

- `VCCA = M_1V8`
- `VCCB = 3V3_SYS`
- `M_1V8` sourced from `A7670G VDD_EXT / VDD_1V8`
- place `100 nF` from `M_1V8` to `GND` near the modem-side logic reference

Current shifted lines and intended direction:

- `ESP32 TX2 -> modem RXD`, `DIR` set from `3V3_SYS` side toward `M_1V8`
- modem `TXD -> ESP32 RX2`, `DIR` set from `M_1V8` side toward `3V3_SYS`
- modem `STATUS -> GPIO4`, `DIR` set from `M_1V8` side toward `3V3_SYS`

Treat `M_1V8` as a modem-provided logic reference only, not as a general external load rail.

## 6. Proposed Power Architecture

The modem should have a dedicated high-current rail.

Current target:

- `VSYS -> TPS63020 buck-boost -> MODEM_VBAT_3V9 -> A7670G VBAT`
- target `MODEM_VBAT_3V9` approximately `3.9 V`

Candidate parts currently being considered:

- regulator: `TPS63020DSJR`, `LCSC C15483`
- inductor: `JIERR PCR0420-1R5-M`, `LCSC C53896855`
- feedback values: `Rtop 680k`, `Rbottom 100k`
- output target: `3.9 V` modem rail

Current power-rail requirements to document:

- at least `2 A` peak current capability
- preferably `3 A` design margin
- `470 uF` bulk capacitance near modem `VBAT` pins `55/56/57`
- `10 uF`, `1 uF`, and `100 nF` ceramic capacitors close to modem `VBAT` pins
- `TVS` shunt to `GND` close to modem input power entry
- wide, short `MODEM_VBAT_3V9` routing with local capacitor placement tight to the modem pins

Important design rule:

- the modem power path should be treated as a burst-current subsystem with its own layout and decoupling needs, not as a small accessory load hanging off the ESP32 rail.

### 6.1 Current regulator control plan

Current GPIO mapping around the regulator:

- `GPIO33 -> 4V_EN -> TPS63020 EN`
- `GPIO35 <- LTE_REG_PGOOD <- TPS63020 PG`

Control intent:

- the regulator should default `OFF`
- add a pull-down on `EN` so the rail stays off through reset and boot
- use `PGOOD` as the first firmware check that the modem rail actually came up before pulsing `PWRKEY`

### 6.2 Modem power-control options to evaluate

Open control options:

- regulator enable only
- load switch only
- `PWRKEY` only
- combination of regulator enable plus `PWRKEY`

Current preferred direction for investigation:

- use a hardware power-disconnect method such as regulator enable or load switch for true standby removal
- use `PWRKEY` for the modem's intended boot and graceful shutdown sequence while the rail is present

This combined approach is likely easier to reason about than relying on `PWRKEY` alone while leaving the modem rail energized indefinitely.

### 6.3 Proposed modem control-state model

Treat the following as a proposed firmware control sequence for implementation planning, not as validated modem behavior:

- `Off`
- `Rail Enabled`
- `Rail Stable`
- `Boot Requested`
- `Status High`
- `UART Ready`
- `Registered`
- `Transport Open`
- `Upload Active`
- `Graceful Shutdown`
- `Rail Disabled`
- `Recovery`

Recommended interpretation:

- the combined rail-enable plus `PWRKEY` model should be the default implementation direction for mothership V2
- the state machine should explicitly separate rail presence, modem boot confirmation, network readiness, transport readiness, and active transfer
- recovery should be able to branch either to a graceful retry path or to forced rail removal if the modem does not respond coherently

## 7. Antenna And RF Path Concept

Current RF concept:

- `A7670G RF_ANT pin 60 -> LTE_RF -> u.FL/IPEX centre pin`
- u.FL/IPEX grounds tied directly to `GND`
- `ESD9L5.0ST5G`, `LCSC C82326`, shunt from `LTE_RF` to `GND` close to the connector

Recommended net naming:

- use `LTE_RF` consistently from modem output to connector

Preferred antenna approach:

- first-pass option: internal adhesive LTE `FPC` antenna
- later option: `u.FL/IPEX` to `SMA` bulkhead external antenna
- `50 ohm` antenna system with wideband LTE coverage, approximately `700-2700 MHz`

Important PCB notes:

- keep the RF path short and controlled
- keep the modem RF section physically away from the battery, buck-boost inductor, ESP32 antenna, SD lines, high-current wiring, and any metal mounting plate as much as the enclosure and board outline allow
- verify that enclosure wall material, adhesive antenna placement, bulkhead position, and pigtail routing do not create an awkward or lossy antenna install

### 7.1 SIM holder integration

Current SIM-holder plan:

- direct `A7670G USIM1` wiring to the SIM holder
- `22 ohm` series resistors on `DATA`, `CLK`, and `RST`
- `USIM1_DET` routed to card-detect
- `VPP` left `NC`
- SIM shell tied to `GND`
- `100 nF` on `USIM1_VDD`
- `ESDA6V1W5`, `LCSC C48677`, shunt protection close to the holder on `VDD`, `DATA`, `CLK`, and `RST`

### 7.2 `NETLIGHT` indicator concept

Current indicator plan uses an `MMBT3904` (`LCSC C51953474`) NPN stage:

- modem `NETLIGHT` drives the base through `4.7k`
- `47k` base pull-down to `GND`
- LED from `3V3_SYS` through `2.2k` to collector
- emitter to `GND`

### 7.3 Recommended test pads

Recommended pads for bring-up and recovery:

- `MODEM_VBAT_3V9`
- `M_1V8`
- `GND`
- `4V_EN`
- `LTE_REG_PGOOD`
- `PWRKEY`
- `A7670G RESET`
- `A7670G STATUS`
- `ESP_MODEM_TX / GPIO17`
- `ESP_MODEM_RX / GPIO16`
- `A7670G TXD`
- `A7670G RXD`
- `USB_DP`
- `USB_DM`
- `USB_VBUS`
- `USB_BOOT`
- `DTR`
- `RI`
- `LTE_RF` if layout allows

USB pads are for debug, recovery, and factory-style testing rather than normal runtime operation.

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
- modem power/control manager
- modem session manager
- upload planner
- streaming transfer engine
- transmission status service

Recommended responsibility boundaries:

- ESP-NOW ingest and SD persistence remain separate from LTE and remain the authoritative data path
- the modem power/control manager owns rail enable, `PWRKEY`, `STATUS`, reset, and recovery transitions
- the modem session manager owns attach, registration, PDP or data-session setup, and orderly teardown
- the upload planner decides when upload is eligible based on schedule, power conditions, backlog, and retry policy
- the streaming transfer engine reads persisted data and pushes it over the selected transport without redefining storage ownership
- the transmission status service publishes modem state, backlog state, and last-transfer outcomes to the existing web UI and later BLE/native interfaces

### 8.2 First-pass upload workflow and storage strategy

- Keep `datalog.csv` authoritative.
- Add a durable upload cursor or manifest on local storage rather than relying on RAM-only progress.
- Track at minimum: file identity, byte offset, last successful newline boundary, last success timestamp, last failure timestamp, and retry eligibility.
- Upload should advance only after a server-accepted transfer boundary that can be resumed or retried deterministically.
- File rotation can remain a later step if `datalog.csv` grows large enough that transfer windows or recovery become awkward.

Avoid this first-pass architecture:

- treating LTE upload success as a prerequisite for considering the data stored
- keeping upload progress only in RAM
- coupling modem session success to whether ESP-NOW ingest is allowed to continue

The local SD record should remain authoritative, and the upload cursor should be treated as derived metadata about remote-transfer progress.

### 8.3 First-pass transport recommendation

Recommended first pass:

- prefer `HTTPS` file upload for the first implementation pass because the current workflow is file-centric and CSV-centric
- keep `MQTT` as a later or alternative option for summaries, alerts, or compact state messages

Reasoning:

- the current mothership workflow already centers on `datalog.csv` export and persisted files
- a file-oriented upload path aligns better with durable byte-offset tracking than a message-by-message transport in the first pass
- `MQTT` may still be useful later once the base modem/session layer is proven and a summary or alert channel becomes valuable

### 8.4 Proposed transmission UI and configuration surface

Ground this in the existing web UI first, then mirror the same concepts into BLE/native once the shared command layer is stable.

Recommended first-pass web UI additions:

- Transmission or Backhaul status panel on the home page showing modem state, registration state, backlog size, last upload result, and next eligible upload window
- settings or configuration panel for enabling backhaul, choosing transport, defining endpoint and auth mode, and setting upload policy constraints
- exports or transfer queue panel showing `datalog.csv`, current upload cursor position, pending backlog estimate, last successful transfer, and manual upload action when allowed

Alignment with current docs:

- the current web UI already owns mothership summary, schedules, and CSV download, so it is the right first surface for transmission controls
- the native app plan already treats web as the operational baseline and BLE or app parity as a later mirror of shared service behavior

### 8.5 Proposed settings and status data model

Treat these objects as first-pass planning shapes for shared web UI and later BLE or native parity work.

Proposed transmission settings object:

```json
{
	"enabled": true,
	"transportType": "https_file",
	"endpointUrl": "https://example.invalid/upload",
	"authMode": "token",
	"siteId": "site-001",
	"deploymentId": "deploy-001",
	"uploadPolicy": "scheduled_batch",
	"uploadIntervalMin": 360,
	"uploadPhaseUnix": 1767225600,
	"minBatteryMv": 3700,
	"maxBytesPerSession": 262144,
	"maxRetriesPerWindow": 3,
	"allowManualUpload": true
}
```

Proposed modem status object:

```json
{
	"powerState": "registered",
	"railEnabled": true,
	"statusPinHigh": true,
	"networkRegistered": true,
	"transportOpen": false,
	"signalQuality": 18,
	"operatorName": "example-carrier",
	"lastAttachUnix": 1767229200,
	"lastError": ""
}
```

Proposed upload backlog object:

```json
{
	"filePath": "/sd/datalog.csv",
	"fileIdentity": "datalog.csv:1234567",
	"nextByteOffset": 98304,
	"lastSuccessfulNewlineOffset": 98112,
	"pendingBytes": 24576,
	"lastSuccessUnix": 1767229205,
	"lastFailureUnix": 0,
	"retryEligible": true,
	"lastResult": "ok"
}
```

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

Document the following proposed upload cycle as an implementation direction, not as bench-validated behavior:

1. Assert `GPIO33` to enable `4V_EN` and bring up `TPS63020`.
2. Wait for `GPIO35` / `LTE_REG_PGOOD` and allow `MODEM_VBAT_3V9` to settle.
3. Pulse `GPIO14` so the `2N7002` stage pulls modem `PWRKEY` low for boot request timing.
4. Wait for modem `STATUS` on `GPIO4` to indicate the modem is up.
5. Start `UART2` on `GPIO16 / GPIO17` and confirm basic `AT` responsiveness.
6. Check `SIM`, signal, and network registration.
7. Open `MQTT` or `HTTPS` session, with `HTTPS` still the preferred first-pass file upload path.
8. Upload queued CSV or summary data.
9. Log the upload result locally on SD.
10. Gracefully shut down the modem using an `AT` command or `PWRKEY` shutdown sequence.
11. Wait for `GPIO4` `STATUS` to drop.
12. Deassert `GPIO33` to disable the rail and remove standby drain.

Recommended implementation note:

- firmware should time-bound each stage and classify failures clearly, for example: rail enable failure, modem not booting, SIM absent, registration timeout, transport connect failure, upload reject, or clean shutdown failure.
- the control implementation should explicitly map transitions through the proposed states in Section 6.2 so logs and UI status do not collapse multiple failure modes into a generic modem error
- a combined rail-enable plus `PWRKEY` model should be the baseline assumption for firmware planning unless modem bench testing disproves it

## 11. Open Questions And Risks

### 11.1 Pre-fabrication checks

Check these items before releasing a board revision:

- confirm all required modem `GND` pins are tied into a low-impedance ground system
- confirm the `2N7002` symbol and footprint pin mapping matches the intended gate, drain, and source wiring
- confirm the `MMBT3904` symbol and footprint pin mapping matches the intended base, collector, and emitter wiring
- confirm each `SN74LVC1T45` `DIR` pin is oriented correctly for `TX2 -> RXD`, `TXD -> RX2`, and `STATUS -> GPIO4`
- confirm `M_1V8` is only used as a logic reference and decoupled locally, not treated as an external supply rail
- confirm intended SIM-detect behavior from `USIM1_DET`
- confirm SIM `VPP` remains `NC`
- confirm the SIM shell is tied to `GND`
- confirm RF routing, connector grounding, and `LTE_RF` keep-out around noisy structures
- confirm regulator `EN` has a pull-down so the modem rail defaults `OFF`
- confirm `GPIO4` modem `STATUS` remains boot-safe because the rail is off until explicitly enabled

### 11.2 Power-system risk

- modem current bursts may upset the rest of the system if rail sizing, grounding, and bulk capacitance are inadequate

### 11.3 Sleep-state ambiguity

- if the modem is not truly power-removed between sessions, standby drain may be much worse than expected

### 11.4 Firmware complexity risk

- adding LTE can create a fragile coupled system if ingest, local storage, modem control, and upload retry are not separated cleanly

### 11.5 RF and enclosure risk

- bulkhead placement, antenna quality, cable losses, and enclosure geometry may dominate real-world link quality more than the modem choice itself

### 11.6 Carrier and regional risk

- even with `B28` support, actual deployment suitability still depends on local carrier behavior, antenna selection, and regional certification assumptions

### 11.7 Recovery and service risk

- if modem debug access is omitted, bring-up and field diagnosis may be unnecessarily slow

## 12. Recommended Implementation And Validation Order

### 12.1 Staged implementation order

Recommended sequence:

1. Define the shared transmission command and settings schema first so web UI and later BLE or native parity do not invent different models.
2. Extract or formalise a reusable export or storage service around `datalog.csv` access rather than letting modem code read ad hoc from unrelated paths.
3. Add the durable upload cursor or manifest so transfer progress survives reset, brownout, and modem failure.
4. Add the modem power and session subsystem around the combined rail-enable plus `PWRKEY` model.
5. Add the scheduler decision layer that determines when upload is eligible based on backlog, interval, phase, battery threshold, and retry windows.
6. Implement `HTTPS` file upload as the first transport path.
7. Add web UI controls and status panels for transmission settings, backlog visibility, and manual actions.
8. Mirror the same commands and status into BLE or native once the web-backed service surface is stable.
9. Consider richer transport or storage options later, such as `MQTT` summaries, rotated upload files, or more advanced manifesting.

### 12.2 Validation sequence

Recommended validation order:

1. Storage validation first: confirm the upload cursor or manifest advances correctly, survives reset, and never makes `datalog.csv` non-authoritative.
2. Modem bench validation: confirm rail enable, `PWRKEY`, `STATUS`, boot timing, registration timing, and clean shutdown behavior on hardware.
3. Transport validation: confirm `HTTPS` upload success, server error handling, resume behavior, and authenticated failure handling.
4. Combined wake validation: confirm mothership scheduling, power budget checks, modem session duration, and post-upload shutdown all behave coherently in one cycle.
5. UI parity validation: confirm the web UI can inspect and control transmission settings first, then verify BLE or native surfaces expose the same fields and state names.
6. Failure-path validation: confirm behavior for no SIM, poor signal, registration timeout, transport reject, partial upload, and forced recovery or power removal.

## 13. Bottom Line

The `SIMCom A7670G` is a plausible mothership V2 LTE backhaul candidate for scheduled remote upload, but it should be treated as a distinct, high-current, still-unvalidated subsystem.

The safest architectural direction is:

- keep `ESP-NOW` as the local telemetry path
- keep `SD` as the primary durable record
- use LTE only as a scheduled backhaul path on top of local persistence
- power the modem only when needed
- keep the hardware and firmware boundaries explicit so LTE complexity does not destabilize the rest of the mothership design