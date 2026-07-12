import math
from pathlib import Path

import FreeCAD as App
import Mesh
import Part
from FreeCAD import Vector

# ============================================================
# FieldMesh Node Box Sensor Deck
#
# Shared adapter between:
#  - AS7341_PAR_STANDALONE.py
#  - sht40_stevenson/modify_thingiverse_stevenson_sht40.py
#
# The two sensor housings use the same compact two-hole M3 interface.
# This deck provides two copies of that interface and mounts to the planned
# 160 x 160 x 90 mm node enclosure. The 110 x 122 mm box pattern is taken from
# the supplied product image and should be checked against the real enclosure.
# ============================================================


# ---------- Document ----------
DOC_NAME = "FieldMesh_Node_Box_Sensor_Deck"
try:
    App.closeDocument(DOC_NAME)
except Exception:
    pass
doc = App.newDocument(DOC_NAME)


# ---------- STL export ----------
EXPORT_STL = True
STL_OUTPUT_DIR = Path(__file__).resolve().parent / "stl"


def export_printable_stl(obj, filename):
    if not EXPORT_STL:
        return None

    STL_OUTPUT_DIR.mkdir(parents=True, exist_ok=True)
    path = STL_OUTPUT_DIR / filename
    Mesh.export([obj], str(path))
    return path


# ---------- Node box reference ----------
NODE_BOX_OUTER_X = 160.0
NODE_BOX_OUTER_Y = 160.0
NODE_BOX_BODY_H = 90.0
NODE_BOX_LID_H = 20.0

# Screenshot-derived box/lid fixing pattern. Verify on the actual box.
BOX_MOUNT_X_SPACING = 110.0
BOX_MOUNT_Y_SPACING = 122.0
BOX_MOUNT_HOLE_D = 4.4
BOX_MOUNT_COUNTERBORE_D = 8.5
BOX_MOUNT_COUNTERBORE_DEPTH = 2.4


# ---------- Deck ----------
DECK_W = 150.0
DECK_H = 150.0
DECK_THK = 5.0
DECK_CORNER_R = 6.0


# ---------- Unified sensor interfaces ----------
SENSOR_INTERFACE_HOLE_SPACING = 64.0
SENSOR_INTERFACE_HOLE_D = 3.4
SENSOR_INSERT_BORE_D = 4.4
SENSOR_INSERT_DEPTH = 4.5
SENSOR_INSERT_LEADIN_D = 5.2
SENSOR_INSERT_LEADIN_DEPTH = 0.8
SENSOR_BOSS_OD = 12.0
SENSOR_BOSS_H = 3.0

# Place PAR and SHT on the same shared M3 interface, separated front/back on
# the 160 mm box lid. The SHT body uses a Y-axis hole pair so its side bracket
# can remain on X without fighting the plate mount.
SENSOR_STATIONS = [
    ("PAR_AS7341", 0.0, 47.0, "x"),
    ("SHT40_SHIELD", 0.0, -33.0, "y"),
]


# ---------- Visual references ----------
BOX_REFERENCE_ENABLE = True
SENSOR_FOOTPRINT_REFERENCE_ENABLE = True
SPECTRAL_BODY_FOOTPRINT_D = 54.0
SHT_BODY_FOOTPRINT_X = 47.5
SHT_BODY_FOOTPRINT_Y = 82.0


# ---------- Helpers ----------
def cyl_z(diameter, height, z_bottom, x=0.0, y=0.0):
    return Part.makeCylinder(
        float(diameter) / 2.0,
        float(height),
        Vector(float(x), float(y), float(z_bottom)),
        Vector(0, 0, 1),
    )


def box_centered(sx, sy, sz, cx=0.0, cy=0.0, cz=0.0):
    return Part.makeBox(
        float(sx),
        float(sy),
        float(sz),
        Vector(float(cx) - float(sx) / 2.0, float(cy) - float(sy) / 2.0, float(cz) - float(sz) / 2.0),
    )


def fuse_all(shapes):
    valid = [shape for shape in shapes if shape is not None]
    if not valid:
        raise ValueError("No shapes to fuse")
    result = valid[0]
    for shape in valid[1:]:
        result = result.fuse(shape)
    try:
        result = result.removeSplitter()
    except Exception:
        pass
    return result


def cut_all(shape, cutters):
    result = shape
    for cutter in cutters:
        if cutter is not None:
            result = result.cut(cutter)
    try:
        result = result.removeSplitter()
    except Exception:
        pass
    return result


