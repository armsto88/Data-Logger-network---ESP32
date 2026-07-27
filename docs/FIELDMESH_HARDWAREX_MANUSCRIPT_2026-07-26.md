# An open-source ESP32 microclimate monitoring platform for fine-scale ecological applications

> **Draft status — 2026-07-26.** Working draft, updated as features land. Verified line-by-line against the firmware
> repository (`Data-Logger-network---ESP32` @ `e8acf05`) and, for the cloud path, against the dashboard repository
> (`FieldMeshDashboard` @ `0102661`) and its live Supabase project.
>
> Scope decisions applied: the ultrasonic wind pathway is out; node and hub electronics go in commercial IP-rated
> junction boxes; the firmware is published whole with an optional local-only build configuration.
>
> **Evidence conventions.** `*PLACEHOLDER*` marks content still to be written or measured. `[EVIDENCE GAP]` marks a
> claim that is implemented and correct but not yet backed by data strong enough to publish — each one states what
> would close it, so the claim can be kept in view and upgraded rather than quietly dropped. Nothing marked
> `[EVIDENCE GAP]` should reach submission in that state: it is either evidenced or removed.
>
> The working appendices at the end (Open items, Change log) are for internal review and should be stripped before
> submission.

> **Title options:**
> - *(current — broader, 12 words)* An open-source ESP32 microclimate monitoring platform for fine-scale ecological applications
> - *(PV-specific)* An open-source ESP32 microclimate monitoring platform for solar photovoltaic landscapes
> - *(with project name)* FieldMesh: an open-source microclimate monitoring network for heterogeneous landscapes
>
> The broader framing widens the HardwareX audience. The solar PV application is prominent in §1 and the abstract, and links directly to Armstrong et al. (2026).

## Authors

Tom Armstrong, Bradley Evans, Eric J. Nordberg*

## Affiliations

Ecosystem Management, School of Environmental and Rural Science, University of New England, Armidale, NSW 2351, Australia

## Corresponding author

*Eric J. Nordberg — Eric.Nordberg@une.edu.au
ORCID: 0000-0002-1333-622X

---

## Abstract

Fine-scale microclimatic variation shapes organism physiology, behaviour, and survival, yet conventional meteorological stations and commercial loggers cannot resolve metre-scale heterogeneity across local microhabitats. This gap limits spatially replicated ecological research, particularly in solar photovoltaic (PV) facilities, where panels, inter-row corridors, and edge zones create distinct microclimatic mosaics (Armstrong et al., 2026). Here we present an open-source, low-cost microclimate monitoring platform built on custom ESP32-based sensor nodes. Each node measures air temperature, relative humidity, spectral light, soil moisture, soil temperature, and wind speed, with the thermal and radiometric sensors mounted in script-generated housings that fix their exposure geometry across builds. Nodes communicate via ESP-NOW to a central field hub providing node discovery, over-the-air configuration, time synchronisation, local logging, and a browser-based field interface — without internet or cellular infrastructure, with an optional LTE backhaul where coverage permits. Both nodes and hub power-gate fully between scheduled wakes, persisting all runtime state in non-volatile memory, which eliminates between-wake current draw and enables automatic recovery after power loss. Bench commissioning confirmed integrated multi-node operation across repeated wake–synchronise cycles, with ESP-NOW delivery of ~99.7 % at 30 m line-of-sight and 84–87 % at 100 m under obstructed line-of-sight. The platform provides a reproducible, transparent framework for distributed microclimate monitoring in renewable energy and other heterogeneous landscapes.

## Keywords

microclimate monitoring; open-source hardware; ESP32; solar photovoltaic; wireless sensor network; environmental sensors

---

## Specifications table

| Field | Value |
|---|---|
| Hardware name | FieldMesh Monitoring system |
| Subject area | Environmental, planetary and agricultural sciences |
| Hardware type | Field measurements and sensors |
| Closest commercial analog | HOBO MX2301A (Onset, ~$150–250 USD per unit, temperature and humidity only); Campbell Scientific CR1000X datalogger systems (~$1,000+ USD per unit); Thermochron iButton DS1921/DS1922 (~$30–80 USD per unit, temperature only). No single commercial system integrates air temperature, relative humidity, spectral light, soil moisture, soil temperature, and wind sensing in a low-cost, open-source, wirelessly networked package designed for fine-scale ecological replication. |
| Open source license | PCB designs: CERN-OHL-S-2.0. Firmware: MPL-2.0. Printed sensor housings: CC BY 4.0. *PLACEHOLDER — proposed, pending confirmation and a UNE IP-ownership check (see Open items).* The SHT4x radiation shield is a derivative work whose CC BY 4.0 terms are inherited and settled. |
| Cost of hardware | *PLACEHOLDER — see §4 for the derivation and what the BOM price columns do and do not cover.* |
| Source file repository | *PLACEHOLDER: Upload design files to OSF or Zenodo and insert DOI URL here* |

---

## 1. Hardware in context

Microclimate — the conditions organisms experience at centimetre-to-metre scales — shapes physiology, behaviour, and survival (Pincebourde et al., 2016). Because organisms respond to their immediate thermal, radiative, and moisture surroundings rather than regional averages, fine-scale variation can exert stronger ecological influence than coarse-scale weather data (Kemppinen et al., 2024). Standard meteorological stations are designed to minimise local variability and therefore often fail to capture heterogeneity within soil layers, vegetation canopies, cavities, or near-surface boundary layers (De Frenne et al., 2025; Maclean et al., 2021). These mosaics change over metres or even centimetres, and biologically meaningful extremes — rapid heating, transient wind shifts, short-term thermal fluctuations — can unfold over minutes to hours (Chen et al., 2024; Maclean et al., 2021). Capturing this spatiotemporal heterogeneity is increasingly recognised as essential for predicting ecological responses to climate change, identifying microrefugia, and informing habitat restoration (De Frenne et al., 2025).

Solar photovoltaic (PV) farms are a rapidly expanding land use that generates just such microclimatic mosaics (Liu et al., 2026). Elevated panels, inter-row spacing, and supporting infrastructure create distinct microhabitat zones — under-panel, inter-row, and edge — that differ in radiation, wind exposure, temperature, and moisture (Armstrong et al., 2026). Under-panel areas typically experience reduced shortwave radiation and moderated soil temperatures (Li et al., 2025; Wu et al., 2022), while inter-row zones remain more directly exposed to open-air conditions. These gradients have direct implications for the co-use potential of solar farms, influencing wildlife habitat suitability, vegetation growth, and livestock welfare (Armstrong et al., 2026; Nordberg et al., 2021; Nordberg & Schwarzkopf, 2023). As PV installations expand into multi-use agrivoltaic and conservoltaic landscapes, resolving these fine-scale gradients will be essential for evaluating ecological and agricultural outcomes — yet they occur at metre and sub-metre scales largely undetected by conventional monitoring.

Despite growing recognition that fine-scale microclimatic variation influences wildlife habitat suitability and conservation planning (Crowley et al., 2026; Enochs et al., 2025; Saikumari et al., 2025), a practical monitoring gap persists. High-end atmospheric systems — eddy covariance towers, WMO-style stations (Baldocchi, 2003; World Meteorological Organization, 2018), and global networks such as FLUXNET, OzFlux, and NEON (Baldocchi, 2003; Beringer et al., 2022; Metzger et al., 2019) — provide valuable ecosystem-scale data but are not designed to resolve centimetre- to metre-scale heterogeneity across local microhabitats.

For fine-scale ecological monitoring, existing logger systems present practical constraints. Sensor combinations are often predefined within hardware configurations, limiting flexibility as research questions evolve (Hayibo & Pearce, 2025). Remote deployments may require periodic physical access for configuration, data retrieval, or maintenance, which becomes increasingly impractical as network size grows (Ďud'ák et al., 2023). Long-term autonomous monitoring also requires trade-offs between power consumption, storage efficiency, firmware design, and sampling density (Ďud'ák et al., 2023). Where fine-scale heterogeneity requires dense spatial replication, per-unit hardware cost directly constrains achievable sampling resolution.

Although open-source platforms have demonstrated that modular systems can reduce cost and improve flexibility (Ali et al., 2016; Hayibo & Pearce, 2025), fewer studies provide detailed, reproducible descriptions that integrate enclosure geometry, field durability, firmware architecture, and ecological deployment strategy into a cohesive monitoring framework. For microclimate monitoring, the physical design of a logger is not separate from the measurements it produces: sensor height, exposure, shielding, airflow, and housing geometry can all influence recorded temperature, humidity, radiation, and wind conditions (Maclean et al., 2021). Systems designed for heterogeneous PV landscapes therefore need to support consistent sensor placement, low-cost replication, and flexible deployment across under-panel, inter-row, edge, and open reference microsites (Armstrong et al., 2026).

Here, we present the design and bench commissioning of an open-source, reproducible, and modular microclimate monitoring platform for fine-scale ecological applications. The platform integrates air temperature, relative humidity, spectral light, soil moisture, soil temperature, and wind speed within a custom ESP32-based node architecture, together with a parametric, script-generated set of sensor housings that fix the exposure and shielding geometry of the radiometric and thermal sensors. By documenting both the system design and its current bench-commissioned limits, this work provides a foundation for future validation and spatially replicated monitoring of the environments that organisms experience in renewable energy landscapes.

---

## 2. Hardware description

The platform was developed as a custom hardware and firmware system guided by five design principles: (1) reproducibility and low-cost replication; (2) distributed sensing with centralised aggregation; (3) modular sensor integration; (4) measurement-aware mechanical design; and (5) environmental robustness with field configurability.

### 2.1 System architecture

