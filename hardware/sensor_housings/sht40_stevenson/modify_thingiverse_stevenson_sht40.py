#!/usr/bin/env python3
"""
Code-driven STL modifier for the Thingiverse mini Stevenson shield.

Run from Blender, not regular Python:

    blender --background --python hardware/sensor_housings/sht40_stevenson/modify_thingiverse_stevenson_sht40.py -- ^
      --source-dir "C:\\Users\\thoma\\Downloads\\Stevenson Shield (mini) - 4915068\\files"

The script starts from the actual downloaded STL meshes and applies parametric
boolean edits. It previews by default; pass --export-stl only when you are ready
to write modified STL files. This keeps the Thingiverse mesh as the starting
point while making the changes repeatable and LLM-editable.

Attribution:
  Adapted from "Stevenson Shield (mini)" by Thingiverse user commonslabgr:
  https://www.thingiverse.com/thing:4915068
  License supplied in downloaded package: Creative Commons - Attribution
"""

from __future__ import annotations

import argparse
import math
import sys
from pathlib import Path

import bpy


# =============================================================================
# Parameters an LLM/user should edit first
# =============================================================================

# Source Thingiverse files.
DEFAULT_SOURCE_DIR = Path(r"C:\Users\thoma\Downloads\Stevenson Shield (mini) - 4915068\files")
SOURCE_CAP = "stevenson_cap.stl"
SOURCE_BASE = "stevenson_base.stl"  # legacy/reference only; normal two-piece build does not use it

# Output next to this script by default.
DEFAULT_OUTPUT_DIR = Path(__file__).resolve().parent / "stl"

# Measured SHT40 probe and gland assembly.
# Nut widths are assumed to be measured across flats.
PROBE_BODY_D = 16.0
PROBE_BODY_MAX_EXPECTED_D = 17.0
PROBE_LENGTH_FROM_FLANGE_SEAT = 52.0

THROUGH_FLANGE_SECTION_D = 12.4
THREAD_THROUGH_FLANGE_LENGTH = 15.7
UNDER_FLANGE_NUT_AF = 17.6
TOP_CLAMP_NUT_AF = 17.2

THROUGH_FLANGE_CLEARANCE_D = 0.6
NUT_FREE_CLEARANCE_D = 1.2
HEX_AF_TO_CORNER_D = 1.0 / math.cos(math.radians(30.0))

PG7_PANEL_HOLE_D = THROUGH_FLANGE_SECTION_D + THROUGH_FLANGE_CLEARANCE_D
TOP_NUT_FREE_D = (TOP_CLAMP_NUT_AF * HEX_AF_TO_CORNER_D) + NUT_FREE_CLEARANCE_D
UNDER_NUT_FREE_D = (UNDER_FLANGE_NUT_AF * HEX_AF_TO_CORNER_D) + NUT_FREE_CLEARANCE_D

# This bore must clear the probe body and the under-flange nut envelope where
# the threaded section drops into the shield collar. The nut is not captured;
# the round bore only provides room for the already-tightened assembly.
PROBE_BORE_D = max(
    PROBE_BODY_MAX_EXPECTED_D + 1.8,
    UNDER_NUT_FREE_D,
)

# Add a top collar to the original cap, then screw a separate PG7 flange to it.
TOP_COLLAR_ENABLE = True
TOP_COLLAR_OD = 44.0
TOP_COLLAR_H = 10.0
TOP_COLLAR_OVERLAP = 0.35

PG7_FLANGE_OD = 44.0
PG7_FLANGE_THK = 5.0
PG7_FLANGE_PREVIEW_Z = 0.0  # exported separately; this is just its own z base

# Optional sandwich bracket that sits between the cap collar and PG7 flange.
# It uses the same OD and screw pattern as the PG7 flange, but the centre bore
# clears the under-flange nut rather than clamping the threaded section.
TOP_BRACKET_ADAPTER_ENABLE = True
TOP_BRACKET_ADAPTER_THK = 5.0
TOP_BRACKET_ADAPTER_CENTER_D = PROBE_BORE_D
TOP_BRACKET_ADAPTER_PREVIEW_Z = 0.0
PREVIEW_STACK_GAP = 0.35

FLANGE_SCREW_COUNT = 3
FLANGE_SCREW_PCD = 32.0
FLANGE_SCREW_ANGLE_OFFSET_DEG = 90.0
FLANGE_SCREW_CLEARANCE_D = 3.4
FLANGE_SCREW_COUNTERBORE_D = 6.8
FLANGE_SCREW_COUNTERBORE_DEPTH = 2.2

TOP_INSERT_BORE_D = 4.4
TOP_INSERT_DEPTH = 5.0
TOP_INSERT_LEADIN_D = 5.2
TOP_INSERT_LEADIN_DEPTH = 0.8

# Integrated bottom closure and mounting, fused to the cap/body.
BOTTOM_CAP_ENABLE = True
BOTTOM_CAP_OD = 47.5
BOTTOM_CAP_ID = 22.0
BOTTOM_CAP_THK = 3.0
BOTTOM_CAP_Z = -3.0

