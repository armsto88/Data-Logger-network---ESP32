# OpenFOAM Matrix Cases

**Consolidated:** 2026-07-17

Prebuilt case matrix cloned from `anemometerExternal`:

- `anemometer_u1_yaw0`
- `anemometer_u5_yaw0`
- `anemometer_u10_yaw0`
- `anemometer_u1_yaw45`
- `anemometer_u5_yaw45`
- `anemometer_u10_yaw45`

## Velocity convention

- `yaw0`: inlet along +X
- `yaw45`: inlet split equally in +X and +Y

## Run one case

From the case folder, run:

```bash
blockMesh
surfaceFeatureExtract
snappyHexMesh -overwrite
checkMesh
simpleFoam
```

## Run all cases (PowerShell)

Use `run_all_cases.ps1` from this folder.

## Shared case instructions

The six case-level `README.md` files were byte-identical copies and were removed
on 2026-07-17. Geometry assumptions, meshing commands, and quick-tweak guidance
remain in the canonical [anemometerExternal README](../anemometerExternal/README.md).
