import math
from pathlib import Path

import FreeCAD as App
import Mesh
import Part
from FreeCAD import Vector

# ============================================================
# AS7341 / PAR Spectral Sensor Standalone Housing
#
# Extracted from MACRO.py PAR geometry:
#  - same 50 mm OD / 20 mm tall cylindrical housing
#  - 40 mm x 1.6 mm diffuser pocket and support ledge
#  - same two internal PCB screw bosses
#  - upgraded waterproof side cable-gland port
#
# The old anemometer roof is replaced with a compact circular base so this can
# be iterated as a standalone waterproof enclosure.
# ============================================================


# ---------- Document ----------
DOC_NAME = "AS7341_PAR_Standalone"
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

    # Export a copy of the final Part shape, not the preview object placement.
    # The FreeCAD document shows the retainer above the housing for inspection,
    # but each printable STL should sit on the slicer bed by itself.
    shape = obj.Shape.copy()
    shape.translate(Vector(0.0, 0.0, -shape.BoundBox.ZMin))

    temp = doc.addObject("Part::Feature", "_STL_export_" + filename.replace(".", "_"))
    temp.Shape = shape
    doc.recompute()
    Mesh.export([temp], str(path))
    doc.removeObject(temp.Name)
    return path


# ---------- Standalone base ----------
# Kept equal to the old MACRO.py roof thickness for this first extraction.
# Tune this downward later if the sensor stack and screw engagement allow it.
BASE_OD = 50.0
BASE_THK = 10.0


# ---------- PAR / Spectral sensor housing ----------
PAR_CENTER = Vector(0, 0, 0)

PAR_HOUSING_WALL = 3.0
PAR_HOUSING_OD = 50.0
PAR_HOUSING_H = 38.5
PAR_APERTURE_D = 33.0

# ---------- AS734x V1 long PCB reference ----------
# From the real board measured with calipers:
# - overall PCB: 25.51 x 11.00 mm
# - mounting-hole center spacing: 19.30 mm
# - optical sensor center is about +3.61 mm from the PCB/hole midpoint
#
# The mounting supports are centered on the housing for clean top access.
# The optical offset is kept as a reference for checking the real board.
SENSOR_PCB_W = 25.51
SENSOR_PCB_H = 11.00
SENSOR_PCB_MOUNT_HOLE_SPACING = 19.30
SENSOR_PCB_MOUNT_HOLE_D = 3.0
SENSOR_PCB_OPTICAL_OFFSET_X = 3.61
SENSOR_PCB_ROT_DEG = 0.0
SENSOR_PCB_PREVIEW_ENABLE = False
SENSOR_PCB_PREVIEW_THK = 0.8
SENSOR_PCB_PREVIEW_OFFSET_X = 0.0
SENSOR_PCB_PREVIEW_OFFSET_Y = -45.0


def _rotate_xy(x, y, angle_deg):
    rad = math.radians(float(angle_deg))
    return (
        (float(x) * math.cos(rad)) - (float(y) * math.sin(rad)),
        (float(x) * math.sin(rad)) + (float(y) * math.cos(rad)),
    )


PAR_BOSS_OFFSET_XY = [
    _rotate_xy(-SENSOR_PCB_MOUNT_HOLE_SPACING / 2.0, 0.0, SENSOR_PCB_ROT_DEG),
    _rotate_xy(+SENSOR_PCB_MOUNT_HOLE_SPACING / 2.0, 0.0, SENSOR_PCB_ROT_DEG),
]
PAR_BOSS_HEIGHT = 15.0
PAR_BOSS_DIAM = 7.0
PAR_SCREW_DIAMETER = 3.1
PAR_BOSS_ROOT_FLARE_ENABLE = True
PAR_BOSS_ROOT_FLARE_H = 2.5
PAR_BOSS_ROOT_FLARE_EXTRA_R = 1.8
PAR_BOSS_BASE_FILLET_R = 1.2
PAR_BOSS_CROSS_GUSSET_ENABLE = False
PAR_BOSS_CROSS_GUSSET_THK = 2.5
PAR_BOSS_CROSS_GUSSET_H = 6.0

# PAR diffuser (top-mounted disc)
PAR_DIFFUSER_ENABLE = True
PAR_DIFFUSER_D = 39.5
PAR_DIFFUSER_THK = 1.6
PAR_DIFFUSER_POCKET_ENABLE = True
PAR_DIFFUSER_RECESS_DEPTH = 1.6
PAR_DIFFUSER_OD_CLEAR_D = 0.6
PAR_DIFFUSER_LEDGE_ENABLE = True
PAR_DIFFUSER_LEDGE_H = 1.6
PAR_DIFFUSER_LEDGE_OVERLAP = 4.0
PAR_DIFFUSER_POCKET_FLOOR_ENABLE = True
PAR_DIFFUSER_POCKET_FLOOR_THK = 1.2
PAR_DIFFUSER_ANTI_OVERHANG_ENABLE = True
PAR_DIFFUSER_ANTI_OVERHANG_H = 5.0
PAR_DIFFUSER_ANTI_OVERHANG_ANGLE_DEG = 30.0
PAR_DIFFUSER_ANTI_OVERHANG_WALL_CLEAR = 0.3
PAR_DIFFUSER_ANTI_OVERHANG_WALL_HIT_DROP = 0.0
PAR_DIFFUSER_RETAINER_ENABLE = False
PAR_DIFFUSER_RETAINER_WALL = 1.0
PAR_DIFFUSER_RETAINER_H = 1.6
PAR_DIFFUSER_RETAINER_RADIAL_MARGIN = 0.2

PAR_CABLE_PORT_Z_OFF = 13.0
PAR_CABLE_PORT_ANGLE = 270.0

# PG7 keeps the PAR cable exit on the same gland family as the SHT40 probe
# mount. Use a long-thread PG7 gland if possible; the printed boss plus wall is
# intentionally kept thin so a standard gland still has useful thread showing.
CABLE_GLAND_THREAD_CLEAR_D = 13.0
CABLE_GLAND_PAD_ENABLE = True
CABLE_GLAND_PAD_OD = 22.0
CABLE_GLAND_PAD_PROJECTION = 0.5
CABLE_GLAND_PAD_WALL_OVERLAP = 0.8
CABLE_GLAND_INTERNAL_FLAT_ENABLE = True
CABLE_GLAND_INTERNAL_FLAT_OD = 28.0
CABLE_GLAND_INTERNAL_FLAT_PROJECTION = 1.5
CABLE_GLAND_INTERNAL_FLAT_WALL_OVERLAP = 0.25
PG7_THREAD_LENGTH = 7.8
PG7_LOCKNUT_AF = 17.7
PG7_LOCKNUT_THK = 5.0
PG7_LOCKNUT_SWING_CLEARANCE_D = 1.2

PAR_CABLE_PORT_D = CABLE_GLAND_THREAD_CLEAR_D
PAR_CABLE_PORT_CUT_EXTRA = CABLE_GLAND_PAD_PROJECTION + CABLE_GLAND_INTERNAL_FLAT_PROJECTION + 3.0

# ---------- Diffuser retaining ring ----------
# Separate printable clamp ring. The main 50 mm housing stays unchanged except
# for local insert lugs, because inserts in the 3 mm wall would open into the
# wet interior.
RETAINER_RING_ENABLE = True
RETAINER_RING_OD = PAR_HOUSING_OD
RETAINER_RING_THK = 3.2
RETAINER_RING_APERTURE_D = PAR_APERTURE_D
RETAINER_PREVIEW_GAP = 18.0

