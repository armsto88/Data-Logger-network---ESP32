# Brainstorm: Wireless Data Logging Network

This project explores the design and development of a modular, wireless data logging system using ESP32 microcontrollers and ESP-NOW for communication. The system is built for ecological field applications where distributed, low-power sensor nodes report to a central "mother ship" that logs the data and timestamps it.

## Goals
- Develop a robust wireless data logger using ESP32 boards.
- Minimize power consumption to support solar-powered deployments.
- Enable multiple sensor inputs per node using I2C or 1-Wire multiplexers.
- Ensure modularity and small form factor (target size: 6 × 3 cm).
- Allow timestamping and synchronized operation using RTCs.

## Mother Ship
- ESP32-WROVER module used for its larger memory and stability.
- Receives data via ESP-NOW from multiple nodes.
- Logs data to SD card in a structured, column-wise CSV format.
- Uses DS3231 RTC to timestamp all data accurately.
- Optionally remains powered for 30 seconds every 30 minutes to await node signals.
- May transmit current time to nodes for RTC synchronization.

## Sensor Nodes
- ESP32 Super Mini boards with one of:
  - DS18B20 sensors (1–4 per node via 1-Wire).
  - DHT22 sensors for temp and humidity (up to 4 per node via GPIO).
- Wake periodically (e.g. every 10–30 seconds) to sniff for mother ship.
- Transmit data when mother ship is detected.
- Optional: light sleep between sniff cycles to save power.
- Optional: DS3231 or other low-power RTC for scheduled wakeups.

## Power Strategy
- Nodes and mother ship powered via small solar modules with LiPo batteries.
- Charging handled by compact solar charging PCBs.
- Nodes optimized for low power draw, aiming for weeks to months of uptime.

## Future Ideas
- Investigate using DA16200MOD for Wi-Fi wake-on-network capabilities.
- Explore 433 MHz wireless triggers as wake signals.
- Replace DS3231 with AB1805 or smaller RTCs if size constraints arise.
- Develop a custom PCB combining ESP32, RTC, mux, and power management in 6×3 cm form.
