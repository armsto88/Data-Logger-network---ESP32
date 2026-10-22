# Node V4 engineering review record

**Consolidated:** 2026-10-22  
**Controlling manufacturing package:**
[2026-10-22 V4 prototype](../release/2026-10-22-v4-prototype/)

This document replaces the verbose intermediate schematic-review transcripts,
closure review, independent-review prompt and pre-production PCB review. Those
files described different revisions and included findings that were later
corrected, accepted as controlled risks, or superseded. The release README and
the dispositions below are authoritative for the prototype order.

## Final disposition

| Area | Prototype disposition |
|---|---|
| Native connectivity | Closed. The EasyEDA netlist resolved the formerly ambiguous battery, solar and CN15/CN16 power connections. |
| DRC | The final layout lineage was reported clean. The last BOM-only resistor-array substitution did not change PCB geometry. |
| Release consistency | BOM and CPL contain the same 325 designators. Copper, masks, paste, outline, drills, component positions, pad geometry and exported net assignments were checked against the preceding validated export. |
| SDI-12 | R178 and R179 are 510 ohm, 1%, 0402 parts (`0402WGF5100TCE`, C25123). The erroneous 510 kohm selection was corrected. |
| Battery-path MOSFET | Q5 is AO3401A (C15127), matching Q2 and providing specified low-voltage gate performance. |
| Reverse battery with live solar | Accepted controlled risk, not an electrical fix. Battery polarity is controlled by the approved keyed harness and incoming inspection; cover or disconnect the panel before battery service. |
| I2C pull-up arrays | RN1, RN2, RN3, RN10 and RN13 are 5.6 kohm `2D02WGJ0562TCE` arrays (C425324). |
| 12 V rail | Maximum combined `12V_SEN` load is 150 mA. Do not enable the RS-485 and SDI-12 sensor supplies simultaneously; include sensor inrush in the limit. |
| Battery | Protected 1S2P Li-ion pack, 3.6/3.7 V nominal, 4.2 V charge, approximately 5200 mAh, at least 600 mA permitted charge and at least 1.5 A continuous discharge. |
| Solar panel | Nominal 6 V, 1–3 W, approximately 6 V at maximum power, and no more than approximately 8 V cold open-circuit voltage. |
| Harnesses | External connector wiring requires a controlled mating-face drawing and continuity/short testing. The removed generic aviation-plug note is not a production specification. |

## Accepted operating constraints

1. Verify `CN1.1 = BAT+` and `CN1.2 = BAT-` on every production battery batch.
2. Never connect, disconnect or reverse the battery while the solar panel is
   energising the charger.
3. Keep all simultaneous 12 V sensor loads at or below 150 mA total.
4. Run the main and downstream I2C buses at 100 kHz when external cable length
   or bus capacitance makes 400 kHz rise-time compliance uncertain.
5. Match every M8 or other field harness to the PCB connector pinout; do not
   infer the cable wiring from connector pin numbers alone.

## Prototype validation still required

- Inspect JLCPCB's placement preview for Q2/Q5, all diodes and TVS parts,
  LEDs, D18/D19, U13 and polarized capacitors.
- Current-limited first power-up; measure off-state current and inrush.
- Measure 3.3 V, 5 V and 12 V regulation and ripple at battery extremes, during
  Wi-Fi transmission and with the 12 V rail loaded to 150 mA.
- Verify CN3791 charge current, CV/termination, hot/cold inhibit, final panel
  hot `Vmp`, cold `Voc` and enclosure temperature rise.
- Calibrate all four analogue channels and confirm the divider ratio over the
  intended 0–12 V input range.
- Exercise every I2C mux channel, both reed inputs, RS-485/Modbus and SDI-12
  with representative field cables and sensors.
- Verify RF performance in the intended enclosure, especially near mounting
  hardware, the ESP32 antenna keepout and cable glands.
- Perform thermal and ESD testing on the assembled prototype before a larger
  production run.

Firmware constants, GPIO behaviour and validation work are tracked separately
in [NODE_V4_FIRMWARE_UPDATES.md](../../../node/docs/NODE_V4_FIRMWARE_UPDATES.md).

## Consolidated source manifest

The following superseded source documents were removed from the active tree
during consolidation. Their SHA-256 hashes preserve provenance if a recoverable
copy or earlier workspace snapshot must be identified.

| Source | SHA-256 |
|---|---|
| `NODE_V4_CLAUDE_PRELAYOUT_REVIEW_2026-10-16.md` | `d35ff8193272dc432cc5ae4224c50eea4710519e84d18671157f0a37a8e1fd6d` |
| `NODE_V4_INDEPENDENT_SCHEMATIC_REVIEW_2026-10-16.md` | `bc78959cec02b39e0217720cb70ebd026e38cf59c4f24c3b2a505fb5b41029b8` |
| `NODE_V4_TARGETED_REVIEW_2026-10-17.md` | `bcd063aa85f28b7c03bdaac323e14835b8cd2beaba7fb8231df33b6ed90b53bf` |
| `NODE_V4_INDEPENDENT_PCB_REVIEW_PROMPT_2026-10-22.md` | `7f199cb2278653a4be2d6d0c26b989d5279e559f92bae5789e2d2d4ddd81f7fa` |
| `NODE_V4_INDEPENDENT_PCB_REVIEW_2026-10-22.md` | `d130b35063ce49c804daa75e843dc4b4adb4066892d23e13dc860274e4ceeb5d` |