RETAINER_SCREW_COUNT = 4
RETAINER_SCREW_ANGLE_OFFSET_DEG = 45.0
RETAINER_SCREW_CLEARANCE_D = 3.4
RETAINER_SCREW_COUNTERBORE_ENABLE = True
RETAINER_SCREW_COUNTERBORE_D = 6.8
RETAINER_SCREW_COUNTERBORE_DEPTH = 1.5

RETAINER_LUG_OD = 13.0
RETAINER_LUG_OVERLAP = 4.0
RETAINER_SCREW_RADIUS = (PAR_HOUSING_OD / 2.0) + (RETAINER_LUG_OD / 2.0) - RETAINER_LUG_OVERLAP

RETAINER_LUG_ROOT_BRIDGE_ENABLE = False
RETAINER_LUG_ROOT_BRIDGE_W = 10.0
RETAINER_LUG_ROOT_BRIDGE_OVERLAP = 4.0
RETAINER_LUG_EDGE_FILLET_R = 0.55
RETAINER_LUG_BRIDGE_ROUNDED_ENDS = True
RETAINER_LUG_ROUND_NECK_ENABLE = True
RETAINER_LUG_ROUND_NECK_D = 11.0

RETAINER_INSERT_PAD_H = 6.0
RETAINER_INSERT_BORE_D = 4.4
RETAINER_INSERT_DEPTH = 4.5

# Lid-side groove on the retainer underside. It sits directly above the housing
# top land, outside the diffuser pocket/lip.
DIFFUSER_SEAL_GROOVE_ENABLE = True
DIFFUSER_SEAL_GROOVE_CENTER_D = 45.0
DIFFUSER_SEAL_GROOVE_W = 1.4
DIFFUSER_SEAL_GROOVE_DEPTH = 0.65

# ---------- Mounting options ----------
BOTTOM_FEET_ENABLE = True
BOTTOM_FEET_COUNT = 2
BOTTOM_FEET_ANGLE_OFFSET_DEG = 0.0
UNIFIED_SENSOR_MOUNT_HOLE_SPACING = 64.0
BOTTOM_FOOT_OD = 18.0
BOTTOM_FOOT_BRIDGE_OVERLAP = 2.0
BOTTOM_FOOT_BRIDGE_W = 13.0
BOTTOM_FOOT_THK = 4.0
BOTTOM_FOOT_HOLE_D = 3.4
BOTTOM_FOOT_COUNTERBORE_ENABLE = True
BOTTOM_FOOT_COUNTERBORE_D = 7.0
BOTTOM_FOOT_COUNTERBORE_DEPTH = 2.0
BOTTOM_FOOT_RADIUS = UNIFIED_SENSOR_MOUNT_HOLE_SPACING / 2.0

# Optional visual reference only. Keep disabled for clean STL export of the
# printable enclosure body.
DIFFUSER_PREVIEW_ENABLE = False


def _safe_refine(shape):
    try:
        return shape.removeSplitter()
    except Exception:
        return shape


def _safe_fillet(shape, radius, label):
    if radius <= 0:
        return shape

    try:
        return shape.makeFillet(float(radius), shape.Edges)
    except Exception as ex:
        print("{} fillet failed at {:.2f} mm: {}".format(label, float(radius), ex))
        return shape


def _polar_xy(radius, angle_deg):
    rad = math.radians(float(angle_deg))
    return (
        PAR_CENTER.x + (float(radius) * math.cos(rad)),
        PAR_CENTER.y + (float(radius) * math.sin(rad)),
    )


def retainer_screw_points():
    if RETAINER_SCREW_COUNT <= 0:
        return []

    step = 360.0 / float(RETAINER_SCREW_COUNT)
    return [
        _polar_xy(RETAINER_SCREW_RADIUS, RETAINER_SCREW_ANGLE_OFFSET_DEG + (step * idx))
        for idx in range(int(RETAINER_SCREW_COUNT))
    ]


def _rotate_about_center(shape, angle_deg):
    shape.rotate(Vector(PAR_CENTER.x, PAR_CENTER.y, 0), Vector(0, 0, 1), float(angle_deg))
    return shape


def _angle_from_xy(x, y):
    return math.degrees(math.atan2(float(y) - PAR_CENTER.y, float(x) - PAR_CENTER.x))


def pg7_locknut_swing_d():
    hex_corner_d = float(PG7_LOCKNUT_AF) / math.cos(math.radians(30.0))
    return hex_corner_d + float(PG7_LOCKNUT_SWING_CLEARANCE_D)


def diffuser_top_reserved_h(seat_inner_r):
    reserved_h = 0.0

    if PAR_DIFFUSER_POCKET_ENABLE and PAR_DIFFUSER_RECESS_DEPTH > 0:
        reserved_h += float(PAR_DIFFUSER_RECESS_DEPTH)

        if PAR_DIFFUSER_POCKET_FLOOR_ENABLE and PAR_DIFFUSER_POCKET_FLOOR_THK > 0 and PAR_DIFFUSER_LEDGE_OVERLAP > 0:
            floor_thk = min(float(PAR_DIFFUSER_POCKET_FLOOR_THK), max(0.2, float(PAR_DIFFUSER_RECESS_DEPTH) - 0.2))
            reserved_h += floor_thk

    if (
        PAR_DIFFUSER_ANTI_OVERHANG_ENABLE
        and PAR_DIFFUSER_RECESS_DEPTH > 0
        and PAR_DIFFUSER_LEDGE_ENABLE
        and PAR_DIFFUSER_LEDGE_OVERLAP > 0
        and PAR_DIFFUSER_ANTI_OVERHANG_H > 0
        and seat_inner_r is not None
    ):
        housing_id_r = max(0.1, (float(PAR_HOUSING_OD) / 2.0) - float(PAR_HOUSING_WALL))
        inner_bottom_target_r = max(float(seat_inner_r) + 0.1, housing_id_r - float(PAR_DIFFUSER_ANTI_OVERHANG_WALL_CLEAR))
        radial_span = inner_bottom_target_r - float(seat_inner_r)
        draft = math.tan(math.radians(float(PAR_DIFFUSER_ANTI_OVERHANG_ANGLE_DEG)))
        required_h = radial_span / draft if draft > 1e-6 else float(PAR_DIFFUSER_ANTI_OVERHANG_H)
        max_h = max(0.6, float(PAR_HOUSING_H) - float(PAR_DIFFUSER_RECESS_DEPTH) - 0.6)

        if PAR_DIFFUSER_ANTI_OVERHANG_WALL_HIT_DROP > 0:
            support_h = min(max_h, float(PAR_DIFFUSER_ANTI_OVERHANG_WALL_HIT_DROP))
        else:
            support_h = min(max_h, max(float(PAR_DIFFUSER_ANTI_OVERHANG_H), required_h))

        reserved_h += support_h

    return reserved_h


