# Main Firmware Source Layout

- `main.cpp`: mothership app entry point and web UI handlers.
- `comms/`: ESP-NOW node registry, pairing, and command transport.
- `storage/`: SD card CSV logging and file utilities.
- `time/`: RTC setup and time formatting helpers.
- `system/`: hardware and build-time configuration constants.

This structure keeps transport, persistence, and timing concerns separated.

## Build Target

- Project file: `platformio.ini` (repo root)
- Environment: `esp32s3`
- Board: `esp32-s3-devkitc-1`

## Build From Repo Root

```powershell
& "$env:USERPROFILE\.platformio\penv\Scripts\platformio.exe" run -e esp32s3
```

## Upload + Monitor

```powershell
& "$env:USERPROFILE\.platformio\penv\Scripts\platformio.exe" run -e esp32s3 -t upload
& "$env:USERPROFILE\.platformio\penv\Scripts\platformio.exe" device monitor -e esp32s3
```
