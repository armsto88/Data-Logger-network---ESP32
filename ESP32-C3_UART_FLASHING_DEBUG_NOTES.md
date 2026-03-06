# ESP32-C3 UART Flashing Debug Notes

## Board Overview

Custom PCB using:

* **ESP32-C3**
* **CH340C USB-to-UART bridge**
* UART flashing intended through **CH340 → RX/TX**
* Native ESP USB (GPIO18 / GPIO19) routed only to a **header**, not a USB connector
* BOOT button connected to **GPIO9**
* RESET button connected to **EN**

Initial goal:
Flash firmware through **USB → CH340 → UART → ESP32-C3**.

---

# Initial Problem

Flashing via PlatformIO / `esptool` failed with:

```
Wrong boot mode detected (0x0)
The chip needs to be in download mode
```

---

# Diagnostic Steps

## 1. Verified USB-UART Interface

Windows detected the CH340 correctly:

```
USB-SERIAL CH340 (COM3)
```

UART console communication worked.

---

## 2. Observed Boot Messages

Serial monitor output during reset:

```
invalid header: 0xffffffff
```

Meaning:

* ESP32-C3 powered correctly
* UART TX from ESP → PC working
* Chip attempting to boot from flash but flash is blank/invalid

---

## 3. BOOT + RESET Behaviour

When pressing BOOT then RESET:

```
ESP-ROM:esp32c3-api1-20210207
rst:0x1 (POWERON),boot:0x0 (USB_BOOT)
wait usb download
```

Interpretation:

* Chip enters **USB download mode**
* ROM waits for flashing via **native USB interface**
* `esptool` on COM3 (UART) cannot connect

---

# Auto-Reset Circuit Investigation

Schematic contains an RTS/DTR transistor network controlling:

* **BOOT (GPIO9)**
* **EN (RESET)**

Concern that this circuit might interfere with manual flashing.

Temporary change made:

```
Removed R36
Removed R37
```

This disables RTS/DTR influence so only the push buttons control boot state.

---

# Result After Removing RTS/DTR

`esptool` output changed to:

```
Invalid head of packet (0x20)
Possible serial noise or corruption
```

This indicates:

* ESP is responding on UART
* but not fully synchronising with esptool

---

# Boot Strap Investigation

ESP32-C3 strap pins affecting boot mode:

| Pin   | Function           |
| ----- | ------------------ |
| GPIO9 | BOOT strap         |
| GPIO8 | boot configuration |
| GPIO2 | boot configuration |

Observed:

```
GPIO8 – not connected
GPIO2 – not connected
```

Therefore both pins are **floating**.

Floating strap pins can cause inconsistent boot mode selection, including:

* USB_BOOT
* Normal flash boot
* Failed UART download entry

---

# Current Behaviour Summary

System behaviour observed:

| Condition       | Result                          |
| --------------- | ------------------------------- |
| Normal reset    | `invalid header 0xffffffff`     |
| BOOT + RESET    | `USB_BOOT wait usb download`    |
| esptool attempt | `Invalid head of packet (0x20)` |
| RTS/DTR removed | behaviour unchanged             |

Interpretation:

* ESP32-C3 is **alive**
* UART TX works
* CH340 interface works
* Boot mode selection is **inconsistent**

---

# Likely Root Cause

**Floating strap pins (GPIO8 and GPIO2)**.

Because they are not pulled to a defined level, the ROM boot logic may randomly select:

* USB download mode
* Flash boot
* incorrect download state

Correct behaviour requires deterministic strap levels.

---

# Recommended Hardware Fix (Next PCB Revision)

Add pull-ups:

```
GPIO8 → 10k → 3.3V
GPIO2 → 10k → 3.3V
```

Existing connections:

```
GPIO9 → 10k → 3.3V
BOOT button → GND
EN → 10k → 3.3V
RESET button → GND
```

This produces a stable boot configuration.

---

# Recommended Auto-Reset Design

Use the standard Espressif circuit:

```
RTS → transistor → EN
DTR → transistor → GPIO9
```

Avoid cross-coupled transistor networks.

---

# Conclusion

Hardware appears functional:

* ESP32-C3 operational
* CH340 UART operational
* Boot control partially functional

Main issue is **undefined strap pin states**, preventing reliable entry into UART download mode.

Next board revision should include proper strap pull-ups to ensure deterministic boot behaviour.