def make_radial_bridge(angle_deg, inner_r, outer_r, width, height, z_bottom, rounded_ends=False):
    length = float(outer_r) - float(inner_r)
    if length <= 0 or width <= 0 or height <= 0:
        return None

    if not rounded_ends or length <= float(width):
        bridge = Part.makeBox(
            length,
            float(width),
            float(height),
            Vector(PAR_CENTER.x + float(inner_r), PAR_CENTER.y - (float(width) / 2.0), float(z_bottom)),
        )
        return _rotate_about_center(bridge, angle_deg)

    cap_r = float(width) / 2.0
    straight_len = max(0.1, length - float(width))
    bridge = Part.makeBox(
        straight_len,
        float(width),
        float(height),
        Vector(PAR_CENTER.x + float(inner_r) + cap_r, PAR_CENTER.y - cap_r, float(z_bottom)),
    )
    inner_cap = Part.makeCylinder(
        cap_r,
        float(height),
        Vector(PAR_CENTER.x + float(inner_r) + cap_r, PAR_CENTER.y, float(z_bottom)),
    )
    outer_cap = Part.makeCylinder(
        cap_r,
        float(height),
        Vector(PAR_CENTER.x + float(outer_r) - cap_r, PAR_CENTER.y, float(z_bottom)),
    )
    bridge = _safe_refine(bridge.fuse(inner_cap).fuse(outer_cap))
    return _rotate_about_center(bridge, angle_deg)


def make_retainer_round_lug(sx, sy, height, z_bottom, body_outer_r):
    lug = Part.makeCylinder(
        float(RETAINER_LUG_OD) / 2.0,
        float(height),
        Vector(sx, sy, float(z_bottom)),
    )

    if RETAINER_LUG_ROUND_NECK_ENABLE and RETAINER_LUG_ROUND_NECK_D > 0:
        angle = math.radians(_angle_from_xy(sx, sy))
        body_r = float(body_outer_r)
        screw_r = math.hypot(float(sx) - PAR_CENTER.x, float(sy) - PAR_CENTER.y)
        neck_r = min(
            screw_r,
            body_r + (max(0.0, screw_r - body_r) * 0.45),
        )
        neck_x = PAR_CENTER.x + neck_r * math.cos(angle)
        neck_y = PAR_CENTER.y + neck_r * math.sin(angle)
        neck = Part.makeCylinder(
            float(RETAINER_LUG_ROUND_NECK_D) / 2.0,
            float(height),
            Vector(neck_x, neck_y, float(z_bottom)),
        )
        lug = lug.fuse(neck)

    return lug


def _fillet_standoff_base_before_fuse(standoff_shape, x, y, base_r, z_target, fillet_r, tol_z=1.0, tol_r=1.5):
    if fillet_r <= 0:
        return standoff_shape

    local = standoff_shape.copy()
    local.translate(Vector(-x, -y, 0))

    edges = []
    zt = float(z_target)
    rt = float(base_r)

    for edge in local.Edges:
        bb = edge.BoundBox
        if abs(bb.ZMin - zt) > float(tol_z) or abs(bb.ZMax - zt) > float(tol_z):
            continue

        ok = True
        for vertex in edge.Vertexes:
            radius = math.hypot(vertex.Point.x, vertex.Point.y)
            if abs(radius - rt) > float(tol_r):
                ok = False
                break
        if ok:
            edges.append(edge)

    if edges:
        try:
            local = local.makeFillet(float(fillet_r), edges)
        except Exception as ex:
            print("Standoff base fillet failed:", ex)

    local.translate(Vector(x, y, 0))
    return local


def make_gland_pad(port_z):
    if not CABLE_GLAND_PAD_ENABLE or CABLE_GLAND_PAD_OD <= 0 or CABLE_GLAND_PAD_PROJECTION <= 0:
        return None

    cx, cy = PAR_CENTER.x, PAR_CENTER.y
    outer_r = float(PAR_HOUSING_OD) / 2.0
    inner_r = outer_r - float(PAR_HOUSING_WALL)
    overlap = max(0.0, float(CABLE_GLAND_PAD_WALL_OVERLAP))
    projection = float(CABLE_GLAND_PAD_PROJECTION)

    # The outside gasket face must not just kiss the curved wall. Run the pad
    # through the full wall thickness so the outside flat is solidly backed.
    pad = Part.makeCylinder(
        float(CABLE_GLAND_PAD_OD) / 2.0,
        (outer_r - inner_r) + projection + overlap,
        Vector(cx + inner_r - overlap, cy, port_z),
        Vector(1, 0, 0),
    )
    return _rotate_about_center(pad, PAR_CABLE_PORT_ANGLE)


def make_internal_gland_flat(port_z):
    if (
        not CABLE_GLAND_INTERNAL_FLAT_ENABLE
        or CABLE_GLAND_INTERNAL_FLAT_OD <= 0
        or CABLE_GLAND_INTERNAL_FLAT_PROJECTION <= 0
    ):
        return None

    cx, cy = PAR_CENTER.x, PAR_CENTER.y
    inner_r = (float(PAR_HOUSING_OD) / 2.0) - float(PAR_HOUSING_WALL)
    projection = float(CABLE_GLAND_INTERNAL_FLAT_PROJECTION)
    overlap = max(0.0, float(CABLE_GLAND_INTERNAL_FLAT_WALL_OVERLAP))

    # Likewise, the inside locknut landing runs back through the wall so the
    # internal flat is not a shallow patch on the curved inner surface.
    flat = Part.makeCylinder(
        float(CABLE_GLAND_INTERNAL_FLAT_OD) / 2.0,
        float(PAR_HOUSING_WALL) + projection + overlap,
        Vector(cx + inner_r - projection, cy, port_z),
        Vector(1, 0, 0),
    )
    return _rotate_about_center(flat, PAR_CABLE_PORT_ANGLE)


def make_retainer_insert_pads(z_top):
    pads = None

    for sx, sy in retainer_screw_points():
        lug = make_retainer_round_lug(
            sx,
            sy,
            height=float(RETAINER_INSERT_PAD_H),
            z_bottom=z_top - float(RETAINER_INSERT_PAD_H),
            body_outer_r=float(PAR_HOUSING_OD) / 2.0,
        )
        if RETAINER_LUG_ROOT_BRIDGE_ENABLE:
            angle = _angle_from_xy(sx, sy)
            bridge = make_radial_bridge(
                angle_deg=angle,
                inner_r=(float(PAR_HOUSING_OD) / 2.0) - float(RETAINER_LUG_ROOT_BRIDGE_OVERLAP),
                outer_r=float(RETAINER_SCREW_RADIUS) + (float(RETAINER_LUG_OD) / 2.0),
                width=float(RETAINER_LUG_ROOT_BRIDGE_W),
                height=float(RETAINER_INSERT_PAD_H),
                z_bottom=z_top - float(RETAINER_INSERT_PAD_H),
                rounded_ends=RETAINER_LUG_BRIDGE_ROUNDED_ENDS,
            )
            if bridge is not None:
                lug = lug.fuse(bridge)

        lug = _safe_fillet(_safe_refine(lug), RETAINER_LUG_EDGE_FILLET_R, "Retainer insert lug")

        if pads is None:
            pads = lug
        else:
            pads = pads.fuse(lug)

    return _safe_refine(pads) if pads is not None else None


def cut_retainer_insert_bores(body, z_top):
    for sx, sy in retainer_screw_points():
        bore = Part.makeCylinder(
            float(RETAINER_INSERT_BORE_D) / 2.0,
            float(RETAINER_INSERT_DEPTH) + 0.2,
            Vector(sx, sy, z_top - float(RETAINER_INSERT_DEPTH) - 0.1),
        )
        body = body.cut(bore)

    return _safe_refine(body)