def rounded_plate(w, h, r, thk):
    r = min(float(r), float(w) / 2.0, float(h) / 2.0)
    parts = [
        box_centered(float(w) - (2.0 * r), float(h), float(thk), cz=float(thk) / 2.0),
        box_centered(float(w), float(h) - (2.0 * r), float(thk), cz=float(thk) / 2.0),
    ]
    for sx in (-1.0, 1.0):
        for sy in (-1.0, 1.0):
            parts.append(cyl_z(2.0 * r, thk, 0.0, x=sx * ((float(w) / 2.0) - r), y=sy * ((float(h) / 2.0) - r)))
    return fuse_all(parts)


def box_mount_points():
    return [
        (sx * BOX_MOUNT_X_SPACING / 2.0, sy * BOX_MOUNT_Y_SPACING / 2.0)
        for sx in (-1.0, 1.0)
        for sy in (-1.0, 1.0)
    ]


def sensor_mount_points(cx, cy, axis="x"):
    if str(axis).lower() == "y":
        return [
            (float(cx), float(cy) - SENSOR_INTERFACE_HOLE_SPACING / 2.0),
            (float(cx), float(cy) + SENSOR_INTERFACE_HOLE_SPACING / 2.0),
        ]

    return [
        (float(cx) - SENSOR_INTERFACE_HOLE_SPACING / 2.0, float(cy)),
        (float(cx) + SENSOR_INTERFACE_HOLE_SPACING / 2.0, float(cy)),
    ]


def make_deck():
    deck = rounded_plate(DECK_W, DECK_H, DECK_CORNER_R, DECK_THK)

    bosses = []
    cutters = []

    for x, y in box_mount_points():
        cutters.append(cyl_z(BOX_MOUNT_HOLE_D, DECK_THK + 0.6, -0.3, x=x, y=y))
        cutters.append(
            cyl_z(
                BOX_MOUNT_COUNTERBORE_D,
                BOX_MOUNT_COUNTERBORE_DEPTH + 0.2,
                DECK_THK - BOX_MOUNT_COUNTERBORE_DEPTH,
                x=x,
                y=y,
            )
        )

    for _name, cx, cy, axis in SENSOR_STATIONS:
        for x, y in sensor_mount_points(cx, cy, axis=axis):
            bosses.append(cyl_z(SENSOR_BOSS_OD, SENSOR_BOSS_H, DECK_THK, x=x, y=y))
            cutters.append(
                cyl_z(
                    SENSOR_INSERT_BORE_D,
                    SENSOR_INSERT_DEPTH + 0.2,
                    DECK_THK + SENSOR_BOSS_H - SENSOR_INSERT_DEPTH,
                    x=x,
                    y=y,
                )
            )
            cutters.append(
                cyl_z(
                    SENSOR_INSERT_LEADIN_D,
                    SENSOR_INSERT_LEADIN_DEPTH + 0.1,
                    DECK_THK + SENSOR_BOSS_H - SENSOR_INSERT_LEADIN_DEPTH,
                    x=x,
                    y=y,
                )
            )

    return cut_all(fuse_all([deck] + bosses), cutters)


def make_box_reference():
    outer = box_centered(NODE_BOX_OUTER_X, NODE_BOX_OUTER_Y, 0.8, cz=-0.4)
    inner = box_centered(NODE_BOX_OUTER_X - 8.0, NODE_BOX_OUTER_Y - 8.0, 1.2, cz=-0.6)
    return outer.cut(inner)


def make_footprint_reference(diameter, cx, cy, z):
    outer = cyl_z(diameter, 0.6, z, x=cx, y=cy)
    inner = cyl_z(max(0.1, diameter - 2.0), 0.8, z - 0.1, x=cx, y=cy)
    return outer.cut(inner)


def make_rect_footprint_reference(sx, sy, cx, cy, z):
    outer = box_centered(sx, sy, 0.6, cx=cx, cy=cy, cz=z + 0.3)
    inner = box_centered(max(0.1, sx - 2.0), max(0.1, sy - 2.0), 0.8, cx=cx, cy=cy, cz=z + 0.3)
    return outer.cut(inner)


# ---------- Build ----------
deck_shape = make_deck()

deck_obj = doc.addObject("Part::Feature", "Node_Box_Sensor_Deck")
deck_obj.Shape = deck_shape

if BOX_REFERENCE_ENABLE:
    box_ref = doc.addObject("Part::Feature", "Node_box_160x160_reference_not_printed")
    box_ref.Shape = make_box_reference()
    if getattr(box_ref, "ViewObject", None):
        box_ref.ViewObject.Transparency = 82
        box_ref.ViewObject.ShapeColor = (0.6, 0.6, 0.6)

