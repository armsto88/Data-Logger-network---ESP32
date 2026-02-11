
# NODE – Hardware System Overview

## 1. System Architecture Overview

This system is a modular environmental sensing platform built around an ESP32-WROOM-32D microcontroller. 
It integrates:

- Ultrasonic anemometer (TX + RX chain)
- 22 V boost supply for ultrasonic drive
- 5 V and 3.3 V regulated rails
- ADS1115 precision ADC
- I²C multiplexer (TCA9546A)
- Level shifting between 3.3 V and 5 V domains
- External sensor connectors with switched power
- USB programming interface

The architecture is segmented into:
- High-voltage pulse domain (22 V)
- Digital control domain (3.3 V)
- Analog receive domain (5 V / mid-rail referenced)
- External sensor domain (5 V / switched rails)

This separation minimizes noise coupling and improves measurement integrity.

---

## 2. Power Architecture

### 2.1 Battery Input & Protection
- Reverse polarity protection via P-channel MOSFET
- Resettable PTC fuse
- Battery rail → RAW_BAT

Protects against reverse insertion and short circuits.

### 2.2 System Rail (VSYS)
High-side P-channel MOSFET enables system power control with bulk capacitance for startup stabilization.

### 2.3 5 V Boost Converter
MT3608 boost converter:
- Input: VSYS
- Output: 5 V_SYS

Supplies ADS1115, external sensors, and level shifter VREF2.

### 2.4 3.3 V Regulation
AP2112K-3.3 LDO:
- Input: VSYS
- Output: 3V3_SYS

Supplies ESP32, I²C mux, and digital logic.

### 2.5 22 V Boost Converter (Ultrasonic Drive)
MT3608 configured to ~22 V.

EN pin driven by RC envelope derived from TX_PWM:
- Active only during TX bursts
- Disabled during RX listening window
- Reduces switching noise during receive phase

---

## 3. Ultrasonic Anemometer Section

### 3.1 Principle of Operation
Wind speed and direction calculated using Time-of-Flight (ToF):

1. Transmit ultrasonic burst
2. Receive on opposing transducer
3. Measure propagation time
4. Repeat across axes
5. Compute differential velocity

### 3.2 Transmit Chain
ESP32 TX_PWM → Gate Driver → High-side MOSFET burst switch → 22V TX_PULSE → Directional MOSFET blocks → Transducer

Key design decisions:
- Gate driver isolates MCU
- 22 V improves acoustic amplitude
- Directional switching enables 4-axis measurement
- Zener clamps protect MOSFET gates

### 3.3 Burst Gating Strategy
RC envelope detector holds 22 V enable high during PWM activity.
Boost disabled during RX phase to minimize noise.

### 3.4 Receive Chain
Transducer → Protection network → AC coupling → Op-Amp Stage 1 → Op-Amp Stage 2 → Comparator → ESP32 GPIO (TOF_EDGE)

Features:
- Mid-rail reference (~2.5 V)
- Band limiting near 40 kHz
- Hysteresis on comparator
- Series protection resistors

---

## 4. I²C Infrastructure

### Level Shifter (PCA9306)
Bridges 3.3 V ESP32 domain and 5 V ADC domain.
Configurable pull-ups allow bus tuning.

### I²C Multiplexer (TCA9546A)
- Prevents address conflicts
- Isolates bus capacitance
- Improves modularity

---

## 5. ADS1115 Precision ADC
Powered at 5 V for full input range.
16-bit resolution with configurable PGA.
Used for external sensor measurements.

---

## 6. External Sensor Ports
Each port includes:
- Switched power rail
- Signal routing to ADS1115
- Ground reference

High-side MOSFET switching ensures controlled power delivery.

---

## 7. ESP32 Control Core
Handles:
- Burst generation
- Time-of-flight measurement
- I²C communication
- Sensor polling
- Wireless communication

Includes proper boot strap handling and auto-programming circuit.

---

## 8. Integration Summary
- 22 V active only during TX
- RX section isolated from switching noise
- Analog mid-rail reference
- Modular I²C expansion
- Noise-managed architecture for precision ToF measurement
