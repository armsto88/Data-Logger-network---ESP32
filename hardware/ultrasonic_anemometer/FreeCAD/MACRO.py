import math
import random
import FreeCAD as App
import Part
from FreeCAD import Vector

# ============================================================
# Ultrasonic Anemometer Head (FreeCAD Python)
#  - 4 tilted pods (45/135/225/315)
#  - Shoulder-retained transducer bores + silicone pocket
#  - Roof with holes + PAR housing
#  - Roof standoffs (0/90/180/270) with TOP heat-set inserts (roof screws)
#  - Belly cup with:
#       * internal fastening tubes aligned with pods (45/135/225/315)
#       * heat-set inserts in those tubes (from top)
#       * no holes through belly floor
#       * wall ports + protrusions (GX12 cluster = single protrusion)
#       * inside ALSO gets matching flat protrusion (inner boss)
#       * PCB standoffs on belly floor
# ============================================================

# ---------------- Exploded view (visual only) ----------------
EXPLODED_VIEW = False
EXPLODE_LOWER_DZ = 0.0
EXPLODE_ROOF_DZ  = 35.0
EXPLODE_PAN_DZ   = -105.0

# ---------------- PARAMETERS ----------------
HEAD_DIAM = 138.0
ROOF_DIAM = 138.0
PLATE_THK = 6.0
PLATE_DROP = 10.0   # mm to extend plate downward (tune)
PLATE_TOP_FILLET_R = 4.0   # try 3–6 mm

POD_RADIUS   = 39.2
POD_DIAM     = 24.0
POD_HEIGHT   = 12.0
POD_TILT_DEG = 32.3
POD_THROUGH  = 5.0

# ---- TOF diagnostic assumptions (print-only; no geometry changes) ----
TOF_AIR_TEMP_C = 20.0
TOF_SAMPLE_WINDS_MPS = [1.0, 5.0, 10.0]
TOF_BEAM_TRACE_ENABLE = True
TOF_BEAM_TRACE_HALF_ANGLES = [25.0, 30.0]
TOF_BEAM_TRACE_RAYS = 4000
TOF_BEAM_TRACE_RECEIVER_RADII_MM = [8.0, 16.0, 25.0]
TOF_BEAM_WEIGHT_EXPONENTS = [6, 10]
TOF_BEAM_TRACE_YAW_DEG = 0.0
TOF_BEAM_TRACE_SEED = 42
TOF_BLOCK_CHECK_ENABLE = True
TOF_BORE_EFFECTIVE_LEN_MM = 8.0
TOF_REFLECTOR_TEST_MODES = ["flat", "dish", "cone"]
TOF_REFLECTOR_DISH_DEPTH_MM = 1.5
TOF_REFLECTOR_DISH_DIAM_MM = 40.0
TOF_REFLECTOR_CONE_DEPTH_MM = 1.5
TOF_REFLECTOR_CONE_DIAM_MM = 40.0

# ---- Transducer geometry ----
TRANSDUCER_CAN_D  = 16.0
TRANSDUCER_NECK_D = 12.0

SENSOR_TOTAL_LEN  = 12.3
SENSOR_NECK_LEN   = 4.0
SENSOR_BODY_LEN   = SENSOR_TOTAL_LEN - SENSOR_NECK_LEN

# Clearances
CAN_CLEARANCE  = 0.3
NECK_CLEARANCE = 0.3
BORE_MAIN_D = TRANSDUCER_CAN_D  + CAN_CLEARANCE
BORE_NECK_D = TRANSDUCER_NECK_D + NECK_CLEARANCE

# Bottom entry lead-in
BOTTOM_ENTRY_CHAMFER_H       = 1.2
BOTTOM_ENTRY_CHAMFER_EXTRA_R = 0.6

# Silicone bead pocket
SIL_SEAL_ENABLE  = True
SIL_SEAL_DEPTH   = 1.2
SIL_SEAL_EXTRA_R = 0.8

# Extend bores/pocket below opening plane (fix clipped trench)
BORE_EXT_BELOW_OPEN   = 2.5
SIL_POCKET_EXT_BELOW  = 1.5

# --- Roof placement ---
ROOF_GAP_ABOVE_PODS = 50.0
ROOF_THK = 10.0

# ---------------- Reflector parameters (legacy/reserved; flat roof currently acts as reflector) ----------------
REFLECTOR_DEPTH = 8.0
REFLECTOR_R_TOP = 50.0
REFLECTOR_R_TIP = 6.0
TIP_FLAT_R      = 6.0
TIP_FLAT_D      = 1.0

# ---------------- Standoffs / inserts ----------------
STANDOFF_ANGLES_BASE  = [0, 90, 180, 270]
STANDOFF_ANGLE_OFFSET = 0.0

# Strength targets
STANDOFF_OD = 12.0                 # was 8.0, now ~10–12 mm
STANDOFF_MIN_INSERT_WALL = 1.8     # target 1.5–2.0mm wall around heat-set bore
STANDOFF_BASE_FILLET_R = 1.6       # try 1.2–2.5 (too big can fail)

# --- IMPORTANT: split roof vs belly bolt circles ---
ROOF_STANDOFF_RIM_MARGIN = 7.0     # <-- what you want for roof standoffs
BELLY_FASTEN_RIM_MARGIN  = 3.0     # <-- what you want for belly fasteners

TOP_INSERT_BORE_D         = 4.6
TOP_INSERT_DEPTH          = 5.0
TOP_INSERT_LEADIN_CHAMFER = 0.6

# --- Teardrop standoff geometry (legacy/reserved; pillar builder is currently cylindrical) ---
STANDOFF_SHAPE = "teardrop"        # "cyl" or "teardrop"

TEARDROP_WIDTH  = STANDOFF_OD
TEARDROP_LENGTH = STANDOFF_OD * 1.15
TEARDROP_NOSE_R = TEARDROP_WIDTH * 0.50
TEARDROP_TAPER_EXP = 1.2

TEARDROP_ORIENT_MODE = "radial_out"
TEARDROP_YAW_OFFSET_DEG = 0.0

# Root reinforcement (optional; improves strength where pillar meets plate)
STANDOFF_ROOT_FLARE_ENABLE = True
STANDOFF_ROOT_FLARE_H = 4.0
STANDOFF_ROOT_FLARE_EXTRA_R = 2.0

# ---------------- Roof holes (standoff -> roof plate) ----------------
ROOF_HOLE_D       = 3.4
ADD_COUNTERBORE   = True
COUNTERBORE_D     = 7.0
COUNTERBORE_DEPTH = 2.5

# ---------------- Belly cup ----------------
BELLY_ENABLE   = True
BELLY_BASE_THK = 3.0
BELLY_WALL_H   = 62.0
BELLY_WALL_THK = 6.0

# Beef up internal belly fastening tubes:
BELLY_TUBE_OD      = 12.0
BELLY_FLOOR_HOLES_ENABLE = False   # no holes through belly floor

# ---------------- Drip edge (external skirt under rim) ----------------
DRIP_EDGE_ENABLE   = False
DRIP_EDGE_DROP     = 3.0
DRIP_EDGE_OUTSET   = 1.5
DRIP_EDGE_OVERLAP  = 0.25

# ---------------- Belly seal groove ----------------
BELLY_SEAL_ENABLE       = True
BELLY_SEAL_CORD_D       = 2.0
BELLY_SEAL_GROOVE_W     = 2.2
BELLY_SEAL_GROOVE_DEPTH = 1.5
BELLY_SEAL_CLEAR_TUBE        = 0.8
BELLY_SEAL_CENTER_IN_WALL    = True
BELLY_SEAL_MIN_OUTER_MARGIN  = 2.0

# ---------------- Belly fastening pillars (inside belly) ----------------
BELLY_FASTEN_OFFSET = 35.0
BELLY_FASTEN_ANGLES = [a + BELLY_FASTEN_OFFSET for a in [43,133,223,313]]

# ---------------- Plate-to-belly bolt holes ----------------
PLATE_BOLT_HOLE_D       = 3.4   # M3 clearance
PLATE_BOLT_CB_ENABLE    = True
PLATE_BOLT_CB_D         = 7.0
PLATE_BOLT_CB_DEPTH     = 2.5

# ---------------- Wall ports ----------------
CONNECTORS_ENABLE = True

GX12_HOLE_D     = 11.0
GX12_NUT_D      = 15.0
GX12_QTY = 4                    # legacy/reserved (current placement derives from row angles below)

GX12_ROW_CENTER_ANGLE = 320.0
GX12_ROW_SPACING_DEG  = 25.0
GX12_ROW_OFFSET_DEG   = -15.0
GX12_Z_CENTER         = -25.0
GX12_PAIR_SEPARATION_DEG = 210.0  # legacy/reserved (not currently used in gx12_angles build)

PG7_HOLE_D    = 13.0
PG7_NUT_D     = 17.9
PG7_ANGLE_DEG = 225.0

SHT_HOLE_D    = 11.5
SHT_NUT_D     = 15.0
SHT_ANGLE_DEG = 45.0

PORT_Z_CENTER        = -27.0
PORT_CUT_EXTRA       = 2.0
PORT_AVOID_STANDOFF_DEG = 12.0

# ---------------- Protrusions / pads ----------------
PORT_PADS_ENABLE = True
PORT_PAD_EXTRA_W = 5.0
PORT_PAD_EXTRA_H = 5.0
PORT_PAD_THK     = 2.0
PORT_PAD_INSET   = 0.8

PORT_PAD_CHAMFER_Z   = 3.0
PORT_PAD_CHAMFER_RUN = 3.5

PORT_PAD_INNER_ENABLE = True
PORT_PAD_INNER_THK    = 3.0

GX12_CLUSTER_PAD_ENABLE     = True
GX12_CLUSTER_PAD_ANG_MARGIN = 6.0

# ---------------- PCB mounts (inside belly) ----------------
PCB_ENABLE = True
PCB_W      = 84.2
PCB_H      = 84.2
PCB_HOLE_D = 3.2

PCB_EDGE_MARGIN = 4.1   # (84.2 - 76.0)/2 to match 76mm hole-center spacing
PCB_HOLES = [
    (PCB_EDGE_MARGIN, PCB_EDGE_MARGIN),
    (PCB_W - PCB_EDGE_MARGIN, PCB_EDGE_MARGIN),
    (PCB_W - PCB_EDGE_MARGIN, PCB_H - PCB_EDGE_MARGIN),
    (PCB_EDGE_MARGIN, PCB_H - PCB_EDGE_MARGIN),
]

PCB_POST_OD          = 8.0
PCB_STANDOFF_H       = 20.0
PCB_INSERT_BORE_D    = 4.6
PCB_INSERT_DEPTH     = 5.0
PCB_INSERT_CHAMFER_H = 0.6
PCB_OFFSET_X = 0.0
PCB_OFFSET_Y = 0.0

PCB_ROT_DEG = BELLY_FASTEN_OFFSET + 45.0

# ---------------- Battery envelope check (diagnostic only) ----------------
BATTERY_CHECK_ENABLE = True
BATTERY_L_MM = 100.0
BATTERY_W_MM = 60.0
BATTERY_H_MM = 12.0

# ---------------- SHT "CIRCULAR ROOF" (NO GILLS) ----------------
SHT_RS_ENABLE = False
SHT_CANOPY_OD = 58.0
SHT_CANOPY_THK = 2.0
SHT_CANOPY_RAISE = 10.0
SHT_CANOPY_OUTSET = 8.0
SHT_CANOPY_STRUT_W = 4.0
SHT_CANOPY_STRUT_Z = 6.0
SHT_CANOPY_STRUT_Y_OFFSET = (SHT_NUT_D / 2.0) + 6.0
SHT_Z = -BELLY_WALL_H / 2.0

# ---------------- PAR / Spectral sensor housing ----------------
PAR_ENABLE = True
PAR_CENTER = Vector(0, 0, 0)

PAR_HOUSING_WALL = 3.0
PAR_HOUSING_OD   = 50.0
PAR_HOUSING_H    = 20.0
PAR_APERTURE_D   = 30.0

PAR_BOSS_OFFSET_XY = [(0.0, 9.3), (0.0, -9.3)]
PAR_BOSS_HEIGHT    = 10.0
PAR_BOSS_DIAM      = 7.0
PAR_SCREW_DIAMETER = 3.1
PAR_BOSS_ROOT_FLARE_ENABLE  = True
PAR_BOSS_ROOT_FLARE_H       = 2.5
PAR_BOSS_ROOT_FLARE_EXTRA_R = 1.8
PAR_BOSS_BASE_FILLET_R = 1.2
PAR_BOSS_CROSS_GUSSET_ENABLE = True
PAR_BOSS_CROSS_GUSSET_THK    = 2.5
PAR_BOSS_CROSS_GUSSET_H      = 6.0