BODY_RIGHT_ANGLE_BRACKET_ENABLE = False
BRACKET_SIDE = "positive_x"
BRACKET_ARM_L = 42.0
BRACKET_ARM_W = 18.0
BRACKET_ARM_THK = 5.0
BRACKET_ARM_Z = -3.0
BRACKET_ARM_OVERLAP = 3.0
BRACKET_VERTICAL_W = 18.0
BRACKET_VERTICAL_THK = 5.0
BRACKET_VERTICAL_H = 46.0
BRACKET_VERTICAL_Z = -3.0
BRACKET_VERTICAL_HOLE_D = 4.3
BRACKET_VERTICAL_HOLE_SPACING = 24.0
BRACKET_VERTICAL_HOLE_Z_CENTER = 22.0
BRACKET_FILLET_BLOCK_ENABLE = True
BRACKET_FILLET_BLOCK_R = 18.0
BRACKET_DRILL_CLEARANCE_W = 9.0

# Optional unified FieldMesh sensor deck mount, matching NODE_BOX_SENSOR_DECK.py
# and the PAR housing. It is placed on the Y axis so it can coexist with the
# right-angle bracket on X. The pitch is kept compact to improve deck clearance.
UNIFIED_FEET_ENABLE = True
UNIFIED_FEET_AXIS = "y"
UNIFIED_SENSOR_MOUNT_HOLE_SPACING = 64.0
MOUNT_FOOT_OD = 18.0
MOUNT_FOOT_THK = 4.0
MOUNT_FOOT_HOLE_D = 3.4
MOUNT_FOOT_COUNTERBORE_D = 7.0
MOUNT_FOOT_COUNTERBORE_DEPTH = 2.0
MOUNT_FOOT_BRIDGE_W = 13.0
MOUNT_FOOT_BRIDGE_OVERLAP = 2.0
MOUNT_FOOT_Z = -3.0

# Base STL coordinate assumptions for the legacy original-base modifier.
BASE_SENSOR_CENTER_X = -110.0
BASE_SENSOR_CENTER_Y = 0.0
BASE_BODY_REFERENCE_OD = 28.8

# Mesh/boolean quality.
CYLINDER_VERTICES = 128
BOOLEAN_SOLVER = "EXACT"


# =============================================================================
# Blender helpers
# =============================================================================

def parse_args() -> argparse.Namespace:
    argv = sys.argv
    script_args = argv[argv.index("--") + 1 :] if "--" in argv else []
    parser = argparse.ArgumentParser(description="Modify Thingiverse Stevenson shield STLs for the SHT40 probe.")
    parser.add_argument("--source-dir", type=Path, default=DEFAULT_SOURCE_DIR, help="Folder containing stevenson_cap.stl and stevenson_base.stl.")
    parser.add_argument("--output-dir", type=Path, default=DEFAULT_OUTPUT_DIR, help="Folder to receive modified STL files.")
    parser.add_argument("--include-original-base", action="store_true", help="Also modify/export the original Thingiverse base as a legacy/reference part.")
    parser.add_argument("--skip-base", action="store_true", help="Deprecated alias kept for old commands; the original base is skipped by default.")
    parser.add_argument("--export-stl", action="store_true", help="Write STL files after building the modified Blender scene.")
    parser.add_argument("--preview-only", action="store_true", help="Deprecated alias; preview is now the default unless --export-stl is supplied.")
    return parser.parse_args(script_args)


def clear_scene() -> None:
    bpy.ops.object.select_all(action="SELECT")
    bpy.ops.object.delete()


def import_stl(path: Path, name: str) -> bpy.types.Object:
    if hasattr(bpy.ops.wm, "stl_import"):
        bpy.ops.wm.stl_import(filepath=str(path))
    else:
        bpy.ops.import_mesh.stl(filepath=str(path))
    obj = bpy.context.object
    obj.name = name
    bpy.context.view_layer.objects.active = obj
    obj.select_set(True)
    bpy.ops.object.transform_apply(location=False, rotation=False, scale=True)
    return obj


def export_stl(obj: bpy.types.Object, path: Path) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    bpy.ops.object.select_all(action="DESELECT")
    obj.select_set(True)
    bpy.context.view_layer.objects.active = obj
    if hasattr(bpy.ops.wm, "stl_export"):
        bpy.ops.wm.stl_export(filepath=str(path), export_selected_objects=True)
    else:
        bpy.ops.export_mesh.stl(filepath=str(path), use_selection=True)


def world_bounds(obj: bpy.types.Object) -> tuple[float, float, float, float, float, float]:
    corners = [obj.matrix_world @ mathutils_vector(corner) for corner in obj.bound_box]
    xs = [p.x for p in corners]
    ys = [p.y for p in corners]
    zs = [p.z for p in corners]
    return min(xs), max(xs), min(ys), max(ys), min(zs), max(zs)


def move_min_z_to(obj: bpy.types.Object, z_bottom: float) -> None:
    _min_x, _max_x, _min_y, _max_y, min_z, _max_z = world_bounds(obj)
    obj.location.z += float(z_bottom) - min_z


def mathutils_vector(values: tuple[float, float, float]):
    from mathutils import Vector

    return Vector(values)


def set_active(obj: bpy.types.Object) -> None:
    bpy.ops.object.select_all(action="DESELECT")
    obj.select_set(True)
    bpy.context.view_layer.objects.active = obj


def make_cylinder(name: str, diameter: float, height: float, z_bottom: float, x: float = 0.0, y: float = 0.0) -> bpy.types.Object:
    bpy.ops.mesh.primitive_cylinder_add(
        vertices=CYLINDER_VERTICES,
        radius=diameter / 2.0,
        depth=height,
        end_fill_type="NGON",
        location=(x, y, z_bottom + height / 2.0),
    )
    obj = bpy.context.object
    obj.name = name
    return obj


