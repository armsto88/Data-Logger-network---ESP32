# Node V4 prototype manufacturing release

Release date: 2026-10-22

This is the package intended for the first Node V4 prototype PCB/PCBA order.

## Manufacturing files

- `Node_V4_2026-10-22_v4_Gerbers.zip` — four-layer PCB fabrication package.
- `Node_V4_2026-10-22_v4_BOM.csv` — assembly bill of materials.
- `Node_V4_2026-10-22_v4_CPL.csv` — component placement/pick-and-place file.
- `Node_V4_2026-10-22_v4_EasyEDA.epro2` — editable EasyEDA Pro source project.

Upload the Gerber ZIP, BOM and CPL from this directory together. Do not mix
them with files from an earlier Node V4 export.

## Validation recorded before release

- The BOM and CPL each contain the same 325 unique designators, with no
  missing or extra references.
- R178 and R179 are 510 ohm, 1%, 0402 parts (`0402WGF5100TCE`, LCSC C25123).
- Q5 is `AO3401A`, LCSC C15127, matching Q2 for component standardisation.
- The five dual I2C pull-up arrays RN1, RN2, RN3, RN10 and RN13 are 5.6 kohm
  `2D02WGJ0562TCE` parts, LCSC C425324. They retain the existing 0402x2,
  four-pad footprint and placement.
- Q5 pad/net mapping in the Gerber flying-probe data is:
  - pin 1 / gate: `$2N32215`
  - pin 2 / source: `RAW_BAT`
  - pin 3 / drain: `$2N32221`
- Compared with the immediately preceding export, SW1 moved 0.254 mm in Y and
  its local copper pour/routing was regenerated. One `INT_RTC` via and one GND
  via were repositioned with their net assignments unchanged.
- All exported component-pin/net assignments in the Gerber flying-probe data
  match the preceding export.
- The only engineering-field BOM changes from the preceding export are the
  five resistor arrays above: 4.7 kohm/C474299 was replaced by
  5.6 kohm/C425324.
- The CPL contains no footprint, coordinate, layer or rotation changes. All
  manufacturing geometry in the copper, solder-mask, paste, silkscreen,
  outline and drill outputs is identical to the preceding export.
- Both the EasyEDA project archive and Gerber archive pass ZIP integrity
  checks.
- The Gerber package contains top, bottom and two inner copper layers, solder
  mask, paste, outline and plated/non-plated drill files.

The preceding layout revision was reported DRC-clean. This replacement only
changes resistor-array ordering metadata and values; it does not change PCB
geometry. A fresh DRC log was not included with this package, so run EasyEDA
DRC on this exact project before ordering if that final check has not already
been completed.

## Accepted design constraint

The battery connector polarity must be checked before installation. The
existing circuit is not guaranteed to block a reversed battery while solar
power is present. This fault case was explicitly accepted for the prototype;
do not connect or reverse a battery while the solar input is energised.

## Assembly-order checks

Before submitting the PCBA order, inspect the manufacturer's placement preview
for Q5, Q2, all diodes/TVS parts, LEDs, D18/D19 and the USB-C connector. Also
confirm that the BOM maps Q2 and Q5 to C15127 and R178/R179 to C25123.
Confirm RN1, RN2, RN3, RN10 and RN13 map to C425324 in the parts-selection
preview.