# PAR diffuser (top-mounted disc)
PAR_DIFFUSER_ENABLE         = True
PAR_DIFFUSER_D              = 38.6
PAR_DIFFUSER_THK            = 1.6
PAR_DIFFUSER_POCKET_ENABLE  = True
PAR_DIFFUSER_RECESS_DEPTH   = 1.6   # pocket depth for seated diffuser
PAR_DIFFUSER_OD_CLEAR_D     = 0.6   # diametral assembly clearance (fit-safe for 38.6mm disc)
PAR_DIFFUSER_LEDGE_ENABLE   = True
PAR_DIFFUSER_LEDGE_H        = 1.6   # seat depth (match diffuser thickness)
PAR_DIFFUSER_LEDGE_OVERLAP  = 5.0   # radial glue/support land under diffuser edge
PAR_DIFFUSER_POCKET_FLOOR_ENABLE = True
PAR_DIFFUSER_POCKET_FLOOR_THK    = 0.8
PAR_DIFFUSER_ANTI_OVERHANG_ENABLE    = True
PAR_DIFFUSER_ANTI_OVERHANG_H         = 5.0   # minimum vertical drop for underside support slope
PAR_DIFFUSER_ANTI_OVERHANG_ANGLE_DEG = 30.0  # gentle support slope angle toward wall
PAR_DIFFUSER_ANTI_OVERHANG_WALL_CLEAR = 0.3  # keep a small inner wall thickness margin
PAR_DIFFUSER_ANTI_OVERHANG_WALL_HIT_DROP = 0.0  # 0 = auto-depth to wall (stronger print support)
PAR_DIFFUSER_RETAINER_ENABLE = False
PAR_DIFFUSER_RETAINER_WALL  = 1.0
PAR_DIFFUSER_RETAINER_H     = 1.6   # retaining lip height (match diffuser thickness)
PAR_DIFFUSER_RETAINER_RADIAL_MARGIN = 0.2

PAR_CABLE_PORT_D     = 7.0
PAR_CABLE_PORT_Z_OFF = 5.0
PAR_CABLE_PORT_ANGLE = 90.0
PAR_CABLE_PORT_CUT_EXTRA = 6.0   # extra inward cut to ensure full breakthrough after all fuses

# ---------------- Aerodynamic edge rounding ----------------
EDGE_ROUND_ENABLE = True
HEAD_RIM_FILLET_R = 4.0
ROOF_RIM_FILLET_R = 2.0
RIM_RADIUS_TOL = 0.6

# ---------------- Support root reinforcement (fillet-like gussets) ----------------
SUPPORT_GUSSET_ENABLE = True
STANDOFF_GUSSET_H       = 3.0
STANDOFF_GUSSET_EXTRA_R = 1.5
PCB_POST_GUSSET_H       = 2.0
PCB_POST_GUSSET_EXTRA_R = 2.0

I2C_CONDUIT_ENABLE = True
I2C_CONDUIT_STANDOFF_INDEX = 1   # 0,1,2,3  -> 90° if your angles are [0,90,180,270]

I2C_CABLE_BORE_D = 4.5      # for ~3.55mm cable with print tolerance
I2C_ENTRY_HOLE_D = 4.8      # slight lead-in clearance vs bore
I2C_ENTRY_Z_FROM_TOP = 8.0       # legacy/reserved (current entry_z is derived from safe_stop_z)
I2C_ENTRY_LEN_EXTRA = 8.0        # legacy/reserved (entry_len currently computed from standoff OD)

I2C_EXIT_ENABLE = True           # legacy/reserved (exit branch currently follows conduit enable/index)
I2C_EXIT_W = 7.0                 # legacy/reserved
I2C_EXIT_H = 10.0                # legacy/reserved
I2C_EXIT_Z_FROM_PLATE = 14.0
I2C_EXIT_RELIEF_ENABLE = True   # legacy/reserved
I2C_TOP_CAP_THK = 2.0    # legacy/reserved
I2C_INSERT_CLEAR_THK = 1.0  # legacy/reserved
I2C_ENTRY_FUNNEL_ENABLE = True
I2C_ENTRY_FUNNEL_D = 7.5      # mouth diameter (try 6.5–9.0)
I2C_ENTRY_FUNNEL_L = 3.0      # funnel length/depth (2–4)
I2C_EXIT_LEN_EXTRA = 1.5      # mm past inner standoff wall (keep small to avoid plate over-cut)

# ---------- Document ----------
DOC_NAME = "Ultrasonic_Head_Improved"
try:
    App.closeDocument(DOC_NAME)
except Exception:
    pass
doc = App.newDocument(DOC_NAME)

# ---------------- Derived values ----------------
HEAD_R = HEAD_DIAM / 2.0
ROOF_R = ROOF_DIAM / 2.0

roof_z     = (PLATE_THK + POD_HEIGHT) + ROOF_GAP_ABOVE_PODS
roof_top_z = roof_z + ROOF_THK

STANDOFF_TOTAL_H = roof_z - 0.0

# Split radii (THIS is the key change)
ROOF_STANDOFF_RADIUS = HEAD_R - (STANDOFF_OD / 2.0) - float(ROOF_STANDOFF_RIM_MARGIN)
BELLY_FASTEN_RADIUS  = HEAD_R - (STANDOFF_OD / 2.0) - float(BELLY_FASTEN_RIM_MARGIN)

STANDOFF_ANGLES  = [a + STANDOFF_ANGLE_OFFSET for a in STANDOFF_ANGLES_BASE]

if PORT_Z_CENTER is None:
    PORT_Z_CENTER = -BELLY_WALL_H / 2.0

# ---------------- Helpers ----------------
def to_world(shape_local, x, y, base_z, rotate_center, axis, angle_deg):
    s = shape_local.copy()
    s.translate(Vector(x, y, base_z))
    s.rotate(rotate_center, axis, angle_deg)
    return s

def clamp(v, lo, hi):
    return max(lo, min(hi, v))

def angle_ok(a_deg):
    for sa in STANDOFF_ANGLES:
        d = abs(((a_deg - sa + 180) % 360) - 180)
        if d < PORT_AVOID_STANDOFF_DEG:
            return False
    return True

def cut_radial_port(solid, hole_d, angle_deg, z_center,
                    wall_inner_r, wall_outer_r,
                    extra=2.0,
                    outer_extra=0.0,
                    inner_extra=0.0):
    zc = clamp(z_center, -BELLY_WALL_H + 6.0, -6.0)

    x0 = (wall_inner_r - extra) - float(inner_extra)
    x1 = (wall_outer_r + extra) + float(outer_extra)
    length = max(0.1, x1 - x0)

    cutter = Part.makeCylinder(
        hole_d / 2.0,
        length,
        Vector(x0, 0, zc),
        Vector(1, 0, 0)
    )
    cutter.rotate(Vector(0, 0, 0), Vector(0, 0, 1), angle_deg)
    return solid.cut(cutter)

def add_inner_port_pad(solid, angle_deg, zc, nut_d, wall_inner_r,
                       inner_thk=3.0, inset=0.8,
                       extra_w=8.0, extra_h=8.0,
                       chamfer_z=3.0, chamfer_run=3.0,
                       chamfer_y=None, chamfer_run_y=None):
    pad_w = float(nut_d) + 2.0 * float(extra_w)
    pad_h = float(nut_d) + 2.0 * float(extra_h)

    if chamfer_y is None:
        chamfer_y = chamfer_z
    if chamfer_run_y is None:
        chamfer_run_y = chamfer_run

    cz  = max(0.0, float(chamfer_z))
    cr  = max(0.0, float(chamfer_run))
    cy  = max(0.0, float(chamfer_y))
    cyr = max(0.0, float(chamfer_run_y))

    x0    = float(wall_inner_r) - float(inner_thk)
    x_len = float(inner_thk) + float(inset)

    y0 = -pad_w / 2.0
    z0 = float(zc) - pad_h / 2.0

    pad = Part.makeBox(x_len, pad_w, pad_h, Vector(x0, y0, z0))

    x_in = x0
    y1 = y0 + pad_w
    z1 = z0 + pad_h

    if cz > 0.0 and cr > 0.0 and cz < pad_h:
        tri_pts = [
            Vector(x_in,       0, z0),
            Vector(x_in,       0, z0 + cz),
            Vector(x_in + cr,  0, z0),
            Vector(x_in,       0, z0),
        ]
        tri = Part.makePolygon(tri_pts)
        tri_face = Part.Face(tri)
        bot = tri_face.extrude(Vector(0, pad_w, 0))
        bot.translate(Vector(0, y0, 0))
        pad = pad.cut(bot)

        tri_pts2 = [
            Vector(x_in,       0, z1),
            Vector(x_in,       0, z1 - cz),
            Vector(x_in + cr,  0, z1),
            Vector(x_in,       0, z1),
        ]
        tri2 = Part.makePolygon(tri_pts2)
        tri2_face = Part.Face(tri2)
        top = tri2_face.extrude(Vector(0, pad_w, 0))
        top.translate(Vector(0, y0, 0))
        pad = pad.cut(top)

    if cy > 0.0 and cyr > 0.0 and cy < pad_w:
        triL = [
            Vector(x_in,       y0, 0),
            Vector(x_in,       y0 + cy, 0),
            Vector(x_in + cyr, y0, 0),
            Vector(x_in,       y0, 0),
        ]
        polyL = Part.makePolygon(triL)
        faceL = Part.Face(polyL)
        wedgeL = faceL.extrude(Vector(0, 0, pad_h))
        wedgeL.translate(Vector(0, 0, z0))
        pad = pad.cut(wedgeL)

        triR = [
            Vector(x_in,       y1, 0),
            Vector(x_in,       y1 - cy, 0),
            Vector(x_in + cyr, y1, 0),
            Vector(x_in,       y1, 0),
        ]
        polyR = Part.makePolygon(triR)
        faceR = Part.Face(polyR)
        wedgeR = faceR.extrude(Vector(0, 0, pad_h))
        wedgeR.translate(Vector(0, 0, z0))
        pad = pad.cut(wedgeR)

    pad.rotate(Vector(0, 0, 0), Vector(0, 0, 1), float(angle_deg))
    return solid.fuse(pad).removeSplitter()

def fillet_body_outer_top_rim(shape, target_r, fillet_r, top_z, tol_r=1.5, tol_z=1.2):
    if fillet_r <= 0:
        return shape

    tz = float(top_z)
    tr = float(target_r)
    tr_tol = float(tol_r)
    tz_tol = float(tol_z)

    edges = []
    for e in shape.Edges:
        bb = e.BoundBox
        if (bb.ZMax < (tz - tz_tol)) or (bb.ZMin > (tz + tz_tol)):
            continue

        ok = True
        for v in e.Vertexes:
            x, y = v.Point.x, v.Point.y
            r = (x*x + y*y) ** 0.5
            if abs(r - tr) > tr_tol:
                ok = False
                break
        if not ok:
            continue

        try:
            p0 = e.FirstParameter
            p1 = e.LastParameter
            pm = 0.5 * (p0 + p1)
            pt = e.valueAt(pm)
            rm = (pt.x*pt.x + pt.y*pt.y) ** 0.5
            if abs(rm - tr) > tr_tol:
                continue
        except Exception:
            pass

        edges.append(e)

    print("Top-rim candidate edges:", len(edges), " top_z:", tz, " target_r:", tr)

    if not edges:
        return shape

    try:
        return shape.makeFillet(float(fillet_r), edges)
    except Exception as ex:
        print("Fillet failed:", ex)
        return shape

def add_port_pad(solid, angle_deg, zc, nut_d, wall_outer_r,
                 pad_thk=3.0, inset=0.8,
                 extra_w=8.0, extra_h=8.0,
                 chamfer_z=3.0, chamfer_run=3.0,
                 chamfer_y=None, chamfer_run_y=None):
    pad_w = float(nut_d) + 2.0 * float(extra_w)
    pad_h = float(nut_d) + 2.0 * float(extra_h)

    x0    = float(wall_outer_r) - float(inset)
    x_len = float(inset) + float(pad_thk)

    y0 = -pad_w / 2.0
    z0 = float(zc) - pad_h / 2.0

    pad = Part.makeBox(x_len, pad_w, pad_h, Vector(x0, y0, z0))

    if chamfer_y is None:
        chamfer_y = chamfer_z
    if chamfer_run_y is None:
        chamfer_run_y = chamfer_run

    cz = max(0.0, float(chamfer_z))
    cr = max(0.0, float(chamfer_run))
    cy = max(0.0, float(chamfer_y))
    cyr = max(0.0, float(chamfer_run_y))

    x_out = x0 + x_len
    y1 = y0 + pad_w
    z1 = z0 + pad_h

    if cz > 0.0 and cr > 0.0 and cz < pad_h:
        tri_pts = [
            Vector(x_out,      0, z0),
            Vector(x_out,      0, z0 + cz),
            Vector(x_out - cr, 0, z0),
            Vector(x_out,      0, z0),
        ]
        tri = Part.makePolygon(tri_pts)
        tri_face = Part.Face(tri)
        bot = tri_face.extrude(Vector(0, pad_w, 0))
        bot.translate(Vector(0, y0, 0))
        pad = pad.cut(bot)

        tri_pts2 = [
            Vector(x_out,      0, z1),
            Vector(x_out,      0, z1 - cz),
            Vector(x_out - cr, 0, z1),
            Vector(x_out,      0, z1),
        ]
        tri2 = Part.makePolygon(tri_pts2)
        tri2_face = Part.Face(tri2)
        top = tri2_face.extrude(Vector(0, pad_w, 0))
        top.translate(Vector(0, y0, 0))
        pad = pad.cut(top)

    if cy > 0.0 and cyr > 0.0 and cy < pad_w:
        triL = [
            Vector(x_out,       y0, 0),
            Vector(x_out,       y0 + cy, 0),
            Vector(x_out - cyr, y0, 0),
            Vector(x_out,       y0, 0),
        ]
        polyL = Part.makePolygon(triL)
        faceL = Part.Face(polyL)
        wedgeL = faceL.extrude(Vector(0, 0, pad_h))
        wedgeL.translate(Vector(0, 0, z0))
        pad = pad.cut(wedgeL)

        triR = [
            Vector(x_out,       y1, 0),
            Vector(x_out,       y1 - cy, 0),
            Vector(x_out - cyr, y1, 0),
            Vector(x_out,       y1, 0),
        ]
        polyR = Part.makePolygon(triR)
        faceR = Part.Face(polyR)
        wedgeR = faceR.extrude(Vector(0, 0, pad_h))
        wedgeR.translate(Vector(0, 0, z0))
        pad = pad.cut(wedgeR)

    pad.rotate(Vector(0, 0, 0), Vector(0, 0, 1), float(angle_deg))
    return solid.fuse(pad).removeSplitter()

