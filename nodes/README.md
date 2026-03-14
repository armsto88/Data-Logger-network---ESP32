# Sensor Node Projects

This folder holds node firmware projects that pair with the mothership over ESP-NOW.

## Layout

```text
nodes/
|- shared/                    # shared headers (protocol, sensor slots)
|- air-temperature-node/      # active ESP32-C3 node firmware
|- soil-moisture-node/        # placeholder for dedicated soil node firmware
```

## Active Build: air-temperature-node

- Project: `nodes/air-temperature-node`
- PlatformIO env: `esp32c3`
- Board: `esp32-c3-devkitm-1`

Build from repo root:

```powershell
& "$env:USERPROFILE\.platformio\penv\Scripts\platformio.exe" run -d .\nodes\air-temperature-node -e esp32c3
```

Upload + monitor:

```powershell
& "$env:USERPROFILE\.platformio\penv\Scripts\platformio.exe" run -d .\nodes\air-temperature-node -e esp32c3 -t upload
& "$env:USERPROFILE\.platformio\penv\Scripts\platformio.exe" device monitor -d .\nodes\air-temperature-node -e esp32c3
```

Port defaults are currently pinned in `nodes/air-temperature-node/platformio.ini`:

- `upload_port = COM3`
- `monitor_port = COM3`

## Source Layout Reference

- `nodes/air-temperature-node/src/README.md` describes node source modules.
- `nodes/soil-moisture-node/src/README.md` defines the target module pattern for the upcoming soil node firmware.