def make_cylinder_x(name: str, diameter: float, length: float, x_start: float, y: float = 0.0, z: float = 0.0) -> bpy.types.Object:
    bpy.ops.mesh.primitive_cylinder_add(
        vertices=CYLINDER_VERTICES,
        radius=diameter / 2.0,
        depth=length,
        end_fill_type="NGON",
        location=(x_start + length / 2.0, y, z),
        rotation=(0.0, math.radians(90.0), 0.0),
    )
    obj = bpy.context.object
    obj.name = name
    return obj


def make_box(name: str, sx: float, sy: float, sz: float, cx: float, cy: float, z_bottom: float) -> bpy.types.Object:
    bpy.ops.mesh.primitive_cube_add(size=1.0, location=(cx, cy, z_bottom + sz / 2.0))
    obj = bpy.context.object
    obj.name = name
    obj.dimensions = (sx, sy, sz)
    set_active(obj)
    bpy.ops.object.transform_apply(location=False, rotation=False, scale=True)
    return obj


def boolean_apply(target: bpy.types.Object, cutter: bpy.types.Object, operation: str, keep_cutter: bool = False) -> None:
    set_active(target)
    mod = target.modifiers.new(name=f"{operation}_{cutter.name}", type="BOOLEAN")
    mod.operation = operation
    mod.object = cutter
    if hasattr(mod, "solver"):
        mod.solver = BOOLEAN_SOLVER
    bpy.ops.object.modifier_apply(modifier=mod.name)
    if not keep_cutter:
        bpy.data.objects.remove(cutter, do_unlink=True)


def union_objects(name: str, objects: list[bpy.types.Object]) -> bpy.types.Object:
    if not objects:
        raise ValueError("No objects to union")
    result = objects[0]
    result.name = name
    for obj in objects[1:]:
        boolean_apply(result, obj, "UNION")
    return result


def make_ring(name: str, outer_d: float, inner_d: float, height: float, z_bottom: float, x: float = 0.0, y: float = 0.0) -> bpy.types.Object:
    outer = make_cylinder(name, outer_d, height, z_bottom, x=x, y=y)
    inner = make_cylinder(f"{name}_inner_cut", inner_d, height + 2.0, z_bottom - 1.0, x=x, y=y)
    boolean_apply(outer, inner, "DIFFERENCE")
    return outer


def polar_xy(radius: float, angle_deg: float, cx: float = 0.0, cy: float = 0.0) -> tuple[float, float]:
    rad = math.radians(angle_deg)
    return cx + radius * math.cos(rad), cy + radius * math.sin(rad)


def flange_screw_points(cx: float = 0.0, cy: float = 0.0) -> list[tuple[float, float]]:
    step = 360.0 / FLANGE_SCREW_COUNT
    return [
        polar_xy(FLANGE_SCREW_PCD / 2.0, FLANGE_SCREW_ANGLE_OFFSET_DEG + step * i, cx=cx, cy=cy)
        for i in range(FLANGE_SCREW_COUNT)
    ]


# =============================================================================
# Modification operations
# =============================================================================

def modify_cap(cap_path: Path) -> bpy.types.Object:
    cap = import_stl(cap_path, "stevenson_cap_sht40_modified")
    min_x, max_x, min_y, max_y, min_z, max_z = world_bounds(cap)
    cap_h = max_z - min_z

    # Ensure top/probe path clearance. This is intentionally through-cut, so it
    # also opens any top skin the original STL may have.
    bore = make_cylinder("probe_bore_cut", PROBE_BORE_D, cap_h + TOP_COLLAR_H + 8.0, min_z - 4.0)
    boolean_apply(cap, bore, "DIFFERENCE")

    if TOP_COLLAR_ENABLE:
        collar_z = max_z - TOP_COLLAR_OVERLAP
        collar = make_ring("pg7_top_collar_add", TOP_COLLAR_OD, PROBE_BORE_D, TOP_COLLAR_H + TOP_COLLAR_OVERLAP, collar_z)
        boolean_apply(cap, collar, "UNION")

        collar_top = collar_z + TOP_COLLAR_H + TOP_COLLAR_OVERLAP
        for idx, (sx, sy) in enumerate(flange_screw_points()):
            insert = make_cylinder(
                f"top_insert_bore_cut_{idx + 1}",
                TOP_INSERT_BORE_D,
                TOP_INSERT_DEPTH + 0.3,
                collar_top - TOP_INSERT_DEPTH - 0.15,
                x=sx,
                y=sy,
            )
            boolean_apply(cap, insert, "DIFFERENCE")
            leadin = make_cylinder(
                f"top_insert_leadin_cut_{idx + 1}",
                TOP_INSERT_LEADIN_D,
                TOP_INSERT_LEADIN_DEPTH + 0.15,
                collar_top - TOP_INSERT_LEADIN_DEPTH,
                x=sx,
                y=sy,
            )
            boolean_apply(cap, leadin, "DIFFERENCE")

    bottom_mount = make_bottom_cap_and_bracket()
    boolean_apply(cap, bottom_mount, "UNION")

    return cap


