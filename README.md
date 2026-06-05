# ESP32 Sensor Network � Mothership + Nodes

An **ESP-NOW**�based environmental sensor network: field-deployed nodes collect data, a mothership hub aggregates and logs it, and an optional LTE backhaul uploads to the cloud.

---

## Repository Structure

```
mothership/           Mothership hub (ESP32-WROOM)
  docs/                 Power, wake, LTE, PCB design notes
  firmware/src/         Production firmware (main, BLE, ESP-NOW, SD, RTC)

node/                 Sensor nodes (ESP32-WROOM)
  docs/                 Hardware, sensors, ultrasonic design notes
  firmware/
    src/                Production firmware (main, drivers, sensors, storage, RTC)
    shared/             Shared protocol headers (protocol.h, sensors.h)
    tests/              Bring-up and validation sketches

docs/                 Shared/cross-system documentation
  concept_overview.md   System architecture and ESP-NOW messaging
  FIRMWARE_AND_HARDWARE_NOTES.md  Integration notes
  NATIVE_APP_*.md       App integration and data schemas
  MULTI_NODE_VALIDATION_*.md  Deployment validation
  archive/              Manuscript drafts and reference files

hardware/             PCB, CAD, simulation, and manufacturing assets
  ultrasonic_anemometer/  Ultrasonic wind sensor subproject

platformio.ini       Build configuration (all targets)
CONTRIBUTING.md      Repo organization and contribution rules
```

---

## Quick Links

- **Mothership docs:** [mothership/docs/README.md](mothership/docs/README.md)
- **Node docs:** [node/docs/README.md](node/docs/README.md)
- **Shared docs index:** [docs/README.md](docs/README.md)
- **Contribution guide:** [CONTRIBUTING.md](CONTRIBUTING.md)

---

## System Overview

### Mothership (ESP32-WROOM)

- Runs a Wi-Fi access point with a web UI dashboard
- Receives sensor data from nodes via ESP-NOW
- Logs all data to CSV on a microSD card
- Keeps time via a DS3231 RTC (the network time authority)
- Manages node discovery, pairing, deployment, and scheduling
- Power-gated design: DS3231 alarm or config button wakes the board; ESP32 latches power via `PWR_HOLD`
- Two power domains: `KEEP_ALIVE` (always-on, RTC + config latch) and `3V3_SYS` (switched, ESP32 + SD + logic)
- Optional LTE backhaul via SIMCom A7670G modem (UART2 + level shifters)
- CH340C USB-UART for programming and serial console

### Sensor Nodes (ESP32-WROOM)

- Sleep between measurement intervals (DS3231 Alarm 1 + FET power gate)
- Environmental sensors: SHT40 (temp/humidity), AS7343 (light spectrum), ADS1015 (soil probes)
- Ultrasonic anemometer subsystem: 40 kHz transducers, MT3608 22 V boost, TC4427 driver, TLV9062IDR RX amplifier
- I2C sensor mux via PCA9548A
- Transmit readings to the mothership via ESP-NOW
- Automatically request time sync when needed
- Resume operation after power cuts (NVS state + RTC coin cell)
- RUN/KILL hard switch for full power cut

### Communication

- **Data:** node -> mothership (`SENSOR_DATA` packets)
- **Control:** mothership -> node (`PAIR_NODE`, `DEPLOY_NODE`, `SET_SCHEDULE`, `UNPAIR_NODE`, `TIME_SYNC`)
- **Discovery:** nodes broadcast `DISCOVER_REQUEST`; mothership responds

---

## Building

The root `platformio.ini` defines all build targets. Build from the repo root:

```bash
# Mothership
pio run -e esp32s3

# Node main systems bring-up
pio run -e esp32wroom-v2-main-systems

# Individual bring-up tests
pio run -e esp32wroom-i2c-scan
pio run -e esp32wroom-ds3231-alarm-10s
pio run -e esp32wroom-sht40-as7343-mux
# ... see platformio.ini for all environments
```

---

## Key Design Documents

| Document | Location | Description |
|---|---|---|
| Power & Wake Architecture | [mothership/docs/MOTHERSHIP_POWER_AND_WAKE_DESIGN_NOTE.md](mothership/docs/MOTHERSHIP_POWER_AND_WAKE_DESIGN_NOTE.md) | Power gating, RTC wake, pin allocation |
| PCB Schematic Review | [mothership/docs/MOTHERSHIP_PCB_SCHEMATIC_REVIEW_2026-06-05.md](mothership/docs/MOTHERSHIP_PCB_SCHEMATIC_REVIEW_2026-06-05.md) | Consolidated pre-order review with critical checks |
| LTE Backhaul Concept | [mothership/docs/MOTHERSHIP_LTE_BACKHAUL_CONCEPT.md](mothership/docs/MOTHERSHIP_LTE_BACKHAUL_CONCEPT.md) | A7670G modem integration and upload workflow |
| Node Hardware Checklist | [node/docs/NODE_HARDWARE_V2_CHECKLIST.md](node/docs/NODE_HARDWARE_V2_CHECKLIST.md) | V2 hardware bring-up and validation |
| Node PCB Overview | [node/docs/NODE-PCB-OVERVIEW.md](node/docs/NODE-PCB-OVERVIEW.md) | Node PCB design summary and pin mapping |
| Local Storage Contract | [node/docs/NODE-LOCAL-STORAGE-CONTRACT-V1.md](node/docs/NODE-LOCAL-STORAGE-CONTRACT-V1.md) | Queue storage and data format specification |
| Concept Overview | [docs/concept_overview.md](docs/concept_overview.md) | High-level system architecture |

