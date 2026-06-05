# Mothership

ESP32-S3 mothership hub: Wi-Fi AP, SD logging, ESP-NOW coordination, RTC time sync, and LTE backhaul.

## Contents

- **docs/** — mothership-specific design notes
  - Power and wake architecture
  - LTE backhaul concept
  - PCB schematic review and reconciliation
  - MT3608 brownout analysis
- **firmware/src/** — production firmware source
  - `main.cpp` — entry point, web UI, ESP-NOW RX loop
  - `ble/` — BLE peripheral for mobile app comms
  - `comms/` — ESP-NOW manager (RX, time sync, node pairing)
  - `storage/` — SD card CSV logging
  - `system/` — compile-time config and pin definitions
  - `time/` — DS3231 RTC control

## Build

The mothership build target is defined in the root `platformio.ini`. Build from the repo root:

```bash
pio run -e mothership
```

## Related

- Shared docs: [docs/](../docs/)
- Node firmware and docs: [node/](../node/)
- Hardware assets: [hardware/](../hardware/)