def make_pg7_flange(z_bottom: float = PG7_FLANGE_PREVIEW_Z) -> bpy.types.Object:
    flange = make_cylinder("pg7_probe_retainer_flange", PG7_FLANGE_OD, PG7_FLANGE_THK, z_bottom)

    panel_hole = make_cylinder("pg7_panel_hole_cut", PG7_PANEL_HOLE_D, PG7_FLANGE_THK + 1.0, z_bottom - 0.5)
    boolean_apply(flange, panel_hole, "DIFFERENCE")

    for idx, (sx, sy) in enumerate(flange_screw_points()):
        screw = make_cylinder(
            f"flange_screw_clearance_cut_{idx + 1}",
            FLANGE_SCREW_CLEARANCE_D,
            PG7_FLANGE_THK + 1.0,
            z_bottom - 0.5,
            x=sx,
            y=sy,
        )
        boolean_apply(flange, screw, "DIFFERENCE")
        counterbore = make_cylinder(
            f"flange_screw_counterbore_cut_{idx + 1}",
            FLANGE_SCREW_COUNTERBORE_D,
            FLANGE_SCREW_COUNTERBORE_DEPTH + 0.2,
            z_bottom + PG7_FLANGE_THK - FLANGE_SCREW_COUNTERBORE_DEPTH,
            x=sx,
            y=sy,
        )
        boolean_apply(flange, counterbore, "DIFFERENCE")

    return flange


def make_top_bracket_adapter(z_bottom: float = TOP_BRACKET_ADAPTER_PREVIEW_Z) -> bpy.types.Object:
    parts: list[bpy.types.Object] = []
    cutters: list[bpy.types.Object] = []

    adapter = make_ring(
        "right_angle_bracket_adapter_ring",
        PG7_FLANGE_OD,
        TOP_BRACKET_ADAPTER_CENTER_D,
        TOP_BRACKET_ADAPTER_THK,
        z_bottom,
    )
    parts.append(adapter)

    side_sign = 1.0 if BRACKET_SIDE == "positive_x" else -1.0
    adapter_r = PG7_FLANGE_OD / 2.0
    arm_inner_x = side_sign * (adapter_r - BRACKET_ARM_OVERLAP)
    arm_outer_x = side_sign * (adapter_r + BRACKET_ARM_L)
    arm_cx = (arm_inner_x + arm_outer_x) / 2.0
    arm_len = abs(arm_outer_x - arm_inner_x)
    parts.append(
        make_box(
            "right_angle_adapter_arm",
            arm_len,
            BRACKET_ARM_W,
            TOP_BRACKET_ADAPTER_THK,
            arm_cx,
            0.0,
            z_bottom,
        )
    )

    plate_cx = arm_outer_x + side_sign * (BRACKET_VERTICAL_THK / 2.0)
    parts.append(
        make_box(
            "right_angle_adapter_vertical_plate",
            BRACKET_VERTICAL_THK,
            BRACKET_VERTICAL_W,
            BRACKET_VERTICAL_H,
            plate_cx,
            0.0,
            z_bottom,
        )
    )

    if BRACKET_FILLET_BLOCK_ENABLE:
        gusset_len = min(BRACKET_FILLET_BLOCK_R, BRACKET_ARM_L)
        gusset_cx = arm_outer_x - side_sign * (gusset_len / 2.0)
        side_rib_w = max(0.0, (BRACKET_ARM_W - BRACKET_DRILL_CLEARANCE_W) / 2.0)
        if side_rib_w > 0.8:
            for sign_y in (-1.0, 1.0):
                rib_y = sign_y * ((BRACKET_DRILL_CLEARANCE_W / 2.0) + (side_rib_w / 2.0))
                parts.append(
                    make_box(
                        "right_angle_adapter_side_gusset",
                        gusset_len,
                        side_rib_w,
                        min(BRACKET_FILLET_BLOCK_R, BRACKET_VERTICAL_H),
                        gusset_cx,
                        rib_y,
                        z_bottom,
                    )
                )
        else:
            parts.append(
                make_box(
                    "right_angle_adapter_gusset_block",
                    gusset_len,
                    BRACKET_ARM_W,
                    min(BRACKET_FILLET_BLOCK_R, BRACKET_VERTICAL_H),
                    gusset_cx,
                    0.0,
                    z_bottom,
                )
            )

    for idx, (sx, sy) in enumerate(flange_screw_points()):
        cutters.append(
            make_cylinder(
                f"bracket_adapter_screw_clearance_cut_{idx + 1}",
                FLANGE_SCREW_CLEARANCE_D,
                TOP_BRACKET_ADAPTER_THK + 1.0,
                z_bottom - 0.5,
                x=sx,
                y=sy,
            )
        )

    hole_x_start = plate_cx - side_sign * (BRACKET_VERTICAL_THK / 2.0) - side_sign * 0.5
    if side_sign < 0:
        hole_x_start = plate_cx - BRACKET_VERTICAL_THK / 2.0 - 0.5
    hole_len = BRACKET_VERTICAL_THK + 1.0
    for idx, z in enumerate(
        [
            z_bottom + BRACKET_VERTICAL_HOLE_Z_CENTER - BRACKET_VERTICAL_HOLE_SPACING / 2.0,
            z_bottom + BRACKET_VERTICAL_HOLE_Z_CENTER + BRACKET_VERTICAL_HOLE_SPACING / 2.0,
        ]
    ):
        cutters.append(
            make_cylinder_x(
                f"right_angle_adapter_wall_hole_cut_{idx + 1}",
                BRACKET_VERTICAL_HOLE_D,
                hole_len,
                hole_x_start,
                y=0.0,
                z=z,
            )
        )

    bracket_adapter = union_objects("right_angle_top_bracket_adapter", parts)
    for cutter in cutters:
        boolean_apply(bracket_adapter, cutter, "DIFFERENCE")
    return bracket_adapter


