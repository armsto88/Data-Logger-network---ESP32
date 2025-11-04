# ESP32-S3 Mothership — Sensor Network Receiver

## Project Overview

This is a complete sensor network system with a central **Mothership** (ESP32-S3) that coordinates multiple **Sensor Nodes** (ESP32-C3 Mini) via ESP-NOW wireless communication.

### 🏠 **Mothership (ESP32-S3-DevKitC-1)**
- **Coordinates sensor network** via ESP-NOW
- **Web interface** for configuration and data download
- **RTC timestamps** all sensor data
- **SD card logging** of all network data
- **WiFi Access Point** for configuration

### 📡 **Sensor Nodes (ESP32-C3 Mini)**
- **Battery-powered** with deep sleep
- **Dedicated sensors** (temperature, soil moisture, etc.)
- **Automatic registration** with mothership
- **Configurable wake intervals** via mothership

## Repository Structure

```
📁 Data-Logger-network---ESP32/
├── 📄 platformio.ini           # Mothership configuration
├── 📁 src/                     # Mothership source code
│   ├── main.cpp               # Web interface & coordination
│   ├── rtc_manager.cpp/.h     # RTC time management
│   ├── sd_manager.cpp/.h      # SD card data logging
│   ├── espnow_manager.cpp/.h  # ESP-NOW network management
│   └── config.h               # Pin definitions
└── 📁 nodes/                  # Sensor node projects
    ├── 📁 shared/             # Common protocol definitions
    ├── 📁 air-temperature-node/  # DHT22 temp/humidity node
    ├── 📁 soil-moisture-node/    # Soil moisture node (planned)
    └── 📄 README.md           # Node documentation
```

| Function      | Device      | ESP32-S3 Pin | Typical Arduino Name |
|---------------|-------------|--------------|---------------------|
| RTC SDA       | DS3231      | GPIO 42      | 42                  |
| RTC SCL       | DS3231      | GPIO 41      | 41                  |
| RTC INT/SQW   | DS3231      | GPIO 3       | 3                   |
| SD CS         | SD module   | GPIO 10      | 10                  |
| SD MOSI       | SD module   | GPIO 11      | 11                  |
| SD MISO       | SD module   | GPIO 13      | 13                  |
| SD SCK        | SD module   | GPIO 12      | 12                  |
| Onboard LED   | ESP32-S3    | GPIO 48      | 48                  |

Update pin numbers in `src/config.h` if needed for your board!

## Features

- Listens for ESP-NOW sensor node messages (e.g., soil node)
- Adds RTC timestamp to all received data
- Logs time-stamped CSV data to SD card
- Easy to expand for WiFi/cloud forwarding

## Build/Flash Instructions

1. Install [Positron IDE](https://positron.host/) or PlatformIO/Arduino IDE.
2. Clone this repo and open the project folder.
3. Wire hardware as above.
4. Build and flash firmware.
5. Open Serial Monitor for logs.

## Next Steps

- Implement complete RTC alarm/time logic in `rtc_manager.cpp/.h`
- Expand ESP-NOW data parsing and error handling
- Add support for multiple sensor node message types
