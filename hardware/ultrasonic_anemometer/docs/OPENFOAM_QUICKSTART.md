# OpenFOAM Quickstart for Ultrasonic Anemometer Housing

This is a practical first workflow to simulate airflow around your current CAD housing.

## Scope (important)

Use this first simulation for **external airflow** (flow quality around pods/head/roof), not ultrasonic wave propagation.

- OpenFOAM is excellent for wind/pressure/turbulence fields.
- 40 kHz acoustic ToF wave simulation is a different class of problem and is usually handled with dedicated acoustics tools.

## 1) Export geometry from FreeCAD

Export one clean, watertight outer shell for CFD:

1. In FreeCAD, create a single solid of the assembled external geometry (head + roof + belly as needed).
2. Remove tiny cosmetic details that do not matter for airflow (very small chamfers, screw text, etc.).
3. Export as STL (binary).
4. Save as: `anemometer_outer.stl`.

Recommended: keep a version specifically for CFD meshing.

## 2) Put STL into OpenFOAM case

Create this folder inside your OpenFOAM case:

- `constant/triSurface/anemometer_outer.stl`

If STL is in mm from CAD, convert to meters before meshing:

```bash
surfaceTransformPoints -scale '(0.001 0.001 0.001)' \
  constant/triSurface/anemometer_outer.stl \
  constant/triSurface/anemometer_outer_m.stl
mv constant/triSurface/anemometer_outer_m.stl constant/triSurface/anemometer_outer.stl
```

## 3) Start from a known external-flow tutorial

Use a proven template (recommended):

- `incompressible/simpleFoam/motorBike`

Copy it and rename:

```bash
cp -r $FOAM_TUTORIALS/incompressible/simpleFoam/motorBike anemometerExternal
cd anemometerExternal
```

Then replace tutorial STL with your STL in `constant/triSurface/`.

## 4) Domain and orientation

Align model so expected wind is along +X (or your preferred axis).

Set domain extents roughly:

- Inlet: 5 body lengths upstream
- Outlet: 10 body lengths downstream
- Sides/top: 5 body widths away
- Ground: either slip (free-air study) or wall (near-ground installation study)

For your first pass, keep it simple: free-air style domain (no ground effects).

## 5) Meshing workflow

Typical steps:

```bash
blockMesh
surfaceFeatureExtract
snappyHexMesh -overwrite
checkMesh
```

Key snappy settings to tune first:

- Near-body refinement around pod/roof region
- Boundary layer (optional in first run; add once base mesh is stable)
- Feature angle/refinement to preserve pod geometry

## 6) Solver setup (first run)

Use steady RANS for first screening:

- Solver: `simpleFoam`
- Turbulence model: `kOmegaSST` (good default for separated external flow)
- Inlet velocity: test 1, 3, 5, 10 m/s

Typical boundaries:

- Inlet: fixed velocity, turbulence values
- Outlet: fixed pressure (0), zeroGradient velocity
- Sides/top: slip or symmetry
- Anemometer surface: noSlip wall

## 7) What to evaluate first

For your design questions, prioritize:

1. Velocity magnitude around pod mouths
2. Recirculation zones near roof/PAR housing
3. Turbulence intensity in ultrasonic measurement region
4. Symmetry of flow around opposite pod pairs

If large asymmetry appears at zero yaw, check geometry orientation and small protrusions.

## 8) Link to your current CAD findings

You already validated acoustic center-ray alignment in CAD.

Use CFD now to answer a different question:

- Does the body shape create local flow distortion that could bias ToF wind estimation?

This is exactly where OpenFOAM adds value.

## 9) Suggested first test matrix

Run 6 cases first:

- Wind speeds: 1, 5, 10 m/s
- Yaw: 0° and 45°

Keep all else fixed; compare flow fields near pods.

## 10) Next step after first runs

Once base runs converge:

- Add mesh independence check (coarse/medium/fine)
- Add mild atmospheric turbulence sensitivity
- Compare current pod shape vs one alternative CAD variant

---

If you want, next I can scaffold a starter OpenFOAM case folder in this repo (with placeholder `system` and `0` files) so you only need to drop in the STL and run.