def make_bottom_cap_and_bracket() -> bpy.types.Object:
    parts: list[bpy.types.Object] = []
    cutters: list[bpy.types.Object] = []

    if BOTTOM_CAP_ENABLE:
        parts.append(make_ring("integrated_bottom_air_cap", BOTTOM_CAP_OD, BOTTOM_CAP_ID, BOTTOM_CAP_THK, BOTTOM_CAP_Z))

    if UNIFIED_FEET_ENABLE:
        parts.append(make_unified_plate_mount_feet())

    if BODY_RIGHT_ANGLE_BRACKET_ENABLE:
        side_sign = 1.0 if BRACKET_SIDE == "positive_x" else -1.0
        body_r = BOTTOM_CAP_OD / 2.0
        arm_inner_x = side_sign * (body_r - BRACKET_ARM_OVERLAP)
        arm_outer_x = side_sign * (body_r + BRACKET_ARM_L)
        arm_cx = (arm_inner_x + arm_outer_x) / 2.0
        arm_len = abs(arm_outer_x - arm_inner_x)
        parts.append(
            make_box(
                "right_angle_mount_arm",
                arm_len,
                BRACKET_ARM_W,
                BRACKET_ARM_THK,
                arm_cx,
                0.0,
                BRACKET_ARM_Z,
            )
        )

        plate_cx = arm_outer_x + side_sign * (BRACKET_VERTICAL_THK / 2.0)
        parts.append(
            make_box(
                "right_angle_mount_vertical_plate",
                BRACKET_VERTICAL_THK,
                BRACKET_VERTICAL_W,
                BRACKET_VERTICAL_H,
                plate_cx,
                0.0,
                BRACKET_VERTICAL_Z,
            )
        )

        if BRACKET_FILLET_BLOCK_ENABLE:
            # A square gusset block is less pretty than a curved Blender mesh,
            # but it prints well and strongly ties the arm into the wall plate.
            gusset_len = min(BRACKET_FILLET_BLOCK_R, BRACKET_ARM_L)
            gusset_cx = arm_outer_x - side_sign * (gusset_len / 2.0)
            parts.append(
                make_box(
                    "right_angle_mount_gusset_block",
                    gusset_len,
                    BRACKET_ARM_W,
                    min(BRACKET_FILLET_BLOCK_R, BRACKET_VERTICAL_H),
                    gusset_cx,
                    0.0,
                    BRACKET_ARM_Z,
                )
            )

        hole_x_start = plate_cx - side_sign * (BRACKET_VERTICAL_THK / 2.0) - side_sign * 0.5
        if side_sign < 0:
            hole_x_start = plate_cx - BRACKET_VERTICAL_THK / 2.0 - 0.5
        hole_len = BRACKET_VERTICAL_THK + 1.0
        for idx, z in enumerate(
            [
                BRACKET_VERTICAL_Z + BRACKET_VERTICAL_HOLE_Z_CENTER - BRACKET_VERTICAL_HOLE_SPACING / 2.0,
                BRACKET_VERTICAL_Z + BRACKET_VERTICAL_HOLE_Z_CENTER + BRACKET_VERTICAL_HOLE_SPACING / 2.0,
            ]
        ):
            cutters.append(
                make_cylinder_x(
                    f"right_angle_mount_wall_hole_cut_{idx + 1}",
                    BRACKET_VERTICAL_HOLE_D,
                    hole_len,
                    hole_x_start if side_sign > 0 else hole_x_start,
                    y=0.0,
                    z=z,
                )
            )

    mount = union_objects("integrated_bottom_cap_and_mount", parts)
    for cutter in cutters:
        boolean_apply(mount, cutter, "DIFFERENCE")
    return mount


def make_unified_plate_mount_feet() -> bpy.types.Object:
    foot_radius = UNIFIED_SENSOR_MOUNT_HOLE_SPACING / 2.0
    body_r = BOTTOM_CAP_OD / 2.0
    parts: list[bpy.types.Object] = []
    cutters: list[bpy.types.Object] = []
    bridge_start = body_r - MOUNT_FOOT_BRIDGE_OVERLAP
    bridge_end = foot_radius + MOUNT_FOOT_OD / 2.0
    bridge_len = max(0.1, bridge_end - bridge_start)
    axis_is_x = UNIFIED_FEET_AXIS.lower() == "x"

    for sign in (-1.0, 1.0):
        if axis_is_x:
            foot_x = sign * foot_radius
            foot_y = 0.0
            bridge_cx = sign * (bridge_start + bridge_len / 2.0)
            bridge_cy = 0.0
            bridge_sx = bridge_len
            bridge_sy = MOUNT_FOOT_BRIDGE_W
        else:
            foot_x = 0.0
            foot_y = sign * foot_radius
            bridge_cx = 0.0
            bridge_cy = sign * (bridge_start + bridge_len / 2.0)
            bridge_sx = MOUNT_FOOT_BRIDGE_W
            bridge_sy = bridge_len

        parts.append(make_cylinder("plate_mount_foot", MOUNT_FOOT_OD, MOUNT_FOOT_THK, MOUNT_FOOT_Z, x=foot_x, y=foot_y))
        parts.append(make_box("plate_mount_bridge", bridge_sx, bridge_sy, MOUNT_FOOT_THK, bridge_cx, bridge_cy, MOUNT_FOOT_Z))

        cutters.append(make_cylinder("plate_mount_hole_cut", MOUNT_FOOT_HOLE_D, MOUNT_FOOT_THK + 1.0, MOUNT_FOOT_Z - 0.5, x=foot_x, y=foot_y))
        cutters.append(
            make_cylinder(
                "plate_mount_counterbore_cut",
                MOUNT_FOOT_COUNTERBORE_D,
                MOUNT_FOOT_COUNTERBORE_DEPTH + 0.2,
                MOUNT_FOOT_Z + MOUNT_FOOT_THK - MOUNT_FOOT_COUNTERBORE_DEPTH,
                x=foot_x,
                y=foot_y,
            )
        )

    feet = union_objects("unified_plate_mount_feet", parts)
    for cutter in cutters:
        boolean_apply(feet, cutter, "DIFFERENCE")
    return feet


