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
#       * wall ports + nut flats
#       * PCB standoffs on belly floor
# ============================================================

# ---------------- Exploded view (visual only) ----------------
EXPLODED_VIEW = False
EXPLODE_LOWER_DZ = 0.0
EXPLODE_ROOF_DZ  = 35.0
EXPLODE_PAN_DZ   = -105.0

# ---------------- PARAMETERS ----------------
HEAD_DIAM = 150.0
ROOF_DIAM = 150.0
PLATE_THK = 6.0

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

STANDOFF_OD         = 8.0
STANDOFF_RIM_MARGIN = 4.0

TOP_INSERT_BORE_D         = 4.6
TOP_INSERT_DEPTH          = 5.0
TOP_INSERT_LEADIN_CHAMFER = 0.6

# Old belly-screw scheme (into standoff bottoms) — disable by default now
STANDOFF_BOTTOM_INSERT_ENABLE = False
BOTTOM_INSERT_BORE_D         = 4.6
BOTTOM_INSERT_DEPTH          = 5.0
BOTTOM_INSERT_LEADIN_CHAMFER = 0.6

BOTTOM_ACCESS_ENABLE = False
BOTTOM_ACCESS_D     = BOTTOM_INSERT_BORE_D
BOTTOM_ACCESS_EXTRA = 0.4

# Roof holes (standoff -> roof plate)
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
BELLY_TUBE_OD      = 12.0     # was 10.0 (stronger around insert)
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
BELLY_FASTEN_ANGLES = [a + BELLY_FASTEN_OFFSET for a in [45,135,225,315]]

# ---------------- Plate-to-belly bolt holes ----------------
PLATE_BOLT_HOLE_D       = 3.4   # M3 clearance
PLATE_BOLT_CB_ENABLE    = True
PLATE_BOLT_CB_D         = 7.0
PLATE_BOLT_CB_DEPTH     = 2.5

# ---------------- Wall ports ----------------
CONNECTORS_ENABLE = True

GX12_HOLE_D     = 11.0
GX12_NUT_D      = 15.0
# ---- GX12 2x2 grid ----
GX12_QTY = 4

# Center angle for the row (pick where you want the row to sit)
GX12_ROW_CENTER_ANGLE = 40.0   # tweak (deg)

# Angular spacing between adjacent GX12s
GX12_ROW_SPACING_DEG = 18.0    # tweak (deg) 16–22 usually sensible

GX12_ROW_OFFSET_DEG = -5.0   # + = rotate clockwise (to the right when viewed from above)


# Z position for the whole row (defaults to PORT_Z_CENTER if None)
GX12_Z_CENTER = -30.0


PG7_HOLE_D    = 13.0
PG7_NUT_D     = 17.9
PG7_ANGLE_DEG = 240.0

SHT_HOLE_D    = 11.5
SHT_NUT_D     = 15.0
SHT_ANGLE_DEG = 315.0

PORT_Z_CENTER = None
PORT_CUT_EXTRA = 2.0
PORT_AVOID_STANDOFF_DEG = 12.0

# Nut-seat flats (CUTS)
NUT_FLATS_ENABLE   = True
NUT_FLAT_DEPTH_OUT = 1.0
NUT_FLAT_DEPTH_IN  = 1.0
NUT_FLAT_MARGIN    = 2.0
NUT_FLAT_Z_MARGIN  = 2.0

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

# Rotate PCB mounting pattern so posts align with belly fastening angles
PCB_ROT_DEG = BELLY_FASTEN_OFFSET   # 35.0 to match your belly fasteners


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

SHT_RS_NUT_CLEAR_ENABLE = True
SHT_RS_NUT_CLEAR_MARGIN = 3.5
SHT_RS_NUT_CLEAR_OUTSET = 18.0
SHT_RS_NUT_CLEAR_Z      = 3.5

SHT_RS_TUNNEL_ENABLE = True
SHT_RS_TUNNEL_D      = SHT_NUT_D + 4.0
SHT_RS_TUNNEL_EXTRA  = 35.0

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

# outer rim of the ultrasonic head (plate/body)
HEAD_RIM_FILLET_R = 2.0     # try 1.5–3.0

# outer rim of the roof disc
ROOF_RIM_FILLET_R = 2.0     # try 1.5–3.0

# how strict we are when detecting the rim radius
RIM_RADIUS_TOL = 0.6        # mm


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
STANDOFF_RADIUS  = HEAD_R - (STANDOFF_OD / 2.0) - STANDOFF_RIM_MARGIN
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