def make_retainer_ring(z_bottom):
    cx, cy = PAR_CENTER.x, PAR_CENTER.y

    retainer = Part.makeCylinder(
        float(RETAINER_RING_OD) / 2.0,
        float(RETAINER_RING_THK),
        Vector(cx, cy, z_bottom),
    )

    for sx, sy in retainer_screw_points():
        lug = make_retainer_round_lug(
            sx,
            sy,
            height=float(RETAINER_RING_THK),
            z_bottom=z_bottom,
            body_outer_r=float(RETAINER_RING_OD) / 2.0,
        )
        if RETAINER_LUG_ROOT_BRIDGE_ENABLE:
            angle = _angle_from_xy(sx, sy)
            bridge = make_radial_bridge(
                angle_deg=angle,
                inner_r=(float(RETAINER_RING_OD) / 2.0) - float(RETAINER_LUG_ROOT_BRIDGE_OVERLAP),
                outer_r=float(RETAINER_SCREW_RADIUS) + (float(RETAINER_LUG_OD) / 2.0),
                width=float(RETAINER_LUG_ROOT_BRIDGE_W),
                height=float(RETAINER_RING_THK),
                z_bottom=z_bottom,
                rounded_ends=RETAINER_LUG_BRIDGE_ROUNDED_ENDS,
            )
            if bridge is not None:
                lug = lug.fuse(bridge)

        lug = _safe_fillet(_safe_refine(lug), RETAINER_LUG_EDGE_FILLET_R, "Retainer ring lug")

        retainer = retainer.fuse(lug)

    aperture = Part.makeCylinder(
        float(RETAINER_RING_APERTURE_D) / 2.0,
        float(RETAINER_RING_THK) + 0.4,
        Vector(cx, cy, z_bottom - 0.2),
    )
    retainer = retainer.cut(aperture)

    retainer = cut_retainer_underside_seal_groove(
        retainer,
        z_bottom=z_bottom,
        diffuser_pocket_r=(float(PAR_DIFFUSER_D) + float(PAR_DIFFUSER_OD_CLEAR_D)) / 2.0,
        lid_outer_r=float(RETAINER_RING_OD) / 2.0,
    )

    for sx, sy in retainer_screw_points():
        screw = Part.makeCylinder(
            float(RETAINER_SCREW_CLEARANCE_D) / 2.0,
            float(RETAINER_RING_THK) + 0.4,
            Vector(sx, sy, z_bottom - 0.2),
        )
        retainer = retainer.cut(screw)

        if RETAINER_SCREW_COUNTERBORE_ENABLE and RETAINER_SCREW_COUNTERBORE_D > RETAINER_SCREW_CLEARANCE_D:
            cb = Part.makeCylinder(
                float(RETAINER_SCREW_COUNTERBORE_D) / 2.0,
                float(RETAINER_SCREW_COUNTERBORE_DEPTH) + 0.2,
                Vector(sx, sy, z_bottom + float(RETAINER_RING_THK) - float(RETAINER_SCREW_COUNTERBORE_DEPTH)),
            )
            retainer = retainer.cut(cb)

    return _safe_refine(retainer)


def cut_retainer_underside_seal_groove(shape, z_bottom, diffuser_pocket_r, lid_outer_r):
    if not DIFFUSER_SEAL_GROOVE_ENABLE or DIFFUSER_SEAL_GROOVE_W <= 0 or DIFFUSER_SEAL_GROOVE_DEPTH <= 0:
        return shape

    cx, cy = PAR_CENTER.x, PAR_CENTER.y
    groove_center_r = float(DIFFUSER_SEAL_GROOVE_CENTER_D) / 2.0
    groove_w = float(DIFFUSER_SEAL_GROOVE_W)
    groove_depth = float(DIFFUSER_SEAL_GROOVE_DEPTH)
    groove_outer_r = groove_center_r + (groove_w / 2.0)
    groove_inner_r = max(0.1, groove_center_r - (groove_w / 2.0))

    if groove_inner_r <= (float(diffuser_pocket_r) + 0.8) or groove_outer_r >= (float(lid_outer_r) - 0.8):
        print("WARNING: Lid-side silicone gutter is too close to the diffuser pocket/lip or outer wall.")
        return shape

    groove_outer = Part.makeCylinder(
        groove_outer_r,
        groove_depth + 0.2,
        Vector(cx, cy, z_bottom - 0.1),
    )
    groove_inner = Part.makeCylinder(
        groove_inner_r,
        groove_depth + 0.4,
        Vector(cx, cy, z_bottom - 0.2),
    )
    groove = groove_outer.cut(groove_inner)
    return _safe_refine(shape.cut(groove))


def bottom_foot_points():
    if BOTTOM_FEET_COUNT <= 0:
        return []

    step = 360.0 / float(BOTTOM_FEET_COUNT)
    return [
        _polar_xy(BOTTOM_FOOT_RADIUS, BOTTOM_FEET_ANGLE_OFFSET_DEG + (step * idx))
        for idx in range(int(BOTTOM_FEET_COUNT))
    ]


def make_bottom_mount_feet():
    if not BOTTOM_FEET_ENABLE:
        return None

    feet = None
    bridge_inner_r = (float(BASE_OD) / 2.0) - float(BOTTOM_FOOT_BRIDGE_OVERLAP)
    bridge_outer_r = float(BOTTOM_FOOT_RADIUS) + (float(BOTTOM_FOOT_OD) / 2.0)

    for fx, fy in bottom_foot_points():
        foot = Part.makeCylinder(
            float(BOTTOM_FOOT_OD) / 2.0,
            float(BOTTOM_FOOT_THK),
            Vector(fx, fy, 0.0),
        )

        bridge = make_radial_bridge(
            angle_deg=_angle_from_xy(fx, fy),
            inner_r=bridge_inner_r,
            outer_r=bridge_outer_r,
            width=float(BOTTOM_FOOT_BRIDGE_W),
            height=float(BOTTOM_FOOT_THK),
            z_bottom=0.0,
            rounded_ends=True,
        )
        if bridge is not None:
            foot = foot.fuse(bridge)

        hole = Part.makeCylinder(
            float(BOTTOM_FOOT_HOLE_D) / 2.0,
            float(BOTTOM_FOOT_THK) + 0.4,
            Vector(fx, fy, -0.2),
        )
        foot = foot.cut(hole)

        if BOTTOM_FOOT_COUNTERBORE_ENABLE and BOTTOM_FOOT_COUNTERBORE_D > BOTTOM_FOOT_HOLE_D:
            cb = Part.makeCylinder(
                float(BOTTOM_FOOT_COUNTERBORE_D) / 2.0,
                float(BOTTOM_FOOT_COUNTERBORE_DEPTH) + 0.2,
                Vector(fx, fy, float(BOTTOM_FOOT_THK) - float(BOTTOM_FOOT_COUNTERBORE_DEPTH)),
            )
            foot = foot.cut(cb)

        if feet is None:
            feet = foot
        else:
            feet = feet.fuse(foot)

    return _safe_refine(feet) if feet is not None else None


