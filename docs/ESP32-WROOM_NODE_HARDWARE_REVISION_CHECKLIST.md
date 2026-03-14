# ESP32 Hardware Revision Checklist

**Target board:** ESP32-WROOM-32D + CH340C USB-UART  
**Purpose:** Ensure reliable UART flashing, reset behaviour, and deterministic boot configuration.

---

# 1. Programming / UART Interface

## USB-UART Bridge

Use CH340C (or equivalent) connected to UART0.

```
CH340 TXD → ESP U0RXD (GPIO3)
CH340 RXD → ESP U0TXD (GPIO1)
```

Optional protection resistors:

```
CH340 TX → 330Ω → ESP RX
ESP TX → 330Ω → CH340 RX
```

Benefits:

* reduces contention during development
* protects MCU during external UART debugging

---

# 2. Auto-Reset / Auto-Programming Circuit

Use the standard Espressif DevKit logic using **DTR and RTS**.

Goal: allow `esptool` to automatically toggle:

* **EN (reset)**
* **GPIO0 (BOOT)**

Typical structure:

```
CH340 RTS → transistor → GPIO0
CH340 DTR → transistor → EN
```

Each transistor stage:

```
CH340 signal
   │
 10k
   │
  B
 NPN transistor
  C → target pin (EN or GPIO0)
  E → GND
```

Important design rules:

* EN and GPIO0 control circuits **must remain independent**
* avoid cross-coupled transistor nodes
* ensure logic matches Espressif reference

Reference design:

ESP32-DevKitC schematic

---

# 3. Reset Circuit

EN must have a pull-up and timing capacitor.

```
3.3V
 │
10k
 │
EN
 │
1µF
 │
GND
```

Purpose:

* ensures reset pulse is long enough
* prevents unstable boot behaviour

Manual reset button:

```
EN → button → GND
```

---

# 4. Boot Mode Button

GPIO0 determines download mode.

Normal configuration:

```
3.3V
 │
10k
 │
GPIO0
 │
BOOT button
 │
GND
```

Manual flash sequence:

```
Hold BOOT
Press RESET
Release RESET
Release BOOT
```

---

# 5. ESP32 Strapping Pins (Critical)

The ESP32 samples these pins at reset.

They **must not float**.

| Pin           | Function            | Recommended |
| ------------- | ------------------- | ----------- |
| GPIO0         | Download mode       | 10k pull-up |
| GPIO2         | Boot strap          | pull-up     |
| GPIO5         | Boot strap          | pull-up     |
| GPIO12 (MTDI) | Flash voltage strap | pull-down   |
| GPIO15 (MTDO) | Boot log config     | pull-up     |

Incorrect states can cause:

* boot failures
* flash voltage errors
* random boot modes

---

# 6. Power and Decoupling

Minimum recommended near module:

```
10µF bulk capacitor
100nF decoupling capacitors
```

Place as close as possible to:

```
VDD33 pins
```

Stable power is essential for:

* RF operation
* flash access
* boot stability

---

# 7. Programming Header (Recommended)

Include a debugging header:

```
GND
3V3
TX
RX
EN
BOOT
```

This allows emergency programming with external USB-UART if the onboard bridge fails.

---

# 8. USB-UART Bridge Power

CH340C supply:

```
3.3V_SYS → VCC
100nF decoupling
```

Ensure:

* clean 3.3 V rail
* solid ground reference

---

# 9. Layout Recommendations

### UART

Keep traces short between:

```
CH340 ↔ ESP32 UART0
```

### Reset / Boot

Route EN and GPIO0 carefully.

Avoid:

* long traces
* noisy switching signals nearby

### Power

Place decoupling capacitors close to the ESP module.

---

# 10. Lessons Learned From Previous Board

From debugging the ESP32-C3 board:

* Floating strap pins cause **random boot modes**
* Incorrect auto-reset circuits prevent flashing
* Missing reset capacitor causes unreliable resets
* Native USB can interfere with UART flashing if misconfigured

For the ESP32-WROOM board:

* maintain deterministic strap states
* use the official DevKit auto-program circuit
* keep the reset network stable

---

# 11. Pre-Fabrication Sanity Checks

Before sending PCB to manufacturing:

✔ GPIO0 pull-up present  
✔ EN pull-up and capacitor present  
✔ UART0 routed correctly  
✔ No strap pins floating  
✔ Auto-reset transistors wired correctly  
✔ Power decoupling placed near module

---

# Summary

If the following are correct, flashing should always work:

```
CH340 TX ↔ ESP RX
CH340 RX ↔ ESP TX
GPIO0 pull-up + button
EN pull-up + capacitor
Correct RTS/DTR auto-reset circuit
Stable strap pin states
```

This configuration matches Espressif's official reference designs and ensures reliable programming and boot behaviour.