def make_pcb_posts(z_floor, inner_r):
    x0 = -PCB_W / 2.0 + PCB_OFFSET_X
    y0 = -PCB_H / 2.0 + PCB_OFFSET_Y

    rot = math.radians(float(PCB_ROT_DEG))
    cr = math.cos(rot)
    sr = math.sin(rot)

    posts = []
    for (hx, hy) in PCB_HOLES:
        px0 = x0 + hx
        py0 = y0 + hy

        px = (px0 * cr) - (py0 * sr)
        py = (px0 * sr) + (py0 * cr)

        if math.hypot(px, py) > (inner_r - 3.0):
            continue

        post = Part.makeCylinder(PCB_POST_OD / 2.0, PCB_STANDOFF_H, Vector(px, py, z_floor))

        if SUPPORT_GUSSET_ENABLE and PCB_POST_GUSSET_H > 0 and PCB_POST_GUSSET_EXTRA_R > 0:
            gus = Part.makeCone(
                (PCB_POST_OD / 2.0) + PCB_POST_GUSSET_EXTRA_R,
                (PCB_POST_OD / 2.0),
                PCB_POST_GUSSET_H,
                Vector(px, py, z_floor)
            )
            post = post.fuse(gus).removeSplitter()

        bore_z0 = z_floor + PCB_STANDOFF_H - PCB_INSERT_DEPTH
        bore = Part.makeCylinder(
            PCB_INSERT_BORE_D / 2.0,
            PCB_INSERT_DEPTH + 0.3,
            Vector(px, py, bore_z0 - 0.15)
        )
        post = post.cut(bore)

        if PCB_INSERT_CHAMFER_H > 0:
            cham = Part.makeCone(
                (PCB_INSERT_BORE_D / 2.0) + 0.35,
                (PCB_INSERT_BORE_D / 2.0),
                PCB_INSERT_CHAMFER_H,
                Vector(px, py, z_floor + PCB_STANDOFF_H - PCB_INSERT_CHAMFER_H)
            )
            post = post.cut(cham)

        posts.append(post)

    if not posts:
        return None

    fused = posts[0]
    for s in posts[1:]:
        fused = fused.fuse(s)
    return fused

def make_sht_canopy_roof(wall_outer_r, zc, angle_deg):
    OD = float(SHT_CANOPY_OD)
    thk = float(SHT_CANOPY_THK)
    raise_z = float(SHT_CANOPY_RAISE)
    outset = float(SHT_CANOPY_OUTSET)

    strut_w = float(SHT_CANOPY_STRUT_W)
    strut_z = float(SHT_CANOPY_STRUT_Z)
    y_off = float(SHT_CANOPY_STRUT_Y_OFFSET)

    xc = wall_outer_r + outset + (OD / 2.0)
    disc = Part.makeCylinder(OD / 2.0, thk, Vector(xc, 0, zc + raise_z))

    x0 = wall_outer_r - 1.0
    x_len = (xc - x0) + (OD * 0.15)
    z0 = (zc + raise_z) - strut_z

    strut1 = Part.makeBox(x_len, strut_w, strut_z, Vector(x0, +y_off - strut_w/2.0, z0))
    strut2 = Part.makeBox(x_len, strut_w, strut_z, Vector(x0, -y_off - strut_w/2.0, z0))

    roof = disc.fuse(strut1).fuse(strut2)
    roof.rotate(Vector(0, 0, 0), Vector(0, 0, 1), angle_deg)
    return roof

def fillet_outer_rim_by_radius(shape, rim_radius, fillet_r, tol=0.5):
    if fillet_r <= 0:
        return shape
    try:
        edges = []
        for e in shape.Edges:
            c = getattr(e, "Curve", None)
            if c and hasattr(c, "Radius"):
                if abs(float(c.Radius) - float(rim_radius)) <= float(tol):
                    edges.append(e)
        if edges:
            return shape.makeFillet(float(fillet_r), edges)
    except Exception:
        pass
    return shape

# ---------------- Base plate (extended downward) ----------------
plate = Part.makeCylinder(HEAD_R, PLATE_THK + PLATE_DROP, Vector(0, 0, -PLATE_DROP))

# ---------------- Fillet the TOP outer edge of the plate (do this early!) ----------------
def fillet_plate_top_rim(plate_shape, outer_r, top_z, fillet_r, tol_r=0.6, tol_z=0.6):
    if fillet_r <= 0:
        return plate_shape

    edges = []
    for e in plate_shape.Edges:
        bb = e.BoundBox

        # must be near the plate TOP plane
        if abs(bb.ZMax - float(top_z)) > float(tol_z):
            continue

        # must be near the outer radius
        ok = True
        for v in e.Vertexes:
            x, y = v.Point.x, v.Point.y
            r = math.hypot(x, y)
            if abs(r - float(outer_r)) > float(tol_r):
                ok = False
                break
        if ok:
            edges.append(e)

    if not edges:
        print("Plate-top rim fillet: no candidate edges found")
        return plate_shape

    try:
        return plate_shape.makeFillet(float(fillet_r), edges)
    except Exception as ex:
        print("Plate-top rim fillet failed:", ex)
        return plate_shape

plate = fillet_plate_top_rim(
    plate,
    outer_r=HEAD_R,
    top_z=PLATE_THK,
    fillet_r=PLATE_TOP_FILLET_R,
    tol_r=0.8,
    tol_z=0.8
)

trim_box = Part.makeBox(
    HEAD_DIAM * 2.0,
    HEAD_DIAM * 2.0,
    2000.0,
    Vector(-HEAD_DIAM, -HEAD_DIAM, -1000.0)
)

# ---------------- Pod outer (local) ----------------
pod_total_h = POD_HEIGHT + POD_THROUGH
bottom_open_local_z = POD_THROUGH
top_open_local_z    = pod_total_h

R = POD_DIAM / 2.0

POD_TAPER_LEN   = 12.0
POD_TAPER_TOP_R = R - 3.0

taper_len = max(0.0, float(POD_TAPER_LEN))
taper_start_z = top_open_local_z - taper_len
taper_start_z = max(taper_start_z, bottom_open_local_z + 0.01)
taper_len = top_open_local_z - taper_start_z

lower_cyl = Part.makeCylinder(R, taper_start_z, Vector(0, 0, 0))

if taper_len > 0.05 and POD_TAPER_TOP_R < R:
    upper_frustum = Part.makeCone(R, float(POD_TAPER_TOP_R), taper_len, Vector(0, 0, taper_start_z))
    pod_outer_local = lower_cyl.fuse(upper_frustum).removeSplitter()
else:
    pod_outer_local = Part.makeCylinder(R, pod_total_h, Vector(0, 0, 0))

try:
    pod_outer_local = pod_outer_local.makeFillet(0.8, [e for e in pod_outer_local.Edges])
except Exception:
    pass

shoulder_local_z = top_open_local_z - SENSOR_NECK_LEN
shoulder_local_z = max(0.5, min(shoulder_local_z, pod_total_h - 0.5))

# ---------------- Build body with pods + shoulder bore ----------------
POD_ANGLES = [45, 135, 225, 315]
body = plate
OVERSHOOT = 80.0

for a in POD_ANGLES:
    rad = math.radians(a)
    x = POD_RADIUS * math.cos(rad)
    y = POD_RADIUS * math.sin(rad)

    axis = Vector(math.sin(rad), -math.cos(rad), 0)
    base_z        = -POD_THROUGH
    rotate_center = Vector(x, y, PLATE_THK)

    pod_world = pod_outer_local.copy()
    pod_world.translate(Vector(x, y, base_z))
    pod_world.rotate(rotate_center, axis, POD_TILT_DEG)

    neck_bore_local = Part.makeCylinder(
        BORE_NECK_D / 2.0,
        pod_total_h + 2 * OVERSHOOT,
        Vector(0, 0, -OVERSHOOT)
    )

    main_bore_bottom_local = Part.makeCylinder(
        BORE_MAIN_D / 2.0,
        shoulder_local_z + OVERSHOOT,
        Vector(0, 0, -OVERSHOOT)
    )

    if BOTTOM_ENTRY_CHAMFER_H > 0:
        cham_z = bottom_open_local_z - 0.001
        cham = Part.makeCone(
            (BORE_MAIN_D / 2.0) + BOTTOM_ENTRY_CHAMFER_EXTRA_R,
            (BORE_MAIN_D / 2.0),
            BOTTOM_ENTRY_CHAMFER_H,
            Vector(0, 0, cham_z)
        )
        main_bore_bottom_local = main_bore_bottom_local.fuse(cham)

    if float(BORE_EXT_BELOW_OPEN) > 0:
        ext_h = float(BORE_EXT_BELOW_OPEN)

        ext_main = Part.makeCylinder(
            BORE_MAIN_D / 2.0,
            ext_h,
            Vector(0, 0, bottom_open_local_z - ext_h)
        )
        main_bore_bottom_local = main_bore_bottom_local.fuse(ext_main)

        ext_neck = Part.makeCylinder(
            BORE_NECK_D / 2.0,
            ext_h,
            Vector(0, 0, bottom_open_local_z - ext_h)
        )
        neck_bore_local = neck_bore_local.fuse(ext_neck)

    if SIL_SEAL_ENABLE:
        pocket_r = (BORE_MAIN_D / 2.0) + float(SIL_SEAL_EXTRA_R)
        pocket_h = float(SIL_SEAL_DEPTH) + float(SIL_POCKET_EXT_BELOW)

        pocket = Part.makeCylinder(
            pocket_r,
            pocket_h,
            Vector(0, 0, bottom_open_local_z - pocket_h)
        )
        main_bore_bottom_local = main_bore_bottom_local.fuse(pocket)

    neck_bore_w = to_world(neck_bore_local, x, y, base_z, rotate_center, axis, POD_TILT_DEG)
    main_bot_w  = to_world(main_bore_bottom_local, x, y, base_z, rotate_center, axis, POD_TILT_DEG)

    pod_world = pod_world.cut(neck_bore_w)
    pod_world = pod_world.cut(main_bot_w)
    pod_world = pod_world.common(trim_box)

    body = body.fuse(pod_world)
    body = body.cut(neck_bore_w)
    body = body.cut(main_bot_w)

# ---------------- Roof standoffs (ROUND, thicker, top inserts) ----------------

def _fillet_standoff_base_before_fuse(standoff_shape, x, y, base_r, z_target, fillet_r, tol_z=1.0, tol_r=1.5):
    if fillet_r <= 0:
        return standoff_shape

    local = standoff_shape.copy()
    local.translate(Vector(-x, -y, 0))

    edges = []
    zt = float(z_target)
    rt = float(base_r)

    for e in local.Edges:
        bb = e.BoundBox
        # Keep ONLY edges lying on the base plane (avoids selecting vertical seam edges)
        if abs(bb.ZMin - zt) > float(tol_z) or abs(bb.ZMax - zt) > float(tol_z):
            continue

        ok = True
        for v in e.Vertexes:
            r = math.hypot(v.Point.x, v.Point.y)
            if abs(r - rt) > float(tol_r):
                ok = False
                break
        if ok:
            edges.append(e)

    if edges:
        try:
            local = local.makeFillet(float(fillet_r), edges)
        except Exception as ex:
            print("Standoff base fillet failed:", ex)

    local.translate(Vector(x, y, 0))
    return local

