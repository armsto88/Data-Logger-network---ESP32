# A7670G Modem Bring-up Notes

## Source

Document reviewed:

**A7670 Series Hardware Design V1.00**  
SIMCom Wireless Solutions Limited  
Released: 2020-06-09

Project modem:

```text
SIMCom A7670G LTE modem module
```

---

# 1. Power Supply / VBAT

## VBAT operating range

```text
VBAT min: 3.4 V
VBAT typ: 3.8 V
VBAT max: 4.2 V
Peak current target: up to ~2.8 A
Maximum preferred VBAT droop: <300 mV
```

The modem rail must be treated as a **high-current burst rail**, not a normal low-current logic rail.

Current mothership topology:

```text
VSYS
→ TPS63020 buck-boost regulator
→ ~4 V modem rail
→ M_VBAT
→ A7670G VBAT pins
```

## Local capacitance

Mothership update:

```text
470 µF bulk capacitance added on modem VBAT
```

This is a good improvement and exceeds the 300 µF minimum target discussed during review.

Keep additional local caps close to VBAT:

```text
10 µF
1 µF
100 nF
small RF bypass caps if following the reference design exactly
```

## Routing notes

- Keep the 4 V rail wide and low impedance.
- Use multiple vias when changing layers.
- Place VBAT bulk and ceramic caps close to the modem VBAT pins.
- Make the ground return as strong as the 4 V feed.
- Avoid skinny neck-downs in the modem power path.
- Keep VBAT burst-current paths away from SIM, RF, USB, SD clock, and ADC traces.

---

# 2. VBAT TVS / Protection

## Correct topology

The TVS must be used as a **shunt clamp**, not in series.

Correct:

```text
M_VBAT / 4V rail
       |
       └── TVS diode
             |
            GND
```

Wrong:

```text
4V rail → TVS/diode in series → modem VBAT
```

## Orientation for unidirectional TVS

For a unidirectional TVS such as SMAJ5.0A:

```text
TVS cathode / banded end → M_VBAT / 4V rail
TVS anode / unbanded end → GND
```

Mothership update:

```text
The previous series TVS issue has been corrected.
The TVS is now a shunt device to GND.
```

---

# 3. PWRKEY Control

## Modem behaviour

The A7670G PWRKEY pin is active low. The modem internally pulls PWRKEY high. The host should pull it low to simulate pressing the power key.

## Mothership circuit

```text
ESP32 GPIO14 / M_PWRK
→ 100 Ω gate resistor
→ 2N7002 gate

2N7002 source → GND
2N7002 drain  → A7670G PWRKEY
2N7002 gate pulldown → 100 kΩ to GND
```

This is correct as an open-drain style pull-down.

## Firmware logic

```text
M_PWRK LOW  = PWRKEY released
M_PWRK HIGH = PWRKEY pulled low / key pressed
```

## Power-on sequence

```text
1. Set 4V_EN HIGH to enable modem regulator.
2. Wait for ESP_PG / 4 V rail stable.
3. Drive M_PWRK HIGH for about 50–100 ms.
4. Drive M_PWRK LOW to release PWRKEY.
5. Wait for modem STATUS or UART readiness.
6. Allow up to ~12 s for full UART/module readiness.
```

## Power-off sequence

Do not normally hard-cut modem VBAT.

```text
1. Send AT+CPOF
   or drive M_PWRK HIGH for at least 2.5 s.
2. Wait for STATUS to indicate modem off.
3. Add a short buffer before restart if needed.
4. Disable 4V_EN only after graceful shutdown.
```

## Hardware warning

```text
PWRKEY capacitance should remain below 10 nF.
```

Do not add large capacitance to PWRKEY.

---

# 4. RESET Pin

RESET should not be used for normal power cycling.

Recommended treatment:

```text
RESET → test pad or emergency control only
```

Normal operation should use:

```text
PWRKEY
AT+CPOF
4V_EN after graceful shutdown
```

---

# 5. UART Interface and ESP32 Level Shifting

## Voltage domain

The A7670G digital IO is 1.8 V logic. The ESP32-WROOM is 3.3 V logic.

Therefore, the UART must be level shifted.

Do not connect ESP32 UART directly to A7670G UART pins.

## Confirmed mothership level-shifter mapping

```text
U60:
B side = ESP32_TX2 / GPIO17 / 3.3 V
A side = modem_RXD / 1.8 V
DIR = LOW
Direction = B → A

U61:
A side = modem_TXD / 1.8 V
B side = ESP32_RX2 / GPIO16 / 3.3 V
DIR = HIGH
Direction = A → B

U62:
A side = modem_STATUS / 1.8 V
B side = ESP32_M_STS / GPIO4 / 3.3 V
DIR = HIGH
Direction = A → B
```

This is correct.

## UART speed

```text
Default / initial bring-up baud: 115200
```

## Basic AT test set

```text
AT
ATE0
ATI
CPIN?
CSQ
CREG?
CEREG?
CGATT?
CGDCONT?
```

---

# 6. VDD_1V8 / M_1V8

The modem provides a 1.8 V output rail.

Use this as:

```text
M_1V8 / VDD_1V8 → level shifter VCCA reference
```

Do not use M_1V8 as a general-purpose power rail.

Rules:

```text
Use only for light logic/reference loads.
Do not heavily load it.
Do not pull 1.8 V modem IO to 3.3 V.
```

---

# 7. STATUS and NETLIGHT

