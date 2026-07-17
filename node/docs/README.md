# Node Documentation

**Last reviewed:** 2026-07-17

Design notes, hardware checklists, sensor implementation records, and bring-up results for the ESP32-WROOM sensor nodes.

## Hardware and bring-up

- [NODE_HARDWARE_V2_CHECKLIST.md](NODE_HARDWARE_V2_CHECKLIST.md) - V2 hardware validation checklist.
- [NODE-PCB-OVERVIEW.md](NODE-PCB-OVERVIEW.md) - PCB architecture and pin mapping.
- [Node V2 bring-up results](../firmware/tests/NODE_V2_BRINGUP_RESULTS.md) - canonical V2 test record; the former one-line pointer in this folder was removed on 2026-07-17.
- [Node V3 bring-up results](../firmware/tests/NODE_V3_BRINGUP_RESULTS.md) - canonical V3 test record.
- [NODE_V3_OVERVIEW.md](NODE_V3_OVERVIEW.md) - complete V3 board overview.
- [NODE_V3_22V_BOOST_UPDATE.md](NODE_V3_22V_BOOST_UPDATE.md) - V3 22 V boost changes.
- [NODE_V3_ULTRASONIC_CHANGES_BRINGUP_FIRMWARE_GUIDE_2026-06-12.md](NODE_V3_ULTRASONIC_CHANGES_BRINGUP_FIRMWARE_GUIDE_2026-06-12.md) - V3 design changes and production firmware guidance.
- [NODE_V4_ULTRASONIC_REDESIGN_NOTES.md](NODE_V4_ULTRASONIC_REDESIGN_NOTES.md) - V4 shared bidirectional transducer redesign.

## Storage and robustness

- [NODE-LOCAL-STORAGE-CONTRACT-V1.md](NODE-LOCAL-STORAGE-CONTRACT-V1.md) - queue invariants, recovery, capacity, and tests.
- [NODE_ROBUSTNESS_FIX_PROMPTS.md](NODE_ROBUSTNESS_FIX_PROMPTS.md) - historical task-level robustness instructions retained for traceability.

## Sensors

- [SENSOR_AND_CSV_BRINGUP.md](SENSOR_AND_CSV_BRINGUP.md) - sensor initialisation and CSV bring-up.
- [CWT_TH-A_SOIL_SENSOR_IMPLEMENTATION_NOTES.md](CWT_TH-A_SOIL_SENSOR_IMPLEMENTATION_NOTES.md) - soil sensor driver implementation.
- [shared sensor-package checklist](../../docs/ADDING_A_NEW_SENSOR_CHECKLIST.md) - required end-to-end integration checks.

## Ultrasonic anemometer

- [ultrasonic_anemometer_function_note.md](ultrasonic_anemometer_function_note.md) - hardware/firmware function specification.
- [ULTRASONIC_ANEMOMETER_WORK_PLAN.md](ULTRASONIC_ANEMOMETER_WORK_PLAN.md) - development and validation plan.
- [ultrasonic_circuit_v2_design_advice.md](ultrasonic_circuit_v2_design_advice.md) - V2 circuit recommendations.
- [ULTRASONIC_NOISE_ANALYSIS_2026-04-11.md](ULTRASONIC_NOISE_ANALYSIS_2026-04-11.md) - dated signal/noise findings.
- [NODE_V3_ULTRASONIC_CHECKLIST.md](NODE_V3_ULTRASONIC_CHECKLIST.md) - V3 electrical and acoustic validation.