def make_sensor_pcb_reference(z_bottom):
    cx, cy = PAR_CENTER.x, PAR_CENTER.y
    ref_dx = float(SENSOR_PCB_PREVIEW_OFFSET_X)
    ref_dy = float(SENSOR_PCB_PREVIEW_OFFSET_Y)
    board_center_x = cx + ref_dx
    board_center_y = cy + ref_dy

    pcb = Part.makeBox(
        float(SENSOR_PCB_W),
        float(SENSOR_PCB_H),
        float(SENSOR_PCB_PREVIEW_THK),
        Vector(
            board_center_x - (float(SENSOR_PCB_W) / 2.0),
            board_center_y - (float(SENSOR_PCB_H) / 2.0),
            z_bottom,
        ),
    )

    if abs(float(SENSOR_PCB_ROT_DEG)) > 1e-6:
        pcb = _rotate_about_center(pcb, SENSOR_PCB_ROT_DEG)

    for bx, by in PAR_BOSS_OFFSET_XY:
        hole = Part.makeCylinder(
            float(SENSOR_PCB_MOUNT_HOLE_D) / 2.0,
            float(SENSOR_PCB_PREVIEW_THK) + 0.4,
            Vector(PAR_CENTER.x + bx + ref_dx, PAR_CENTER.y + by + ref_dy, z_bottom - 0.2),
        )
        pcb = pcb.cut(hole)

    sensor_local_x, sensor_local_y = _rotate_xy(SENSOR_PCB_OPTICAL_OFFSET_X, 0.0, SENSOR_PCB_ROT_DEG)
    # Small square on the reference PCB marks the optical IC center.
    sensor_mark = Part.makeBox(
        2.0,
        2.0,
        float(SENSOR_PCB_PREVIEW_THK) + 0.1,
        Vector(cx + ref_dx + sensor_local_x - 1.0, cy + ref_dy + sensor_local_y - 1.0, z_bottom),
    )
    pcb = pcb.fuse(sensor_mark)
    return _safe_refine(pcb)


