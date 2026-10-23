# Hardware index

PCB source, manufacturing releases, enclosure models and simulation assets live
under this directory. Use the README inside each board directory to identify the
controlling release; do not mix BOM, placement or Gerber files from different
revisions.

## Boards

- [Node V4](NODE_v4/README.md) — current sensor-node prototype and its verified
  manufacturing package.
- [FieldHub V2](FieldHub_v2/README.md) — hub fabrication history; the latest
  listed release is the printed prototype baseline.
- [Node V3](NODE_v3/README.md) — field-proven historical node baseline.

## Mechanical and sensor hardware

- [Sensor housings](sensor_housings/README.md)
- [Ultrasonic anemometer](ultrasonic_anemometer/README.md)

## Connector documentation rule

Connector pin numbers alone are not a safe cable specification: mating-face
and solder-face views can mirror each other, and board connector numbering need
not match an external connector. Every production harness should have a
controlled drawing containing:

- PCB reference and pin number;
- signal name and wire colour;
- every intermediate connector and its viewing direction;
- external connector pin number;
- continuity and adjacent-pin short-test results.

The former generic aviation-plug pinout was removed because later FieldHub
reviews showed that a same-number-to-same-number harness could interchange
power and ground. It must not be used as a production wiring specification.
