# Sensor Node Network

This directory contains individual sensor nodes that communicate with the mothership via ESP-NOW.

## Repository Structure

```
📁 nodes/
├── 📁 shared/
│   └── protocol.h          # Communication protocol definitions
├── 📁 air-temperature-node/
│   ├── platformio.ini      # ESP32-C3 Mini configuration
│   └── src/main.cpp        # DHT22 temperature/humidity sensor
├── 📁 soil-moisture-node/
│   └── (to be created)     # Soil moisture sensor
└── 📁 other-nodes/
    └── (future nodes)      # Light, pH, etc.
```

## Node Types Planned

| Node Type | Sensor | ESP32 Board | Status |
|-----------|--------|-------------|---------|
| Air Temperature | DHT22 | ESP32-C3 Mini | ✅ Ready |
| Soil Moisture | Capacitive | ESP32-C3 Mini | 🔄 Planned |
| Light Level | LDR/BH1750 | ESP32-C3 Mini | 🔄 Planned |
| pH Level | pH Probe | ESP32-C3 Mini | 🔄 Planned |

## Current Air Temperature Node

### Hardware Requirements:
- **ESP32-C3 Mini** development board
- **DHT22** temperature/humidity sensor
- **3.3V power supply** (or USB for testing)
- **Jumper wires**

### Wiring:
```
DHT22 Pin    ESP32-C3 Mini Pin
VCC          3.3V
GND          GND
DATA         GPIO 2
```

### Features:
- 🌡️ **Reads temperature and humidity** every wake cycle
- 📡 **Sends data to mothership** via ESP-NOW
- 😴 **Deep sleep mode** for power efficiency
- 📅 **Configurable wake intervals** (controlled by mothership)
- 🔋 **Low power design** for battery operation

### Configuration:
- **Node ID**: `TEMP_001`
- **Default wake interval**: 5 minutes
- **Mothership MAC**: `30:ed:a0:aa:67:84` (update if different)

## Getting Started

1. **Wire the DHT22 sensor** to the ESP32-C3 Mini
2. **Update the mothership MAC address** in the node code if needed
3. **Compile and upload** using PlatformIO
4. **Place the node** in your monitoring location
5. **Check the mothership web interface** to see incoming data

## Power Consumption

The air temperature node is designed for battery operation:
- **Active time**: ~2-3 seconds per wake cycle
- **Deep sleep current**: ~10-20µA
- **Estimated battery life**: Several months on 18650 battery (depending on wake interval)

## Next Steps

1. Test the air temperature node with your mothership
2. Create soil moisture node
3. Add more sensor types as needed
4. Implement battery monitoring
5. Add solar charging capability