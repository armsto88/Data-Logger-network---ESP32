import math
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
EXPLODED_VIEW = True
EXPLODE_LOWER_DZ = 0.0
EXPLODE_ROOF_DZ  = 35.0
EXPLODE_PAN_DZ   = -105.0

# ---------------- PARAMETERS ----------------
HEAD_DIAM = 150.0
ROOF_DIAM = 150.0
PLATE_THK = 6.0
PLATE_DROP = 10.0   # mm to extend plate downward (tune)
PLATE_TOP_FILLET_R = 4.0   # try 3–6 mm

POD_RADIUS   = 39.2
POD_DIAM     = 24.0
POD_HEIGHT   = 12.0
POD_TILT_DEG = 30.0
POD_THROUGH  = 5.0

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

# ---------------- Reflector parameters ----------------
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

# --- Teardrop standoff geometry (present but you can ignore; you’re using ROUND now) ---
STANDOFF_SHAPE = "teardrop"        # "cyl" or "teardrop"

TEARDROP_WIDTH  = STANDOFF_OD
TEARDROP_LENGTH = STANDOFF_OD * 1.15
TEARDROP_NOSE_R = TEARDROP_WIDTH * 0.50
TEARDROP_TAPER_EXP = 1.2

TEARDROP_ORIENT_MODE = "radial_out"
TEARDROP_YAW_OFFSET_DEG = 0.0

# Root reinforcement (optional; improves strength where pillar meets plate)
STANDOFF_ROOT_FLARE_ENABLE = True
STANDOFF_ROOT_FLARE_H = 6.0
STANDOFF_ROOT_FLARE_EXTRA_R = 4.0

# ---------------- Roof holes (standoff -> roof plate) ----------------
ROOF_HOLE_D       = 3.4
ADD_COUNTERBORE   = True
COUNTERBORE_D     = 7.0
COUNTERBORE_DEPTH = 2.5

# ---------------- Belly cup ----------------
BELLY_ENABLE   = True
BELLY_BASE_THK = 3.0
BELLY_WALL_H   = 75.0
BELLY_WALL_THK = 6.0

# Beef up internal belly fastening tubes:
BELLY_TUBE_OD      = 12.0
BELLY_FLOOR_HOLES_ENABLE = False   # no holes through belly floor

# ---------------- Drip edge (external skirt under rim) ----------------
DRIP_EDGE_ENABLE   = True
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
GX12_QTY = 4

GX12_ROW_CENTER_ANGLE = 320.0
GX12_ROW_SPACING_DEG  = 25.0
GX12_ROW_OFFSET_DEG   = -15.0
GX12_Z_CENTER         = -30.0
GX12_PAIR_SEPARATION_DEG = 210.0

PG7_HOLE_D    = 13.0
PG7_NUT_D     = 17.9
PG7_ANGLE_DEG = 225.0

SHT_HOLE_D    = 11.5
SHT_NUT_D     = 15.0
SHT_ANGLE_DEG = 45.0

PORT_Z_CENTER        = None
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
PCB_W      = 100.0
PCB_H      = 100.0
PCB_HOLE_D = 3.2

PCB_EDGE_MARGIN = 12.0
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

PCB_ROT_DEG = BELLY_FASTEN_OFFSET

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
PAR_HOUSING_OD   = 45.0
PAR_HOUSING_H    = 20.0
PAR_APERTURE_D   = 30.0

PAR_BOSS_OFFSET_XY = [(0.0, 8.69), (0.0, -8.69)]
PAR_BOSS_HEIGHT    = 10.0
PAR_BOSS_DIAM      = 5.0
PAR_SCREW_DIAMETER = 2.2

PAR_CABLE_PORT_D     = 7.0
PAR_CABLE_PORT_Z_OFF = 5.0
PAR_CABLE_PORT_ANGLE = 90.0

# ---------------- Aerodynamic edge rounding ----------------
EDGE_ROUND_ENABLE = True
HEAD_RIM_FILLET_R = 4.0
ROOF_RIM_FILLET_R = 2.0
RIM_RADIUS_TOL = 0.6

# ---------------- Support root reinforcement (fillet-like gussets) ----------------
SUPPORT_GUSSET_ENABLE = True
STANDOFF_GUSSET_H       = 3.0
STANDOFF_GUSSET_EXTRA_R = 3.0
PCB_POST_GUSSET_H       = 2.0
PCB_POST_GUSSET_EXTRA_R = 2.0

I2C_CONDUIT_ENABLE = True
I2C_CONDUIT_STANDOFF_INDEX = 1   # 0,1,2,3  -> 90° if your angles are [0,90,180,270]

I2C_CABLE_BORE_D = 4.0
I2C_ENTRY_HOLE_D = 4.2
I2C_ENTRY_Z_FROM_TOP = 8.0       # mm below roof_top_z (keeps it under roof)
I2C_ENTRY_LEN_EXTRA = 8.0        # extra length so the side hole always reaches the bore

