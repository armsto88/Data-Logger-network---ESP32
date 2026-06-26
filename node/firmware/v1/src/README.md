# Sensor Node Source Layout

- `main.cpp`: node lifecycle, pairing/deploy state, ESPNOW loop, and wake logic.
- `drivers/`: low-level sensor interface drivers (e.g., ADS1115).
- `sensors/`: sensor backends and calibration helpers.
- `time/`: DS3231 wake interval/alarm helper logic.

Shared interfaces are in `../shared/` (for example `protocol.h` and `sensors.h`).

## Build Target

- Project file: `platformio.ini` (repo root)
- Environment: `esp32wroom-v2-main-systems`
- Board: `esp32dev` (ESP32-WROOM)

## Build From Repo Root

```powershell
pio run -e esp32wroom-v2-main-systems
```

## Individual Bring-up Tests

See `platformio.ini` for all `esp32wroom-*` environments. Common ones:

```powershell
pio run -e esp32wroom-i2c-scan
pio run -e esp32wroom-ds3231-alarm-10s
pio run -e esp32wroom-sht40-as7343-mux
```
