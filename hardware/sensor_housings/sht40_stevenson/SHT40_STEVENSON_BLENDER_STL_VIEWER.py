from pathlib import Path

import FreeCAD as App
import Mesh

# ============================================================
# SHT40 Thingiverse/Blender STL Viewer
#
# Loads the recently generated Blender-modified Stevenson shield STLs into a
# FreeCAD document. The editable source for these meshes is:
#
#   hardware/sensor_housings/sht40_stevenson/
#       modify_thingiverse_stevenson_sht40.py
#
# The STL outputs are mesh objects, not parametric FreeCAD solids.
# ============================================================


DOC_NAME = "SHT40_Blender_Modified_Stevenson"
try:
    App.closeDocument(DOC_NAME)
except Exception:
    pass
doc = App.newDocument(DOC_NAME)


SCRIPT_DIR = Path(__file__).resolve().parent
STL_DIR = SCRIPT_DIR / "stl"

HAS_BRACKET_ADAPTER = (STL_DIR / "sht40_right_angle_bracket_adapter.stl").exists()

PARTS = [
    ("SHT40_Stevenson_Body_modified_mesh", "sht40_stevenson_body_modified.stl", (0.86, 0.86, 0.80), (0.0, 0.0, 0.0), True),
]

if HAS_BRACKET_ADAPTER:
    PARTS.append(
        ("Right_Angle_Bracket_Adapter_mesh", "sht40_right_angle_bracket_adapter.stl", (0.76, 0.82, 0.92), (0.0, 0.0, 82.0), False)
    )

PARTS.append(
    ("PG7_Retainer_Flange_modified_mesh", "sht40_pg7_retainer_flange.stl", (0.92, 0.92, 0.82), (0.0, 0.0, 88.0 if HAS_BRACKET_ADAPTER else 82.0), True)
)


def add_mesh_object(label, filename, color, translation, required):
    path = STL_DIR / filename
    if not path.exists():
        if not required:
            print("Skipping optional missing STL: {}".format(path))
            return None
        raise FileNotFoundError("Missing STL: {}".format(path))

    mesh = Mesh.Mesh(str(path))
    obj = doc.addObject("Mesh::Feature", label)
    obj.Mesh = mesh
    obj.Placement.Base = App.Vector(*translation)

    if getattr(obj, "ViewObject", None):
        obj.ViewObject.ShapeColor = color

    return obj


objects = [obj for obj in (add_mesh_object(*part) for part in PARTS) if obj is not None]
doc.recompute()

try:
    import FreeCADGui as Gui

    Gui.ActiveDocument.ActiveView.viewAxometric()
    Gui.SendMsgToActiveView("ViewFit")
except Exception:
    pass


print("\n---- SHT40 Blender-modified Stevenson STL Viewer ----")
print("Loaded STL directory: {}".format(STL_DIR))
for obj in objects:
    bb = obj.Mesh.BoundBox
    print(
        "{:<38} {:>7.2f} x {:>7.2f} x {:>7.2f} mm".format(
            obj.Label + ":",
            bb.XLength,
            bb.YLength,
            bb.ZLength,
        )
    )
print("\nThese are mesh objects imported from Blender-generated STL files.")
print("Modify them by editing and rerunning modify_thingiverse_stevenson_sht40.py.")