if SENSOR_FOOTPRINT_REFERENCE_ENABLE:
    refs = []
    for name, cx, cy, _axis in SENSOR_STATIONS:
        ref = doc.addObject("Part::Feature", "{}_footprint_reference_not_printed".format(name))
        if "PAR" in name:
            ref.Shape = make_footprint_reference(SPECTRAL_BODY_FOOTPRINT_D, cx, cy, DECK_THK + SENSOR_BOSS_H + 0.3)
        else:
            ref.Shape = make_rect_footprint_reference(SHT_BODY_FOOTPRINT_X, SHT_BODY_FOOTPRINT_Y, cx, cy, DECK_THK + SENSOR_BOSS_H + 0.3)
        if getattr(ref, "ViewObject", None):
            ref.ViewObject.Transparency = 80
            ref.ViewObject.ShapeColor = (0.2, 0.6, 1.0)
        refs.append(ref)

try:
    deck_obj.ViewObject.ShapeColor = (0.88, 0.88, 0.84)
except Exception:
    pass

doc.recompute()

exported_stl_path = export_printable_stl(deck_obj, "node_box_sensor_deck.stl")

try:
    import FreeCADGui as Gui

    Gui.ActiveDocument.ActiveView.viewAxometric()
    Gui.SendMsgToActiveView("ViewFit")
except Exception:
    pass


# ---------- Report ----------
sensor_insert_floor = (DECK_THK + SENSOR_BOSS_H) - SENSOR_INSERT_DEPTH
sensor_pair_clearance = abs(SENSOR_STATIONS[0][2] - SENSOR_STATIONS[1][2]) - ((SPECTRAL_BODY_FOOTPRINT_D / 2.0) + (SHT_BODY_FOOTPRINT_Y / 2.0))
box_edge_margin_x = (NODE_BOX_OUTER_X - DECK_W) / 2.0
box_edge_margin_y = (NODE_BOX_OUTER_Y - DECK_H) / 2.0
box_mount_edge_x = (DECK_W / 2.0) - (BOX_MOUNT_X_SPACING / 2.0)
box_mount_edge_y = (DECK_H / 2.0) - (BOX_MOUNT_Y_SPACING / 2.0)


def status(label, ok):
    return "{} [{}]".format(label, "OK" if ok else "CHECK")


print("\n---- FieldMesh Node Box Sensor Deck ----")
print("Node box outer X/Y/H:          {:.2f} / {:.2f} / {:.2f} mm".format(NODE_BOX_OUTER_X, NODE_BOX_OUTER_Y, NODE_BOX_BODY_H))
print("Deck W/H/thickness:            {:.2f} / {:.2f} / {:.2f} mm".format(DECK_W, DECK_H, DECK_THK))
print("Box mount X/Y spacing:         {:.2f} / {:.2f} mm".format(BOX_MOUNT_X_SPACING, BOX_MOUNT_Y_SPACING))
print("Box mount hole/counterbore:    {:.2f} / {:.2f} mm".format(BOX_MOUNT_HOLE_D, BOX_MOUNT_COUNTERBORE_D))
print("Unified sensor mount spacing:  {:.2f} mm".format(SENSOR_INTERFACE_HOLE_SPACING))
print("Sensor insert bore/depth:      {:.2f} / {:.2f} mm".format(SENSOR_INSERT_BORE_D, SENSOR_INSERT_DEPTH))
for name, cx, cy, axis in SENSOR_STATIONS:
    print("{} center / mount axis:     X {:.2f} / Y {:.2f} / {}".format(name, cx, cy, axis.upper()))

print("\n---- Fit / Printability Check ----")
print(status("Deck inside 160 mm box top X", box_edge_margin_x >= 5.0))
print(status("Deck inside 160 mm box top Y", box_edge_margin_y >= 5.0))
print(status("Box mount edge margin X", box_mount_edge_x >= 8.0))
print(status("Box mount edge margin Y", box_mount_edge_y >= 8.0))
print(status("Sensor insert floor >= 3.0 mm", sensor_insert_floor >= 3.0))
print(status("Sensor body clearance >= 3 mm", sensor_pair_clearance >= 3.0))
print(status("Shape validity", deck_shape.isValid()))

print("\nPrint orientation notes:")
print("- Print flat with sensor insert bosses facing upward.")
print("- Install heat-set inserts from the top side of the bosses.")
print("- Box mount spacing is screenshot-derived; verify 110 x 122 mm on the real enclosure.")
print("- Visual reference objects are not intended for STL export.")
if EXPORT_STL and exported_stl_path is not None:
    print("\nSTL export:")
    print("- {}".format(exported_stl_path))