def cut_radial_port(solid, hole_d, angle_deg, z_center, wall_inner_r, wall_outer_r, extra=2.0):
    zc = clamp(z_center, -BELLY_WALL_H + 6.0, -6.0)
    length = (wall_outer_r - wall_inner_r) + 2.0 * extra
    x0 = wall_inner_r - extra

    cutter = Part.makeCylinder(
        hole_d / 2.0,
        length,
        Vector(x0, 0, zc),
        Vector(1, 0, 0)
    )
    cutter.rotate(Vector(0, 0, 0), Vector(0, 0, 1), angle_deg)
    return solid.cut(cutter)

def cut_nut_flats(solid, angle_deg, zc, nut_d, wall_inner_r, wall_outer_r,
                  depth_out=1.0, depth_in=1.0, margin=2.0, z_margin=2.0):
    w = nut_d + 2.0 * margin
    h = nut_d + 2.0 * z_margin

    out_x0 = wall_outer_r - depth_out
    out_len_x = depth_out + 30.0
    outer_box = Part.makeBox(out_len_x, w, h, Vector(out_x0, -w/2.0, zc - h/2.0))
    outer_box.rotate(Vector(0, 0, 0), Vector(0, 0, 1), angle_deg)
    solid = solid.cut(outer_box)

    in_len_x = (wall_inner_r + depth_in + 1.0)
    inner_box = Part.makeBox(in_len_x, w, h, Vector(0.0, -w/2.0, zc - h/2.0))
    inner_box.rotate(Vector(0, 0, 0), Vector(0, 0, 1), angle_deg)
    solid = solid.cut(inner_box)

    return solid

def cut_radial_box(solid, angle_deg, zc, x_start, x_len, y_w, z_h):
    box = Part.makeBox(
        x_len, y_w, z_h,
        Vector(x_start, -y_w/2.0, zc - z_h/2.0)
    )
    box.rotate(Vector(0, 0, 0), Vector(0, 0, 1), angle_deg)
    return solid.cut(box)