def make_unified_mount_feet(z_bottom: float, cx: float, cy: float) -> bpy.types.Object:
    foot_radius = UNIFIED_SENSOR_MOUNT_HOLE_SPACING / 2.0
    parts: list[bpy.types.Object] = []
    bridge_start = BASE_BODY_REFERENCE_OD / 2.0 - MOUNT_FOOT_BRIDGE_OVERLAP
    bridge_end = foot_radius + MOUNT_FOOT_OD / 2.0
    bridge_len = bridge_end - bridge_start
    axis_is_x = UNIFIED_FEET_AXIS.lower() == "x"

    for sign in (-1.0, 1.0):
        if axis_is_x:
            foot_x = cx + sign * foot_radius
            foot_y = cy
            bridge_cx = cx + sign * (bridge_start + bridge_len / 2.0)
            bridge_cy = cy
            bridge_sx = bridge_len
            bridge_sy = MOUNT_FOOT_BRIDGE_W
        else:
            foot_x = cx
            foot_y = cy + sign * foot_radius
            bridge_cx = cx
            bridge_cy = cy + sign * (bridge_start + bridge_len / 2.0)
            bridge_sx = MOUNT_FOOT_BRIDGE_W
            bridge_sy = bridge_len

        foot = make_cylinder(f"unified_mount_foot_{'positive' if sign > 0 else 'negative'}", MOUNT_FOOT_OD, MOUNT_FOOT_THK, z_bottom, x=foot_x, y=foot_y)
        parts.append(foot)

        bridge = make_box(
            f"unified_mount_bridge_{'positive' if sign > 0 else 'negative'}",
            bridge_sx,
            bridge_sy,
            MOUNT_FOOT_THK,
            bridge_cx,
            bridge_cy,
            z_bottom,
        )
        parts.append(bridge)

    feet = union_objects("unified_70mm_mount_feet", parts)

    for sign in (-1.0, 1.0):
        if axis_is_x:
            foot_x = cx + sign * foot_radius
            foot_y = cy
        else:
            foot_x = cx
            foot_y = cy + sign * foot_radius

        hole = make_cylinder("unified_mount_hole_cut", MOUNT_FOOT_HOLE_D, MOUNT_FOOT_THK + 1.0, z_bottom - 0.5, x=foot_x, y=foot_y)
        boolean_apply(feet, hole, "DIFFERENCE")
        counterbore = make_cylinder(
            "unified_mount_counterbore_cut",
            MOUNT_FOOT_COUNTERBORE_D,
            MOUNT_FOOT_COUNTERBORE_DEPTH + 0.2,
            z_bottom + MOUNT_FOOT_THK - MOUNT_FOOT_COUNTERBORE_DEPTH,
            x=foot_x,
            y=foot_y,
        )
        boolean_apply(feet, counterbore, "DIFFERENCE")

    return feet


def modify_base(base_path: Path) -> bpy.types.Object:
    base = import_stl(base_path, "stevenson_base_sht40_modified")
    _min_x, _max_x, _min_y, _max_y, min_z, _max_z = world_bounds(base)

    # Add the shared deck mount to the original base mesh. The center values are
    # parameters because this is the part most likely to need a small visual tweak.
    feet = make_unified_mount_feet(min_z - 0.15, BASE_SENSOR_CENTER_X, BASE_SENSOR_CENTER_Y)
    boolean_apply(base, feet, "UNION")
    return base


def write_attribution(output_dir: Path) -> None:
    output_dir.mkdir(parents=True, exist_ok=True)
    (output_dir / "ATTRIBUTION_thingiverse_4915068.txt").write_text(
        "Modified from Stevenson Shield (mini) by Thingiverse user commonslabgr:\n"
        "https://www.thingiverse.com/thing:4915068\n"
        "License supplied in downloaded package: Creative Commons - Attribution\n",
        encoding="utf-8",
    )


def print_bounds(label: str, obj: bpy.types.Object) -> None:
    min_x, max_x, min_y, max_y, min_z, max_z = world_bounds(obj)
    print(f"{label:<30} {max_x - min_x:>7.2f} x {max_y - min_y:>7.2f} x {max_z - min_z:>7.2f} mm")


def position_preview_stack(cap: bpy.types.Object, bracket_adapter: bpy.types.Object | None, flange: bpy.types.Object) -> None:
    _min_x, _max_x, _min_y, _max_y, _min_z, cap_top_z = world_bounds(cap)
    next_z = cap_top_z + PREVIEW_STACK_GAP

    if bracket_adapter is not None:
        move_min_z_to(bracket_adapter, next_z)
        _min_x, _max_x, _min_y, _max_y, _min_z, next_z = world_bounds(bracket_adapter)
        next_z += PREVIEW_STACK_GAP

    move_min_z_to(flange, next_z)


