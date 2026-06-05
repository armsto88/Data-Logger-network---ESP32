# Sensor Node Source Layout

- `main.cpp`: node lifecycle, pairing/deploy state, ESPNOW loop, and wake logic.
- `drivers/`: low-level sensor interface drivers (e.g., ADS1115).
- `sensors/`: sensor backends and calibration helpers.
- `time/`: DS3231 wake interval/alarm helper logic.

Shared interfaces are in `../shared/` (for example `protocol.h` and `sensors.h`).

## Build Target

- Project file: `firmware/nodes/sensor-node/platformio.ini`
- Environment: `esp32wroom`
- Board: `esp32dev`

## Build From Repo Root

```powershell
& "$env:USERPROFILE\.platformio\penv\Scripts\platformio.exe" run -d .\firmware\nodes\sensor-node -e esp32wroom
```

## Upload + Monitor

`firmware/nodes/sensor-node/platformio.ini` currently pins:
- `upload_port = COM3`
- `monitor_port = COM3`

```powershell
& "$env:USERPROFILE\.platformio\penv\Scripts\platformio.exe" run -d .\firmware\nodes\sensor-node -e esp32wroom -t upload
& "$env:USERPROFILE\.platformio\penv\Scripts\platformio.exe" device monitor -d .\firmware\nodes\sensor-node -e esp32wroom
```