def make_pcb_posts(z_floor, inner_r):
    # PCB origin (centered), then holes defined in PCB coords
    x0 = -PCB_W / 2.0 + PCB_OFFSET_X
    y0 = -PCB_H / 2.0 + PCB_OFFSET_Y

    # Rotation to align PCB posts with belly fasten pillars
    rot = math.radians(float(PCB_ROT_DEG))
    cr = math.cos(rot)
    sr = math.sin(rot)

    posts = []
    for (hx, hy) in PCB_HOLES:
        # hole position in PCB coordinates (centered)
        px0 = x0 + hx
        py0 = y0 + hy

        # rotate around origin (0,0)
        px = (px0 * cr) - (py0 * sr)
        py = (px0 * sr) + (py0 * cr)

        # stay inside the belly cavity
        if math.hypot(px, py) > (inner_r - 3.0):
            continue

        # post solid
        post = Part.makeCylinder(PCB_POST_OD/2.0, PCB_STANDOFF_H, Vector(px, py, z_floor))

        # heat-set insert bore at top of post
        bore_z0 = z_floor + PCB_STANDOFF_H - PCB_INSERT_DEPTH
        bore = Part.makeCylinder(PCB_INSERT_BORE_D/2.0, PCB_INSERT_DEPTH + 0.3, Vector(px, py, bore_z0 - 0.15))
        post = post.cut(bore)

        # lead-in chamfer
        if PCB_INSERT_CHAMFER_H > 0:
            cham = Part.makeCone(
                (PCB_INSERT_BORE_D/2.0) + 0.35,
                (PCB_INSERT_BORE_D/2.0),
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
    """
    Fillet only circular edges whose circle radius ~= rim_radius.
    This reliably catches the outer circumference edges on cylinders/discs.
    """
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


# ---------------- Base plate ----------------
plate = Part.makeCylinder(HEAD_R, PLATE_THK, Vector(0, 0, 0))

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

# ---------------- Roof standoffs (TOP inserts only now) ----------------
for a in STANDOFF_ANGLES:
    rad = math.radians(a)
    x = STANDOFF_RADIUS * math.cos(rad)
    y = STANDOFF_RADIUS * math.sin(rad)

    pillar = Part.makeCylinder(STANDOFF_OD / 2.0, STANDOFF_TOTAL_H, Vector(x, y, 0.0))

    top_bore_z0 = roof_z - TOP_INSERT_DEPTH
    top_insert_bore = Part.makeCylinder(TOP_INSERT_BORE_D / 2.0, TOP_INSERT_DEPTH + 0.2, Vector(x, y, top_bore_z0 - 0.1))
    pillar = pillar.cut(top_insert_bore)

    if TOP_INSERT_LEADIN_CHAMFER > 0:
        cham = Part.makeCone(
            (TOP_INSERT_BORE_D / 2.0) + 0.35,
            (TOP_INSERT_BORE_D / 2.0),
            TOP_INSERT_LEADIN_CHAMFER,
            Vector(x, y, roof_z - TOP_INSERT_LEADIN_CHAMFER)
        )
        pillar = pillar.cut(cham)

    if STANDOFF_BOTTOM_INSERT_ENABLE:
        bot_insert_bore = Part.makeCylinder(BOTTOM_INSERT_BORE_D / 2.0, BOTTOM_INSERT_DEPTH + 0.2, Vector(x, y, -0.1))
        pillar = pillar.cut(bot_insert_bore)

        if BOTTOM_INSERT_LEADIN_CHAMFER > 0:
            cham2 = Part.makeCone(
                (BOTTOM_INSERT_BORE_D / 2.0) + 0.35,
                (BOTTOM_INSERT_BORE_D / 2.0),
                BOTTOM_INSERT_LEADIN_CHAMFER,
                Vector(x, y, 0.0)
            )
            pillar = pillar.cut(cham2)

    body = body.fuse(pillar)

# Optional old access holes (disabled)
if BOTTOM_ACCESS_ENABLE and STANDOFF_BOTTOM_INSERT_ENABLE:
    for a in STANDOFF_ANGLES:
        rad = math.radians(a)
        x = STANDOFF_RADIUS * math.cos(rad)
        y = STANDOFF_RADIUS * math.sin(rad)
        access = Part.makeCylinder((BOTTOM_ACCESS_D / 2.0), PLATE_THK + BOTTOM_ACCESS_EXTRA + 1.0, Vector(x, y, -0.5))
        body = body.cut(access)

# ---------------- Plate bolt-through holes to match belly fastening tubes ----------------
for a in BELLY_FASTEN_ANGLES:
    rad = math.radians(a)
    x = STANDOFF_RADIUS * math.cos(rad)
    y = STANDOFF_RADIUS * math.sin(rad)

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

if EDGE_ROUND_ENABLE:
    body = fillet_outer_rim_by_radius(body, rim_radius=HEAD_R, fillet_r=HEAD_RIM_FILLET_R, tol=RIM_RADIUS_TOL)

lower_obj = doc.addObject("Part::Feature", "LowerBody")
lower_obj.Shape = body


# ---------------- Roof ----------------
roof_with_holes = Part.makeCylinder(ROOF_R, ROOF_THK, Vector(0, 0, roof_z))

for a in STANDOFF_ANGLES:
    rad = math.radians(a)
    x = STANDOFF_RADIUS * math.cos(rad)
    y = STANDOFF_RADIUS * math.sin(rad)

    hole = Part.makeCylinder(ROOF_HOLE_D / 2.0, ROOF_THK + 1.0, Vector(x, y, roof_z - 0.5))
    roof_with_holes = roof_with_holes.cut(hole)

    if ADD_COUNTERBORE and (COUNTERBORE_D > ROOF_HOLE_D) and (COUNTERBORE_DEPTH > 0):
        cb = Part.makeCylinder(COUNTERBORE_D / 2.0, COUNTERBORE_DEPTH, Vector(x, y, roof_top_z - COUNTERBORE_DEPTH))
        roof_with_holes = roof_with_holes.cut(cb)

roof_final = roof_with_holes

# PAR housing + bosses on roof
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

    # ---- Internal fastening tubes aligned with pods, with heat-set inserts from top ----
    for a in BELLY_FASTEN_ANGLES:
        rad = math.radians(a)
        x = STANDOFF_RADIUS * math.cos(rad)
        y = STANDOFF_RADIUS * math.sin(rad)

        tube_outer = Part.makeCylinder(BELLY_TUBE_OD / 2.0, cup_total_h, Vector(x, y, cup_base_z))
        pan = pan.fuse(tube_outer)

        # Insert pocket at TOP lip (z=0), bored downward
        insert_bore = Part.makeCylinder(
            BOTTOM_INSERT_BORE_D / 2.0,
            BOTTOM_INSERT_DEPTH + 0.2,
            Vector(x, y, -BOTTOM_INSERT_DEPTH - 0.1)
        )
        pan = pan.cut(insert_bore)

        if BOTTOM_INSERT_LEADIN_CHAMFER > 0:
            cham = Part.makeCone(
                (BOTTOM_INSERT_BORE_D / 2.0) + 0.35,
                (BOTTOM_INSERT_BORE_D / 2.0),
                BOTTOM_INSERT_LEADIN_CHAMFER,
                Vector(x, y, -BOTTOM_INSERT_LEADIN_CHAMFER)
            )
            pan = pan.cut(cham)

    # ---- NO holes through belly floor (intentionally removed) ----
    if BELLY_FLOOR_HOLES_ENABLE:
        pass

    # Seal groove (top face z=0)
    if BELLY_SEAL_ENABLE:
        wall_inner_r = inner_r
        wall_outer_r = HEAD_R
        tube_outer_r = STANDOFF_RADIUS + (BELLY_TUBE_OD / 2.0)

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

    # Drip edge
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

    # Ports + nut flats
    if CONNECTORS_ENABLE:
        wall_inner_r = belly_inner_r
        wall_outer_r = HEAD_R
        zc_ports = PORT_Z_CENTER

        # ---------------- GX12 (2x2 grid: two angles x two Z levels) ----------------
               # GX12 (1x4 row at same Z)
        gx12_zc = zc_ports if (GX12_Z_CENTER is None) else float(GX12_Z_CENTER)
        center = float(GX12_ROW_CENTER_ANGLE) + float(GX12_ROW_OFFSET_DEG)
        step   = float(GX12_ROW_SPACING_DEG)

        gx12_angles = [center + (i - 1.5) * step for i in range(4)]

       

        for ang in gx12_angles:
            
                pan = cut_radial_port(
                    pan, GX12_HOLE_D, ang, gx12_zc,
                    wall_inner_r, wall_outer_r,
                    extra=PORT_CUT_EXTRA
                )
                if NUT_FLATS_ENABLE:
                    pan = cut_nut_flats(
                        pan, ang, gx12_zc, GX12_NUT_D,
                        wall_inner_r, wall_outer_r,
                        depth_out=NUT_FLAT_DEPTH_OUT, depth_in=NUT_FLAT_DEPTH_IN,
                        margin=NUT_FLAT_MARGIN, z_margin=NUT_FLAT_Z_MARGIN
                    )


        # ---------------- PG7 ----------------
        if angle_ok(PG7_ANGLE_DEG):
            pan = cut_radial_port(pan, PG7_HOLE_D, PG7_ANGLE_DEG, zc_ports, wall_inner_r, wall_outer_r, extra=PORT_CUT_EXTRA)
            if NUT_FLATS_ENABLE:
                pan = cut_nut_flats(
                    pan, PG7_ANGLE_DEG, zc_ports, PG7_NUT_D,
                    wall_inner_r, wall_outer_r,
                    depth_out=NUT_FLAT_DEPTH_OUT, depth_in=NUT_FLAT_DEPTH_IN,
                    margin=NUT_FLAT_MARGIN, z_margin=NUT_FLAT_Z_MARGIN
                )

        # ---------------- SHT ----------------
        if angle_ok(SHT_ANGLE_DEG):
            pan = cut_radial_port(pan, SHT_HOLE_D, SHT_ANGLE_DEG, zc_ports, wall_inner_r, wall_outer_r, extra=PORT_CUT_EXTRA)
            if NUT_FLATS_ENABLE:
                pan = cut_nut_flats(
                    pan, SHT_ANGLE_DEG, zc_ports, SHT_NUT_D,
                    wall_inner_r, wall_outer_r,
                    depth_out=NUT_FLAT_DEPTH_OUT, depth_in=NUT_FLAT_DEPTH_IN,
                    margin=NUT_FLAT_MARGIN, z_margin=NUT_FLAT_Z_MARGIN
                )

        # Optional SHT canopy (currently disabled)
        if SHT_RS_ENABLE and angle_ok(SHT_ANGLE_DEG):
            roof_shield = make_sht_canopy_roof(wall_outer_r=wall_outer_r, zc=SHT_Z, angle_deg=SHT_ANGLE_DEG)
            pan = pan.fuse(roof_shield)

    # PCB posts on belly floor
    if PCB_ENABLE:
        z_floor = cup_base_z + BELLY_BASE_THK
        posts = make_pcb_posts(z_floor=z_floor, inner_r=inner_r)
        if posts:
            pan = pan.fuse(posts)

    pan = pan.removeSplitter()
    pan_obj = doc.addObject("Part::Feature", "BellyPan")
    pan_obj.Shape = pan


doc.recompute()

# ---------------- Exploded view placements (visual only) ----------------
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

# ---------------- Console report ----------------
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
print("Insert bore D:                 {:.2f} mm".format(BOTTOM_INSERT_BORE_D))
print("Insert depth:                  {:.2f} mm".format(BOTTOM_INSERT_DEPTH))
print("\n---- Key Geometry ----")
print("Ports Z center:                {:.1f} mm".format(PORT_Z_CENTER))
print("Available pod depth:           {:.2f} mm".format(available_body_depth))
print("\n---- 3D Printing Tips ----")
print("• Print belly pan UPSIDE DOWN (base on bed)")
print("• Material: PETG or ASA for outdoor use")
print("\n============================================================")
