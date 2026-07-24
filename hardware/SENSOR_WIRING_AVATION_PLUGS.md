# Sensor Wiring — Aviation Plug Pinout

Reference pin assignments for sensors wired through aviation plugs.

## I²C Sensors (AS7341, SHT41, etc.)

All I²C sensors use the same 4-pin aviation plug mapping:

| Pin | Signal |
|-----|--------|
| 1   | GND    |
| 2   | SDA    |
| 3   | SCL    |
| 4   | PWR    |

## Wind Sensor (Ultrasonic Anemometer)

2-pin aviation plug:

| Pin | Signal   | Wire colour |
|-----|----------|-------------|
| 1   | GND      | Black       |
| 4   | SIGNAL   | Red         |

## Soil Moisture Sensor

4-pin aviation plug:

| Pin | Signal   | Wire colour |
|-----|----------|-------------|
| 1   | GND      | Black       |
| 4   | PWR      | Brown       |
| 3   | Moisture | Yellow      |
| 2   | Temp     | Blue        |