I2C_EXIT_ENABLE = True
I2C_EXIT_W = 7.0
I2C_EXIT_H = 10.0
I2C_EXIT_Z_FROM_PLATE = 14.0
I2C_EXIT_RELIEF_ENABLE = True
I2C_TOP_CAP_THK = 2.0    # mm of solid material above the conduit
I2C_INSERT_CLEAR_THK = 1.0  # mm below the insert bore start
I2C_ENTRY_FUNNEL_ENABLE = True
I2C_ENTRY_FUNNEL_D = 7.5      # mouth diameter (try 6.5–9.0)
I2C_ENTRY_FUNNEL_L = 3.0      # funnel length/depth (2–4)

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
        if abs(bb.ZMax - zt) > float(tol_z) and abs(bb.ZMin - zt) > float(tol_z):
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
            exit_z = PLATE_THK - float(I2C_EXIT_Z_FROM_PLATE)

            ix = -math.cos(rad)
            iy = -math.sin(rad)

            exit_len = (STANDOFF_OD / 2.0) + 10.0  # generous so it breaks out inward reliably
            exit_cut_body = Part.makeCylinder(
                float(I2C_CABLE_BORE_D) / 2.0,
                exit_len,
                Vector(x, y, exit_z),
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
    drill_len = wall_thk + 2.0

    cable_cutter = Part.makeCylinder(
        float(PAR_CABLE_PORT_D) / 2.0,
        drill_len,
        Vector(start_x, cy, port_z),
        Vector(-1, 0, 0)
    )
    cable_cutter.rotate(Vector(cx, cy, 0), Vector(0, 0, 1), float(PAR_CABLE_PORT_ANGLE))
    ring = ring.cut(cable_cutter)

    roof_final = roof_final.fuse(ring)

    for (dx, dy) in PAR_BOSS_OFFSET_XY:
        boss = Part.makeCylinder(PAR_BOSS_DIAM / 2.0, PAR_BOSS_HEIGHT, Vector(cx + dx, cy + dy, roof_top_z))
        hole = Part.makeCylinder(PAR_SCREW_DIAMETER / 2.0, PAR_BOSS_HEIGHT + 0.5, Vector(cx + dx, cy + dy, roof_top_z - 0.25))
        boss = boss.cut(hole)
        roof_final = roof_final.fuse(boss)

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
            if PORT_PADS_ENABLE:
                pan = add_port_pad(
                    pan, ang, gx12_zc, GX12_NUT_D, wall_outer_r,
                    pad_thk=PORT_PAD_THK, inset=PORT_PAD_INSET,
                    extra_w=PORT_PAD_EXTRA_W, extra_h=PORT_PAD_EXTRA_H,
                    chamfer_z=PORT_PAD_CHAMFER_Z, chamfer_run=PORT_PAD_CHAMFER_RUN,
                    chamfer_y=PORT_PAD_CHAMFER_Z, chamfer_run_y=PORT_PAD_CHAMFER_RUN
                )
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
                extra=PORT_CUT_EXTRA + (float(PORT_PAD_THK) if PORT_PADS_ENABLE else 0.0),
                outer_extra=(float(PORT_PAD_THK) if PORT_PADS_ENABLE else 0.0),
                inner_extra=(float(PORT_PAD_THK) if PORT_PADS_ENABLE else 0.0),
            )

        if angle_ok(PG7_ANGLE_DEG):
            if PORT_PADS_ENABLE:
                pan = add_port_pad(
                    pan, PG7_ANGLE_DEG, zc_ports, PG7_NUT_D, wall_outer_r,
                    pad_thk=PORT_PAD_THK, inset=PORT_PAD_INSET,
                    extra_w=PORT_PAD_EXTRA_W, extra_h=PORT_PAD_EXTRA_H,
                    chamfer_z=PORT_PAD_CHAMFER_Z, chamfer_run=PORT_PAD_CHAMFER_RUN,
                    chamfer_y=PORT_PAD_CHAMFER_Z, chamfer_run_y=PORT_PAD_CHAMFER_RUN
                )
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
                extra=PORT_CUT_EXTRA + (float(PORT_PAD_THK) if PORT_PADS_ENABLE else 0.0),
                outer_extra=(float(PORT_PAD_THK) if PORT_PADS_ENABLE else 0.0),
                inner_extra=(float(PORT_PAD_THK) if PORT_PADS_ENABLE else 0.0),
            )

        if angle_ok(SHT_ANGLE_DEG):
            if PORT_PADS_ENABLE:
                pan = add_port_pad(
                    pan, SHT_ANGLE_DEG, zc_ports, SHT_NUT_D, wall_outer_r,
                    pad_thk=PORT_PAD_THK, inset=PORT_PAD_INSET,
                    extra_w=PORT_PAD_EXTRA_W, extra_h=PORT_PAD_EXTRA_H,
                    chamfer_z=PORT_PAD_CHAMFER_Z, chamfer_run=PORT_PAD_CHAMFER_RUN,
                    chamfer_y=PORT_PAD_CHAMFER_Z, chamfer_run_y=PORT_PAD_CHAMFER_RUN
                )
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
                extra=PORT_CUT_EXTRA + (float(PORT_PAD_THK) if PORT_PADS_ENABLE else 0.0),
                outer_extra=(float(PORT_PAD_THK) if PORT_PADS_ENABLE else 0.0),
                inner_extra=(float(PORT_PAD_THK) if PORT_PADS_ENABLE else 0.0),
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
print("\n---- 3D Printing Tips ----")
print("• Print belly pan UPSIDE DOWN (base on bed)")
print("• Material: PETG or ASA for outdoor use")
print("\n============================================================")