## STATUS

STATUS indicates whether the modem is powered on.

Mothership use:

```text
A7670G STATUS
→ level shifter
→ ESP32 GPIO4 / M_STS
```

Firmware should use this to confirm modem state during power-on and power-off.

## NETLIGHT

NETLIGHT indicates network state/activity.

Possible uses:

```text
debug LED
test pad
optional ESP32 input if level shifted
```

If connected to ESP32, treat it as a 1.8 V modem-domain signal unless shifted.

---

# 8. SIM Interface

## SIM voltage

The A7670G supports:

```text
1.8 V SIM
3.0 V SIM
```

USIM_VDD is generated by the modem.

## Recommended topology

```text
A7670G SIM pins
→ 22 Ω series resistors close to modem
→ SIM traces
→ ESDA6V1W5 close to SIM socket
→ SIM card holder
```

## SIM nets

```text
USIM_VDD
USIM_CLK
USIM_RST
USIM_DATA
GND
```

## Mothership layout rules

- Place 22 Ω series resistors close to modem SIM pins.
- Place ESDA6V1W5 close to SIM socket.
- Keep ESDA ground path short and direct.
- Add 100 nF from USIM_VDD to GND near SIM socket.
- Keep SIM traces grouped.
- Keep SIM_CLK especially clean.
- Avoid routing SIM near RF, 4 V regulator, inductor, modem VBAT current path, USB, SD_SCK, and ADC traces.
- Longest SIM line around 45–53 mm is acceptable if cleanly routed.

---

# 9. Modem USB Debug Interface

## Purpose

The A7670G USB interface is useful for:

```text
firmware recovery
modem diagnostics
driver-level debugging
software upgrade
```

It is separate from the ESP32/CH340 programming USB.

## Mothership addition

A 4-pin modem USB debug header has been added.

Recommended pin set:

```text
MODEM_USB_VBUS
MODEM_USB_DM
MODEM_USB_DP
GND
```

## Important note

MODEM_USB_VBUS does not replace the modem VBAT supply.

The modem still needs:

```text
4 V modem rail enabled
PWRKEY power-on sequence completed
```

Then the USB header can be connected to a PC.

## Suggested debug sequence

```text
1. Power board normally.
2. Enable 4V_EN.
3. Press modem PWRKEY via firmware.
4. Confirm STATUS high.
5. Connect modem USB VBUS / DM / DP / GND to PC.
6. Check if the modem enumerates.
```

---

# 10. USB_BOOT

USB_BOOT is for forced download / recovery mode.

Rule:

```text
Do not pull USB_BOOT up during normal boot.
```

Recommended hardware treatment:

```text
USB_BOOT → test pad only
```

If needed for recovery, it can be manually pulled to M_1V8 according to the modem recovery procedure.

---

# 11. RF / Antenna Interface

## RF impedance

The antenna path must be treated as a 50 Ω RF route.

Design requirement:

```text
A7670G RF pin
→ 50 Ω controlled impedance trace
→ matching network
→ antenna connector
```

## Matching network

Reserve matching components as per the reference design:

```text
series 0 Ω / matching component
shunt capacitor footprints
optional RF TVS if appropriate
```

Final values may need tuning.

## Layout rules

- Keep RF trace short.
- Maintain continuous ground reference.
- Use ground stitching along RF route.
- Avoid sharp bends and stubs.
- Keep away from TPS63020, inductor, modem VBAT rail, SD, USB, SIM, and ESP32 antenna.
- Do not run RF parallel to noisy digital/power traces.
- If routing on the back layer, ensure the stack-up supports a 50 Ω trace with ground reference.

---

# 12. ESD Strategy

ESD protection belongs near user-accessible connectors.

Mothership placement rules:

```text
SIM ESD → close to SIM holder
USB ESD → close to USB connector/header/test pads
VBAT TVS → close to modem VBAT entry/caps
```

Keep ESD ground paths:

```text
short
wide
low inductance
direct to ground plane
```

---

# 13. Firmware Bring-up Checklist

## Initial hardware sequence

```text
1. Boot ESP32.
2. Assert PWR_HOLD early.
3. Keep modem off by default.
4. Enable 4V_EN.
5. Wait for ESP_PG.
6. Drive M_PWRK HIGH for 50–100 ms.
7. Drive M_PWRK LOW.
8. Wait for M_STS / STATUS.
9. Open UART2 at 115200 baud.
10. Send AT until OK.
```

## Basic AT command test set

```text
AT
ATE0
ATI
CPIN?
CSQ
CREG?
CEREG?
CGATT?
CGDCONT?
```

## Shutdown sequence

```text
AT+CPOF
or M_PWRK HIGH for ≥2.5 s
wait for STATUS low
then disable 4V_EN
```

## Do not do this as normal operation

```text
Disable 4V_EN without graceful shutdown.
```

Hard-cutting VBAT should only be a last-resort recovery action.

---

# 14. Current Mothership Design Status

Confirmed / updated:

```text
470 µF bulk capacitance added on modem VBAT.
VBAT TVS corrected from series to shunt.
PWRKEY circuit uses 2N7002 open-drain pull-down.
UART and STATUS level shifting confirmed correct.
SIM ESD should be close to SIM socket.
SIM resistors should be close to modem.
4-pin modem USB debug header added.
USB_BOOT should remain test-pad/recovery only.
```

Remaining important PCB review item:

```text
Review the RF antenna route and matching network next.
```