# ---------------- Roof standoffs (ROUND, thicker, top inserts + I2C conduit option) ----------------
# This version guarantees:
#  - vertical bore continues through the PLATE (cut applied to BODY after fuse)
#  - side entry is below insert zone and includes a funnel/countersink for easy feeding
#  - exit tunnel opens inward into the belly (cut applied to BODY after fuse)

_insert_wall = (STANDOFF_OD - TOP_INSERT_BORE_D) / 2.0
if _insert_wall < STANDOFF_MIN_INSERT_WALL:
    print(
        "WARNING: standoff insert wall thickness is {:.2f}mm (< {:.2f}mm target).".format(
            _insert_wall, STANDOFF_MIN_INSERT_WALL
        )
    )

for i, a in enumerate(STANDOFF_ANGLES):
    rad = math.radians(a)
    x = ROOF_STANDOFF_RADIUS * math.cos(rad)
    y = ROOF_STANDOFF_RADIUS * math.sin(rad)

    # Base pillar (ROUND)
    pillar = Part.makeCylinder(STANDOFF_OD / 2.0, STANDOFF_TOTAL_H, Vector(x, y, 0.0))

    # Root flare reinforcement (strong junction at plate)
    if STANDOFF_ROOT_FLARE_ENABLE and STANDOFF_ROOT_FLARE_H > 0 and STANDOFF_ROOT_FLARE_EXTRA_R > 0:
        flare = Part.makeCone(
            (STANDOFF_OD / 2.0) + float(STANDOFF_ROOT_FLARE_EXTRA_R),
            (STANDOFF_OD / 2.0),
            float(STANDOFF_ROOT_FLARE_H),
            Vector(x, y, PLATE_THK),
        )
        pillar = pillar.fuse(flare).removeSplitter()

    # Optional gusset reinforcement
    if SUPPORT_GUSSET_ENABLE and STANDOFF_GUSSET_H > 0 and STANDOFF_GUSSET_EXTRA_R > 0:
        gus = Part.makeCone(
            (STANDOFF_OD / 2.0) + float(STANDOFF_GUSSET_EXTRA_R),
            (STANDOFF_OD / 2.0),
            float(STANDOFF_GUSSET_H),
            Vector(x, y, PLATE_THK),
        )
        pillar = pillar.fuse(gus).removeSplitter()

    # Fillet the standoff base flare edge (before fusing into body)
    if STANDOFF_BASE_FILLET_R > 0:
        base_r = (STANDOFF_OD / 2.0)
        if STANDOFF_ROOT_FLARE_ENABLE and STANDOFF_ROOT_FLARE_EXTRA_R > 0:
            base_r = (STANDOFF_OD / 2.0) + float(STANDOFF_ROOT_FLARE_EXTRA_R)

        pillar = _fillet_standoff_base_before_fuse(
            standoff_shape=pillar,
            x=x, y=y,
            base_r=base_r,
            z_target=PLATE_THK,
            fillet_r=float(STANDOFF_BASE_FILLET_R),
            tol_z=1.2,
            tol_r=1.8
        )

    # ---------------- I2C conduit cutters (prepared here, applied after fuse to guarantee plate is cut) ----------------
    make_i2c = (("I2C_CONDUIT_ENABLE" in globals()) and I2C_CONDUIT_ENABLE and (i == int(I2C_CONDUIT_STANDOFF_INDEX)))

    conduit_cut_body = None
    exit_cut_body    = None
    entry_cut_pillar = None
    entry_funnel_pillar = None

    if make_i2c:
        print("I2C conduit: cutting standoff index", i, "angle", a)

        # Insert bore region occupies [roof_z - TOP_INSERT_DEPTH, roof_z]
        top_bore_z0 = roof_z - TOP_INSERT_DEPTH

        # Stop the conduit below:
        #  - standoff top face
        #  - AND below the insert bore region
        safe_stop_z = min(roof_z - 2.0, top_bore_z0 - 1.0)

        # Start the conduit below the plate, into the belly region
        bore_z0 = -float(PLATE_DROP) - 2.0
        bore_h  = safe_stop_z - bore_z0

        if bore_h <= 2.0:
            print("WARNING: I2C conduit bore height too small:", bore_h, "mm. Check roof_z/TOP_INSERT_DEPTH/PLATE_DROP.")
        else:
            # Wall thickness check vs insert bore
            _min_wall = (STANDOFF_OD / 2.0) - (TOP_INSERT_BORE_D / 2.0) - (float(I2C_CABLE_BORE_D) / 2.0)
            print("I2C conduit: min wall (insert+conduit) =", round(_min_wall, 2), "mm")
            if _min_wall < 1.2:
                print("WARNING: conduit + insert leaves thin wall. Increase STANDOFF_OD or reduce I2C_CABLE_BORE_D.")

            # (A) Body conduit cut: ensures hole continues through PLATE (pillar alone can't cut below z=0)
            conduit_cut_body = Part.makeCylinder(
                float(I2C_CABLE_BORE_D) / 2.0,
                bore_h,
                Vector(x, y, bore_z0)
            )

            # (B) Side entry cut (pillar-only is fine) below insert zone
            entry_z = safe_stop_z - 2.0

            # radial outward -> inward
            dx = math.cos(rad)
            dy = math.sin(rad)

            start_r = (STANDOFF_OD / 2.0) + 0.8
            sx = x + dx * start_r
            sy = y + dy * start_r

            entry_len = (STANDOFF_OD / 2.0) + 3.0
            entry_cut_pillar = Part.makeCylinder(
                float(I2C_ENTRY_HOLE_D) / 2.0,
                entry_len,
                Vector(sx, sy, entry_z),
                Vector(-dx, -dy, 0)
            )

            # Funnel/countersink for easy feeding (optional)
            if ("I2C_ENTRY_FUNNEL_ENABLE" in globals()) and I2C_ENTRY_FUNNEL_ENABLE:
                # Requires: I2C_ENTRY_FUNNEL_D, I2C_ENTRY_FUNNEL_L
                entry_funnel_pillar = Part.makeCone(
                    float(I2C_ENTRY_FUNNEL_D) / 2.0,         # mouth radius
                    float(I2C_ENTRY_HOLE_D) / 2.0,           # matches entry hole
                    float(I2C_ENTRY_FUNNEL_L),               # length along drill axis
                    Vector(sx, sy, entry_z),                 # starts at outer surface
                    Vector(-dx, -dy, 0)                      # points inward
                )

            # (C) Exit tunnel into belly interior (inward toward housing center)
            # Keep this cut in/near the standoff zone (not deep in plate-drop solid),
            # then only break through inner wall by a small controlled extra.
            exit_z_raw = PLATE_THK - float(I2C_EXIT_Z_FROM_PLATE)
            exit_z = clamp(exit_z_raw, 0.6, safe_stop_z - 1.0)
            if abs(exit_z - exit_z_raw) > 1e-6:
                print("I2C exit_z clamped from", round(exit_z_raw, 2), "to", round(exit_z, 2), "to stay in standoff region")

            ix = -math.cos(rad)
            iy = -math.sin(rad)

            # Start at conduit wall (inward side) so we don't over-cut through the conduit centerline.
            conduit_r = float(I2C_CABLE_BORE_D) / 2.0
            wall_run = (STANDOFF_OD / 2.0) - conduit_r
            exit_len = max(0.4, wall_run + float(I2C_EXIT_LEN_EXTRA))

            sx_exit = x + ix * conduit_r
            sy_exit = y + iy * conduit_r

            exit_cut_body = Part.makeCylinder(
                float(I2C_CABLE_BORE_D) / 2.0,
                exit_len,
                Vector(sx_exit, sy_exit, exit_z),
                Vector(ix, iy, 0)
            )

    # Apply pillar-only entry cuts now
    if entry_cut_pillar is not None:
        pillar = pillar.cut(entry_cut_pillar)
    if entry_funnel_pillar is not None:
        pillar = pillar.cut(entry_funnel_pillar)

    # ---------------- TOP heat-set insert bore (roof screw) ----------------
    top_bore_z0 = roof_z - TOP_INSERT_DEPTH
    top_insert_bore = Part.makeCylinder(
        TOP_INSERT_BORE_D / 2.0,
        TOP_INSERT_DEPTH + 0.2,
        Vector(x, y, top_bore_z0 - 0.1),
    )
    pillar = pillar.cut(top_insert_bore)

    if TOP_INSERT_LEADIN_CHAMFER > 0:
        cham = Part.makeCone(
            (TOP_INSERT_BORE_D / 2.0) + 0.35,
            (TOP_INSERT_BORE_D / 2.0),
            float(TOP_INSERT_LEADIN_CHAMFER),
            Vector(x, y, roof_z - TOP_INSERT_LEADIN_CHAMFER),
        )
        pillar = pillar.cut(cham)

    # Fuse pillar into body FIRST
    body = body.fuse(pillar).removeSplitter()

    # Now apply body-level conduit cuts so the hole continues through the plate + opens into belly
    if conduit_cut_body is not None:
        body = body.cut(conduit_cut_body)

    if exit_cut_body is not None:
        body = body.cut(exit_cut_body)

    body = body.removeSplitter()
# ---------------- Plate bolt-through holes to match belly fastening tubes ----------------
for a in BELLY_FASTEN_ANGLES:
    rad = math.radians(a)
    x = BELLY_FASTEN_RADIUS * math.cos(rad)
    y = BELLY_FASTEN_RADIUS * math.sin(rad)

    thru = Part.makeCylinder(PLATE_BOLT_HOLE_D / 2.0, PLATE_THK + 2.0, Vector(x, y, -1.0))
    body = body.cut(thru)

    if PLATE_BOLT_CB_ENABLE and (PLATE_BOLT_CB_D > PLATE_BOLT_HOLE_D) and (PLATE_BOLT_CB_DEPTH > 0):
        cb = Part.makeCylinder(
            PLATE_BOLT_CB_D / 2.0,
            PLATE_BOLT_CB_DEPTH,
            Vector(x, y, PLATE_THK - PLATE_BOLT_CB_DEPTH)
        )
        body = body.cut(cb)

body = body.removeSplitter()

# ---------------- BODY: top-only rim fillet (robust) ----------------
if EDGE_ROUND_ENABLE:
    body = fillet_body_outer_top_rim(
        body,
        target_r=HEAD_R,
        fillet_r=HEAD_RIM_FILLET_R,
        top_z=PLATE_THK,
        tol_r=2.5,
        tol_z=1.2
    )

lower_obj = doc.addObject("Part::Feature", "LowerBody")
lower_obj.Shape = body

# ---------------- Roof ----------------
roof_with_holes = Part.makeCylinder(ROOF_R, ROOF_THK, Vector(0, 0, roof_z))

for a in STANDOFF_ANGLES:
    rad = math.radians(a)
    x = ROOF_STANDOFF_RADIUS * math.cos(rad)
    y = ROOF_STANDOFF_RADIUS * math.sin(rad)

    hole = Part.makeCylinder(ROOF_HOLE_D / 2.0, ROOF_THK + 1.0, Vector(x, y, roof_z - 0.5))
    roof_with_holes = roof_with_holes.cut(hole)

    if ADD_COUNTERBORE and (COUNTERBORE_D > ROOF_HOLE_D) and (COUNTERBORE_DEPTH > 0):
        cb = Part.makeCylinder(COUNTERBORE_D / 2.0, COUNTERBORE_DEPTH, Vector(x, y, roof_top_z - COUNTERBORE_DEPTH))
        roof_with_holes = roof_with_holes.cut(cb)

roof_final = roof_with_holes

