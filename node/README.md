# Node

ESP32-C3 sensor nodes: sleep/wake, sensor read, ESP-NOW TX, RTC scheduling, local queue storage.

## Contents

- **docs/** — node-specific design notes
  - Hardware V2 checklist and PCB overview
  - Local storage contract
  - Sensor implementation notes (soil, SHT41, AS7343)
  - Ultrasonic anemometer design notes
  - Bring-up results
- **firmware/src/** — production firmware source
  - `main.cpp` — entry point, sleep/wake, sensor read, TX, sync
  - `drivers/` — ADS1115 ADC helper
  - `sensors/` — sensor drivers (SHT41, AS7343, soil, ultrasonic wind)
  - `storage/` — local NVS queue for readings
  - `time/` — DS3231 RTC wake scheduling
- **firmware/shared/** — shared protocol headers
  - `protocol.h` — ESP-NOW message formats and payloads
  - `sensors.h` — sensor ID and data format constants
- **firmware/tests/** — bring-up and validation sketches
  - Individual hardware test programs for each peripheral
  - V2 system validation tests
- **firmware/platformio.ini** — node build configuration

## Build

The node build target is defined in `node/firmware/platformio.ini`. Build from the repo root:

```bash
pio run -e sensor-node
```

## Related

- Shared docs: [docs/](../docs/)
- Mothership firmware and docs: [mothership/](../mothership/)
- Hardware assets: [hardware/](../hardware/)