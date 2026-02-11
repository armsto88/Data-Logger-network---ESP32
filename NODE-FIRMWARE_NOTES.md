
# NODE – Firmware Architecture Notes

## 1. WIND Measurement Cycle

1. Enable TX mode
2. Allow 22 V boost warm-up (5–20 ms)
3. Emit ultrasonic burst
4. Disable burst
5. Enter RX listening window
6. Capture comparator edge
7. Repeat for opposite direction

---

## 2. 22 V Enable Logic
22 V boost enable derived from TX_PWM envelope.
Ensure warm-up window before measurement.
Avoid sampling during boost ramp.

---

## 3. TOF Measurement
Use hardware timer capture on comparator GPIO.
Disable interrupts during capture window.
Capture first valid edge only.

---

## 4. RX Blanking
Ignore comparator edges for 200–800 µs after TX to avoid self-ring artifacts.

---

## 5. Burst Parameters
Carrier: ~40 kHz  
Burst duration: 0.5–5 ms  
Repetition rate tuned to acoustic decay

---

## 6. Direction Control
Only one directional driver active at a time.
Add short dead-time between switching.

---

## 7. ADS1115 Operation
Use single-shot mode.
Allow conversion time before read.
Select appropriate PGA range.

---

## 8. I²C Multiplexer
Select channel before sensor read.
Allow short settling time.

---

## 9. Noise Management
Avoid WiFi transmission during TOF capture.
Suspend non-critical tasks during measurement.

---

## 10. Calibration
Wind speed derived from differential ToF:

Wind Speed ≈ (d / 2) × (Δt / t²)

Where d = transducer spacing and t = average propagation time.

Apply temperature compensation if available.