if PAR_ENABLE:
    cx, cy = PAR_CENTER.x, PAR_CENTER.y
    housing_id_r = max(0.1, (PAR_HOUSING_OD / 2.0) - PAR_HOUSING_WALL)

    outer = Part.makeCylinder(PAR_HOUSING_OD / 2.0, PAR_HOUSING_H, Vector(cx, cy, roof_top_z))
    inner = Part.makeCylinder(housing_id_r, PAR_HOUSING_H + 0.5, Vector(cx, cy, roof_top_z - 0.25))
    ring  = outer.cut(inner)

    port_z = roof_top_z + float(PAR_CABLE_PORT_Z_OFF)
    outer_r = float(PAR_HOUSING_OD) / 2.0
    wall_thk = float(PAR_HOUSING_WALL)

    start_x = cx + outer_r + 0.8
    drill_len = wall_thk + float(PAR_CABLE_PORT_CUT_EXTRA)

    cable_cutter = Part.makeCylinder(
        float(PAR_CABLE_PORT_D) / 2.0,
        drill_len,
        Vector(start_x, cy, port_z),
        Vector(-1, 0, 0)
    )
    cable_cutter.rotate(Vector(cx, cy, 0), Vector(0, 0, 1), float(PAR_CABLE_PORT_ANGLE))
    ring = ring.cut(cable_cutter)

    # Diffuser top support (flush mating) + optional retainer collar
    if PAR_DIFFUSER_ENABLE:
        z_top = roof_top_z + float(PAR_HOUSING_H)

        diffuser_r = float(PAR_DIFFUSER_D) / 2.0
        diffuser_fit_r = diffuser_r + (float(PAR_DIFFUSER_OD_CLEAR_D) / 2.0)
        seat_inner_r = max(0.1, diffuser_fit_r - float(PAR_DIFFUSER_LEDGE_OVERLAP))
        pocket_underside_z = z_top - float(PAR_DIFFUSER_RECESS_DEPTH)

        # (1) Flush support ledge under diffuser edge (top face remains flush)
        if PAR_DIFFUSER_LEDGE_ENABLE and PAR_DIFFUSER_LEDGE_H > 0 and PAR_DIFFUSER_LEDGE_OVERLAP > 0:
            ledge_outer_r = float(PAR_HOUSING_OD) / 2.0
            ledge_inner_r = seat_inner_r

            if ledge_outer_r > (ledge_inner_r + 0.15):
                ledge_outer = Part.makeCylinder(ledge_outer_r, float(PAR_DIFFUSER_LEDGE_H), Vector(cx, cy, z_top - float(PAR_DIFFUSER_LEDGE_H)))
                ledge_inner = Part.makeCylinder(ledge_inner_r, float(PAR_DIFFUSER_LEDGE_H) + 0.2, Vector(cx, cy, z_top - float(PAR_DIFFUSER_LEDGE_H) - 0.1))
                ledge = ledge_outer.cut(ledge_inner)
                ring = ring.fuse(ledge).removeSplitter()
            else:
                print("WARNING: PAR diffuser support ledge too narrow. Check diffuser size/clearance/overlap.")

        # (1b) Explicit recessed seat for diffuser OD fit
        if PAR_DIFFUSER_POCKET_ENABLE and PAR_DIFFUSER_RECESS_DEPTH > 0:
            seat_depth = float(PAR_DIFFUSER_RECESS_DEPTH)
            pocket_underside_z = z_top - seat_depth
            seat_cut = Part.makeCylinder(
                diffuser_fit_r,
                seat_depth + 0.2,
                Vector(cx, cy, z_top - seat_depth - 0.1)
            )
            ring = ring.cut(seat_cut).removeSplitter()

            # Restore explicit pocket support shelf at pocket floor (for real seating contact)
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
                        Vector(cx, cy, floor_top_z - floor_thk)
                    )
                    floor_inner = Part.makeCylinder(
                        floor_inner_r,
                        floor_thk + 0.2,
                        Vector(cx, cy, floor_top_z - floor_thk - 0.1)
                    )
                    floor_ring = floor_outer.cut(floor_inner)
                    ring = ring.fuse(floor_ring).removeSplitter()

        # (1c) Anti-overhang underside support slope back to inner wall (print-friendly)
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
                    Vector(cx, cy, support_base_z)
                )
                support_inner = Part.makeCone(
                    inner_bottom_r,
                    inner_top_r,
                    support_h + 0.2,
                    Vector(cx, cy, support_base_z - 0.1)
                )
                support_slope = support_outer.cut(support_inner)
                ring = ring.fuse(support_slope).removeSplitter()

        # (2) Optional raised retainer collar (disabled by default to keep mating surface flush)
        if PAR_DIFFUSER_RETAINER_ENABLE and PAR_DIFFUSER_RETAINER_H > 0 and PAR_DIFFUSER_RETAINER_WALL > 0:
            collar_id_r = max(diffuser_fit_r, housing_id_r + 0.05)
            collar_od_r_raw = collar_id_r + float(PAR_DIFFUSER_RETAINER_WALL)
            collar_od_r_max = (float(PAR_HOUSING_OD) / 2.0) - float(PAR_DIFFUSER_RETAINER_RADIAL_MARGIN)
            collar_od_r = min(collar_od_r_raw, collar_od_r_max)

            if collar_od_r > (collar_id_r + 0.2):
                collar_outer = Part.makeCylinder(collar_od_r, float(PAR_DIFFUSER_RETAINER_H), Vector(cx, cy, z_top))
                collar_inner = Part.makeCylinder(collar_id_r, float(PAR_DIFFUSER_RETAINER_H) + 0.2, Vector(cx, cy, z_top - 0.1))
                collar = collar_outer.cut(collar_inner)
                ring = ring.fuse(collar).removeSplitter()
            else:
                print("WARNING: PAR diffuser retainer collar could not be created. Check PAR_HOUSING_OD / PAR_HOUSING_WALL / diffuser size.")

        # Re-cut cable port after all added support/seat features so passage is guaranteed open
        ring = ring.cut(cable_cutter).removeSplitter()

    roof_final = roof_final.fuse(ring)

    par_support = None
    boss_points = []

    for (dx, dy) in PAR_BOSS_OFFSET_XY:
        bx = cx + dx
        by = cy + dy
        boss_points.append((bx, by))

        boss = Part.makeCylinder(PAR_BOSS_DIAM / 2.0, PAR_BOSS_HEIGHT, Vector(bx, by, roof_top_z))

        if PAR_BOSS_ROOT_FLARE_ENABLE and PAR_BOSS_ROOT_FLARE_H > 0 and PAR_BOSS_ROOT_FLARE_EXTRA_R > 0:
            root_flare = Part.makeCone(
                (PAR_BOSS_DIAM / 2.0) + float(PAR_BOSS_ROOT_FLARE_EXTRA_R),
                (PAR_BOSS_DIAM / 2.0),
                float(PAR_BOSS_ROOT_FLARE_H),
                Vector(bx, by, roof_top_z)
            )
            boss = boss.fuse(root_flare).removeSplitter()

        if PAR_BOSS_BASE_FILLET_R > 0:
            base_r = (PAR_BOSS_DIAM / 2.0)
            if PAR_BOSS_ROOT_FLARE_ENABLE and PAR_BOSS_ROOT_FLARE_EXTRA_R > 0:
                base_r = (PAR_BOSS_DIAM / 2.0) + float(PAR_BOSS_ROOT_FLARE_EXTRA_R)

            boss = _fillet_standoff_base_before_fuse(
                standoff_shape=boss,
                x=bx,
                y=by,
                base_r=base_r,
                z_target=roof_top_z,
                fillet_r=float(PAR_BOSS_BASE_FILLET_R),
                tol_z=1.0,
                tol_r=1.5,
            )

        if par_support is None:
            par_support = boss
        else:
            par_support = par_support.fuse(boss)

    if PAR_BOSS_CROSS_GUSSET_ENABLE and len(boss_points) >= 2:
        ys = [p[1] for p in boss_points]
        y_min = min(ys)
        y_max = max(ys)
        web_len = y_max - y_min
        if web_len > 0.2:
            web_h = min(float(PAR_BOSS_CROSS_GUSSET_H), float(PAR_BOSS_HEIGHT))
            web = Part.makeBox(
                float(PAR_BOSS_CROSS_GUSSET_THK),
                web_len,
                web_h,
                Vector(cx - (float(PAR_BOSS_CROSS_GUSSET_THK) / 2.0), y_min, roof_top_z)
            )
            par_support = par_support.fuse(web)

    for (bx, by) in boss_points:
        hole = Part.makeCylinder(
            PAR_SCREW_DIAMETER / 2.0,
            PAR_BOSS_HEIGHT + 0.5,
            Vector(bx, by, roof_top_z - 0.25)
        )
        par_support = par_support.cut(hole)

    roof_final = roof_final.fuse(par_support)

roof_final = roof_final.removeSplitter()

if EDGE_ROUND_ENABLE:
    roof_final = fillet_outer_rim_by_radius(roof_final, rim_radius=ROOF_R, fillet_r=ROOF_RIM_FILLET_R, tol=RIM_RADIUS_TOL)

roof_obj = doc.addObject("Part::Feature", "RoofPlate")
roof_obj.Shape = roof_final

