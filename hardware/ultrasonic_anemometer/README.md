# Ultrasonic Anemometer – Final BOM and Build Guide
Optimised for ESP32-C3 wireless sensor node using MT3608, MCP6022, LM311P, and IRLZ44N.

## Bill of Materials (Final)

### 1. Ultrasonic Front-End
- 4 x Hailege 40.5 kHz waterproof ultrasonic transducers
- 5 x MCP6022 dual op-amp
- 10 x LM311P comparator (DIP-8)
- 1 x resistor kit (assorted values)
- 1 x ceramic capacitor kit
- 1 x electrolytic capacitor kit

### 2. TX High-Voltage Driver
- 5 x MT3608 boost converter
- 10 x IRLZ44N logic-level MOSFET (TO-220)
- Gate resistors: 10–100 ohm
- Snubber resistors: 10–47 ohm
- Optional snubber diode: 1N4148 or UF4007

### 3. Power and Regulation
- 1 x LM2596 buck converter (12 V to 5–9 V)
- 3 to 5 x MCP1700 LDO 3.3 V regulators
- Decoupling capacitors: 100 nF and 10 µF near all analog ICs

### 4. ESP32 Integration
- 1 x ESP32-C3 Mini
- 1 x DS3231 RTC (optional)
- 1 x SD card module (optional)
- JST-PH connectors (2-pin and 4-pin)
- Breadboard or prototype board

### 5. Mechanical Parts
- PETG or ASA filament for 3D-printed arms and enclosure pieces
- 4 x PG7 or PG9 cable glands
- ABS or polycarbonate enclosure
- M3 stainless screws, nuts, washers

---

## Build Guide

### 1. TX Stage (Transmit)

Goal: Generate a clean 40 kHz, 8–12 cycle ultrasonic burst at approximately 18–28 Vpp using MT3608 and IRLZ44N.

#### Steps:

1. Set MT3608 output to approximately 24 V. Add 10 µF and 100 nF capacitors at the output.
2. Wire the MOSFET as follows:

   MT3608 (+24 V) -> Transducer -> IRLZ44N Drain  
   IRLZ44N Source -> Ground  
   IRLZ44N Gate -> ESP32 GPIO through 47–100 ohm resistor  
   IRLZ44N Gate -> 100k ohm pull-down to Ground  

3. Generate a 40 kHz burst using ESP32 RMT:
   - 8 to 12 cycles
   - 50 percent duty cycle

4. Repeat for each axis:
   - North to South, then South to North
   - East to West, then West to East

---

### 2. RX Stage (Receive)

Goal: Amplify and filter the returning ultrasonic pulse, then detect its edge using LM311P.

#### Steps:

1. Build a 40 kHz band-pass filter using MCP6022 op-amp:
   - Gain stage example: R1 = 10k, R2 = 100k (gain = 10)
   - Band-pass typical values: R around 3.3k–4.7k, C around 1–2 nF

2. Create a 2.5 V bias reference:
   - 100k resistor to 5 V
   - 100k resistor to Ground
   - 10 µF capacitor to Ground

3. LM311 comparator:
   - Non-inverting input receives filtered signal (AC-coupled and biased)
   - Inverting input receives 2.5 V reference
   - LM311 output is open collector:
     - Add pull-up resistor: 4.7k to 3.3 V
     - Connect this node to ESP32 GPIO (RMT input)

---

### 3. ESP32 Firmware Integration

Goal: Measure time-of-flight (TOF) for both directions per axis.

#### Steps:

1. Send 40 kHz burst.
2. Start RMT timer.
3. Wait for comparator edge.
4. Capture timestamp.
5. Repeat in opposite direction.
6. Compute wind speed.

### Wind Speed Formula

For a single axis:

U = (d / 2) * (1 / t_forward - 1 / t_reverse)

Where:
- d = transducer spacing in meters
- t_forward = TOF in one direction
- t_reverse = TOF in the opposite direction

Compute for both axes and combine vector components.

---

### 4. Calibration

1. Zero-wind calibration:
   - Indoors, no airflow
   - Capture 200–500 TOF samples
   - Average offset and store in ESP32 NVS

2. Gain calibration:
   - Use a reference anemometer and a fan
   - Fit linear correction curve: measured = gain * true + offset

3. Angular calibration:
   - Rotate device in 10-degree steps and verify direction output

4. Temperature compensation:
   - Speed of sound (m/s) = 331.3 + 0.606 * TemperatureC

---

### 5. Mechanical Assembly

1. 3D print low-profile cross arms using PETG or ASA.
2. Mount ultrasonic transducers equally spaced (50–70 mm spacing).
3. Route cables through PG7 glands into enclosure.
4. Mount electronics in a central waterproof pod.
5. Ensure no obstruction in each ultrasonic path.

---

### 6. Integration with Sensor Node

- Output data via I2C or ESP-NOW.
- Log to SD card on ESP32-S3 mothership if needed.
- Integrate calibration parameters into ecoPi web UI.

---

## Summary

This document contains the full bill of materials and a clear step-by-step build guide for constructing a time-of-flight ultrasonic anemometer using ESP32-C3, MT3608 boost converters, MCP6022 op-amps, LM311P comparators, and IRLZ44N MOSFETs. The system includes transmit and receive signal chains, firmware timing logic, calibration workflow, and mechanical requirements.