def report_check(label: str, value: float, minimum: float | None = None, maximum: float | None = None, unit: str = "mm") -> None:
    status = "OK"
    if minimum is not None and value < minimum:
        status = "WARN"
    if maximum is not None and value > maximum:
        status = "WARN"

    limit = ""
    if minimum is not None:
        limit += f" min {minimum:.2f}"
    if maximum is not None:
        limit += f" max {maximum:.2f}"

    print(f"{label + ':':<34} {value:>7.2f} {unit} [{status}{limit}]")


def print_printability_report(body: bpy.types.Object) -> None:
    min_x, max_x, min_y, max_y, min_z, max_z = world_bounds(body)
    body_height = max_z - min_z
    probe_tip_clearance = body_height - PROBE_LENGTH_FROM_FLANGE_SEAT
    thread_below_flange = max(0.0, THREAD_THROUGH_FLANGE_LENGTH - PG7_FLANGE_THK)

    top_insert_outer_wall = (TOP_COLLAR_OD / 2.0) - ((FLANGE_SCREW_PCD / 2.0) + (TOP_INSERT_BORE_D / 2.0))
    top_insert_inner_wall = (FLANGE_SCREW_PCD / 2.0) - (TOP_INSERT_BORE_D / 2.0) - (PROBE_BORE_D / 2.0)
    top_insert_depth_margin = TOP_COLLAR_H - TOP_INSERT_DEPTH
    probe_radial_clearance = (PROBE_BORE_D - PROBE_BODY_MAX_EXPECTED_D) / 2.0
    through_flange_radial_clearance = (PG7_PANEL_HOLE_D - THROUGH_FLANGE_SECTION_D) / 2.0

    flange_hole_outer_wall = (PG7_FLANGE_OD / 2.0) - ((FLANGE_SCREW_PCD / 2.0) + (FLANGE_SCREW_CLEARANCE_D / 2.0))
    flange_counterbore_outer_wall = (PG7_FLANGE_OD / 2.0) - ((FLANGE_SCREW_PCD / 2.0) + (FLANGE_SCREW_COUNTERBORE_D / 2.0))
    flange_counterbore_floor = PG7_FLANGE_THK - FLANGE_SCREW_COUNTERBORE_DEPTH
    top_nut_corner_d = TOP_CLAMP_NUT_AF * HEX_AF_TO_CORNER_D
    under_nut_corner_d = UNDER_FLANGE_NUT_AF * HEX_AF_TO_CORNER_D
    top_nut_radial_clearance = (TOP_NUT_FREE_D - top_nut_corner_d) / 2.0
    under_nut_radial_clearance = (PROBE_BORE_D - under_nut_corner_d) / 2.0
    top_nut_to_counterbore = (FLANGE_SCREW_PCD / 2.0) - (TOP_NUT_FREE_D / 2.0) - (FLANGE_SCREW_COUNTERBORE_D / 2.0)
    under_nut_to_screw_hole = (FLANGE_SCREW_PCD / 2.0) - (UNDER_NUT_FREE_D / 2.0) - (FLANGE_SCREW_CLEARANCE_D / 2.0)
    top_nut_to_flange_edge = (PG7_FLANGE_OD - TOP_NUT_FREE_D) / 2.0
    adapter_screw_inner_wall = (FLANGE_SCREW_PCD / 2.0) - (FLANGE_SCREW_CLEARANCE_D / 2.0) - (TOP_BRACKET_ADAPTER_CENTER_D / 2.0)
    adapter_screw_outer_wall = (PG7_FLANGE_OD / 2.0) - ((FLANGE_SCREW_PCD / 2.0) + (FLANGE_SCREW_CLEARANCE_D / 2.0))

    foot_wall = (MOUNT_FOOT_OD - MOUNT_FOOT_HOLE_D) / 2.0
    foot_counterbore_floor = MOUNT_FOOT_THK - MOUNT_FOOT_COUNTERBORE_DEPTH
    bracket_hole_side_wall = (BRACKET_VERTICAL_W - BRACKET_VERTICAL_HOLE_D) / 2.0
    lower_hole_bottom_margin = BRACKET_VERTICAL_HOLE_Z_CENTER - (BRACKET_VERTICAL_HOLE_SPACING / 2.0) - (BRACKET_VERTICAL_HOLE_D / 2.0)
    upper_hole_top_margin = BRACKET_VERTICAL_H - (BRACKET_VERTICAL_HOLE_Z_CENTER + (BRACKET_VERTICAL_HOLE_SPACING / 2.0) + (BRACKET_VERTICAL_HOLE_D / 2.0))
    weakest_hole_z_margin = min(lower_hole_bottom_margin, upper_hole_top_margin)
    bracket_drill_channel_side_clearance = (BRACKET_DRILL_CLEARANCE_W - BRACKET_VERTICAL_HOLE_D) / 2.0
    bottom_cap_ring_wall = (BOTTOM_CAP_OD - BOTTOM_CAP_ID) / 2.0

    print("\n---- Printability / Mounting Check ----")
    report_check("Probe length clearance", probe_tip_clearance, minimum=5.0)
    report_check("Probe radial clearance", probe_radial_clearance, minimum=0.6)
    report_check("Through-hole radial clearance", through_flange_radial_clearance, minimum=0.25)
    report_check("Thread below flange", thread_below_flange, maximum=TOP_COLLAR_H + 1.0)
    report_check("Top insert outer wall", top_insert_outer_wall, minimum=3.0)
    report_check("Top insert inner wall", top_insert_inner_wall, minimum=2.5)
    report_check("Top insert depth margin", top_insert_depth_margin, minimum=2.0)
    report_check("Flange screw outer wall", flange_hole_outer_wall, minimum=3.0)
    report_check("Flange counterbore wall", flange_counterbore_outer_wall, minimum=2.0)
    report_check("Flange counterbore floor", flange_counterbore_floor, minimum=1.2)
    report_check("Top nut radial free clearance", top_nut_radial_clearance, minimum=0.5)
    report_check("Under nut radial free clearance", under_nut_radial_clearance, minimum=0.5)
    report_check("Top nut to screw counterbore", top_nut_to_counterbore, minimum=1.5)
    report_check("Under nut to screw hole", under_nut_to_screw_hole, minimum=1.5)
    report_check("Top nut to flange edge", top_nut_to_flange_edge, minimum=5.0)
    if TOP_BRACKET_ADAPTER_ENABLE:
        report_check("Adapter screw inner wall", adapter_screw_inner_wall, minimum=2.5)
        report_check("Adapter screw outer wall", adapter_screw_outer_wall, minimum=3.0)
        report_check("Adapter thickness", TOP_BRACKET_ADAPTER_THK, minimum=4.0)
        report_check("Adapter arm overlap", BRACKET_ARM_OVERLAP, minimum=2.0)
        report_check("Adapter drill channel side", bracket_drill_channel_side_clearance, minimum=2.0)
        print("Bracket adapter adds:          {:.2f} mm to required flange screw length".format(TOP_BRACKET_ADAPTER_THK))
    report_check("Bottom air-cap ring wall", bottom_cap_ring_wall, minimum=3.0)
    report_check("Plate mount foot wall", foot_wall, minimum=3.0)
    report_check("Plate mount counterbore floor", foot_counterbore_floor, minimum=1.2)
    report_check("Adapter bracket hole wall", bracket_hole_side_wall, minimum=5.0)
    report_check("Adapter bracket hole Z margin", weakest_hole_z_margin, minimum=3.0)
    report_check("Plate foot bridge overlap", MOUNT_FOOT_BRIDGE_OVERLAP, minimum=1.5)