def make_par_housing(base_top_z):
    cx, cy = PAR_CENTER.x, PAR_CENTER.y
    housing_id_r = max(0.1, (PAR_HOUSING_OD / 2.0) - PAR_HOUSING_WALL)

    outer = Part.makeCylinder(PAR_HOUSING_OD / 2.0, PAR_HOUSING_H, Vector(cx, cy, base_top_z))
    inner = Part.makeCylinder(housing_id_r, PAR_HOUSING_H + 0.5, Vector(cx, cy, base_top_z - 0.25))
    ring = outer.cut(inner)

    port_z = base_top_z + float(PAR_CABLE_PORT_Z_OFF)
    outer_r = float(PAR_HOUSING_OD) / 2.0
    wall_thk = float(PAR_HOUSING_WALL)

    gland_projection = float(CABLE_GLAND_PAD_PROJECTION) if CABLE_GLAND_PAD_ENABLE else 0.0
    gland_pad = make_gland_pad(port_z)
    if gland_pad is not None:
        ring = _safe_refine(ring.fuse(gland_pad))

    internal_flat = make_internal_gland_flat(port_z)
    if internal_flat is not None:
        ring = _safe_refine(ring.fuse(internal_flat))

    start_x = cx + outer_r + gland_projection + 0.8
    drill_len = wall_thk + float(PAR_CABLE_PORT_CUT_EXTRA)

    cable_cutter = Part.makeCylinder(
        float(PAR_CABLE_PORT_D) / 2.0,
        drill_len,
        Vector(start_x, cy, port_z),
        Vector(-1, 0, 0),
    )
    cable_cutter.rotate(Vector(cx, cy, 0), Vector(0, 0, 1), float(PAR_CABLE_PORT_ANGLE))
    ring = ring.cut(cable_cutter)

    diffuser_fit_r = None
    seat_inner_r = None

    if PAR_DIFFUSER_ENABLE:
        z_top = base_top_z + float(PAR_HOUSING_H)

        diffuser_r = float(PAR_DIFFUSER_D) / 2.0
        diffuser_fit_r = diffuser_r + (float(PAR_DIFFUSER_OD_CLEAR_D) / 2.0)
        seat_inner_r = max(0.1, diffuser_fit_r - float(PAR_DIFFUSER_LEDGE_OVERLAP))
        pocket_underside_z = z_top - float(PAR_DIFFUSER_RECESS_DEPTH)

        if PAR_DIFFUSER_LEDGE_ENABLE and PAR_DIFFUSER_LEDGE_H > 0 and PAR_DIFFUSER_LEDGE_OVERLAP > 0:
            ledge_outer_r = float(PAR_HOUSING_OD) / 2.0
            ledge_inner_r = seat_inner_r

            if ledge_outer_r > (ledge_inner_r + 0.15):
                ledge_outer = Part.makeCylinder(
                    ledge_outer_r,
                    float(PAR_DIFFUSER_LEDGE_H),
                    Vector(cx, cy, z_top - float(PAR_DIFFUSER_LEDGE_H)),
                )
                ledge_inner = Part.makeCylinder(
                    ledge_inner_r,
                    float(PAR_DIFFUSER_LEDGE_H) + 0.2,
                    Vector(cx, cy, z_top - float(PAR_DIFFUSER_LEDGE_H) - 0.1),
                )
                ledge = ledge_outer.cut(ledge_inner)
                ring = _safe_refine(ring.fuse(ledge))
            else:
                print("WARNING: PAR diffuser support ledge too narrow. Check diffuser size/clearance/overlap.")

        if PAR_DIFFUSER_POCKET_ENABLE and PAR_DIFFUSER_RECESS_DEPTH > 0:
            seat_depth = float(PAR_DIFFUSER_RECESS_DEPTH)
            pocket_underside_z = z_top - seat_depth
            seat_cut = Part.makeCylinder(
                diffuser_fit_r,
                seat_depth + 0.2,
                Vector(cx, cy, z_top - seat_depth - 0.1),
            )
            ring = _safe_refine(ring.cut(seat_cut))

            if PAR_DIFFUSER_POCKET_FLOOR_ENABLE and PAR_DIFFUSER_POCKET_FLOOR_THK > 0 and PAR_DIFFUSER_LEDGE_OVERLAP > 0:
                floor_top_z = z_top - seat_depth
                floor_thk = min(float(PAR_DIFFUSER_POCKET_FLOOR_THK), max(0.2, seat_depth - 0.2))
                floor_outer_r = diffuser_fit_r
                floor_inner_r = max(0.1, seat_inner_r)
                pocket_underside_z = floor_top_z - floor_thk

                if floor_outer_r > (floor_inner_r + 0.15):
                    floor_outer = Part.makeCylinder(
                        floor_outer_r,
                        floor_thk,
                        Vector(cx, cy, floor_top_z - floor_thk),
                    )
                    floor_inner = Part.makeCylinder(
                        floor_inner_r,
                        floor_thk + 0.2,
                        Vector(cx, cy, floor_top_z - floor_thk - 0.1),
                    )
                    floor_ring = floor_outer.cut(floor_inner)
                    ring = _safe_refine(ring.fuse(floor_ring))

        if (
            PAR_DIFFUSER_ANTI_OVERHANG_ENABLE
            and PAR_DIFFUSER_RECESS_DEPTH > 0
            and PAR_DIFFUSER_LEDGE_ENABLE
            and PAR_DIFFUSER_LEDGE_OVERLAP > 0
            and PAR_DIFFUSER_ANTI_OVERHANG_H > 0
        ):
            seat_depth = float(PAR_DIFFUSER_RECESS_DEPTH)
            support_h = float(PAR_DIFFUSER_ANTI_OVERHANG_H)
            support_top_z = pocket_underside_z
            draft = math.tan(math.radians(float(PAR_DIFFUSER_ANTI_OVERHANG_ANGLE_DEG)))
            inner_top_r = seat_inner_r
            inner_bottom_target_r = max(inner_top_r + 0.1, housing_id_r - float(PAR_DIFFUSER_ANTI_OVERHANG_WALL_CLEAR))
            radial_span = inner_bottom_target_r - inner_top_r
            max_h = max(0.6, float(PAR_HOUSING_H) - seat_depth - 0.6)

            wall_hit_drop = float(PAR_DIFFUSER_ANTI_OVERHANG_WALL_HIT_DROP)
            if wall_hit_drop > 0:
                support_h = min(max_h, wall_hit_drop)
                inner_bottom_r = inner_bottom_target_r
            else:
                if draft > 1e-6:
                    required_h = radial_span / draft
                else:
                    required_h = support_h

                support_h = min(max_h, max(support_h, required_h))
                inner_bottom_r = inner_top_r + (support_h * draft)
                inner_bottom_r = min(inner_bottom_r, inner_bottom_target_r)

            support_base_z = support_top_z - support_h

            if inner_bottom_r > (inner_top_r + 0.1) and support_h > 0.2:
                support_outer = Part.makeCylinder(
                    housing_id_r,
                    support_h,
                    Vector(cx, cy, support_base_z),
                )
                support_inner = Part.makeCone(
                    inner_bottom_r,
                    inner_top_r,
                    support_h + 0.2,
                    Vector(cx, cy, support_base_z - 0.1),
                )
                support_slope = support_outer.cut(support_inner)
                ring = _safe_refine(ring.fuse(support_slope))

        if PAR_DIFFUSER_RETAINER_ENABLE and PAR_DIFFUSER_RETAINER_H > 0 and PAR_DIFFUSER_RETAINER_WALL > 0:
            collar_id_r = max(diffuser_fit_r, housing_id_r + 0.05)
            collar_od_r_raw = collar_id_r + float(PAR_DIFFUSER_RETAINER_WALL)
            collar_od_r_max = (float(PAR_HOUSING_OD) / 2.0) - float(PAR_DIFFUSER_RETAINER_RADIAL_MARGIN)
            collar_od_r = min(collar_od_r_raw, collar_od_r_max)

            if collar_od_r > (collar_id_r + 0.2):
                collar_outer = Part.makeCylinder(collar_od_r, float(PAR_DIFFUSER_RETAINER_H), Vector(cx, cy, z_top))
                collar_inner = Part.makeCylinder(collar_id_r, float(PAR_DIFFUSER_RETAINER_H) + 0.2, Vector(cx, cy, z_top - 0.1))
                collar = collar_outer.cut(collar_inner)
                ring = _safe_refine(ring.fuse(collar))
            else:
                print("WARNING: PAR diffuser retainer collar could not be created. Check PAR_HOUSING_OD / PAR_HOUSING_WALL / diffuser size.")

        ring = _safe_refine(ring.cut(cable_cutter))

    par_support = None
    boss_points = []

    for dx, dy in PAR_BOSS_OFFSET_XY:
        bx = cx + dx
        by = cy + dy
        boss_points.append((bx, by))

        boss = Part.makeCylinder(PAR_BOSS_DIAM / 2.0, PAR_BOSS_HEIGHT, Vector(bx, by, base_top_z))

        if PAR_BOSS_ROOT_FLARE_ENABLE and PAR_BOSS_ROOT_FLARE_H > 0 and PAR_BOSS_ROOT_FLARE_EXTRA_R > 0:
            root_flare = Part.makeCone(
                (PAR_BOSS_DIAM / 2.0) + float(PAR_BOSS_ROOT_FLARE_EXTRA_R),
                (PAR_BOSS_DIAM / 2.0),
                float(PAR_BOSS_ROOT_FLARE_H),
                Vector(bx, by, base_top_z),
            )
            boss = _safe_refine(boss.fuse(root_flare))

        if PAR_BOSS_BASE_FILLET_R > 0:
            base_r = PAR_BOSS_DIAM / 2.0
            if PAR_BOSS_ROOT_FLARE_ENABLE and PAR_BOSS_ROOT_FLARE_EXTRA_R > 0:
                base_r = (PAR_BOSS_DIAM / 2.0) + float(PAR_BOSS_ROOT_FLARE_EXTRA_R)

            boss = _fillet_standoff_base_before_fuse(
                standoff_shape=boss,
                x=bx,
                y=by,
                base_r=base_r,
                z_target=base_top_z,
                fillet_r=float(PAR_BOSS_BASE_FILLET_R),
                tol_z=1.0,
                tol_r=1.5,
            )

        if par_support is None:
            par_support = boss
        else:
            par_support = par_support.fuse(boss)

    if PAR_BOSS_CROSS_GUSSET_ENABLE and len(boss_points) >= 2:
        xs = [p[0] for p in boss_points]
        ys = [p[1] for p in boss_points]
        x_min = min(xs)
        x_max = max(xs)
        y_min = min(ys)
        y_max = max(ys)
        web_h = min(float(PAR_BOSS_CROSS_GUSSET_H), float(PAR_BOSS_HEIGHT))

        if (x_max - x_min) >= (y_max - y_min) and (x_max - x_min) > 0.2:
            web = Part.makeBox(
                x_max - x_min,
                float(PAR_BOSS_CROSS_GUSSET_THK),
                web_h,
                Vector(x_min, cy - (float(PAR_BOSS_CROSS_GUSSET_THK) / 2.0), base_top_z),
            )
            par_support = par_support.fuse(web)
        elif (y_max - y_min) > 0.2:
            web = Part.makeBox(
                float(PAR_BOSS_CROSS_GUSSET_THK),
                y_max - y_min,
                web_h,
                Vector(cx - (float(PAR_BOSS_CROSS_GUSSET_THK) / 2.0), y_min, base_top_z),
            )
            par_support = par_support.fuse(web)

    for bx, by in boss_points:
        hole = Part.makeCylinder(
            PAR_SCREW_DIAMETER / 2.0,
            PAR_BOSS_HEIGHT + 0.5,
            Vector(bx, by, base_top_z - 0.25),
        )
        par_support = par_support.cut(hole)

    body = ring.fuse(par_support)

    if RETAINER_RING_ENABLE:
        retainer_pads = make_retainer_insert_pads(base_top_z + float(PAR_HOUSING_H))
        if retainer_pads is not None:
            body = body.fuse(retainer_pads)
            body = cut_retainer_insert_bores(body, base_top_z + float(PAR_HOUSING_H))

    return _safe_refine(body), diffuser_fit_r, seat_inner_r


base = Part.makeCylinder(BASE_OD / 2.0, BASE_THK, Vector(PAR_CENTER.x, PAR_CENTER.y, 0))
bottom_feet = make_bottom_mount_feet()
if bottom_feet is not None:
    base = _safe_refine(base.fuse(bottom_feet))

housing, diffuser_fit_r, seat_inner_r = make_par_housing(BASE_THK)

body = _safe_refine(base.fuse(housing))
body_obj = doc.addObject("Part::Feature", "AS7341_PAR_Standalone_Housing")
body_obj.Shape = body

