# Anemometer External Flow Case (OpenFOAM)

Starter case for external airflow around the ultrasonic anemometer housing.

## Case assumptions

- Solver: `simpleFoam`
- Turbulence model: `kOmegaSST`
- Inlet wind (default): `5 m/s` along +X
- Domain: free-air style box around the model

## Geometry input

STL path expected by this case:

- `constant/triSurface/anemometer_outer.stl`

If your STL is still in mm units, scale after meshing prep:

```bash
surfaceTransformPoints -scale '(0.001 0.001 0.001)' \
  constant/triSurface/anemometer_outer.stl \
  constant/triSurface/anemometer_outer_m.stl
mv constant/triSurface/anemometer_outer_m.stl constant/triSurface/anemometer_outer.stl
```

## Run

```bash
blockMesh
surfaceFeatureExtract
snappyHexMesh -overwrite
checkMesh
simpleFoam
```

## Quick tweaks

- Change wind speed in `0/U` (`inlet` value).
- Change mesh refinement in `system/snappyHexMeshDict`.
- Change domain size in `system/blockMeshDict`.