The system comprises battery-powered sensor nodes and a central field hub, both built on custom PCBs carrying an ESP32-WROOM-32D-N4 module, designed in EasyEDA and fabricated through JLCPCB. The sensor node described here is the NODE_v3 hardware revision, a hardened iteration of the prior NODE_v2 board with refined power and charging circuitry; electrical bring-up confirmed parity with NODE_v2 across every subsystem used by the platform described here. Nodes measure environmental variables and transmit wirelessly via ESP-NOW (channel 11) to the hub, which coordinates node management, time synchronisation, local data logging, and a browser-based field interface. An optional LTE backhaul (SIMCom A7670G Cat-1 modem) enables cloud upload where cellular coverage exists; where it does not, the modem section is left unpopulated and data are retained locally on the hub.

Each node carries a DS3231 real-time clock backed by a CR1220 coin cell, which serves as the sole time authority and wake source. The hub maintains its own DS3231 and is the clock authority for the fleet; it is set from the operator's browser in UTC, and all recorded timestamps throughout the system are UTC.

### 2.2 Power architecture: hard power-cut, not sleep

A defining feature of the design is that neither nodes nor the hub enter deep sleep between scheduled tasks — they power off entirely.

**Node.** On completing a wake cycle, the node releases a power-hold latch (`PWR_HOLD`, GPIO23) and the board loses all power. Wake is initiated by a DS3231 alarm, whose interrupt is inverted and drives a high-side P-channel MOSFET latch that switches the system rail (`VSYS`) on; the ESP32 then boots, re-asserts `PWR_HOLD`, and only then clears the alarm flag. The order matters — clearing the alarm before asserting `PWR_HOLD` would drop the rail mid-boot. An always-on keep-alive LDO powers the RTC and its CR1220 backup while the main system is off.

**Hub.** The hub uses the same principle on a different trigger set. It wakes on any of three sources, resolved in firmware to a single wake reason: a DS3231 Alarm 1 (scheduled sync and upload), a config-button press latched by an SN74LVC2G74 flip-flop (operator servicing), or USB VBUS presence (bench and service mode). A config-button wake takes priority over a concurrent RTC alarm, so an operator can always seize the device at a site visit. The hub holds `PWR_HOLD` (GPIO26) for the duration of its task and clears the config latch (GPIO25) on exit.

This architecture eliminates quiescent draw between tasks and extends battery life. Because RTC memory does not survive the power cut, all runtime state — hub association, deployment status, sampling interval, synchronisation metadata, sensor selection, and the queued snapshot buffer — is persisted in non-volatile storage (NVS) and reloaded on every boot. A hardware timer watchdog (120 s) reboots the node if a hung loop — for example an I²C stall — would otherwise leave it powered on and draining the pack.

The node uses a single-cell 3000 mAh LiPo with dual charging: a CN3163 solar charger and a TP5100 USB-C charger, allowing field recharging via a small solar panel or USB. Reverse-polarity protection is provided by a P-channel FET plus a resettable fuse.

### 2.3 Dual-alarm wake schedule

The DS3231 drives two independent alarms:

- **Alarm 1 (data wake):** fires at `now + wakeInterval`, triggering sensor capture to a local queue with the radio off.
- **Alarm 2 (sync wake):** fires at a fleet-shared, minute-aligned phase anchor, opening the ESP-NOW radio for a coordinated synchronisation session.

When both alarms fall on the same wake, the node samples first, then synchronises, in a single power cycle. The synchronisation interval is auto-derived from the wake interval (×18) and is not user-settable; §2.5 explains why the multiplier is fixed.

### 2.4 Sensor payload

Each node carries a fixed environmental payload plus expansion capacity:

| Measurement | Sensor | Interface |
|---|---|---|
| Air temperature and relative humidity | Sensirion SHT4x (SHT40/41) | I²C 0x44 via mux ch0 |
| Spectral light (8 visible bands + Clear + NIR) | ams AS7341 | I²C 0x39 via mux ch1 |
| Soil moisture and temperature (×2 probes) | CWT TH-A via ADS1015 (12-bit ADC) | I²C 0x48, analog A0–A3 |
| Wind speed | WH-SP-WS01 reed-cup anemometer | GPIO4 pulse count |
| Battery voltage | On-board divider | GPIO35 ADC |

The AS7341 reports eight visible channels with nominal centres at 415, 445, 480, 515, 555, 590, 630 and 680 nm, plus a Clear channel and a near-infrared channel. Each snapshot also carries the acquisition metadata needed to interpret those counts — gain, integration time, and a saturation flag — so a reading can be rejected or rescaled downstream rather than being silently misinterpreted.

Digital sensors are isolated through a TCA9546A four-channel I²C multiplexer (address 0x71), with channels 0–1 assigned to the standard payload and channels 2–3 reserved for expansion. The root I²C bus (SDA = GPIO18, SCL = GPIO19) carries the RTC, the multiplexer, and the soil ADC directly; a PCA9306 level translator bridges to the expansion side. Two auxiliary I²C slots (AUX1/AUX2) and two analog channels support site-specific additions. A per-node sensor mask selects which sensors are active at deployment: self-identifying I²C parts (SHT4x, AS7341) are auto-detected, while passive sensors (wind, soil, AUX) are registered only when their mask bit is set.

The reed-cup anemometer uses an adaptive two-stage acquisition to keep the wake short in calm conditions: a 2 s presence probe and, only if at least two edges are seen, a 10 s averaging window. Reed contact bounce is rejected by a 5 ms debounce in the interrupt handler, and the two-edge minimum also discards a single stray electrical glitch. Frequency is converted to wind speed with the manufacturer's linear constant (1 Hz = 2.4 km h⁻¹, i.e. 0.6667 m s⁻¹ per Hz). This sensor reports wind speed only; wind direction is reserved in the data schema but is not populated.

Soil moisture is logged as raw sensor volts. Conversion to volumetric water content is deferred rather than performed on-node, so probe-specific calibration curves can be applied during analysis without reflashing; the platform does not yet ship a calibration curve, and the published data product is uncalibrated probe voltage (§7.5). Soil temperature is converted on-node using the CWT TH-A transfer function.

### 2.5 Coordinated synchronisation and data queue

Nodes do not transmit on every wake. Readings are captured to a checksummed non-volatile queue and flushed during the scheduled sync window. The queue is a ~3.5 kB circular byte slab held in NVS, storing variable-length records (one per wake) under an FNV-1a checksum over the whole structure, with a layout identifier so a firmware upgrade cannot misread an older blob. A record is admitted only if the whole record fits; if the slab is full the oldest records are dropped and the new record is tagged with a `DROPPED` quality flag, so the loss is visible in the data rather than silent.

Queue overflow is prevented structurally rather than by runtime clamping. The sync interval is auto-derived as `syncMin = wakeMin × 18`, which places worst-case queue fill at approximately 82 % of the safe ceiling of 22 wakes for a fully populated payload, leaving roughly four wakes of headroom. Because the multiplier is fixed in firmware and the sync interval is never exposed to the operator, a correctly configured node cannot overflow its queue during normal operation — it would have to miss six consecutive sync windows before overflow became possible.

The hub opens a bounded, coordinated pull session: it rosters nodes via jittered hellos, grants each a fairness-limited transmission window, and confirms receipt before the node pops records from its queue. Records are only removed after delivery confirmation, preventing silent data loss, and a wall-clock deadline prevents a single node's flush from consuming the entire listen window. Session and grant identifiers make late packets from an earlier wake harmless. If a node misses a sync window, its queue is left intact and its alarms re-armed.

Each wake produces a single structured snapshot (`NODE_SNAPSHOT2`) rather than one packet per sensor: a 48-byte header plus up to 33 six-byte key–value readings, keeping the largest possible packet at 246 bytes, inside the 250-byte ESP-NOW limit. Every wire structure in the shared protocol header is size-locked with a compile-time assertion, so a field added on one side cannot silently desynchronise node and hub. If a node's last time sync is older than 24 hours, it runs a bounded ESP-NOW recovery during a data wake; the hub independently assists nodes it infers are stale.

### 2.6 Field hub, storage, and connectivity

The field hub (FieldHub V2) is a custom PCB hosting an ESP32-WROOM-32D-N4, a DS3231MZ RTC (I²C on SDA = GPIO21, SCL = GPIO22), USB-C charging and serial (CH340C), a config-button wake latch, and two local storage paths: an internal LittleFS partition and a microSD card socket on SPI (CS = 13, SCK = 18, MISO = 19, MOSI = 23). Where cellular coverage permits, a SIMCom A7670G Cat-1 LTE modem provides cloud upload over HTTPS; it is powered through a TPS63020 buck-boost with a soft-started rail and communicates over Serial2 (TX = GPIO17, RX = GPIO16) at 115 200 baud through level shifters, using the modem's own TLS stack with chunked 1 kB transfers.

Storage is tiered by intent. The LittleFS partition (768 kB) is the working store: it buffers received snapshots and holds the upload queue between hub wakes. The microSD socket provides bulk archival capacity for deployments without a backhaul, where the hub must retain a full season of data locally. At the time of writing the snapshot logging path writes to LittleFS only, and routing it to the card is scheduled for the next hub hardware revision (§7.5).

The hub serves a captive-portal Wi-Fi access point (SSID `FieldHub(<MAC>)`, WPA2-protected, on the same channel 11 as the ESP-NOW link, maximum four clients, reachable at 192.168.4.1) through which a phone, tablet, or laptop can discover nodes, assign identities, set sampling intervals, select sensors, deploy or stop nodes, and export data — all without reflashing or serial reconfiguration.

Over-the-air configuration is declarative and version-gated: the hub holds each node's desired state and re-broadcasts it every sync window until the node's echoed config version matches. Changeable without reflashing: wake interval, sensor mask, deploy/stop/unpair, sync phase, and RTC time.