if SENSOR_PCB_PREVIEW_ENABLE:
    pcb_reference_z = BASE_THK + PAR_BOSS_HEIGHT + 0.2
    pcb_reference = make_sensor_pcb_reference(pcb_reference_z)
    pcb_obj = doc.addObject("Part::Feature", "AS734x_PCB_reference_not_printed")
    pcb_obj.Shape = pcb_reference

retainer_obj = None
if RETAINER_RING_ENABLE:
    retainer_z = BASE_THK + PAR_HOUSING_H + RETAINER_PREVIEW_GAP
    retainer = make_retainer_ring(retainer_z)
    retainer_obj = doc.addObject("Part::Feature", "Diffuser_Retainer_Ring")
    retainer_obj.Shape = retainer

if DIFFUSER_PREVIEW_ENABLE:
    diffuser_z = BASE_THK + PAR_HOUSING_H - PAR_DIFFUSER_THK
    diffuser = Part.makeCylinder(PAR_DIFFUSER_D / 2.0, PAR_DIFFUSER_THK, Vector(PAR_CENTER.x, PAR_CENTER.y, diffuser_z))
    diffuser_obj = doc.addObject("Part::Feature", "DiffuserDisc_reference_not_printed")
    diffuser_obj.Shape = diffuser

doc.recompute()

exported_stl_paths = [
    export_printable_stl(body_obj, "as7341_par_housing.stl"),
]
if retainer_obj is not None:
    exported_stl_paths.append(export_printable_stl(retainer_obj, "as7341_par_diffuser_retainer_ring.stl"))

try:
    import FreeCADGui as Gui

    Gui.ActiveDocument.ActiveView.viewAxometric()
    Gui.SendMsgToActiveView("ViewFit")
except Exception:
    pass


def report_check(label, value, minimum=None, maximum=None, unit="mm"):
    status = "OK"
    if minimum is not None and value < minimum:
        status = "WARN"
    if maximum is not None and value > maximum:
        status = "WARN"

    limit = ""
    if minimum is not None:
        limit += " min {:.2f}".format(float(minimum))
    if maximum is not None:
        limit += " max {:.2f}".format(float(maximum))

    print("{:<34} {:>7.2f} {} [{}{}]".format(label + ":", float(value), unit, status, limit))


print("\n---- AS7341 PAR Standalone Housing ----")
print("Base OD / thickness:           {:.2f} / {:.2f} mm".format(BASE_OD, BASE_THK))
print("Housing OD / wall / height:    {:.2f} / {:.2f} / {:.2f} mm".format(PAR_HOUSING_OD, PAR_HOUSING_WALL, PAR_HOUSING_H))
print("Overall height:                {:.2f} mm".format(BASE_THK + PAR_HOUSING_H))
print("Diffuser target D:             {:.2f} mm".format(PAR_DIFFUSER_D))
print("Pocket cut D (effective):      {:.2f} mm".format(PAR_DIFFUSER_D + PAR_DIFFUSER_OD_CLEAR_D))
print("Pocket recess depth:           {:.2f} mm".format(PAR_DIFFUSER_RECESS_DEPTH))
print("Nominal aperture target D:     {:.2f} mm".format(PAR_APERTURE_D))
if seat_inner_r is not None:
    print("Current ledge aperture D:      {:.2f} mm".format(2.0 * seat_inner_r))
if len(PAR_BOSS_OFFSET_XY) >= 2:
    b0 = PAR_BOSS_OFFSET_XY[0]
    b1 = PAR_BOSS_OFFSET_XY[1]
    boss_spacing = math.hypot(b1[0] - b0[0], b1[1] - b0[1])
    print("PCB size W/H:                  {:.2f} / {:.2f} mm".format(SENSOR_PCB_W, SENSOR_PCB_H))
    print("PCB hole spacing target/CAD:   {:.2f} / {:.2f} mm".format(SENSOR_PCB_MOUNT_HOLE_SPACING, boss_spacing))
    print("PCB post height / top Z:       {:.2f} / {:.2f} mm".format(PAR_BOSS_HEIGHT, BASE_THK + PAR_BOSS_HEIGHT))
    print("PCB optical offset X:          {:.2f} mm".format(SENSOR_PCB_OPTICAL_OFFSET_X))
    print("PCB boss offsets from center:  ({:.2f}, {:.2f}) / ({:.2f}, {:.2f}) mm".format(
        b0[0],
        b0[1],
        b1[0],
        b1[1],
    ))
print("Gland port D / angle / Z:      {:.2f} mm / {:.1f} deg / {:.2f} mm".format(
    PAR_CABLE_PORT_D,
    PAR_CABLE_PORT_ANGLE,
    BASE_THK + PAR_CABLE_PORT_Z_OFF,
))
if CABLE_GLAND_PAD_ENABLE:
    print("Gland pad OD / projection:     {:.2f} / {:.2f} mm".format(CABLE_GLAND_PAD_OD, CABLE_GLAND_PAD_PROJECTION))
    internal_flat_projection = CABLE_GLAND_INTERNAL_FLAT_PROJECTION if CABLE_GLAND_INTERNAL_FLAT_ENABLE else 0.0
    print("Internal gland flat OD/proj:   {:.2f} / {:.2f} mm".format(
        CABLE_GLAND_INTERNAL_FLAT_OD if CABLE_GLAND_INTERNAL_FLAT_ENABLE else 0.0,
        internal_flat_projection,
    ))
    print("Gland boss panel thickness:    {:.2f} mm".format(PAR_HOUSING_WALL + CABLE_GLAND_PAD_PROJECTION + internal_flat_projection))
    print("PG7 measured thread length:    {:.2f} mm".format(PG7_THREAD_LENGTH))
    print("PG7 locknut AF / swing D:      {:.2f} / {:.2f} mm".format(PG7_LOCKNUT_AF, pg7_locknut_swing_d()))
if RETAINER_RING_ENABLE:
    max_lug_d = 2.0 * (float(RETAINER_SCREW_RADIUS) + (float(RETAINER_LUG_OD) / 2.0))
    print("Retainer ring thickness:       {:.2f} mm".format(RETAINER_RING_THK))
    print("Retainer screw count / PCD:    {} / {:.2f} mm".format(RETAINER_SCREW_COUNT, 2.0 * RETAINER_SCREW_RADIUS))
    print("Retainer max lug footprint:    {:.2f} mm".format(max_lug_d))
    print("Retainer lug overlap:          {:.2f} mm".format(RETAINER_LUG_OVERLAP))
    print("Retainer lug edge fillet R:    {:.2f} mm".format(RETAINER_LUG_EDGE_FILLET_R))
    print("Retainer lug round neck D:     {:.2f} mm".format(RETAINER_LUG_ROUND_NECK_D if RETAINER_LUG_ROUND_NECK_ENABLE else 0.0))
    print("Retainer root bridges:         {}".format("enabled" if RETAINER_LUG_ROOT_BRIDGE_ENABLE else "disabled"))
    print("Insert bore D / depth:         {:.2f} / {:.2f} mm".format(RETAINER_INSERT_BORE_D, RETAINER_INSERT_DEPTH))
    if DIFFUSER_SEAL_GROOVE_ENABLE:
        diffuser_pocket_r = (float(PAR_DIFFUSER_D) + float(PAR_DIFFUSER_OD_CLEAR_D)) / 2.0
        groove_inner_r = (float(DIFFUSER_SEAL_GROOVE_CENTER_D) - float(DIFFUSER_SEAL_GROOVE_W)) / 2.0
        groove_outer_r = (float(DIFFUSER_SEAL_GROOVE_CENTER_D) + float(DIFFUSER_SEAL_GROOVE_W)) / 2.0
        groove_gap_from_pocket = groove_inner_r - diffuser_pocket_r
        groove_gap_to_outer = (float(PAR_HOUSING_OD) / 2.0) - groove_outer_r
        print("Lid silicone groove D/W/D:     {:.2f} / {:.2f} / {:.2f} mm".format(
            DIFFUSER_SEAL_GROOVE_CENTER_D,
            DIFFUSER_SEAL_GROOVE_W,
            DIFFUSER_SEAL_GROOVE_DEPTH,
        ))
        print("Gutter gap lip/outer wall:     {:.2f} / {:.2f} mm".format(
            groove_gap_from_pocket,
            groove_gap_to_outer,
        ))