def main() -> None:
    args = parse_args()
    source_dir = args.source_dir
    output_dir = args.output_dir
    cap_path = source_dir / SOURCE_CAP
    base_path = source_dir / SOURCE_BASE

    if not cap_path.exists():
        raise FileNotFoundError(f"Missing cap STL: {cap_path}")
    include_original_base = bool(args.include_original_base)
    if include_original_base and not base_path.exists():
        raise FileNotFoundError(f"Missing base STL: {base_path}")

    clear_scene()

    cap = modify_cap(cap_path)
    flange = make_pg7_flange()
    bracket_adapter = make_top_bracket_adapter() if TOP_BRACKET_ADAPTER_ENABLE else None
    base = modify_base(base_path) if include_original_base else None

    if args.export_stl:
        export_stl(cap, output_dir / "sht40_stevenson_body_modified.stl")
        export_stl(flange, output_dir / "sht40_pg7_retainer_flange.stl")
        if bracket_adapter is not None:
            export_stl(bracket_adapter, output_dir / "sht40_right_angle_bracket_adapter.stl")
        if base is not None:
            export_stl(base, output_dir / "sht40_stevenson_original_base_legacy_modified.stl")

    position_preview_stack(cap, bracket_adapter, flange)

    print("\n---- SHT40 Thingiverse STL Modifier ----")
    print(f"Source dir:   {source_dir}")
    print(f"Output dir:   {output_dir}")
    print(f"Probe bore D: {PROBE_BORE_D:.2f} mm")
    print(f"PG7 hole D:   {PG7_PANEL_HOLE_D:.2f} mm")
    print(f"Probe length: {PROBE_LENGTH_FROM_FLANGE_SEAT:.2f} mm from flange seat")
    print(f"Thread length:{THREAD_THROUGH_FLANGE_LENGTH:>6.2f} mm through flange")
    print(f"Nut AF under/top: {UNDER_FLANGE_NUT_AF:.2f} / {TOP_CLAMP_NUT_AF:.2f} mm")
    print(f"Mount PCD:    {UNIFIED_SENSOR_MOUNT_HOLE_SPACING:.2f} mm")
    print(f"Mount axis:   {UNIFIED_FEET_AXIS.upper()}")
    print(f"Top bracket adapter: {'enabled' if bracket_adapter is not None else 'disabled'}")
    print_bounds("Body bounding box:", cap)
    if bracket_adapter is not None:
        print_bounds("Bracket adapter box:", bracket_adapter)
    print_bounds("PG7 flange bounding box:", flange)
    print_printability_report(cap)
    if args.export_stl:
        print("Wrote modified body, PG7 flange, and optional bracket adapter STL outputs.")
        if include_original_base:
            print("Also wrote legacy modified original base output.")
    else:
        print("Preview mode: built modified Blender scene without exporting STL files.")
        print("Pass --export-stl when you are ready to write the STL outputs.")


if __name__ == "__main__":
    main()
