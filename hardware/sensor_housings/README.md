# FieldMesh Sensor Housings

This folder contains the current standalone sensor-housing builds. Edit the
Python source files first, then regenerate the STL outputs.

## AS7341 PAR spectral housing

Source:

- `as7341_par/AS7341_PAR_STANDALONE.py`

Generated STL outputs:

- `as7341_par/stl/as7341_par_housing.stl`
- `as7341_par/stl/as7341_par_diffuser_retainer_ring.stl`

Current measured PCB assumptions:

- PCB length: `25.51 mm`
- PCB width: `11.00 mm`
- PCB hole centres: `19.30 mm`
- PCB post height: `15.00 mm`
- Diffuser diameter: `39.50 mm`
- Diffuser pocket diameter: `40.10 mm`
- Diffuser glue ledge width: `3.70 mm`
- Overall housing height: `48.50 mm`
- PCB screw top-access margin: `4.85 mm`
- PAR cable gland port: PG7 clearance, `13.00 mm`
- PAR cable gland centre height: `23.00 mm`
- PAR gland boss: `22.00 mm` OD, about `5.00 mm` effective panel thickness
- PAR gland faces: outside `22.00 mm` OD and inside `28.00 mm` OD, both running through the wall
- PAR internal PG7 flat: `28.00 mm` OD, `1.50 mm` projection into the housing
- PAR PG7 thread length: `7.80 mm`
- PAR internal PG7 locknut envelope: `17.70 mm` AF / `21.64 mm` swing diameter
- Retainer screws/inserts: M3 clearance screws into short M3 heat-set inserts
- Retainer screw PCD: `55.00 mm`
- Retainer lug overlap into body/ring: `4.00 mm`
- Retainer lug edge fillet radius: `0.55 mm`
- Retainer lug root bridges: disabled, using round ears with `11.00 mm` round necks
- Shared deck mount pitch: `64.00 mm`

Regenerate and preview/check:

```powershell
& "C:\Program Files\FreeCAD 1.0\bin\freecadcmd.exe" hardware\sensor_housings\as7341_par\AS7341_PAR_STANDALONE.py
```

The FreeCAD document shows the retainer above the housing for inspection; the
exported STL files are normalized individually to `Z=0` for slicing.

## SHT40 Stevenson shield

Source:

- `sht40_stevenson/modify_thingiverse_stevenson_sht40.py`
- `sht40_stevenson/SHT40_STEVENSON_BLENDER_STL_VIEWER.py`

Generated STL outputs:

- `sht40_stevenson/stl/sht40_stevenson_body_modified.stl`
- `sht40_stevenson/stl/sht40_pg7_retainer_flange.stl`
- `sht40_stevenson/stl/sht40_right_angle_bracket_adapter.stl`

The body starts from the downloaded Thingiverse mini Stevenson shield STL and
is modified in Blender so the mesh-derived design remains repeatable.

Current measured probe/gland assumptions:

- Probe length from flange seat: `52.0 mm`
- Through-flange section diameter: `12.4 mm`
- Thread length through flange: `15.7 mm`
- Under-flange nut width across flats: `17.6 mm`
- Top clamp nut width across flats: `17.2 mm`
- Generated body/collar clearance bore: `21.52 mm`
- Generated flange through-hole: `13.00 mm`

The PG7/probe flange is a plain clamp plate, not a hex nut trap. Tighten the
probe/gland nuts onto the loose flange first. For right-angle mounting, place
the optional bracket adapter between the shield collar and the probe flange,
then use longer screws through the flange and adapter into the shield inserts.
The adapter and shield collar bores provide free clearance for the under-flange
nut.

Preview in Blender without writing STL files:

```powershell
& "C:\Program Files\Blender Foundation\Blender 5.1\blender.exe" --python hardware\sensor_housings\sht40_stevenson\modify_thingiverse_stevenson_sht40.py -- --source-dir "C:\Users\thoma\Downloads\Stevenson Shield (mini) - 4915068\files"
```

Regenerate STL outputs only after the Blender preview looks right:

```powershell
& "C:\Program Files\Blender Foundation\Blender 5.1\blender.exe" --background --python hardware\sensor_housings\sht40_stevenson\modify_thingiverse_stevenson_sht40.py -- --source-dir "C:\Users\thoma\Downloads\Stevenson Shield (mini) - 4915068\files" --export-stl
```

Open the generated STLs in a FreeCAD document:

```powershell
& "C:\Program Files\FreeCAD 1.0\bin\freecadcmd.exe" hardware\sensor_housings\sht40_stevenson\SHT40_STEVENSON_BLENDER_STL_VIEWER.py
```

## Node box sensor deck

Source:

- `node_box_sensor_deck/NODE_BOX_SENSOR_DECK.py`

Generated STL output:

- `node_box_sensor_deck/stl/node_box_sensor_deck.stl`

This deck is the shared adapter for the AS7341 PAR housing and the SHT40 shield.
Both use a 64 mm two-hole M3 mounting interface. The SHT40 station is rotated
onto the Y axis so its right-angle bracket can stay on the X side.
All housing/deck heat-set locations now target short M3/Voron-style inserts
with `4.4 mm` printed bores.

Regenerate and preview/check:

```powershell
& "C:\Program Files\FreeCAD 1.0\bin\freecadcmd.exe" hardware\sensor_housings\node_box_sensor_deck\NODE_BOX_SENSOR_DECK.py
```

The node-box lid spacing is still screenshot-derived. Verify the 110 x 122 mm
box fixing pattern on the physical enclosure before final drilling or printing.
