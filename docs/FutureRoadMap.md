# ESP-NOW Environmental Sensor Network – Long-Term Roadmap

**Version:** 2025-01  
**Author:** Tom Armstrong

---

## 🌐 Overview

This roadmap describes the long-term development plan for the ESP32-based “mothership + sensor nodes” environmental monitoring system. It outlines major features, hardware expansions, calibration systems, node configuration, and future research directions.

**Goal:** Build a modular, robust, field-deployable sensor ecosystem supporting many sensor types, dynamic configuration, plug-and-play ports, OTA updates, and long-term reliability for conservation technology applications.

---

## 1. Node-Level Features

### 1.1 Per-Node Wake Interval
- Mothership sends `SET_SCHEDULE` command to each node.
- Node stores interval in NVS and programs DS3231 Alarm 1.
- UI exposes per-node interval adjustments.

**Future Enhancements**
- Per-sensor intervals (soil: 10 min, wind: 1 min, etc.).
- Adaptive intervals based on conditions.
- Node reports next scheduled wake to UI.

---

## 2. Calibration System

External calibration (Pico/MicroPython/R scripts) and internal application on nodes.

### 2.1 Per-Sensor Calibration Entry (UI)
Users enter coefficients for:
- Linear (`a + b*x`)
- Quadratic (`a + b*x + c*x^2`)
- Piecewise (for wind)
- LUT (future)
- Clamp limits (`lo`, `hi`) configurable.

### 2.2 Visualisation in UI
- Live plot of raw input vs calibrated output.
- Compare current vs proposed calibration.
- Validation of curve sanity.

### 2.3 Firmware Integration
- Node stores calibration in NVS.
- Sensor backends pull calib from NVS automatically.
- Mothership holds canonical calibration JSON.
- `CALIB_UPDATE` packet updates nodes wirelessly.

---

## 3. Input Configuration System (“Port Mapper”)
**Target:** 8 physical ports, each supporting any sensor.

### 3.1 Hardware Architecture per Port
Each port can consist of:
- MUX channel
- ADS channel
- TS5A23159 analog switch
- Optional GPIO/interrupt/frequency input

### 3.2 UI Workflow
User chooses:
- Port number
- Sensor type (SoilTemp, SoilMoist, PAR, Wind, Thermocouple, etc.)

System determines:
- Required ADS/MUX channels & switch state
- Power requirements
- Sample interval
- Calibration ID

Node receives a `CONFIG_PORT` packet and configures hardware+backend dynamically.

### 3.3 Plug-and-Play Ports
**Goal:** “Plug anything you want here; configure it in the UI.”

---

## 4. Dynamic Sensor Registry (Node)

Nodes support dynamic registration of sensors per port configuration.

- Each port can expose 0–N logical sensors.
- Backends created dynamically.
- Node reports new sensors to mothership.

**Examples:**
- Soil probe: 2 sensors (temperature + moisture)
- PAR sensor: raw + PPFD
- Wind: Hz + m/s
- DS18B20 splitter: multiple temps

---

## 5. OTA Firmware Updates

Support OTA via ESP-NOW or WiFi AP mode.

**OTA Pipeline:**
- Node wakes and requests update.
- Mothership sends binary in chunks.
- Node verifies and swaps partitions.

UI can push OTA to:
- Single node
- Entire fleet

---

## 6. Diagnostics & Health Monitoring

Nodes send periodic health data:
- Battery voltage
- Solar charge rate
- Internal temperature
- Uptime
- Reset reasons
- Missed alarms
- I²C scan results
- Calibration version applied

UI displays:
- Health sparklines
- Status indicators (OK / warning / offline)
- Node health scoring

---

## 7. Fleet Management

**Future features:**
- Bulk deployment
- Bulk calibration updates
- Syncing configuration profiles
- Version tracking per node
- Auto-detection of incompatible configs

---

## 8. Hardware Roadmap

### 8.1 Node PCB (future)

- ESP32-C3 module/chip
- DS3231 RTC
- PCA9548A (I²C mux)
- MCP23017 (GPIO expander)
- 8-channel ADS1115 equivalent
- 8× TS5A23159 analog switches
- MPPT LiPo charger
- Buck regulator / LDO for ESP32
- Off-board M8 connectors
- ESD protection
- Analog/digital ground plane separation

### 8.2 Modular Sensor Boards

**Future plug-in breakouts:**
- Soil probes (temp + VWC)
- PAR sensors
- Thermocouple modules
- Ultrasonic anemometer
- Micro weather shield

---

## 9. Configuration Profiles

Users can save entire system configurations.

**Profiles can be exported/imported.**

**Example Profile:**  
```yaml
SolarFarmProfile_A:
  intervals:
    TEMP_001: 10
    TEMP_002: 5
  ports:
    TEMP_001:
      1: SoilMoistTemp
      2: PAR
      3: WindFreq
  calibration_sets:
    Soil: LoamProfile_2025
    Wind: BlendFit2025
    PAR: Cal_TSL2591_revB
```
---

## 10. Machine Learning (Long Term)

**Future ML integrations:**
- On-node anomaly detection
- On-node frost prediction
- Edge-processing for audio or weather data
- Drift detection for recalibration alerts
- Cloud offline ML for sensor diagnostics

---

## 11. Additional Ideas

### 11.1 Sensor Templates
- UI can auto-fill configuration for known sensors.

### 11.2 “Sensor Packs”
- 3rd parties can ship a `.sensorpack` file containing:
  - Calibration
  - Wiring
  - ADS requirements
  - Switching configuration
  - Suggested sampling intervals

### 11.3 Auto-Detect Sensors
- For DS18B20 / I²C sensors, node can propose detected sensor list.

### 11.4 Per-Port Power Control
- Allow warm-up cycles or sensor power savings.

---

## End of Roadmap

*This file is a living document and should be updated as capabilities expand and new sensor types are added.*
