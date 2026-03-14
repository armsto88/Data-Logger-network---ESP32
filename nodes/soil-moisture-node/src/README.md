# Soil-Moisture Node Source Layout

This source folder is reserved for the soil-moisture node firmware.

Recommended module pattern (matching other node firmware):
- `main.cpp` for node state machine and ESPNOW flow
- `drivers/` for chip-level interfaces
- `sensors/` for sensor backends and calibration logic
- `time/` for RTC alarm/wake helpers
