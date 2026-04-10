# Firmware Structure

Firmware is split by device role:

- `firmware/mothership/src`: ESP32-S3 mothership runtime (web UI, ESP-NOW manager, RTC, SD logging).
- `firmware/nodes`: Node-side firmware projects and shared node headers.
- `firmware/nodes/bringup`: Node hardware bring-up and diagnostic sketches (ESP32-WROOM test targets).

## Build Mapping (repo root `platformio.ini`)

- `env:esp32s3` compiles from `firmware/mothership/src/**`.
- `env:esp32wroom-*` bring-up targets compile from `firmware/nodes/bringup/*.cpp`.

This keeps mothership production code separate from node production code and node bring-up/testing code.
