---
name: easyeda-outline
description: 'Use for EasyEDA PCB outline planning from enclosure measurements, battery-under-PCB constraints, mounting-hole assumptions, and conservative mechanical prompts or coordinate tables.'
argument-hint: 'Describe the enclosure measurements and the mechanical constraint'
---

# EasyEDA Outline Planning

## When to Use
- Converting enclosure measurements into a PCB outline prompt
- Planning conservative first-pass board dimensions
- Documenting mounting-hole assumptions and keepout margins
- Generating coordinate-style outputs for EasyEDA board edges

## Procedure
1. Collect the actual internal enclosure dimensions, not just storefront dimensions.
2. Identify the controlling mechanical constraint such as battery footprint, board stack height, cable entry, or mounting hardware.
3. Treat unknown internal offsets, wall thicknesses, and boss positions as variables rather than inventing them.
4. Prefer a conservative rectangular outline before introducing more complex geometry.
5. Output the result in a form that can be used directly in EasyEDA or in a prompt to another model.

## Expected Output Shape
- mechanical interpretation
- recommended board size
- alternate board size
- origin-based coordinate outline
- remaining physical measurements to verify