Firmware images are updated through a separate signed pathway rather than through the configuration channel. The mechanism — an Ed25519-signed manifest, a SHA-256 gate over the image, A/B application slots, and deferred-verify rollback on the stock ESP32 bootloader — is common to both device roles and has been bench-proven on both node and hub silicon. The delivery paths differ in maturity: the hub can fetch, verify, and install a release over LTE end-to-end (bench-proven on a 1.3 MB image), whereas nodes report their firmware and slot state to the hub but have no image-transfer path, because a power-gated node is only reachable for a short window each sync cycle. Node firmware update therefore remains a documented design, not a delivered capability (§7.5).

### 2.7 Deployment without connectivity

The platform is designed to be useful with no internet or cellular infrastructure at all, and this is the configuration we expect most ecological deployments to use. The ESP-NOW link, coordinated sync, scheduling, node configuration, local logging, and the browser-based field interface are all local to the hub and require no backhaul. Where LTE is not needed, the modem, its buck-boost converter, and its level shifters are simply left unpopulated (§5.5), which removes their cost and their current draw. *PLACEHOLDER: a corresponding firmware build configuration that compiles out the modem and upload paths is planned but not yet implemented; until then an unpopulated hub simply fails its modem probe and continues to log locally.*

### 2.8 Field hub duty cycle and remote management

