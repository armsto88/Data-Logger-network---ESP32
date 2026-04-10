# Sensor Node Projects

This folder holds node firmware projects that pair with the mothership over ESP-NOW.

## Layout

```text
firmware/nodes/
|- shared/                    # shared headers (protocol, sensor slots)
|- bringup/                   # ESP32-WROOM node bring-up and hardware diagnostics
|- sensor-node/               # active ESP32-C3 node firmware
```

The mothership webserver firmware is separate and lives in `firmware/mothership/src`.

## Active Build: sensor-node

- Project: `firmware/nodes/sensor-node`
- PlatformIO env: `esp32c3`
- Board: `esp32-c3-devkitm-1`

Build from repo root:

```powershell
& "$env:USERPROFILE\.platformio\penv\Scripts\platformio.exe" run -d .\firmware\nodes\sensor-node -e esp32c3
```

Upload + monitor:

```powershell
& "$env:USERPROFILE\.platformio\penv\Scripts\platformio.exe" run -d .\firmware\nodes\sensor-node -e esp32c3 -t upload
& "$env:USERPROFILE\.platformio\penv\Scripts\platformio.exe" device monitor -d .\firmware\nodes\sensor-node -e esp32c3
```

Port defaults are currently pinned in `firmware/nodes/sensor-node/platformio.ini`:

- `upload_port = COM3`
- `monitor_port = COM3`

## Source Layout Reference

- `firmware/nodes/sensor-node/src/README.md` describes node source modules.

## Node Bring-Up Targets (Root platformio.ini)

Node-level bring-up environments are defined in the repo root `platformio.ini` and pull sources from `firmware/nodes/bringup`.

Examples:

- `esp32wroom-serial-counter`
- `esp32wroom-i2c-scan`
- `esp32wroom-ultrasonic-first-test`
- `esp32wroom-ds3231-alarm-10s`
- `esp32wroom-battery-io35`