print("\n---- Printability / Mounting Check ----")
diffuser_pocket_r = (float(PAR_DIFFUSER_D) + float(PAR_DIFFUSER_OD_CLEAR_D)) / 2.0
diffuser_actual_r = float(PAR_DIFFUSER_D) / 2.0
groove_inner_r = (float(DIFFUSER_SEAL_GROOVE_CENTER_D) - float(DIFFUSER_SEAL_GROOVE_W)) / 2.0
groove_outer_r = (float(DIFFUSER_SEAL_GROOVE_CENTER_D) + float(DIFFUSER_SEAL_GROOVE_W)) / 2.0
groove_gap_from_pocket = groove_inner_r - diffuser_pocket_r
groove_gap_to_outer = (float(PAR_HOUSING_OD) / 2.0) - groove_outer_r
top_land_w = (float(PAR_HOUSING_OD) / 2.0) - diffuser_pocket_r
ledge_aperture_r = seat_inner_r if seat_inner_r is not None else float(PAR_APERTURE_D) / 2.0
diffuser_radial_clearance = diffuser_pocket_r - diffuser_actual_r
diffuser_glue_ledge_w = diffuser_actual_r - ledge_aperture_r
pcb_access_margin = ledge_aperture_r - ((float(SENSOR_PCB_MOUNT_HOLE_SPACING) / 2.0) + (float(PAR_SCREW_DIAMETER) / 2.0))
retainer_lug_wall = (float(RETAINER_LUG_OD) - float(RETAINER_INSERT_BORE_D)) / 2.0
insert_depth_margin = float(RETAINER_INSERT_PAD_H) - float(RETAINER_INSERT_DEPTH)
lid_groove_floor = float(RETAINER_RING_THK) - float(DIFFUSER_SEAL_GROOVE_DEPTH)
retainer_cb_floor = float(RETAINER_RING_THK) - float(RETAINER_SCREW_COUNTERBORE_DEPTH)
gland_pad_wall = (float(CABLE_GLAND_PAD_OD) - float(PAR_CABLE_PORT_D)) / 2.0
internal_flat_projection = float(CABLE_GLAND_INTERNAL_FLAT_PROJECTION) if CABLE_GLAND_INTERNAL_FLAT_ENABLE else 0.0
gland_panel_thk = float(PAR_HOUSING_WALL) + float(CABLE_GLAND_PAD_PROJECTION) + internal_flat_projection
pg7_thread_margin = float(PG7_THREAD_LENGTH) - gland_panel_thk
pg7_locknut_r = pg7_locknut_swing_d() / 2.0
top_reserved_h = diffuser_top_reserved_h(seat_inner_r)
pg7_locknut_lower_clearance = float(PAR_CABLE_PORT_Z_OFF) - pg7_locknut_r
pg7_locknut_upper_clearance = float(PAR_HOUSING_H) - float(PAR_CABLE_PORT_Z_OFF) - pg7_locknut_r - top_reserved_h
internal_flat_wall = (float(CABLE_GLAND_INTERNAL_FLAT_OD) - pg7_locknut_swing_d()) / 2.0 if CABLE_GLAND_INTERNAL_FLAT_ENABLE else 0.0

report_check("Diffuser outer land", top_land_w, minimum=3.0)
report_check("Diffuser radial clearance", diffuser_radial_clearance, minimum=0.20, maximum=0.45)
report_check("Diffuser glue ledge width", diffuser_glue_ledge_w, minimum=3.0)
report_check("PCB screw top access margin", pcb_access_margin, minimum=2.0)
report_check("Lid groove floor left", lid_groove_floor, minimum=1.6)
report_check("Gutter gap from diffuser lip", groove_gap_from_pocket if DIFFUSER_SEAL_GROOVE_ENABLE else 0.0, minimum=1.2)
report_check("Gutter gap to outer wall", groove_gap_to_outer if DIFFUSER_SEAL_GROOVE_ENABLE else 0.0, minimum=1.2)
report_check("Retainer insert wall", retainer_lug_wall, minimum=2.5)
report_check("Insert depth margin", insert_depth_margin, minimum=1.0)
report_check("Retainer counterbore floor", retainer_cb_floor, minimum=1.2)
report_check("Gland pad radial wall", gland_pad_wall, minimum=2.0)
report_check("Internal PG7 flat radial land", internal_flat_wall, minimum=1.0)
report_check("PG7 thread margin", pg7_thread_margin, minimum=2.0)
report_check("PG7 internal lower nut clearance", pg7_locknut_lower_clearance, minimum=2.0)
report_check("PG7 internal upper nut clearance", pg7_locknut_upper_clearance, minimum=2.0)
if RETAINER_LUG_ROOT_BRIDGE_ENABLE:
    report_check("Lug bridge width", RETAINER_LUG_ROOT_BRIDGE_W, minimum=4.0)
else:
    report_check("Round lug body overlap", RETAINER_LUG_OVERLAP, minimum=3.0)

if BOTTOM_FEET_ENABLE:
    foot_wall = (float(BOTTOM_FOOT_OD) - float(BOTTOM_FOOT_HOLE_D)) / 2.0
    foot_cb_floor = float(BOTTOM_FOOT_THK) - float(BOTTOM_FOOT_COUNTERBORE_DEPTH)
    foot_footprint = 2.0 * (float(BOTTOM_FOOT_RADIUS) + (float(BOTTOM_FOOT_OD) / 2.0))
    print("\nBottom feet:")
    report_check("Foot hole wall", foot_wall, minimum=3.0)
    report_check("Foot counterbore floor", foot_cb_floor, minimum=1.2)
    print("Unified mount PCD:             {:.2f} mm".format(2.0 * BOTTOM_FOOT_RADIUS))
    print("Foot OD / footprint:           {:.2f} / {:.2f} mm".format(BOTTOM_FOOT_OD, foot_footprint))

print("\nPrint orientation notes:")
print("- Print housing upright with bottom feet on the bed; diffuser pocket faces up.")
print("- Print retainer ring flat, underside groove facing the bed only if your printer handles shallow circular grooves cleanly; otherwise flip it groove-up.")
print("- The PG7 gland hole may need light reaming after printing; use a gasket/O-ring on the flat boss face.")
if EXPORT_STL:
    print("\nSTL exports:")
    for path in exported_stl_paths:
        if path is not None:
            print("- {}".format(path))
