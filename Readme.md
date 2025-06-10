# Brainstorm: Wireless Data Logging Network

This project outlines a modular, low-power wireless sensor network using ESP32 microcontrollers and ESP-NOW. It is designed for environmental data collection, where multiple sensor nodes transmit readings to a central “mother ship” for timestamped logging.

---

## Goals
- Enable low-power, solar-powered sensor nodes
- Wireless data transfer via ESP-NOW without a router
- Compact PCB form factor (~6 × 3 cm per node)
- Support up to 4 sensors per node using multiplexers
- Accurate timestamping with onboard RTCs (e.g. DS3231)

---

## Mother Ship
- Based on an ESP32-WROVER for SD reliability and memory
- Receives ESP-NOW packets from multiple nodes
- Adds timestamps via DS3231 and logs to SD card (CSV format)
- Wakes every 30 minutes, stays active for 30–60 seconds
- Can broadcast its current time to synchronize node RTCs

---

## Sensor Nodes
- Built around ESP32 Super Mini boards
- Configurable to support:
  - DS18B20 temperature sensors (up to 4 per node)
  - DHT22 sensors for temp and humidity (up to 4 per node)
- Operate in light sleep, waking periodically to “sniff” for the mother ship
- Transmit data when the mother ship is detected
- Optional RTC per node for consistent wake cycles and syncing

---

## Power Strategy
- Each node and the mother ship powered by a LiPo battery + solar module
- Charging handled by compact solar charge controllers
- Optimized for low power draw and long unattended operation

---

## Future Considerations
- Evaluate DA16200MOD for persistent Wi-Fi connectivity and remote wake
- Explore 433 MHz RX modules for low-power wake signaling
- Consider smaller RTCs like AB1805 or RV3028 for tighter layouts
- Develop custom PCB combining ESP32, RTC, mux, and charging circuit

---

> Next step: synchronize wake timing between mother ship and nodes, then prototype a 6 × 3 cm PCB layout for field deployment.
