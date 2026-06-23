---
description: "Convert enclosure measurements into a conservative PCB outline for EasyEDA: board dimensions, mounting-hole assumptions, keepout margins, and origin-based coordinate table."
argument-hint: "Describe the enclosure measurements and the mechanical constraint"
---

# EasyEDA PCB Outline Planning

You are converting enclosure measurements into a PCB outline suitable for EasyEDA.

## Procedure
1. Collect the actual internal enclosure dimensions provided — not storefront dimensions.
2. Identify the controlling mechanical constraint:
   - battery footprint (if battery sits under PCB)
   - board stack height
   - cable entry point
   - mounting hardware and boss positions
3. Treat unknown internal offsets, wall thicknesses, and boss positions as **variables** — do not invent numbers.
4. Prefer a conservative rectangular outline before introducing chamfers or cutouts.
5. Apply standard keepout margins:
   - 1–2 mm from enclosure wall to board edge (conservative first pass)
   - 3.2 mm diameter holes for M3 mounting (adjust if M2.5 or M2)
   - 5 mm keepout radius around mounting holes (no traces or components)
6. Output coordinates origin-based (bottom-left = 0,0) in millimeters.

## Output Shape
- **Mechanical interpretation**: what the controlling constraint is and why
- **Recommended board size**: W × H in mm with reasoning
- **Alternate board size**: conservative fallback if measurements are uncertain
- **Origin-based coordinate outline**: corner coordinates in mm (copy-paste ready for EasyEDA)
- **Mounting hole positions**: X, Y coordinates and diameter
- **Remaining physical measurements to verify**: what must be measured before committing to fab

$ARGUMENTS