Because the hub power-gates like the nodes, its behaviour is scheduled rather than continuous. On an RTC-alarm wake it opens the sync window (nominally 120 s, long enough to roster and drain multiple nodes against each node's ~60 s listen window), writes received snapshots to local storage, optionally uploads over LTE, re-arms its alarm, and powers down. On a config-button wake it instead brings up the Wi-Fi access point and stays available for operator interaction until the operator finishes. The browser interface is therefore not continuously available at a deployed site: an operator presses the config button to summon it. This is a deliberate design property, not a limitation of the interface.

### 2.9 Mechanical design and sensor housings

For a microclimate instrument, the housing is part of the measurement. The platform therefore specifies the exposure geometry of the radiometric and thermal sensors explicitly, and generates those housings from parametric scripts rather than hand-edited solid models, so a build can be regenerated and audited rather than copied.

**Electronics enclosure.** Node and hub electronics are housed in standard commercial waterproof polycarbonate junction boxes rather than a custom printed enclosure. This is a deliberate choice: an off-the-shelf IP-rated box with a certified ingress rating, UV-stable material, and available cable glands is cheaper, more durable, and more reproducible than a printed equivalent, and it removes the highest-risk part of a replication attempt from the 3D printer. Sensor cables enter through glands, and external sensors terminate in aviation-plug pigtails so a probe can be replaced in the field without opening the box. *PLACEHOLDER: specify box manufacturer, model, internal dimensions, and IP rating for node and hub, and add both to the BOM.*

**Air temperature and humidity — radiation shield.** The SHT4x probe is mounted in a multi-plate Stevenson-type radiation shield, with a printed clamp flange carrying a PG7 gland for the probe body and an optional right-angle bracket adapter for horizontal mounting. The shield body is a modification of a published mini Stevenson shield mesh (Thingiverse, CC BY 4.0), edited by script rather than by hand so the modification is repeatable and auditable; the unmodified upstream meshes are redistributed alongside the generator, as the licence permits, so the part can be regenerated from source without an external download.

**Spectral light — diffuser housing.** The AS7341 is mounted behind a 39.5 mm diffuser held in a printed housing with a screw-retained retainer ring (M3 clearance screws into heat-set inserts on a 55 mm pitch circle) and a PG7 cable gland for the I²C lead. Fixing the diffuser and its aperture geometry is what makes the spectral channel comparable between builds; the housing does not, by itself, make the channel radiometrically calibrated (§7.5).

**Mounting.** Both housings mount to the enclosure lid on an M3 pattern, which fixes the exposure height and relative placement of the two sensors across nodes — the practical requirement for spatially replicated comparison. *PLACEHOLDER: confirm and document the lid drilling pattern against the chosen box.*

**Status.** The housings exist as generator scripts and exported STLs. Physical fabrication, fit against the chosen enclosure, gland sealing, and outdoor durability have not been assessed (§7.5).

### 2.10 How the platform helps researchers

- **Distributed microclimate monitoring at low cost** — spatially replicated measurements across fine-scale environmental gradients (e.g. under-panel, inter-row, edge, and open reference zones in solar farms) at a fraction of the cost of commercial logging systems.
- **Modular and expandable sensor integration** — air temperature, relative humidity, spectral light, soil moisture, soil temperature, and wind, with two auxiliary I²C and two analog expansion channels for site-specific additions.
- **Centralised network management without internet infrastructure** — wireless node discovery, scheduling, time synchronisation, and data logging through a browser-based interface, with optional LTE cloud upload where coverage permits.
- **Install-and-forget power architecture** — hard power-cut between readings, dual-alarm scheduling, and NVS-backed recovery let nodes run unattended on a single LiPo charge with solar top-up, resuming automatically after power loss.
- **Controlled, reproducible sensor exposure** — a script-generated radiation shield and diffuser housing fix the exposure geometry that would otherwise vary between hand-built units, on a standard IP-rated enclosure any group can source.
- **Reproducible and open-source build** — PCB designs, housing scripts and STLs, firmware, and bills of materials are openly available for replication, modification, and extension.
- **Validation of biophysical models** — the sensor payload aligns with key input variables for mechanistic microclimate and biophysical modelling frameworks such as NicheMapR (Kearney & Porter, 2017), supporting model validation and ecological prediction.

---

## 3. Design files summary

| Design file name | File type | Open source licence | Location |
|---|---|---|---|
| Node PCB schematic | EasyEDA source / PDF | CERN-OHL-S-2.0 | *PLACEHOLDER: Repository URL* |
| Node PCB layout | EasyEDA source / Gerber | CERN-OHL-S-2.0 | *PLACEHOLDER* |
| Node PCB BOM + pick-and-place | CSV | CERN-OHL-S-2.0 | *PLACEHOLDER* |
| Gerber and drill files | Gerber / Excellon | CERN-OHL-S-2.0 | *PLACEHOLDER* |
| Field hub PCB design + BOM + pick-and-place | EasyEDA / Gerber / CSV | CERN-OHL-S-2.0 | *PLACEHOLDER* |
| SHT4x radiation shield (+ upstream source meshes) | Python (Blender) generator + STL | CC BY 4.0 (derivative — see attribution chain) | *PLACEHOLDER* |
| AS7341 diffuser housing + retainer ring | Python (FreeCAD) generator + STL + STEP | CC BY 4.0 | *PLACEHOLDER* |
| Node firmware | C++ (PlatformIO) | MPL-2.0 | *PLACEHOLDER* |
| Field hub firmware | C++ (PlatformIO) | MPL-2.0 | *PLACEHOLDER* |
| Shared protocol header | C++ header | MPL-2.0 | *PLACEHOLDER* |
| Bring-up test sketches | C++ (PlatformIO) | MPL-2.0 | *PLACEHOLDER* |

- **Node PCB schematic** — Full circuit schematic for the NODE_v3 sensor node: ESP32 module, power regulation and charging, RTC and power-latch gate, I²C sensor interfaces, analog input pathways, and an unpopulated ultrasonic transmit/receive section (§7.5).
- **Node PCB layout** — Board layout for fabrication: component placement, routing, and copper layers.
- **Node PCB BOM + pick-and-place** — Component-level bill of materials with quantities, designators, footprints, and LCSC/JLCPCB part numbers, plus the placement file for SMT assembly.
- **Gerber and drill files** — Manufacturing outputs for PCB fabrication through JLCPCB or an equivalent prototype service.
- **Field hub PCB design + BOM + pick-and-place** — Equivalent design and manufacturing data for the FieldHub V2 board, including the optional LTE backhaul section.
- **SHT4x radiation shield** — Script that modifies a published mini Stevenson shield mesh to accept the SHT4x probe through a PG7 gland, plus the printed clamp flange and right-angle bracket adapter. The unmodified upstream meshes are redistributed with the generator so the part can be rebuilt from source.
- **AS7341 diffuser housing + retainer ring** — Parametric FreeCAD script generating the spectral sensor housing, its 39.5 mm diffuser pocket and glue ledge, screw-retained retainer ring, and PG7 cable port.
- **Node firmware** — PlatformIO firmware for the ESP32 sensor node: sensor acquisition and registry, NVS snapshot queue, ESP-NOW communication, RTC dual-alarm scheduling, power-latch management, and deployment state control.
- **Field hub firmware** — PlatformIO firmware for the FieldHub: wake-reason resolution, node discovery and registry, coordinated sync sessions, declarative node configuration, local CSV logging, LTE upload, signed self-update, and the browser-based field interface.
- **Shared protocol header** — Single source of truth for every ESP-NOW message structure, sensor identifier, and capability bit, shared verbatim by both firmware builds with compile-time size assertions.
- **Bring-up test sketches** — Self-contained PlatformIO sketches for staged commissioning of each subsystem: RTC and alarms, I²C and multiplexer, ADC and soil, power-hold gate, battery ADC, ESP-NOW range, reed anemometer, spectral metadata, queue robustness, config store, OTA slots and rollback, manifest verification, and modem power, UART, registration and HTTPS.

---

## 4. Bill of materials

The complete component-level BOMs for both boards (NODE_v3, 103 line items; FieldHub V2, 65 line items) are provided as spreadsheets in the source file repository (§3), each carrying quantities, designators, footprints, manufacturer part numbers, and LCSC/JLCPCB supplier part numbers. The summary below lists the functionally significant parts.

*PLACEHOLDER — cost. The exported BOMs include JLCPCB/LCSC unit-price columns, but a naive extension of those columns yields implausibly low subtotals (approximately USD 5.9 for the node board and USD 5.5 for the hub board), because those are volume-tier catalogue prices excluding PCB fabrication, assembly setup and stencil charges, extended-part fees, shipping and duty, and everything not mounted on the board. The per-node figure quoted in the paper should be built from actual order invoices and must additionally include: the external SHT4x and AS7341 modules, two CWT TH-A soil probes, the reed anemometer, the LiPo cell, the solar panel, the junction box and glands, aviation-plug pigtails and cabling, fasteners and heat-set inserts, and printed housing filament.*

| Designator | Component | Number | Cost per unit (AUD) | Total cost (AUD) | Source of materials | Material type |
|---|---|---|---|---|---|---|
| U45 | ESP32-WROOM-32D-N4 | 1 | *PLACEHOLDER* | *PLACEHOLDER* | JLCPCB / LCSC | Semiconductor |
| U5 | DS3231MZ+ RTC | 1 | *PLACEHOLDER* | *PLACEHOLDER* | JLCPCB / LCSC | Semiconductor |
| U33 | TCA9546APWR I²C multiplexer (4-channel) | 1 | *PLACEHOLDER* | *PLACEHOLDER* | JLCPCB / LCSC | Semiconductor |
| U30 | ADS1015IDGSR ADC (12-bit) | 1 | *PLACEHOLDER* | *PLACEHOLDER* | JLCPCB / LCSC | Semiconductor |
| U29 | PCA9306DC1 I²C level translator | 1 | *PLACEHOLDER* | *PLACEHOLDER* | JLCPCB / LCSC | Semiconductor |
| — | SHT4x temperature/RH sensor module (external) | 1 | *PLACEHOLDER* | *PLACEHOLDER* | *PLACEHOLDER* | Semiconductor |
| — | AS7341 spectral sensor module (external) | 1 | *PLACEHOLDER* | *PLACEHOLDER* | *PLACEHOLDER* | Semiconductor |
| — | CWT TH-A soil probe | 2 | *PLACEHOLDER* | *PLACEHOLDER* | *PLACEHOLDER* | Composite |
| — | WH-SP-WS01 reed-cup anemometer | 1 | *PLACEHOLDER* | *PLACEHOLDER* | *PLACEHOLDER* | Composite |
| U21 | MT3608 boost converter (5 V rail) | 1 | *PLACEHOLDER* | *PLACEHOLDER* | JLCPCB / LCSC | Semiconductor |
| U20 / U8 | AP2112K-3.3 main LDO / TPL720F33 keep-alive LDO | 1 each | *PLACEHOLDER* | *PLACEHOLDER* | JLCPCB / LCSC | Semiconductor |
| U23 / U24 | CN3163 solar charger / TP5100 USB charger | 1 each | *PLACEHOLDER* | *PLACEHOLDER* | JLCPCB / LCSC | Semiconductor |
| B1 | CR1220 RTC backup cell | 1 | *PLACEHOLDER* | *PLACEHOLDER* | JLCPCB / LCSC | Non-specific |
| — | LiPo battery, single cell, 3000 mAh | 1 | *PLACEHOLDER* | *PLACEHOLDER* | *PLACEHOLDER* | Composite |
| — | Solar panel (node top-up charging) | 1 | *PLACEHOLDER* | *PLACEHOLDER* | *PLACEHOLDER* | Composite |
| CN11–CN14 | JST B2B-PH-SM4-TB, 4-pin top-entry | 4 | *PLACEHOLDER* | *PLACEHOLDER* | JLCPCB / LCSC | Non-specific |
| CN15–CN20 | JST BM04B-SRSS-TB, 4-pin side-entry | 6 | *PLACEHOLDER* | *PLACEHOLDER* | JLCPCB / LCSC | Non-specific |
| — | Custom node PCB (JLCPCB) | 1 | *PLACEHOLDER* | *PLACEHOLDER* | JLCPCB | Composite |
| — | Passive components (resistors, capacitors, diodes, FETs) | ~300 | *PLACEHOLDER* | *PLACEHOLDER* | LCSC | Non-specific |
| — | Waterproof polycarbonate junction box (node enclosure) | 1 | *PLACEHOLDER* | *PLACEHOLDER* | *PLACEHOLDER* | Polymer |
| — | Cable glands, aviation-plug pigtails, fasteners, heat-set inserts | 1 set | *PLACEHOLDER* | *PLACEHOLDER* | *PLACEHOLDER* | Non-specific |
| — | 3D-printed housings (radiation shield, diffuser housing) | 1 set | *PLACEHOLDER* | *PLACEHOLDER* | Consumer FDM printer | Polymer |
| — | FieldHub V2 PCB + enclosure (one per network) | 1 | *PLACEHOLDER* | *PLACEHOLDER* | JLCPCB | Composite |
| | | | **Total per node:** | *PLACEHOLDER* | | |

Passive component costs are typically very low (~$0.01–0.05 per unit) when ordered in bulk through JLCPCB/LCSC assembly services.

---

## 5. Build instructions

### 5.1 PCB fabrication

1. Open the node PCB design files in EasyEDA.
2. Export Gerber and drill files using EasyEDA's built-in export function.
3. Upload the Gerbers to JLCPCB (or equivalent) and order using standard process settings (1.6 mm FR4, 1 oz copper, HASL finish). Copper weight, solder-mask colour, and panelisation are fabrication choices rather than protocol-critical requirements, provided electrical layout, component placement, and connector geometry are preserved.
4. Order components from the bill of materials. Most are available through LCSC Electronics, which integrates with JLCPCB for SMT assembly.

*PLACEHOLDER: Add photograph of bare PCB and assembled PCB side by side.*

### 5.2 Electronics assembly

1. If using JLCPCB SMT assembly, upload the BOM and pick-and-place files alongside the Gerbers. Most surface-mount components can be machine-placed, reducing manual soldering.
2. Solder manually any components not included in assembly — through-hole connectors, JST headers, screw terminals, and sensor breakout boards.
3. Before first power-up, inspect the assembled board for solder bridges, incomplete joints, connector orientation, and polarity-sensitive placement errors. Pay particular attention to:
   - Diode orientation (D6–D21)
   - Voltage regulator placement (U8, U20)
   - Sensor connector orientation (CN11–CN20)

*PLACEHOLDER: Add annotated photograph of assembled PCB highlighting key components and inspection points.*

### 5.3 Pre-commissioning checks

1. Connect a current-limited bench supply and verify:
   - 3.3 V and 5 V rails at the labelled test points. The board exposes 27 test points, including 3V3, 5V, VSYS, SDA/SCL, and VREF, which makes staged bring-up possible without probing component legs.
   - ESP32 boot behaviour (serial console over USB-C)
   - Reset stability
2. If rework is required (for example the diode orientation correction identified during node bring-up), return to step 1 after correction.

*PLACEHOLDER: Add photograph of bench supply connected to the node PCB, and a serial console screenshot showing a successful boot.*

### 5.4 Sensor connections

1. Connect the SHT4x temperature and humidity sensor to multiplexer channel 0 via its I²C connector (0x44).
2. Connect the AS7341 spectral sensor to multiplexer channel 1 (0x39).
3. Connect the CWT TH-A soil probes to the ADS1015 analog inputs (the ADS1015 sits directly on the root I²C bus at 0x48):
   - A0: Soil probe 1 temperature
   - A1: Soil probe 1 moisture
   - A2: Soil probe 2 moisture
   - A3: Soil probe 2 temperature
4. Connect the WH-SP-WS01 reed-cup anemometer to the AUX WIND connector (GPIO4, pulse count; 1 kΩ series protection recommended).
5. Leave the ultrasonic transducer pathway unpopulated (§7.5).

Aviation-plug pin conventions for the external sensor cables: I²C plugs (4-pin) 1 = GND, 2 = SDA, 3 = SCL, 4 = PWR; soil plugs (4-pin) 1 = GND, 2 = Temp, 3 = Moisture, 4 = PWR; wind plug (2-pin) 1 = GND, 4 = SIGNAL. Two of the board's sensor outputs are voltage-selectable through a high-side P-FET switch and must only be selected to a voltage the attached sensor is rated for.

*PLACEHOLDER: Add annotated photograph or wiring diagram showing sensor connections.*

### 5.5 Field hub assembly

The FieldHub V2 hosts an ESP32-WROOM-32D-N4, DS3231MZ RTC, LittleFS flash storage, a microSD socket, USB-C charging and serial (CH340C), a config-button wake latch (SN74LVC2G74), and — where the LTE backhaul is populated — a SIMCom A7670G Cat-1 modem with micro-SIM socket and U.FL antenna connector.

1. Order the FieldHub V2 PCB and components from the bill of materials (§4). Assemble surface-mount components via JLCPCB SMT assembly or by hand; solder the through-hole connectors (USB-C, microSD socket, U.FL antenna, SIM socket) manually.
2. **If LTE is required**, populate the A7670G modem, the TPS63020 buck-boost, and the SN74LVC1T45 level shifters. **If LTE is not required**, leave these unpopulated — the hub then operates as a fully functional local-logging device (§2.7).
3. Flash the hub firmware via PlatformIO over USB-C.
4. Power the hub (USB-C, or solar via the on-board CN3163 charger) and confirm:
   - The config button wakes the hub and raises the Wi-Fi access point (SSID `FieldHub(<MAC>)`)
   - The browser interface is reachable at `192.168.4.1`
   - Local storage is detected and writable
   - The RTC reads a valid time, and an alarm can be set and cleared

*PLACEHOLDER: Add photograph of the assembled FieldHub V2 PCB and a screenshot of the browser interface.*

### 5.6 Enclosure and sensor housings

*PLACEHOLDER: The printed housings below have been generated from scripts but not physically fabricated or fit-checked against the chosen enclosure before submission. These instructions derive from the design files and are intended for future builds.*

**Electronics enclosure.**
1. Obtain a waterproof polycarbonate junction box for the node and one for the hub. *PLACEHOLDER: specify manufacturer, model, internal dimensions, and IP rating.*
2. Mount the PCB and battery inside on standoffs, leaving clearance for the battery connector and the USB-C port.
3. Fit cable glands for each external sensor lead and for the solar input. Size glands to the pigtail diameter and tighten onto the cable jacket, not the conductors.
4. Drill the lid for the radiation shield and diffuser housing mounting screws. **Verify the pattern against the physical box before drilling** — it has not been checked against a controlled drawing.

**Printed sensor housings.**
1. Regenerate the STLs from their scripts before printing, rather than reusing previously exported meshes — this keeps the printed part traceable to the published parameters. The AS7341 housing and retainer ring regenerate with FreeCAD in console mode; the SHT4x shield body regenerates in Blender from the upstream source meshes supplied alongside the script.
2. Print in PETG or ASA (recommended for UV resistance and outdoor durability). PLA is suitable for bench testing but may degrade under prolonged outdoor exposure. Install short M3 heat-set inserts at the marked locations (bores are sized 4.4 mm for Voron-style inserts).
3. Assemble the spectral housing: bond the 39.5 mm diffuser into its pocket on the glue ledge, fit the retainer ring with M3 screws on the 55 mm pitch circle, and fit the PG7 gland for the I²C lead.
4. Assemble the radiation shield: tighten the probe gland nuts onto the loose clamp flange first, then fit the flange to the shield collar. For horizontal mounting, insert the right-angle bracket adapter between the shield collar and the probe flange and use longer screws.
5. Mount both assemblies to the enclosure lid, keeping the radiation shield clear of the diffuser housing's field of view and both clear of the box's own shadow.

*PLACEHOLDER: Add photographs showing the assembly sequence. Gland sealing performance, ingress rating in service, and outdoor handling have not yet been assessed.*

### 5.7 Safety considerations

- Use appropriate ESD precautions when handling the ESP32 module and sensitive ICs.
- The board includes a boost converter and voltage-selectable sensor outputs that can carry more than the 5 V rail. Confirm the selected output voltage against the attached sensor's rating, and exercise caution when probing these nets.
- LiPo batteries require appropriate handling, charging, and storage. Use the on-board charge controllers and do not exceed rated charge/discharge currents.
- When deploying outdoors, ensure all cable penetrations are sealed and the enclosure gasket is correctly seated to prevent water ingress.

---

## 6. Operation instructions

### 6.1 Powering on

1. Insert a charged LiPo battery into the node and toggle the power switch. The node boots, asserts `PWR_HOLD`, and enters the **unpaired** state — it has no hub association and broadcasts a discovery announcement derived from its MAC address. If not paired within 15 minutes, it powers off to conserve battery.
2. Power the field hub via USB-C or solar, then press the config button. The hub boots, resolves the wake as a configuration request, starts its DS3231, and opens a captive-portal Wi-Fi access point (SSID `FieldHub(<MAC>)`, WPA2-protected, channel 11, reachable at `192.168.4.1`).

### 6.2 Node discovery and pairing

1. Connect a phone, tablet, or laptop to the hub's access point and enter the network key. A captive-portal page should open automatically; if not, navigate to `192.168.4.1`.
2. The browser interface lists discoverable nodes. Select one and assign a numeric ID and friendly name (e.g. "North Hedge 01").
3. Pair the node. The hub sends a `PAIR_NODE` command and the node stores the hub's MAC address in NVS, transitioning to the **paired** state — bound to the hub but not yet sampling.

### 6.3 Configuring deployment

1. Set the wake interval. The interface offers 1, 5, 10, 20, 30, or 60 minutes, and both hub and node validate against exactly this set. The sync interval is auto-derived (wake interval × 18) and is not user-settable.
2. Select active sensors via the sensor mask. Self-identifying I²C parts (SHT4x, AS7341) are auto-detected; passive sensors (wind, soil, AUX) are registered only when their mask bit is set.
3. Set the hub clock. The hub's DS3231 is set from the operator's browser in UTC and is the sole clock authority for the network; nodes take their time from the hub and never from a backend.
4. Deploy the node. The hub sends a `DEPLOY_NODE` command carrying the current RTC time, wake interval, sync interval, sync phase anchor, config version, and sensor mask. The node sets its DS3231 to the received time, stores all parameters in NVS, and transitions to the **deployed** state.

### 6.4 Data acquisition cycle

1. **Data wake (Alarm 1):** the RTC fires at `now + wakeInterval`. The node powers on, reads battery voltage first, then samples enabled sensors in registry order (SHT4x → AS7341 bands → soil → wind → AUX), appends the AS7341 acquisition metadata, assembles a single `NODE_SNAPSHOT2` key–value packet, and appends it to the checksummed NVS queue. The radio stays off. The node re-arms Alarm 1 and powers off.
2. **Sync wake (Alarm 2):** the RTC fires at a fleet-shared, minute-aligned phase anchor. The node powers on, brings up ESP-NOW, sends a `NODE_HELLO` followed by a firmware-capability report, and listens for approximately 60 s. The hub — awake on its own RTC alarm for a nominal 120 s window — opens a coordinated pull session: it rosters nodes via jittered hellos, grants each a fairness-limited transmission window, and confirms receipt. The node flushes queued snapshots, popping records only after delivery confirmation. The hub sends a `SYNC_RELEASE` with the final clock and next phase anchor; the node acknowledges, re-arms Alarm 2, and powers off.
3. **Combined wake:** when both alarms fall together, the node samples first, then synchronises, in a single power cycle.

Between wakes the node is fully powered off — no deep-sleep mode and no quiescent draw. All state survives in NVS.

### 6.5 Data retrieval and export

1. Received snapshots are written to the hub's local storage as a fixed 30-column CSV. Each row carries the node's RTC capture time (ISO 8601, UTC), node identity, sequence number, the sensor-present mask, quality flags, applied config version, battery voltage, air temperature and humidity, the eight spectral bands, wind speed and direction, both soil moisture and temperature pairs, the two auxiliary channels, and the AS7341 metadata (Clear, NIR, gain, integration time, saturation flag). A legacy 25-column header is retained so rows queued before a firmware upgrade drain correctly rather than being discarded or column-shifted.
2. To retrieve data, wake the hub with the config button, connect to its access point, and use the browser interface to inspect and export stored records.
3. Where the LTE backhaul is populated and coverage exists, the hub uploads readings to a cloud endpoint over HTTPS on each scheduled wake, and collects any queued configuration changes in the same exchange (§6.6). The hub posts a flat JSON array of readings plus a `{meta, status}` envelope, authenticated by a per-hub bearer connection key and cross-checked against the FieldHub's registered factory MAC; readings are keyed by node identifier, and 4xx client responses are treated as non-retryable.
4. CSV files import directly into R, Python, or spreadsheet software. Because the wire format is key–value keyed on stable sensor identifiers, adding a sensor extends the payload without renumbering existing channels.

### 6.6 Optional cloud backhaul and remote management

Where the LTE backhaul is populated, the hub uploads to a **configurable HTTPS endpoint** on each scheduled wake and collects any queued configuration changes in the same exchange (§6.5). The endpoint address is set at provisioning, by scanning a code at the hub's own access point so credentials pass from the operator's device to the hub without transiting the internet. The wire contract — payload shape, authentication, and the command-collection response — is documented alongside the firmware, so a research group can direct its hubs at a server of its own. A reference cloud implementation is operated by the authors; no part of the platform depends on it, and none of the capabilities described elsewhere in this paper require it.

Against such an endpoint, a deployment can be managed without a site visit. The governing constraint is that a power-gated node is not addressable on demand: once provisioned, nothing contacts a device directly. The endpoint instead holds durable intent, the hub collects it on its next scheduled check-in, and each node's configuration converges over successive sync windows. A change is therefore reported as applied only once the hub confirms the node has echoed the matching config version, so the record reflects what the fleet has accepted rather than what was requested; a change still queued, and never yet offered to a hub, can be withdrawn.

Nodes can be paused and resumed remotely, and the recording interval changed across a deployment as a single coordinated transition that preserves the sync anchor, so no readings are lost across the change. Undeploy is deliberately excluded from this path and refused at every layer: releasing a node from its hub requires physical presence, so the local field interface remains the definitive safety switch. Hub firmware update over the backhaul is supported (§2.6); node firmware update is not yet available (§7.5).

### 6.7 Stopping, pausing, and unpairing

1. **Pause (standby):** the node remains deployed and continues sync check-ins but skips sampling and the data-wake alarm — useful for temporarily halting collection without losing configuration.
2. **Stop:** clears the deployed flag. The node returns to the paired state, retaining its hub association but no longer sampling.
3. **Unpair:** wipes the hub MAC and all credentials from NVS. The node reverts to unpaired and re-enters discovery on its next boot.

All three are performed over ESP-NOW via the declarative `NODE_CONFIG` message — no reflashing or serial reconfiguration. Because a power-gated node is only reachable during its sync window, the hub re-broadcasts the desired configuration every window until the node echoes a matching config version, applying each version at most once. This is what makes an unpair or a pause reliable against a node that was asleep when the operator pressed the button.

### 6.8 Power-cycle recovery

Because both node and hub use a hard power-cut architecture, every wake is effectively a cold boot. All runtime state — hub association, deployment status, sampling interval, sync metadata, sensor mask, applied config version, and the queued snapshot buffer — is persisted in NVS and reloaded on boot. A node that loses power or resets unexpectedly recovers its deployment state and resumes scheduled acquisition without intervention. The hardware watchdog reboots the node if a hung loop would otherwise leave it powered on.

NVS is never automatically erased to recover from a mount failure. A firmware image still pending verification that cannot mount NVS rolls itself back rather than reformatting, so a bad update cannot destroy a deployed node's identity and queued data.

If the DS3231 loses power and its time is invalid (for example a depleted coin cell), the node detects the lost-power condition and requests a time sync from the hub before resuming sampling. Independently, if a node's last time sync is more than 24 hours old it runs a bounded ESP-NOW recovery attempt during a data wake, and the hub proactively assists nodes it infers to be stale.

*PLACEHOLDER: Add annotated screenshots of the browser interface — node discovery, pairing, schedule configuration, deployment, and data export.*

---

## 7. Validation and characterisation

### 7.1 Bench commissioning overview

The platform was validated through a staged bench commissioning workflow in four stages: (A) hardware and power integrity; (B) individual sensor and interface pathways; (C) hub-controlled network functions; and (D) local CSV logging and restart persistence. Outcomes were classified as pass, partial, pending, or fail (Tables 1 and 2). Each stage has a corresponding self-contained bring-up sketch in the repository, so a replicating group can reproduce the same sequence on their own boards.

The integrated multi-sensor trials in §7.2–7.3 were carried out on NODE_v2 hardware. Subsequent electrical bring-up of the current NODE_v3 board confirmed parity with NODE_v2 across every subsystem used by the platform described here, so the results are presented as characterising the current platform.

### 7.2 Firmware and network validation

Bench commissioning confirmed that the firmware and hub workflow supported node management, scheduling, queueing, and logging. Powered nodes were discoverable over ESP-NOW, could be assigned to the hub, and moved through the unpaired, paired, deployed, and paused states without reflashing or serial reconfiguration. Schedule commands issued through the browser interface were retained by nodes in the deployed state.

Controlled deployment-style testing was completed first on a single fully instrumented node, then across three nodes at a 1-minute wake cadence with an auto-derived 18-minute sync cadence. Records were queued between sync windows, transmitted during the shared sync period, and appended to the local CSV log. Power-cycle testing confirmed that deployment state and schedule metadata persisted after reset or power interruption.

The three-node test demonstrated concurrent wake cycles and sync-window backlog flushing with no evidence of system-level queue or receive-path failure. This is the corrected architecture: an earlier four-node fleet-flush stress test (1-minute wake, 5-minute sync) lost approximately 27 rows over three cycles because storage writes blocked the ESP-NOW receive callback. That failure drove the redesign to one snapshot packet per node per wake with a deferred RAM-FIFO drain to storage, and the three-node run used the fixed architecture. Reporting the failure alongside the fix is what makes the current queue and transport design interpretable.

ESP-NOW range testing confirmed communication at increasing distances: ~98.5 % acknowledgement at 1 m line-of-sight, ~99.7 % at 30 m line-of-sight, and ~84–87 % at 100 m with weak or obstructed line-of-sight — usable but degraded. The delivery-gated queue pop means a degraded link costs retries and window time rather than data.

### 7.3 Integrated sensor payload validation

The integrated non-wind sensor payload was tested through the node firmware and hub logging pathway. Single-node testing confirmed the full path from sensor readout to queued snapshot, sync-window flush, hub reception, and CSV logging. Logged rows contained valid battery, air temperature, relative humidity, spectral, and soil values with the expected sensor-present mask of `0x0137` — battery, air temperature, air humidity, the spectral group, and both soil groups, with no wind or auxiliary channels configured.

Three-node testing demonstrated concurrent operation over repeated wake cycles and a shared sync flush. Two nodes produced complete sensor groups throughout; one reported `0x0133` — the same mask with the spectral bit cleared. This was traced to a local wiring issue rather than a firmware, queueing, transport, or logging failure, and was corrected in hardware. That the mask reported the absence correctly, and that the row was still logged with the remaining channels intact, is itself the evidence that per-sensor fault isolation works as designed.

### 7.4 Subsystem-specific outcomes

**Table 1.** Firmware and network commissioning outcomes.

| Subsystem | Method | Outcome |
|---|---|---|
| DS3231 RTC and dual-alarm (node) | 10 s alarm; GPIO wake, alarm clear, flag re-arm | Pass |
| I²C bus scan | Enumerate devices (0x48, 0x68, 0x71 on root bus; 0x44 on mux ch0) | Pass |
| Power-hold gate, node | Assert/release `PWR_HOLD`; confirm rail drop on release | Pass |
| Power-hold gate and config latch, hub | Assert/release `PWR_HOLD`; set/clear config latch; wake-source resolution (RTC / config button / USB) | Pass |
| ESP-NOW range | Broadcast at 100 ms intervals; delivery rate across distances | Pass (98.5 % at 1 m, 99.7 % at 30 m, 84–87 % at 100 m obstructed) |
| Hub sync session | Coordinated pull: hello roster, dump grant, delivery-gated pop, sync release | Pass |
| Node lifecycle control | Discover, pair/unpair, deploy/pause/stop, schedule updates, state retention | Pass |
| Declarative config convergence | Version-gated `NODE_CONFIG` replay across sync windows; idempotent apply; acknowledgement repair after cold wake | Pass (on-device assertion suite) |
| Local CSV logging | Receive packets; append timestamped 30-column CSV records; legacy 25-column drain | Pass (LittleFS) |
| Snapshot queue robustness | Checksum detection of partial writes; corrupt-record rejection; capacity-drop flagging; V1→V2 migration | Pass |
| Power-cycle persistence | Retained deployment status, schedule, sensor mask, and queue after hard power-cut | Pass |
| Signed A/B OTA mechanism (hub + node) | Ed25519 manifest verify, SHA-256 image gate, slot switch, deferred-verify rollback on stock bootloader | Pass (bench-proven on both roles) |
| Cloud OTA delivery (hub) | Chunked HTTPS range download over LTE, pre-erase, install, rollback — 1.3 MB image | Pass (bench-proven) |
| OTA delivery to nodes | — | Not implemented (§7.5) |
| LTE AT handshake + HTTPS | A7670G: AT, IMEI, SIM, network registration, chunked HTTPS | Pass (bench-proven) |
| End-to-end cloud path | Three nodes → ESP-NOW → hub → LTE → HTTPS → cloud database, 1 min wake / 18 min sync | Pass (commissioning session, 2026-07-19; 384 readings over ~2 h 41 min) `[EVIDENCE GAP]` |
| Remote configuration convergence | Node config changes queued at the cloud endpoint, collected on scheduled check-in, and converged against live nodes | Pass (2 commands converged 19 and 33 min after issue, against an 18 min sync interval; 1 cancelled inside the pre-delivery window) |
| Remote-initiated hub self-update | Queued update request → hub fetch, verify, install | Implemented end-to-end; no remotely initiated update has yet completed against live hardware (§7.5) |
| microSD archival logging | — | Not implemented (§7.5) |

**Table 2.** Sensor-specific validation outcomes.

| Sensor pathway | Validation method | Outcome | Interpretation |
|---|---|---|---|
| Air temperature and RH (SHT4x) | Integrated readout, queueing, sync, CSV logging | Pass | Valid records logged across nodes (e.g. 18.4–19.9 °C, 44–66 % RH) |
| Spectral (AS7341) | Mux selection, 8-band snapshot, dark/illumination response, metadata (Clear/NIR/gain/integration/saturation) | Pass (one local wiring issue) | Functional; one node corrected after hardware inspection. Metadata fields confirmed finite by a dedicated regression test after an earlier null-metadata defect |
| Soil moisture 1 (ADS1015 A1) | Integrated readout and CSV logging; water-dunk response | Pass | Operational readings; not calibrated volumetric water content |
| Soil temperature 1 (ADS1015 A0) | Integrated readout and CSV logging | Pass | Functional bench output; not reference-validated |
| Soil moisture 2 (ADS1015 A2) | Integrated readout and CSV logging; water-dunk response | Pass | Operational readings; not calibrated volumetric water content |
| Soil temperature 2 (ADS1015 A3) | Integrated readout and CSV logging | Pass | Functional bench output; not reference-validated |
| Battery voltage (GPIO35 ADC) | Integrated readout and CSV logging | Pass | Firmware within ~1 mV of DMM after scale calibration |
| Reed-cup wind (GPIO4) | Standalone bring-up sketch: pulse counting, debounce, adaptive probe/window, frequency→speed conversion | Pass (standalone) | Functional wind-speed output. **Not included in the integrated multi-sensor run**; end-to-end capture through the queue, sync, and CSV path is untested |

### 7.5 Capabilities and limitations

**Capabilities**

- Integrated multi-sensor payload (air temperature, relative humidity, spectral light, soil moisture, soil temperature, wind speed, battery voltage) in a single low-cost node
- Wireless node-and-hub architecture supporting distributed deployment with no internet infrastructure, and no loss of core function when the LTE backhaul is unpopulated
- Hard power-cut on both node and hub, with dual-alarm scheduling and NVS-backed recovery — no quiescent draw, automatic resume after power loss
- Browser-based field configuration without reflashing (wake interval, sensor mask, deploy/pause/stop/unpair)
- Declarative, version-gated over-the-air configuration converging reliably against power-gated nodes
- Signed A/B firmware update mechanism (Ed25519 manifest, SHA-256, deferred-verify rollback), bench-proven on both node and hub silicon; delivery implemented end-to-end for the hub over LTE
- Structural queue-overflow prevention via the fixed sync multiplier, rather than runtime clamping
- Coordinated sync pull session with delivery-gated queue pop — no silent data loss, and capacity-driven drops flagged in the data
- Multi-node concurrent operation validated for three nodes
- ESP-NOW delivery of 99.7 % at 30 m line-of-sight and 84–87 % at 100 m obstructed, under bench conditions
- Modular I²C and analog expansion, with a key–value payload keyed on stable sensor identifiers
- Script-generated sensor housings fixing radiation shielding, diffuser geometry, and sensor placement across builds, in a standard IP-rated commercial enclosure
- Open-source PCB, housing scripts, and firmware for full reproducibility

**Limitations**

- Bench-commissioned only; field deployment, environmental durability, and long-term drift have not been assessed
- Battery life has not been measured empirically. The power architecture is designed to eliminate between-wake draw, but no long-duration discharge characterisation has been performed
- The spectral pathway provides a PAR-oriented spectral response but has not been calibrated against a reference PAR sensor; the diffuser housing fixes geometry but supplies no radiometric calibration
- Soil moisture is published as uncalibrated probe voltage. Conversion to volumetric water content is deferred by design so probe-specific curves can be applied during analysis, but no calibration curve is currently shipped at any layer of the platform, and none of the values reported here are volumetric water content `[EVIDENCE GAP — closed by a probe calibration against gravimetric or reference-probe samples, yielding a published curve]`
- Wind is measured as speed only. Wind direction is reserved in the data schema but not populated. The node PCB carries an unpopulated ultrasonic time-of-flight anemometer pathway which did not achieve acoustic discrimination on either board revision and is excluded from this system; its design, bring-up results, and diagnosed root cause are documented in the project repository
- Reed-cup wind was validated standalone and has not been exercised through the integrated queue → sync → CSV path
- Hub archival storage is currently limited. The FieldHub V2 carries a microSD socket, but the snapshot logging path writes only to the 768 kB LittleFS partition, which suits a sync-and-upload buffer rather than long-term standalone logging. Routing logging to the card, with a retention and rotation policy, is scheduled for the next hub hardware revision
- A firmware build configuration that compiles out the modem and upload paths for connectivity-free deployments is planned but not yet implemented; the LTE section can be omitted at the hardware level today
- The cloud path has been demonstrated end-to-end but not sustained. A single commissioning session on 2026-07-19 carried 384 readings from three nodes over approximately 2 h 41 min, over LTE into the cloud database, with the remote-configuration control plane exercised successfully in the same period. No multi-week deployment has been run, so upload reliability, cellular robustness across weather and diurnal RF variation, and hub power behaviour under sustained field duty are uncharacterised `[EVIDENCE GAP — closed by a continuous multi-week deployment reporting upload success rate, gap distribution, and per-session reading yield]`
- Remotely initiated hub self-update is implemented end-to-end but has not yet completed against live hardware; the update mechanism itself is separately bench-proven (Table 1) `[EVIDENCE GAP — closed by one successful cloud-initiated hub update on deployed hardware]`
- Node OTA firmware update is not implemented. The signed install-and-rollback mechanism is bench-proven on node hardware, but there is no image-delivery path to a node, because a power-gated node is only reachable for a short window each sync cycle
- The printed sensor housings were generated from scripts but not physically fabricated; fit against the enclosure, gland sealing, ingress rating in service, and weatherproofing are untested, and the enclosure lid fixing pattern is measurement-derived rather than drawing-controlled
- Sensor accuracy, precision, response time, and equivalence to reference-grade instrumentation have not been established
- Maximum network size has not been tested beyond three nodes

### 7.6 Data schema and reproducibility

Three properties of the implementation determine whether another group can reproduce and extend this work rather than merely rebuild it.

**Single protocol source of truth.** Every ESP-NOW message structure, sensor identifier, and capability bit is defined once in a header shared verbatim by the node and hub builds, and every wire structure carries a compile-time size assertion. A change that would desynchronise the two firmware images fails to compile rather than corrupting data in the field.

**Stable, extensible payload keys.** Readings travel as key–value pairs on stable numeric sensor identifiers grouped by domain (air, spectral, wind, soil, auxiliary), not as fixed positional fields. Adding a sensor extends the payload without renumbering existing channels or invalidating archived data, and the same bit layout serves both the operator-configured "expected sensors" mask and the per-record "sensors present" field — which is what lets the hub detect a silently failed sensor rather than logging a gap.

**Timebase.** The hub's DS3231, set from the operator's browser in UTC, is the sole clock authority for all recorded data. Nodes take their time from the hub. The backend supplies a UTC reference used only for validating command issue and expiry windows and for diagnostics; it never overrides the hub RTC and never touches a stored timestamp. All stored and uploaded timestamps are UTC, and local-time rendering is confined to the presentation layer. This separation is deliberate and load-bearing: an earlier backend field carried local rather than UTC time and desynchronised the fleet by two hours, which is the failure this architecture now forecloses.

*PLACEHOLDER: If available, add a figure showing example CSV output or a time series from the bench commissioning test to illustrate data quality.*

---

## CRediT author statement

*PLACEHOLDER — draft below, to be confirmed:*

Tom Armstrong: Conceptualization, Methodology, Software, Hardware design, Investigation, Data curation, Writing — Original draft, Visualization. Bradley Evans: *PLACEHOLDER — no CRediT roles assigned; confirm contribution or authorship.* Eric J. Nordberg: Conceptualization, Supervision, Funding acquisition, Writing — Reviewing and Editing.

---

## Acknowledgements

*PLACEHOLDER — draft below:*

We acknowledge the Anaiwan people as the Traditional Custodians of the land on which this research was conducted and pay our respects to Elders past, present, and emerging.

The SHT4x radiation shield is derived from "Stevenson Shield (mini)" by Thingiverse user commonslabgr (https://www.thingiverse.com/thing:4915068), itself a remix of "Stevenson Screen" by Thingiverse user raingel (https://www.thingiverse.com/thing:3486326), both under Creative Commons Attribution 4.0. We thank both authors. Our modified version is released under the same licence, and changes were made as described in §2.9 and in the design files.

Funding: This work was supported by the University of New England Internal Funding Scheme (IFS) [grant number *PLACEHOLDER*] and the Renewable Energy Wildlife Institute [grant number *PLACEHOLDER*]. *PLACEHOLDER: add any additional funding sources, volunteer contributions, or technical support.*

---

## Ethics statements

This work did not involve human subjects or animal experiments. No ethics approval was required.

---

## Declaration of competing interests

*PLACEHOLDER — draft below; confirm wording with UNE before submission.*

The authors declare the following competing interests. The hardware designs, sensor housings, and device firmware described in this work are released under open licences (see Specifications table) and are fully functional without any hosted service. The authors additionally operate a reference cloud implementation of the documented upload and remote-management interface described in §6.6, and may offer hosted or assembly services based on this platform. The interface specification is published with the firmware so that any group may implement its own endpoint, and no result reported in this paper depends on the authors' implementation.

---

## References

> Reformat to HardwareX/Elsevier style before submission. Add the repository DOI once design files are deposited, and citations for both upstream Thingiverse models.

---

# Working appendix — strip before submission

## A. Open items

**Blocking (must resolve before deposit)**

1. **UNE IP ownership.** Confirm with the UNE research/commercialisation office who owns the designs and firmware before depositing anything under an open licence. This is the one step that cannot be undone afterwards, and it governs both the licence choice and any later commercialisation.
2. **Licence confirmation.** Proposed: CERN-OHL-S-2.0 (PCBs), MPL-2.0 (firmware), CC BY 4.0 (housings). MPL-2.0 is proposed over GPL-3.0 for firmware specifically to avoid the GPLv3 anti-tivoization interaction with the Ed25519-signed OTA path on commercially distributed devices. The radiation shield's CC BY 4.0 terms are inherited and settled.
3. **Repository deposit.** Upload design files to OSF or Zenodo and obtain a DOI for the Specifications table and §3.

**Content**

4. Build the per-node cost from actual order invoices (§4 explains what the BOM price columns miss).
5. Specify the node and hub junction boxes (manufacturer, model, internal dimensions, IP rating); add to the BOM; confirm the lid drilling pattern.
6. Export STEP alongside STL for the AS7341 housing.
7. ~~Confirm §6.6 against the backend repository~~ — **done 2026-07-26** (`FieldMeshDashboard` @ `0102661` + live database). Corrections applied; §6.6 retained.
8. Confirm the author list and CRediT contributions — Bradley Evans currently has no CRediT entry.
9. Build photographs, browser-interface screenshots, and an example data figure.

**Evidence gaps — data to collect**

Each of these is a claim that is implemented and correct but not yet publishable. Listed in the order that gives the paper most benefit per unit of effort.

10. **Multi-week cloud deployment (highest value).** A continuous month of three or more nodes reporting over LTE would close the largest single gap in §7.5 and convert the cloud path from "demonstrated" to "characterised". Report upload success rate, gap distribution, per-session reading yield, and hub battery behaviour. This also gives §7.5 an empirical battery-life figure, which is currently missing entirely, and would let the abstract claim sustained autonomous operation rather than bench commissioning.
11. **Soil probe calibration.** A gravimetric or reference-probe calibration yielding a published volts→VWC curve. Until then the platform's soil channel is uncalibrated voltage, which weakens one of the six advertised measurements.
12. **Reed anemometer through the integrated path.** Currently standalone-validated only. A single deployment wake capturing wind through queue → sync → CSV would close it, and is cheap.
13. **One cloud-initiated hub firmware update against live hardware.** The mechanism is bench-proven; only the dashboard-initiated end-to-end run is missing.
14. **Per-node cost from real invoices** (see item 4).

**Before quoting the 2026-07-19 session numbers**

15. Confirm whether a database "reading" row is one snapshot or one sensor channel. 384 rows from 3 nodes over ~161 min at a 1 min wake is roughly 80 % of the ~483 snapshots you would expect, and a reviewer will do that arithmetic. Establish whether the shortfall is nodes booting into the window, a genuine loss, or a row-granularity mismatch, and report the number accordingly.
16. Do **not** cite upload-session counts (53 sessions, 10 carrying data). Most were empty scheduled heartbeats taken while the nodes were unpowered, but stated bare it reads as an 81 % upload failure rate and would need a paragraph to explain away.
17. The earlier 2026-07-03→17 prototype run is no longer in the cloud database. If a hub CSV export survives, it is a longer window than anything currently citable and worth recovering before the month-long run starts.

**Engineering defect found during verification (not a manuscript item)**

18. **403 retry mismatch — firmware side fixed 2026-07-26.** The backend returns HTTP 403 for an invalid, revoked, or MAC-mismatched upload; the hub firmware treated only 400/401 as non-retryable, so a revoked key fell into the retryable branch and a deployed hub would retry indefinitely under cooldown. The firmware now classifies all 4xx except 408 and 429 as terminal, via a shared `isNonRetryableHttpStatus()` helper applied at all three upload sites (`mothership/firmware/v2/src/main.cpp`); builds clean. **Still open:** hubs already in the field run the old build and remain affected, and node/hub OTA to deployed hardware is not yet reliable — so a backend change from 403 to 401 for credential failures is requested, which would fix the existing fleet without touching a device. See `docs/FIELDMESH_BACKEND_403_RETRY_CONTRACT_2026-07-26.md`. Resolve before the multi-week run.

**Decided (2026-07-26)**

- Ultrasonic wind pathway scoped out; the board's unpopulated circuitry is noted once in §7.5 because it is visible in the published Gerbers.
- Custom printed head/roof/belly enclosure dropped; node and hub use commercial IP-rated junction boxes.
- Firmware published whole, with an offline build configuration to follow; the backend/dashboard stays a separate unpublished codebase.
- microSD included as hub hardware; the logging path to it is future work and is stated as such.
- §6.6 rewritten to document the **interface** rather than the service (2026-07-26). The hub speaks a documented protocol to a configurable endpoint; the authors' cloud implementation is named as a reference implementation that nothing depends on. This discharges the reproducibility obligation — a reader can run the full platform against their own server — without publishing the hosted service, which is intended to be commercial. Removed: the onboarding walkthrough, the project/ownership and row-level-security model, connection-key issuance mechanics, and dashboard display states. Retained: the convergence semantics and undeploy-local-only, which are firmware design properties rather than service features. Section length 457 → ~290 words. A competing-interests declaration has been drafted to accompany this.
- **Open question for the author:** the interface specification promised in §6.6 ("documented alongside the firmware") does not exist yet as a standalone document. Before deposit, either write it — payload shape, auth scheme, command-collection response, status-code contract — or soften the §6.6 wording to what the published firmware source alone demonstrates. A reviewer testing the reproducibility claim will look for it.

## B. Change log against the previous draft

Verified against `Data-Logger-network---ESP32` @ `e8acf05`.

**Corrections**

1. **Snapshot queue** — "24-slot" was the legacy V1 format. Current queue is a ~3.5 kB circular NVS byte slab under an FNV-1a checksum. Added the structural overflow argument (K = 18 → ~82 % of a 22-wake ceiling).
2. **Wi-Fi access point** — described as "open"; it is WPA2-protected. Corrected in §2.6, §6.1, §6.2, §6.6.
3. **Node OTA** — the draft contradicted itself (§2.6 "bench-proven on both node and hub" vs §7.5 "not implemented"). Separated: the *mechanism* is proven on both roles; the *delivery path* exists only for the hub, over LTE. Table 1 has distinct rows.
4. **Ultrasonic** — removed from scope. The previous framing ("not yet achieved acoustic discrimination") understated the diagnosis: V3 passes every electrical bring-up stage but recorded zero acoustic detections across 98 shots, traced to a board topology error (four transducer connectors all wired to the transmit nets, receive multiplexer on unconnected nets). Full diagnosis retained in `node/docs/NODE_V4_ULTRASONIC_REDESIGN_NOTES.md`.
5. **CSV description** — claimed both "hub logging time" and "node sampling time"; there is one timestamp column, the node's RTC capture time. Replaced with the real 30-column schema and the legacy 25-column drain path.
6. **Abstract** — "reliable ESP-NOW communication at 100 m" overstated an 84–87 % obstructed result; replaced with measured figures.
7. **Reed anemometer** — Table 2 described integrated CSV logging; it was standalone-validated only and explicitly absent from the 5 May run. Corrected and added as a limitation.
8. **Soil ADC wiring** — §5.4 implied the ADS1015 reaches the ESP32 through the PCA9306; it sits directly on the root bus at 0x48.
9. **Cost** — naive BOM extension gives ~USD 5.9/5.5, which are volume-tier catalogue prices, not build costs. §4 now says so.
10. **Hub storage** — the draft implied SD archival logging works. The V2 snapshot path writes to the 768 kB LittleFS partition only.

**Additions**

11. Hub power architecture (§2.2, §2.8, Table 1) — the hub also hard-power-cuts and wakes on RTC alarm, config-button latch, or USB, config button taking priority. The field interface is summoned, not continuously available.
12. Mechanical design and sensor housings (§2.9, §3, §5.6) — radiation shield and diffuser housing, script-generated, on a commercial IP-rated enclosure. This is the evidence for design principle 4, previously asserted but unsubstantiated.
13. Licence provenance — chain resolved: raingel (CC BY 4.0) → commonslabgr → this work. No share-alike or non-commercial term. Upstream meshes and licence text now vendored in the repository; the generator reads them by default.
14. Spectral band centres (§2.4) — 415–680 nm plus Clear, NIR, and per-record gain/integration/saturation metadata.
15. Reed acquisition detail (§2.4) — 2 s adaptive probe, 10 s window, 5 ms debounce, two-edge minimum, 0.6667 m s⁻¹ per Hz.
16. Data schema and reproducibility (§7.6) — shared protocol header with compile-time size assertions, stable key–value identifiers, UTC timebase rule.
17. Deployment without connectivity (§2.7) — the no-backhaul configuration stated as a first-class mode rather than a degraded one.
18. Prior fleet-flush failure and its fix (§7.2) — the four-node run that lost ~27 rows and the redesign it motivated.
19. Snapshot packet sizing (§2.5) — 48-byte header + up to 33 × 6-byte readings = 246 bytes, inside the 250-byte limit.
20. Wake-interval option set (§6.3) — 1, 5, 10, 20, 30, 60 min, validated identically on both sides.
21. Sync window timings (§2.8, §6.4) — node listens ~60 s; hub window nominally 120 s.
22. NVS-preserving rollback (§6.8) — a pending image that cannot mount NVS rolls back rather than reformatting.
23. Test points (§5.3) — 27 labelled test points make the staged bring-up practical.
24. Battery-life limitation (§7.5) — no empirical discharge characterisation exists.
25. CRediT gap — Bradley Evans has no assigned roles.

## C. Backend verification round (2026-07-26)

Verified against `FieldMeshDashboard` @ `0102661` and its live Supabase project, read-only. §6.6 was retained rather than cut: the control-plane result is the only empirical evidence for the declarative convergence design, which is one of the paper's more novel contributions.

**Corrections applied**

27. **Soil volts → VWC** — the draft presented backend-side conversion as a delivered design feature. No conversion exists at any layer; the dashboard correctly labels the channel "Soil 1 Voltage". §2.4 and §7.5 now state that deferral is the design and that the published product is uncalibrated voltage.
28. **Ingest keying** — "keyed by mothership UUID and device MAC" was wrong on both halves. Uploads are authenticated by a per-hub bearer connection key, cross-checked against the registered factory MAC; readings are keyed by node identifier string, and no UUID is sent by the device. §6.5 corrected.
29. **Non-retryable codes** — "400/401 treated as non-retryable" described firmware only, not the contract. §6.5 now says 4xx client responses generally. See open item 18 for the underlying defect.
30. **Timebase** — "the backend no longer asserts an authoritative time" was too strong. The backend does supply a UTC reference, used for command issue/expiry validation and diagnostics; it never overrides the RTC or a stored timestamp. §7.6 now states the precise division, and names the two-hour desync it forecloses.
31. **Project privacy and cardinality** — projects are not strictly owner-private (an additive read-only collaborator path exists), and the one-hub-per-project constraint has been dropped, so a project may hold several FieldHubs. §6.6 corrected.
32. **Sync-anchor preservation** — this is a firmware property, not a backend one. The backend guarantees only atomic project-wide queueing; the anchor is preserved by the hub. §6.6 now attributes each half correctly.
33. **Provisioning contact path** — "the dashboard never talks to a device directly" is true after provisioning, but onboarding routes a QR from the operator's phone to the hub's own access point. §6.6 now scopes the claim to post-provisioning and notes that the key never transits the internet, which is a security property worth stating.
34. **Hub self-update** — softened from a capability claim to "exposed and managed from the dashboard", with the missing live evidence recorded in Table 1 and §7.5.

**Evidence added**

35. Table 1 gained three rows: the end-to-end cloud path (384 readings, three nodes, ~2 h 41 min, 2026-07-19), remote configuration convergence (two commands converged at 19 and 33 minutes against an 18 minute sync interval, one cancelled pre-delivery), and dashboard-initiated hub self-update. The convergence timings are the strongest empirical result in the cloud path: they match what the architecture predicts, on real hardware.

**Confirmed without change**

36. Hardware-code display and server-side MAC verification; connection-key hashing, storage isolation, and re-minting; the Queued → Accepted → Applied lifecycle and its cancel window; sleeping nodes not counted as applied until the echoed config version matches; Paused/Resuming display semantics; undeploy refused at interface, API, and database; spectral metadata arriving and persisting as finite values, confirmed against live rows.