# ---------------- Belly Pan (Cup) ----------------
if BELLY_ENABLE:
    cup_total_h = BELLY_WALL_H + BELLY_BASE_THK
    cup_base_z  = -cup_total_h

    cup_outer = Part.makeCylinder(HEAD_R, cup_total_h, Vector(0, 0, cup_base_z))

    inner_r = max(1.0, HEAD_R - BELLY_WALL_THK)
    belly_inner_r = inner_r

    cavity = Part.makeCylinder(inner_r, BELLY_WALL_H + 0.2, Vector(0, 0, -BELLY_WALL_H - 0.1))
    pan = cup_outer.cut(cavity)

    # --- internal fastening tubes + inserts ---
    for a in BELLY_FASTEN_ANGLES:
        rad = math.radians(a)
        x = BELLY_FASTEN_RADIUS * math.cos(rad)
        y = BELLY_FASTEN_RADIUS * math.sin(rad)

        tube_outer = Part.makeCylinder(BELLY_TUBE_OD / 2.0, cup_total_h, Vector(x, y, cup_base_z))
        pan = pan.fuse(tube_outer)

        insert_bore = Part.makeCylinder(
            TOP_INSERT_BORE_D / 2.0,
            TOP_INSERT_DEPTH + 0.2,
            Vector(x, y, -TOP_INSERT_DEPTH - 0.1)
        )
        pan = pan.cut(insert_bore)

        if TOP_INSERT_LEADIN_CHAMFER > 0:
            cham = Part.makeCone(
                (TOP_INSERT_BORE_D / 2.0) + 0.35,
                (TOP_INSERT_BORE_D / 2.0),
                TOP_INSERT_LEADIN_CHAMFER,
                Vector(x, y, -TOP_INSERT_LEADIN_CHAMFER)
            )
            pan = pan.cut(cham)

    # --- seal groove on lip (z=0) ---
    if BELLY_SEAL_ENABLE:
        wall_inner_r = inner_r
        wall_outer_r = HEAD_R
        tube_outer_r = BELLY_FASTEN_RADIUS + (BELLY_TUBE_OD / 2.0)

        if BELLY_SEAL_CENTER_IN_WALL:
            groove_center_r = (wall_inner_r + wall_outer_r) / 2.0
        else:
            groove_center_r = wall_outer_r - (BELLY_SEAL_MIN_OUTER_MARGIN + BELLY_SEAL_GROOVE_W / 2.0)

        r_in  = groove_center_r - (BELLY_SEAL_GROOVE_W / 2.0)
        r_out = groove_center_r + (BELLY_SEAL_GROOVE_W / 2.0)

        max_r_out = wall_outer_r - BELLY_SEAL_MIN_OUTER_MARGIN
        if r_out > max_r_out:
            shift = r_out - max_r_out
            r_in  -= shift
            r_out -= shift

        min_r_in = tube_outer_r + BELLY_SEAL_CLEAR_TUBE
        if r_in < min_r_in:
            r_in = min_r_in
            r_out = r_in + BELLY_SEAL_GROOVE_W

        max_r_out = wall_outer_r - BELLY_SEAL_MIN_OUTER_MARGIN
        if r_out > max_r_out:
            r_out = max_r_out
            r_in = max(wall_inner_r + 0.8, r_out - BELLY_SEAL_GROOVE_W)

        if (r_out - r_in) >= 0.8 and r_in > 1.0:
            groove_outer = Part.makeCylinder(r_out, BELLY_SEAL_GROOVE_DEPTH, Vector(0, 0, -BELLY_SEAL_GROOVE_DEPTH))
            groove_inner = Part.makeCylinder(r_in,  BELLY_SEAL_GROOVE_DEPTH + 0.2, Vector(0, 0, -BELLY_SEAL_GROOVE_DEPTH - 0.1))
            groove_ring = groove_outer.cut(groove_inner)
            pan = pan.cut(groove_ring)

    # --- drip edge skirt ---
    if DRIP_EDGE_ENABLE and (DRIP_EDGE_OUTSET > 0) and (DRIP_EDGE_DROP > 0):
        drop    = float(DRIP_EDGE_DROP)
        outset  = float(DRIP_EDGE_OUTSET)
        overlap = float(DRIP_EDGE_OVERLAP)

        outer_skirt = Part.makeCone(
            HEAD_R + outset,
            HEAD_R,
            drop,
            Vector(0, 0, -drop)
        )

        drip_inner_r = max(0.1, HEAD_R - overlap)
        inner_cut = Part.makeCylinder(
            drip_inner_r,
            drop + 0.4,
            Vector(0, 0, -drop - 0.2)
        )

        drip_edge = outer_skirt.cut(inner_cut)
        pan = pan.fuse(drip_edge)

    # --- ports + protrusions (OUTSIDE + INSIDE pads) ---
    if CONNECTORS_ENABLE:
        wall_inner_r = belly_inner_r
        wall_outer_r = HEAD_R
        zc_ports = PORT_Z_CENTER
        outer_pads_on = bool(PORT_PADS_ENABLE)
        inner_pads_on = bool(PORT_PADS_ENABLE and PORT_PAD_INNER_ENABLE)

        gx12_zc = zc_ports if (GX12_Z_CENTER is None) else float(GX12_Z_CENTER)

        pair_step = float(GX12_ROW_SPACING_DEG)
        pair_half = pair_step / 2.0

        pairA_center = float(GX12_ROW_CENTER_ANGLE) + float(GX12_ROW_OFFSET_DEG)
        pairB_center = pairA_center + 180.0

        gx12_angles = [
            pairA_center - pair_half, pairA_center + pair_half,
            pairB_center - pair_half, pairB_center + pair_half
        ]

        for ang in gx12_angles:
            if outer_pads_on:
                pan = add_port_pad(
                    pan, ang, gx12_zc, GX12_NUT_D, wall_outer_r,
                    pad_thk=PORT_PAD_THK, inset=PORT_PAD_INSET,
                    extra_w=PORT_PAD_EXTRA_W, extra_h=PORT_PAD_EXTRA_H,
                    chamfer_z=PORT_PAD_CHAMFER_Z, chamfer_run=PORT_PAD_CHAMFER_RUN,
                    chamfer_y=PORT_PAD_CHAMFER_Z, chamfer_run_y=PORT_PAD_CHAMFER_RUN
                )
            if inner_pads_on:
                pan = add_inner_port_pad(
                    pan, ang, gx12_zc, GX12_NUT_D, wall_inner_r,
                    inner_thk=PORT_PAD_THK, inset=PORT_PAD_INSET,
                    extra_w=PORT_PAD_EXTRA_W, extra_h=PORT_PAD_EXTRA_H,
                    chamfer_z=PORT_PAD_CHAMFER_Z, chamfer_run=PORT_PAD_CHAMFER_RUN,
                    chamfer_y=PORT_PAD_CHAMFER_Z, chamfer_run_y=PORT_PAD_CHAMFER_RUN
                )

            pan = cut_radial_port(
                pan, GX12_HOLE_D, ang, gx12_zc,
                wall_inner_r, wall_outer_r,
                extra=PORT_CUT_EXTRA + (float(PORT_PAD_THK) if outer_pads_on else 0.0),
                outer_extra=(float(PORT_PAD_THK) if outer_pads_on else 0.0),
                inner_extra=(float(PORT_PAD_THK) if inner_pads_on else 0.0),
            )

        if angle_ok(PG7_ANGLE_DEG):
            if outer_pads_on:
                pan = add_port_pad(
                    pan, PG7_ANGLE_DEG, zc_ports, PG7_NUT_D, wall_outer_r,
                    pad_thk=PORT_PAD_THK, inset=PORT_PAD_INSET,
                    extra_w=PORT_PAD_EXTRA_W, extra_h=PORT_PAD_EXTRA_H,
                    chamfer_z=PORT_PAD_CHAMFER_Z, chamfer_run=PORT_PAD_CHAMFER_RUN,
                    chamfer_y=PORT_PAD_CHAMFER_Z, chamfer_run_y=PORT_PAD_CHAMFER_RUN
                )
            if inner_pads_on:
                pan = add_inner_port_pad(
                    pan, PG7_ANGLE_DEG, zc_ports, PG7_NUT_D, wall_inner_r,
                    inner_thk=PORT_PAD_THK, inset=PORT_PAD_INSET,
                    extra_w=PORT_PAD_EXTRA_W, extra_h=PORT_PAD_EXTRA_H,
                    chamfer_z=PORT_PAD_CHAMFER_Z, chamfer_run=PORT_PAD_CHAMFER_RUN,
                    chamfer_y=PORT_PAD_CHAMFER_Z, chamfer_run_y=PORT_PAD_CHAMFER_RUN
                )

            pan = cut_radial_port(
                pan, PG7_HOLE_D, PG7_ANGLE_DEG, zc_ports,
                wall_inner_r, wall_outer_r,
                extra=PORT_CUT_EXTRA + (float(PORT_PAD_THK) if outer_pads_on else 0.0),
                outer_extra=(float(PORT_PAD_THK) if outer_pads_on else 0.0),
                inner_extra=(float(PORT_PAD_THK) if inner_pads_on else 0.0),
            )

        if angle_ok(SHT_ANGLE_DEG):
            if outer_pads_on:
                pan = add_port_pad(
                    pan, SHT_ANGLE_DEG, zc_ports, SHT_NUT_D, wall_outer_r,
                    pad_thk=PORT_PAD_THK, inset=PORT_PAD_INSET,
                    extra_w=PORT_PAD_EXTRA_W, extra_h=PORT_PAD_EXTRA_H,
                    chamfer_z=PORT_PAD_CHAMFER_Z, chamfer_run=PORT_PAD_CHAMFER_RUN,
                    chamfer_y=PORT_PAD_CHAMFER_Z, chamfer_run_y=PORT_PAD_CHAMFER_RUN
                )
            if inner_pads_on:
                pan = add_inner_port_pad(
                    pan, SHT_ANGLE_DEG, zc_ports, SHT_NUT_D, wall_inner_r,
                    inner_thk=PORT_PAD_THK, inset=PORT_PAD_INSET,
                    extra_w=PORT_PAD_EXTRA_W, extra_h=PORT_PAD_EXTRA_H,
                    chamfer_z=PORT_PAD_CHAMFER_Z, chamfer_run=PORT_PAD_CHAMFER_RUN,
                    chamfer_y=PORT_PAD_CHAMFER_Z, chamfer_run_y=PORT_PAD_CHAMFER_RUN
                )

            pan = cut_radial_port(
                pan, SHT_HOLE_D, SHT_ANGLE_DEG, zc_ports,
                wall_inner_r, wall_outer_r,
                extra=PORT_CUT_EXTRA + (float(PORT_PAD_THK) if outer_pads_on else 0.0),
                outer_extra=(float(PORT_PAD_THK) if outer_pads_on else 0.0),
                inner_extra=(float(PORT_PAD_THK) if inner_pads_on else 0.0),
            )

        if SHT_RS_ENABLE and angle_ok(SHT_ANGLE_DEG):
            roof_shield = make_sht_canopy_roof(wall_outer_r=wall_outer_r, zc=SHT_Z, angle_deg=SHT_ANGLE_DEG)
            pan = pan.fuse(roof_shield)

    if PCB_ENABLE:
        z_floor = cup_base_z + BELLY_BASE_THK
        posts = make_pcb_posts(z_floor=z_floor, inner_r=inner_r)
        if posts:
            pan = pan.fuse(posts)

    pan = pan.removeSplitter()
    pan_obj = doc.addObject("Part::Feature", "BellyPan")
    pan_obj.Shape = pan

doc.recompute()

# ---------------- Exploded view offsets ----------------
if EXPLODED_VIEW:
    try:
        lower_obj.Placement.Base = Vector(0, 0, EXPLODE_LOWER_DZ)
    except Exception:
        pass
    try:
        roof_obj.Placement.Base = Vector(0, 0, EXPLODE_ROOF_DZ)
    except Exception:
        pass
    if BELLY_ENABLE:
        try:
            pan_obj.Placement.Base = Vector(0, 0, EXPLODE_PAN_DZ)
        except Exception:
            pass
    doc.recompute()

available_body_depth = shoulder_local_z - bottom_open_local_z

# ---- Flat-roof TOF reflection sanity check (diagnostic only; no geometry changes) ----
tof_emit_z = PLATE_THK
tof_reflector_z = roof_z
tof_dz = tof_reflector_z - tof_emit_z
tof_tilt_rad = math.radians(float(POD_TILT_DEG))

tof_required_tilt_deg = None
tof_inward_run_one_leg = None
tof_bounce_x = None
tof_return_x = None
tof_opp_miss = None
tof_leg_len_mm = None
tof_path_len_mm = None
tof_c_ms = None
tof_t0_us = None
tof_beam_proj = None
tof_wind_scale = None
tof_sample_rows = []
tof_beam_trace_rows = []
tof_beam_axis_ratio = None
tof_beam_expected_ratio = None

def _tof_unit(v):
    length = (v.x * v.x + v.y * v.y + v.z * v.z) ** 0.5
    if length < 1e-12:
        return Vector(0.0, 0.0, 1.0)
    return Vector(v.x / length, v.y / length, v.z / length)

def _tof_dot(a, b):
    return a.x * b.x + a.y * b.y + a.z * b.z

def _tof_reflect(v, n):
    k = 2.0 * _tof_dot(v, n)
    return Vector(v.x - k * n.x, v.y - k * n.y, v.z - k * n.z)

def _tof_ray_plane_intersect_z(p0, direction, z_plane):
    if abs(direction.z) < 1e-9:
        return None
    t = (z_plane - p0.z) / direction.z
    if t <= 0.0:
        return None
    return Vector(p0.x + t * direction.x, p0.y + t * direction.y, p0.z + t * direction.z)

def _tof_reflector_shape_z_grad(x, y, mode):
    mode_n = str(mode).lower()
    if mode_n == "dish":
        rr = math.hypot(x, y)
        reflector_r = max(0.1, float(TOF_REFLECTOR_DISH_DIAM_MM) / 2.0)
        depth = max(0.0, float(TOF_REFLECTOR_DISH_DEPTH_MM))
        if (rr <= reflector_r) and (depth > 1e-9):
            base = float(roof_z) - depth
            z_local = base + depth * ((rr * rr) / (reflector_r * reflector_r))
            dzdx = (2.0 * depth * x) / (reflector_r * reflector_r)
            dzdy = (2.0 * depth * y) / (reflector_r * reflector_r)
            return z_local, dzdx, dzdy
        return float(roof_z), 0.0, 0.0

    if mode_n == "cone":
        rr = math.hypot(x, y)
        reflector_r = max(0.1, float(TOF_REFLECTOR_CONE_DIAM_MM) / 2.0)
        depth = max(0.0, float(TOF_REFLECTOR_CONE_DEPTH_MM))
        if (rr <= reflector_r) and (depth > 1e-9):
            base = float(roof_z) - depth
            z_local = base + depth * (rr / reflector_r)
            if rr > 1e-9:
                slope = depth / reflector_r
                dzdx = slope * (x / rr)
                dzdy = slope * (y / rr)
            else:
                dzdx, dzdy = 0.0, 0.0
            return z_local, dzdx, dzdy
        return float(roof_z), 0.0, 0.0

    return float(roof_z), 0.0, 0.0

def _tof_reflector_normal_at(x, y, mode):
    _, dzdx, dzdy = _tof_reflector_shape_z_grad(x, y, mode)
    return _tof_unit(Vector(-dzdx, -dzdy, 1.0))

def _tof_ray_reflector_intersect(p0, direction, mode):
    if abs(direction.z) < 1e-9:
        return None

    t = (float(roof_z) - p0.z) / direction.z
    if t <= 0.0:
        return None

    for _ in range(14):
        x = p0.x + t * direction.x
        y = p0.y + t * direction.y
        z = p0.z + t * direction.z

        fz, dzdx, dzdy = _tof_reflector_shape_z_grad(x, y, mode)
        g = z - fz
        if abs(g) < 1e-5:
            n = _tof_reflector_normal_at(x, y, mode)
            return Vector(x, y, z), n

        dgdt = direction.z - (dzdx * direction.x + dzdy * direction.y)
        if abs(dgdt) < 1e-10:
            return None

        t = t - (g / dgdt)
        if t <= 0.0:
            return None

    return None

def _tof_sample_dir_in_cone(axis_dir, half_angle_deg, rng):
    axis = _tof_unit(axis_dir)
    half_angle = math.radians(float(half_angle_deg))

    u = rng.random()
    cos_theta = (1.0 - u) + (u * math.cos(half_angle))
    sin_theta = max(0.0, 1.0 - (cos_theta * cos_theta)) ** 0.5
    phi = 2.0 * math.pi * rng.random()

    helper = Vector(1.0, 0.0, 0.0) if abs(axis.x) < 0.9 else Vector(0.0, 1.0, 0.0)
    e1 = _tof_unit(axis.cross(helper))
    e2 = _tof_unit(axis.cross(e1))

    direction = Vector(
        axis.x * cos_theta + (e1.x * math.cos(phi) + e2.x * math.sin(phi)) * sin_theta,
        axis.y * cos_theta + (e1.y * math.cos(phi) + e2.y * math.sin(phi)) * sin_theta,
        axis.z * cos_theta + (e1.z * math.cos(phi) + e2.z * math.sin(phi)) * sin_theta,
    )
    return _tof_unit(direction)

def _tof_segment_hits_standoff(p0, p1, sx, sy, sr, z_min, z_max):
    dx = p1.x - p0.x
    dy = p1.y - p0.y
    dz = p1.z - p0.z

    ax = p0.x - sx
    ay = p0.y - sy

    a = dx * dx + dy * dy
    if a < 1e-12:
        return False

    b = 2.0 * (ax * dx + ay * dy)
    c = ax * ax + ay * ay - (sr * sr)
    disc = b * b - 4.0 * a * c
    if disc < 0.0:
        return False

    root = math.sqrt(max(0.0, disc))
    t1 = (-b - root) / (2.0 * a)
    t2 = (-b + root) / (2.0 * a)

    for tt in (t1, t2):
        if 0.0 <= tt <= 1.0:
            zz = p0.z + tt * dz
            if (z_min - 1e-6) <= zz <= (z_max + 1e-6):
                return True
    return False

