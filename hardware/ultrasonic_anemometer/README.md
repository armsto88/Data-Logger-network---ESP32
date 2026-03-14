# Ultrasonic Anemometer Workspace

This folder contains all assets related to the ultrasonic anemometer program.

## Structure

- `docs/` - design and analysis documentation
- `firmware/` - ultrasonic node and mothership firmware prototypes
- `FreeCAD/` - CAD models and FreeCAD macros
- `hardware/` - PCB project files, fabrication outputs, and archived revisions
- `mechanical/` - printable geometry (`scad`, `stl`)
- `sim/` - OpenFOAM simulation cases and matrix runs

## Notes

- Active PCB project lives in `hardware/easyeda/` with `hardware/UltraSonic_Anemometer_HW.eprj`.
- Fabrication outputs are grouped under `hardware/fabrication/`.
- Old design snapshots are grouped under `hardware/archive/`.