def _tof_receiver_hits(recv_pt, receiver, radii):
    dx = recv_pt.x - receiver.x
    dy = recv_pt.y - receiver.y
    rr = math.hypot(dx, dy)
    return rr, [rr <= float(rm) for rm in radii]

def _tof_run_beam_trace(half_angle_deg, n_rays, receiver_radii_mm, yaw_deg, seed, weight_exponents, reflector_mode):
    emit_z = float(PLATE_THK)
    if float(roof_z) <= emit_z:
        return None

    emitter = Vector(+float(POD_RADIUS), 0.0, emit_z)
    receiver = Vector(-float(POD_RADIUS), 0.0, emit_z)

    tilt = math.radians(float(POD_TILT_DEG))
    yaw = math.radians(float(yaw_deg))
    axis = Vector(-math.sin(tilt), 0.0, math.cos(tilt))

    if abs(yaw_deg) > 1e-9:
        cy = math.cos(yaw)
        sy = math.sin(yaw)
        axis = Vector(axis.x * cy - axis.y * sy, axis.x * sy + axis.y * cy, axis.z)

    axis = _tof_unit(axis)

    rng = random.Random(
        int(seed)
        + int(round(half_angle_deg * 100.0))
        + int(round(float(yaw_deg) * 1000.0))
        + (0 if str(reflector_mode).lower() == "flat" else (100000 if str(reflector_mode).lower() == "dish" else 200000))
    )

    radii = [float(rm) for rm in receiver_radii_mm]
    if not radii:
        radii = [8.0]
    weights = [int(n) for n in weight_exponents]
    if not weights:
        weights = [6, 10]

    valid = 0
    miss_radii = []
    hit_counts = [0 for _ in radii]
    weighted_total = {n: 0.0 for n in weights}
    weighted_hits = {n: [0.0 for _ in radii] for n in weights}

    roof_hit_radii = []
    roof_hit_x = []
    roof_hit_y = []

    blocked_standoff = 0
    blocked_bore = 0
    roof_in_par_aperture = 0
    roof_in_par_wall = 0
    pod_rim_clip = 0

    bore_len = max(0.1, float(TOF_BORE_EFFECTIVE_LEN_MM))
    bore_r = float(BORE_MAIN_D) / 2.0
    bore_max_angle = math.atan2(bore_r, bore_len)

    standoff_r = (float(STANDOFF_OD) / 2.0) + max(0.0, float(STANDOFF_ROOT_FLARE_EXTRA_R), float(STANDOFF_GUSSET_EXTRA_R))
    standoff_centers = []
    for aa in STANDOFF_ANGLES:
        rr = math.radians(float(aa))
        standoff_centers.append((float(ROOF_STANDOFF_RADIUS) * math.cos(rr), float(ROOF_STANDOFF_RADIUS) * math.sin(rr)))

    for _ in range(int(n_rays)):
        ray0 = _tof_sample_dir_in_cone(axis, half_angle_deg, rng)

        cos_to_axis = max(-1.0, min(1.0, _tof_dot(ray0, axis)))
        off_axis = math.acos(cos_to_axis)
        if TOF_BLOCK_CHECK_ENABLE and (off_axis > bore_max_angle):
            blocked_bore += 1
            continue

        hit_reflector = _tof_ray_reflector_intersect(emitter, ray0, reflector_mode)
        if hit_reflector is None:
            continue
        roof_pt, roof_n = hit_reflector

        if TOF_BLOCK_CHECK_ENABLE:
            out_end = Vector(
                roof_pt.x - (0.05 * ray0.x),
                roof_pt.y - (0.05 * ray0.y),
                roof_pt.z - (0.05 * ray0.z),
            )
            for sx, sy in standoff_centers:
                if _tof_segment_hits_standoff(emitter, out_end, sx, sy, standoff_r, 0.0, float(roof_z)):
                    blocked_standoff += 1
                    roof_pt = None
                    break
            if roof_pt is None:
                continue

        ray1 = _tof_reflect(ray0, roof_n)

        recv_pt = _tof_ray_plane_intersect_z(roof_pt, ray1, emit_z)
        if recv_pt is None:
            continue

        if TOF_BLOCK_CHECK_ENABLE:
            in_start = Vector(
                roof_pt.x + (0.05 * ray1.x),
                roof_pt.y + (0.05 * ray1.y),
                roof_pt.z + (0.05 * ray1.z),
            )
            blocked = False
            for sx, sy in standoff_centers:
                if _tof_segment_hits_standoff(in_start, recv_pt, sx, sy, standoff_r, 0.0, float(roof_z)):
                    blocked_standoff += 1
                    blocked = True
                    break
            if blocked:
                continue

        rr, hit_flags = _tof_receiver_hits(recv_pt, receiver, radii)
        miss_radii.append(rr)
        if rr > (float(POD_DIAM) / 2.0):
            pod_rim_clip += 1
        roof_hit_r = math.hypot(roof_pt.x, roof_pt.y)
        roof_hit_radii.append(roof_hit_r)
        roof_hit_x.append(roof_pt.x)
        roof_hit_y.append(roof_pt.y)

        if PAR_ENABLE:
            par_outer_r = float(PAR_HOUSING_OD) / 2.0
            par_inner_r = max(0.0, par_outer_r - float(PAR_HOUSING_WALL))
            if roof_hit_r <= par_inner_r:
                roof_in_par_aperture += 1
            elif roof_hit_r <= par_outer_r:
                roof_in_par_wall += 1

        valid += 1

        for i, ok in enumerate(hit_flags):
            if ok:
                hit_counts[i] += 1

        for n in weights:
            w = cos_to_axis ** max(0, int(n))
            weighted_total[n] += w
            for i, ok in enumerate(hit_flags):
                if ok:
                    weighted_hits[n][i] += w

    if valid <= 0:
        return {
            "mode": str(reflector_mode).lower(),
            "half_angle": float(half_angle_deg),
            "n_rays": int(n_rays),
            "valid": 0,
            "receiver_radii": radii,
            "hit_pct": [0.0 for _ in radii],
            "weighted_hit_pct": {n: [0.0 for _ in radii] for n in weights},
            "p50": None,
            "p90": None,
            "p99": None,
            "path_mean_mm": None,
            "path_delta_center_mm": None,
            "blocked_bore": int(blocked_bore),
            "blocked_standoff": int(blocked_standoff),
            "roof_in_par_aperture_pct": 0.0,
            "roof_in_par_wall_pct": 0.0,
            "pod_rim_clip_pct": 0.0,
            "roof_r_p90": None,
            "roof_r_p99": None,
        }

    miss_radii.sort()
    i50 = int(0.50 * (valid - 1))
    i90 = int(0.90 * (valid - 1))
    i99 = int(0.99 * (valid - 1))

    roof_hit_radii.sort()
    ir90 = int(0.90 * (valid - 1))
    ir99 = int(0.99 * (valid - 1))

    path_samples = []
    for i in range(min(valid, 256)):
        rx = roof_hit_x[i]
        ry = roof_hit_y[i]
        rz, _, _ = _tof_reflector_shape_z_grad(rx, ry, reflector_mode)
        leg1 = math.sqrt((rx - emitter.x) ** 2 + (ry - emitter.y) ** 2 + (rz - emitter.z) ** 2)
        leg2 = math.sqrt((rx - receiver.x) ** 2 + (ry - receiver.y) ** 2 + (rz - receiver.z) ** 2)
        path_samples.append(leg1 + leg2)

    path_mean_mm = (sum(path_samples) / float(len(path_samples))) if path_samples else None

    center_pt = _tof_ray_reflector_intersect(emitter, axis, reflector_mode)
    path_delta_center_mm = None
    if center_pt is not None and tof_path_len_mm is not None:
        cpt, _ = center_pt
        c_leg1 = math.sqrt((cpt.x - emitter.x) ** 2 + (cpt.y - emitter.y) ** 2 + (cpt.z - emitter.z) ** 2)
        c_leg2 = math.sqrt((cpt.x - receiver.x) ** 2 + (cpt.y - receiver.y) ** 2 + (cpt.z - receiver.z) ** 2)
        path_delta_center_mm = (c_leg1 + c_leg2) - float(tof_path_len_mm)

    hit_pct = [100.0 * float(hc) / float(valid) for hc in hit_counts]
    weighted_hit_pct = {}
    for n in weights:
        denom = weighted_total[n]
        if denom <= 1e-12:
            weighted_hit_pct[n] = [0.0 for _ in radii]
        else:
            weighted_hit_pct[n] = [100.0 * float(vv) / float(denom) for vv in weighted_hits[n]]

    return {
        "mode": str(reflector_mode).lower(),
        "half_angle": float(half_angle_deg),
        "n_rays": int(n_rays),
        "valid": int(valid),
        "receiver_radii": radii,
        "hit_pct": hit_pct,
        "weighted_hit_pct": weighted_hit_pct,
        "p50": float(miss_radii[i50]),
        "p90": float(miss_radii[i90]),
        "p99": float(miss_radii[i99]),
        "path_mean_mm": path_mean_mm,
        "path_delta_center_mm": path_delta_center_mm,
        "blocked_bore": int(blocked_bore),
        "blocked_standoff": int(blocked_standoff),
        "roof_in_par_aperture_pct": 100.0 * float(roof_in_par_aperture) / float(valid),
        "roof_in_par_wall_pct": 100.0 * float(roof_in_par_wall) / float(valid),
        "pod_rim_clip_pct": 100.0 * float(pod_rim_clip) / float(valid),
        "roof_r_p90": float(roof_hit_radii[ir90]),
        "roof_r_p99": float(roof_hit_radii[ir99]),
    }

if tof_dz > 0 and abs(math.tan(tof_tilt_rad)) > 1e-9:
    # 1D along a pod-opposite-pod diameter line (+R source side, -R opposite side)
    tof_inward_run_one_leg = float(tof_dz) * math.tan(tof_tilt_rad)
    tof_bounce_x = float(POD_RADIUS) - tof_inward_run_one_leg
    tof_return_x = float(POD_RADIUS) - (2.0 * tof_inward_run_one_leg)
    tof_opp_miss = tof_return_x - (-float(POD_RADIUS))
    tof_required_tilt_deg = math.degrees(math.atan2(float(POD_RADIUS), float(tof_dz)))

    tof_leg_len_mm = math.hypot(float(tof_dz), tof_inward_run_one_leg)
    tof_path_len_mm = 2.0 * tof_leg_len_mm
    tof_c_ms = 331.3 + (0.606 * float(TOF_AIR_TEMP_C))
    tof_t0_us = ((tof_path_len_mm / 1000.0) / tof_c_ms) * 1e6
    tof_beam_proj = abs(math.sin(tof_tilt_rad))

    if tof_beam_proj > 1e-6:
        tof_wind_scale = (tof_path_len_mm / 1000.0) / (2.0 * tof_beam_proj)

        for wind_mps in TOF_SAMPLE_WINDS_MPS:
            u_path = float(wind_mps) * tof_beam_proj
            t_ab = ((tof_path_len_mm / 1000.0) / (tof_c_ms + u_path)) * 1e6
            t_ba = ((tof_path_len_mm / 1000.0) / (tof_c_ms - u_path)) * 1e6
            tof_sample_rows.append((float(wind_mps), t_ab, t_ba, abs(t_ba - t_ab)))

if TOF_BEAM_TRACE_ENABLE:
    for mode in TOF_REFLECTOR_TEST_MODES:
        for half_angle in TOF_BEAM_TRACE_HALF_ANGLES:
            row = _tof_run_beam_trace(
                half_angle_deg=float(half_angle),
                n_rays=int(TOF_BEAM_TRACE_RAYS),
                receiver_radii_mm=TOF_BEAM_TRACE_RECEIVER_RADII_MM,
                yaw_deg=float(TOF_BEAM_TRACE_YAW_DEG),
                seed=int(TOF_BEAM_TRACE_SEED),
                weight_exponents=TOF_BEAM_WEIGHT_EXPONENTS,
                reflector_mode=mode,
            )
            if row is not None:
                tof_beam_trace_rows.append(row)

    if abs(math.cos(math.radians(float(POD_TILT_DEG)))) > 1e-9:
        tof_beam_axis_ratio = -math.tan(math.radians(float(POD_TILT_DEG)))
    if tof_dz > 1e-9:
        tof_beam_expected_ratio = -float(POD_RADIUS) / float(tof_dz)

print("\n============================================================")
print("   ULTRASONIC ANEMOMETER HEAD - BUILD COMPLETE")
print("============================================================")
print("\n---- Pod / Bore Sealing ----")
print("Main bore (D):                 {:.2f} mm".format(BORE_MAIN_D))
print("Neck bore (D):                 {:.2f} mm".format(BORE_NECK_D))
print("Bore extend below opening:     {:.2f} mm".format(BORE_EXT_BELOW_OPEN))
print("Sil pocket depth:              {:.2f} mm".format(SIL_SEAL_DEPTH))
print("Sil pocket extra below:        {:.2f} mm".format(SIL_POCKET_EXT_BELOW))
print("Sil pocket extra radius:       {:.2f} mm".format(SIL_SEAL_EXTRA_R))
print("\n---- Plate-to-belly fastening ----")
print("Belly tube OD:                 {:.2f} mm".format(BELLY_TUBE_OD))
print("Plate bolt hole D:             {:.2f} mm".format(PLATE_BOLT_HOLE_D))
print("Insert bore D:                 {:.2f} mm".format(TOP_INSERT_BORE_D))
print("Insert depth:                  {:.2f} mm".format(TOP_INSERT_DEPTH))
print("\n---- Key Geometry ----")
print("ROOF_STANDOFF_RADIUS:          {:.2f} mm".format(ROOF_STANDOFF_RADIUS))
print("BELLY_FASTEN_RADIUS:           {:.2f} mm".format(BELLY_FASTEN_RADIUS))
print("Ports Z center:                {:.1f} mm".format(PORT_Z_CENTER))
print("Available pod depth:           {:.2f} mm".format(available_body_depth))

if PAR_ENABLE:
    par_diffuser_pocket_d = float(PAR_DIFFUSER_D) + float(PAR_DIFFUSER_OD_CLEAR_D)
    par_cable_cut_len = float(PAR_HOUSING_WALL) + float(PAR_CABLE_PORT_CUT_EXTRA)
    print("\n---- PAR / Diffuser Check ----")
    print("Diffuser target D:             {:.2f} mm".format(PAR_DIFFUSER_D))
    print("Pocket cut D (effective):      {:.2f} mm".format(par_diffuser_pocket_d))
    print("Pocket recess depth:           {:.2f} mm".format(PAR_DIFFUSER_RECESS_DEPTH))
    print("PAR cable port cut length:     {:.2f} mm".format(par_cable_cut_len))

if BATTERY_CHECK_ENABLE:
    belly_inner_r_chk = (HEAD_DIAM / 2.0) - BELLY_WALL_THK
    battery_half_diag = 0.5 * math.hypot(float(BATTERY_L_MM), float(BATTERY_W_MM))
    battery_radial_margin = belly_inner_r_chk - battery_half_diag
    battery_height_margin = float(PCB_STANDOFF_H) - float(BATTERY_H_MM)

    # PCB-to-wall radial clearances (using current PCB placement/rotation)
    x0_chk = -PCB_W / 2.0 + PCB_OFFSET_X
    y0_chk = -PCB_H / 2.0 + PCB_OFFSET_Y
    rot_chk = math.radians(float(PCB_ROT_DEG))
    cr_chk = math.cos(rot_chk)
    sr_chk = math.sin(rot_chk)

    pcb_corner_pts = [
        (x0_chk, y0_chk),
        (x0_chk + PCB_W, y0_chk),
        (x0_chk + PCB_W, y0_chk + PCB_H),
        (x0_chk, y0_chk + PCB_H),
    ]

    max_corner_r = 0.0
    for (px0, py0) in pcb_corner_pts:
        px = (px0 * cr_chk) - (py0 * sr_chk)
        py = (px0 * sr_chk) + (py0 * cr_chk)
        max_corner_r = max(max_corner_r, math.hypot(px, py))

    max_post_center_r = 0.0
    for (hx, hy) in PCB_HOLES:
        px0 = x0_chk + hx
        py0 = y0_chk + hy
        px = (px0 * cr_chk) - (py0 * sr_chk)
        py = (px0 * sr_chk) + (py0 * cr_chk)
        max_post_center_r = max(max_post_center_r, math.hypot(px, py))

    pcb_corner_margin = belly_inner_r_chk - max_corner_r
    pcb_post_outer_margin = belly_inner_r_chk - (max_post_center_r + (PCB_POST_OD / 2.0))

    print("\n---- Battery Envelope Check ----")
    print("Battery LxWxH:                 {:.1f} x {:.1f} x {:.1f} mm".format(BATTERY_L_MM, BATTERY_W_MM, BATTERY_H_MM))
    print("Belly inner radius (approx):   {:.2f} mm".format(belly_inner_r_chk))
    print("Battery half-diagonal:         {:.2f} mm".format(battery_half_diag))
    print("Radial fit margin (approx):    {:.2f} mm".format(battery_radial_margin))
    print("Under-PCB height margin:       {:.2f} mm".format(battery_height_margin))
    print("PCB corner radial margin:      {:.2f} mm".format(pcb_corner_margin))
    print("PCB post outer radial margin:  {:.2f} mm".format(pcb_post_outer_margin))

if CONNECTORS_ENABLE:
    z_floor_chk = -float(BELLY_WALL_H)
    pcb_top_z_chk = z_floor_chk + float(PCB_STANDOFF_H)
    batt_top_z_chk = z_floor_chk + float(BATTERY_H_MM)

    gx12_zc_chk = float(PORT_Z_CENTER if (GX12_Z_CENTER is None) else GX12_Z_CENTER)
    gx12_bot_z = gx12_zc_chk - (float(GX12_HOLE_D) / 2.0)

    print("\n---- Connector / Gland Clearance ----")
    print("PCB top Z (post top):          {:.2f} mm".format(pcb_top_z_chk))
    print("Battery top Z (approx):        {:.2f} mm".format(batt_top_z_chk))

    print("GX12 hole center Z:            {:.2f} mm".format(gx12_zc_chk))
    print("GX12 hole bottom Z:            {:.2f} mm".format(gx12_bot_z))
    print("GX12 bottom above PCB top:     {:.2f} mm".format(gx12_bot_z - pcb_top_z_chk))
    print("GX12 bottom above battery top: {:.2f} mm".format(gx12_bot_z - batt_top_z_chk))

    pg7_on = angle_ok(PG7_ANGLE_DEG)
    if pg7_on:
        pg7_bot_z = float(PORT_Z_CENTER) - (float(PG7_HOLE_D) / 2.0)
        print("PG7 enabled:                   yes")
        print("PG7 hole bottom Z:             {:.2f} mm".format(pg7_bot_z))
        print("PG7 bottom above PCB top:      {:.2f} mm".format(pg7_bot_z - pcb_top_z_chk))
    else:
        print("PG7 enabled:                   no (angle blocked by standoff keepout)")

    sht_on = angle_ok(SHT_ANGLE_DEG)
    if sht_on:
        sht_bot_z = float(PORT_Z_CENTER) - (float(SHT_HOLE_D) / 2.0)
        print("SHT enabled:                   yes")
        print("SHT hole bottom Z:             {:.2f} mm".format(sht_bot_z))
        print("SHT bottom above PCB top:      {:.2f} mm".format(sht_bot_z - pcb_top_z_chk))
    else:
        print("SHT enabled:                   no (angle blocked by standoff keepout)")

print("\n---- TOF Flat-Roof Check ----")
if tof_required_tilt_deg is None:
    print("TOF check:                     unavailable (invalid dz/tilt)")
else:
    print("Reflector dz (emit->roof):     {:.2f} mm".format(tof_dz))
    print("Current pod tilt:              {:.2f} deg".format(POD_TILT_DEG))
    print("Tilt for center bounce:        {:.2f} deg".format(tof_required_tilt_deg))
    print("Inward run to roof (1-leg):    {:.2f} mm".format(tof_inward_run_one_leg))
    print("Bounce X at roof (0=center):   {:.2f} mm".format(tof_bounce_x))
    print("Return X at emit z:            {:.2f} mm".format(tof_return_x))
    print("Opposite-pod center miss:      {:.2f} mm".format(tof_opp_miss))
    print("Path length (pod->roof->pod):  {:.2f} mm".format(tof_path_len_mm))
    print("Assumed air temp:              {:.1f} C".format(TOF_AIR_TEMP_C))
    print("Speed of sound (assumed):      {:.2f} m/s".format(tof_c_ms))
    print("No-wind TOF (A->B):            {:.2f} us".format(tof_t0_us))

    if tof_wind_scale is None:
        print("Wind solve factor:             unavailable (beam projection too small)")
    else:
        print("Beam projection (|sin tilt|):  {:.4f}".format(tof_beam_proj))
        print("Wind solve factor K:           {:.6f} m".format(tof_wind_scale))
        print("Wind solve formula:            U_axis = K * (1/t_AB - 1/t_BA)")
        print("(t_AB, t_BA in seconds; positive U along A->B axis)")

        if tof_sample_rows:
            print("Sample timing deltas (at assumed temp):")
            for wind_mps, t_ab, t_ba, dt_us in tof_sample_rows:
                print("  U={:>4.1f} m/s -> t_AB={:>8.2f} us, t_BA={:>8.2f} us, |dt|={:>6.2f} us".format(
                    wind_mps, t_ab, t_ba, dt_us
                ))

if TOF_BEAM_TRACE_ENABLE:
    print("\n---- TOF Beam Trace (reflector + spread diagnostics) ----")
    print("Beam yaw offset:               {:.2f} deg".format(float(TOF_BEAM_TRACE_YAW_DEG)))
    if (tof_beam_axis_ratio is not None) and (tof_beam_expected_ratio is not None):
        print("Axis slope vx/vz (from tilt):  {:.5f}".format(tof_beam_axis_ratio))
        print("Axis slope vx/vz (expected):   {:.5f}".format(tof_beam_expected_ratio))

    if not tof_beam_trace_rows:
        print("Beam trace:                    unavailable (check reflector/emit planes)")
    else:
        for row in tof_beam_trace_rows:
            mode_name = str(row.get("mode", "flat")).lower()
            print(
                "Mode {:>4s} | Half-angle {:>5.1f} deg | Rays {:>5d} | Valid {:>5d}".format(
                    mode_name,
                    row["half_angle"],
                    row["n_rays"],
                    row["valid"],
                )
            )

            radii = row.get("receiver_radii", [])
            hit_pct = row.get("hit_pct", [])
            weighted = row.get("weighted_hit_pct", {})
            if radii and hit_pct:
                radius_parts = []
                for rr, hp in zip(radii, hit_pct):
                    radius_parts.append("R={:.0f}mm:{:.2f}%".format(float(rr), float(hp)))
                print("  Geometric hit fraction:       {}".format(" | ".join(radius_parts)))

            for wn in TOF_BEAM_WEIGHT_EXPONENTS:
                wvals = weighted.get(int(wn), None)
                if wvals is None:
                    continue
                w_parts = []
                for rr, hv in zip(radii, wvals):
                    w_parts.append("R={:.0f}mm:{:.2f}%".format(float(rr), float(hv)))
                print("  Weighted hit (cos^{}):        {}".format(int(wn), " | ".join(w_parts)))

            if row["p50"] is None:
                print("  Miss radius p50/p90/p99:     unavailable")
            else:
                print("  Miss radius p50/p90/p99:     {:.2f} / {:.2f} / {:.2f} mm".format(row["p50"], row["p90"], row["p99"]))

            if row.get("roof_r_p90") is not None:
                print("  Roof footprint radius p90/p99:{:.2f} / {:.2f} mm".format(row["roof_r_p90"], row["roof_r_p99"]))
                print(
                    "  Roof hits in PAR aperture/wall:{:.2f}% / {:.2f}%".format(
                        float(row.get("roof_in_par_aperture_pct", 0.0)),
                        float(row.get("roof_in_par_wall_pct", 0.0)),
                    )
                )

            if row.get("path_delta_center_mm") is not None:
                print("  Center-ray path delta vs flat:{:+.3f} mm".format(row["path_delta_center_mm"]))

            if TOF_BLOCK_CHECK_ENABLE:
                print(
                    "  Blocked rays (bore/standoff): {:d} / {:d} | Pod-rim clip: {:.2f}%".format(
                        int(row.get("blocked_bore", 0)),
                        int(row.get("blocked_standoff", 0)),
                        float(row.get("pod_rim_clip_pct", 0.0)),
                    )
                )

        best_flat = None
        best_alt = None
        for row in tof_beam_trace_rows:
            mode_name = str(row.get("mode", "flat")).lower()
            weighted = row.get("weighted_hit_pct", {})
            w_main = weighted.get(int(TOF_BEAM_WEIGHT_EXPONENTS[-1]), None)
            if not w_main:
                continue
            score = max(w_main)
            if mode_name == "flat":
                if (best_flat is None) or (score > best_flat[0]):
                    best_flat = (score, row)
            else:
                if (best_alt is None) or (score > best_alt[0]):
                    best_alt = (score, row)

        print("\nBeam-trace recommendation:")
        if (best_flat is None) or (best_alt is None):
            print("  Insufficient data for flat-vs-focus recommendation.")
        else:
            flat_score = best_flat[0]
            alt_score = best_alt[0]
            gain = alt_score - flat_score
            if gain < 3.0:
                print("  Flat reflector acceptable (best weighted hit gain < 3%).")
            else:
                print(
                    "  Consider weak focusing reflector (best gain +{:.2f}% in mode '{}').".format(
                        gain,
                        best_alt[1].get("mode", "?")
                    )
                )

print("\n---- 3D Printing Tips ----")
print("• Print belly pan UPSIDE DOWN (base on bed)")
print("• Material: PETG or ASA for outdoor use")
print("\n============================================================")