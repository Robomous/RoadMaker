#!/usr/bin/env python3
"""Generate the bundled low-poly prop meshes (trees + shrub).

The props are *procedurally authored original work* (MIT, "original work (this
repository)") — parametric trunks/cones/blobs, not fetched third-party art. The
script is the single source of truth; it emits two consistent representations
from the same geometry:

  1. ``assets/library/props/<id>.obj`` (+ ``.mtl``) — an inspectable reference
     export a designer can open in Blender/MeshLab. Purely provenance; nothing
     loads it at runtime.
  2. ``core/src/assets/prop_meshes.gen.cpp`` — the embedded, flat-shaded mesh
     table the kernel compiles in. This is what the mesh builder, the glTF/USD
     exporters, and the editor renderer actually consume (via
     ``roadmaker::props::model``). No runtime file IO, works headless,
     cross-platform.

Regenerate after changing any tree parameter:

    python3 scripts/gen_prop_meshes.py

Stdlib only — must run on any CI runner. Kernel frame: right-handed, Z-up,
meters; a prop's origin is the base centre (z=0 sits on the road surface).
"""

from __future__ import annotations

import math
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
OBJ_DIR = REPO_ROOT / "assets" / "library" / "props"
GEN_CPP = REPO_ROOT / "core" / "src" / "assets" / "prop_meshes.gen.cpp"

Vec3 = tuple[float, float, float]
Tri = tuple[Vec3, Vec3, Vec3]


# --------------------------------------------------------------------------- #
# Geometry primitives — each returns a list of outward-facing triangles.
# --------------------------------------------------------------------------- #

def _sub(a: Vec3, b: Vec3) -> Vec3:
    return (a[0] - b[0], a[1] - b[1], a[2] - b[2])


def _cross(a: Vec3, b: Vec3) -> Vec3:
    return (a[1] * b[2] - a[2] * b[1],
            a[2] * b[0] - a[0] * b[2],
            a[0] * b[1] - a[1] * b[0])


def _dot(a: Vec3, b: Vec3) -> float:
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]


def _normalize(a: Vec3) -> Vec3:
    length = math.sqrt(_dot(a, a)) or 1.0
    return (a[0] / length, a[1] / length, a[2] / length)


def _face_normal(t: Tri) -> Vec3:
    return _normalize(_cross(_sub(t[1], t[0]), _sub(t[2], t[0])))


def _orient_outward(tris: list[Tri], centroid: Vec3) -> list[Tri]:
    """Flip any triangle whose normal points toward the part centroid so every
    face of a convex primitive faces out (CCW when viewed from outside)."""
    out: list[Tri] = []
    for t in tris:
        n = _face_normal(t)
        face_center = ((t[0][0] + t[1][0] + t[2][0]) / 3.0,
                       (t[0][1] + t[1][1] + t[2][1]) / 3.0,
                       (t[0][2] + t[1][2] + t[2][2]) / 3.0)
        if _dot(n, _sub(face_center, centroid)) < 0.0:
            out.append((t[0], t[2], t[1]))
        else:
            out.append(t)
    return out


def _ring(segments: int, radius: float, z: float) -> list[Vec3]:
    pts: list[Vec3] = []
    for i in range(segments):
        a = 2.0 * math.pi * i / segments
        pts.append((radius * math.cos(a), radius * math.sin(a), z))
    return pts


def frustum(r0: float, r1: float, z0: float, z1: float,
            segments: int = 8, cap_bottom: bool = True,
            cap_top: bool = True) -> list[Tri]:
    """A tapered tube from radius r0 at z0 to r1 at z1 (cone if r1==0)."""
    bottom = _ring(segments, r0, z0) if r0 > 1e-6 else None
    top = _ring(segments, r1, z1) if r1 > 1e-6 else None
    tris: list[Tri] = []
    apex_top: Vec3 = (0.0, 0.0, z1)
    apex_bot: Vec3 = (0.0, 0.0, z0)
    for i in range(segments):
        j = (i + 1) % segments
        if bottom and top:
            tris.append((bottom[i], bottom[j], top[j]))
            tris.append((bottom[i], top[j], top[i]))
        elif bottom:  # cone narrowing to a point at the top
            tris.append((bottom[i], bottom[j], apex_top))
        elif top:  # cone widening from a point at the bottom
            tris.append((apex_bot, top[j], top[i]))
    if cap_bottom and bottom:
        for i in range(1, segments - 1):
            tris.append((bottom[0], bottom[i], bottom[i + 1]))
    if cap_top and top:
        for i in range(1, segments - 1):
            tris.append((top[0], top[i], top[i + 1]))
    centroid: Vec3 = (0.0, 0.0, (z0 + z1) / 2.0)
    return _orient_outward(tris, centroid)


def cylinder(radius: float, z0: float, z1: float, segments: int = 8) -> list[Tri]:
    return frustum(radius, radius, z0, z1, segments)


def cone(radius: float, z0: float, z1: float, segments: int = 8) -> list[Tri]:
    return frustum(radius, 0.0, z0, z1, segments)


def _icosahedron() -> tuple[list[Vec3], list[tuple[int, int, int]]]:
    t = (1.0 + math.sqrt(5.0)) / 2.0
    verts = [(-1, t, 0), (1, t, 0), (-1, -t, 0), (1, -t, 0),
             (0, -1, t), (0, 1, t), (0, -1, -t), (0, 1, -t),
             (t, 0, -1), (t, 0, 1), (-t, 0, -1), (-t, 0, 1)]
    verts = [_normalize(v) for v in verts]
    faces = [(0, 11, 5), (0, 5, 1), (0, 1, 7), (0, 7, 10), (0, 10, 11),
             (1, 5, 9), (5, 11, 4), (11, 10, 2), (10, 7, 6), (7, 1, 8),
             (3, 9, 4), (3, 4, 2), (3, 2, 6), (3, 6, 8), (3, 8, 9),
             (4, 9, 5), (2, 4, 11), (6, 2, 10), (8, 6, 7), (9, 8, 1)]
    return verts, faces


def blob(cx: float, cy: float, cz: float,
         sx: float, sy: float, sz: float) -> list[Tri]:
    """A low-poly icosahedral crown, scaled per axis and centred at (cx,cy,cz)."""
    verts, faces = _icosahedron()
    placed = [(cx + v[0] * sx, cy + v[1] * sy, cz + v[2] * sz) for v in verts]
    tris = [(placed[a], placed[b], placed[c]) for a, b, c in faces]
    return _orient_outward(tris, (cx, cy, cz))


def box(cx: float, cy: float, cz: float,
        sx: float, sy: float, sz: float) -> list[Tri]:
    """An axis-aligned box centred at (cx,cy,cz) with full extents sx,sy,sz.
    Signal housings and sign plates are boxes; the "front" faces +x (local
    heading 0), so a thin sx makes a plate whose face looks down +x."""
    hx, hy, hz = sx / 2.0, sy / 2.0, sz / 2.0
    v = [(cx - hx, cy - hy, cz - hz), (cx + hx, cy - hy, cz - hz),
         (cx + hx, cy + hy, cz - hz), (cx - hx, cy + hy, cz - hz),
         (cx - hx, cy - hy, cz + hz), (cx + hx, cy - hy, cz + hz),
         (cx + hx, cy + hy, cz + hz), (cx - hx, cy + hy, cz + hz)]
    quads = [(0, 3, 2, 1), (4, 5, 6, 7), (0, 1, 5, 4),
             (2, 3, 7, 6), (1, 2, 6, 5), (3, 0, 4, 7)]
    tris: list[Tri] = []
    for a, b, c, d in quads:
        tris.append((v[a], v[b], v[c]))
        tris.append((v[a], v[c], v[d]))
    return _orient_outward(tris, (cx, cy, cz))


def _regular_polygon(sides: int, radius: float,
                     rot: float = 0.0) -> list[tuple[float, float]]:
    """`sides` (y, z) vertices of a regular polygon of `radius`, rotated by
    `rot` radians — the cross-section of a sign plate in its local y-z plane."""
    return [(radius * math.cos(2.0 * math.pi * i / sides + rot),
             radius * math.sin(2.0 * math.pi * i / sides + rot))
            for i in range(sides)]


def extruded_polygon(verts_yz: list[tuple[float, float]],
                     x_front: float, x_back: float,
                     cy: float = 0.0, cz: float = 0.0) -> list[Tri]:
    """A thin convex prism: a polygon authored in the local y-z plane (its face
    looks down +x), extruded between x_back and x_front. Sign plates (octagons,
    triangles) are these prisms so a plate reads as its true silhouette rather
    than a box. verts_yz are (y, z) offsets from (cy, cz)."""
    n = len(verts_yz)
    front = [(x_front, cy + vy, cz + vz) for vy, vz in verts_yz]
    back = [(x_back, cy + vy, cz + vz) for vy, vz in verts_yz]
    tris: list[Tri] = []
    for i in range(1, n - 1):  # front cap fan
        tris.append((front[0], front[i], front[i + 1]))
    for i in range(1, n - 1):  # back cap fan
        tris.append((back[0], back[i], back[i + 1]))
    for i in range(n):  # side walls
        j = (i + 1) % n
        tris.append((front[i], back[i], back[j]))
        tris.append((front[i], back[j], front[j]))
    centroid: Vec3 = ((x_front + x_back) / 2.0, cy, cz)
    return _orient_outward(tris, centroid)


def _footprint_radius(*half_extents: tuple[float, float]) -> float:
    """Circumscribed footprint radius (m) — the largest box's half-diagonal, so
    a building's bounding sphere covers its plan silhouette for picking."""
    return max(math.hypot(hx, hy) for hx, hy in half_extents)


# --------------------------------------------------------------------------- #
# Tree definitions — each part is (name, color, triangles).
# --------------------------------------------------------------------------- #

BROWN = (0.42, 0.30, 0.17)
BIRCH_BARK = (0.83, 0.83, 0.78)


def tree_pine() -> dict:
    trunk = cylinder(0.15, 0.0, 1.2)
    crown = (cone(1.2, 1.0, 2.4) + cone(0.95, 1.9, 3.2)
             + cone(0.65, 2.8, 4.2))
    return {
        "id": "tree_pine", "label": "Pine tree", "type": "Tree",
        "height": 4.2, "radius": 1.2,
        "parts": [("trunk", BROWN, trunk),
                  ("crown", (0.16, 0.38, 0.22), crown)],
    }


def tree_oak() -> dict:
    trunk = cylinder(0.22, 0.0, 1.9)
    crown = blob(0.0, 0.0, 3.0, 1.8, 1.8, 1.6)
    return {
        "id": "tree_oak", "label": "Oak tree", "type": "Tree",
        "height": 4.6, "radius": 1.8,
        "parts": [("trunk", BROWN, trunk),
                  ("crown", (0.24, 0.50, 0.24), crown)],
    }


def tree_birch() -> dict:
    trunk = cylinder(0.12, 0.0, 2.4)
    crown = blob(0.0, 0.0, 3.3, 1.0, 1.0, 1.4)
    return {
        "id": "tree_birch", "label": "Birch tree", "type": "Tree",
        "height": 4.7, "radius": 1.0,
        "parts": [("trunk", BIRCH_BARK, trunk),
                  ("crown", (0.44, 0.63, 0.32), crown)],
    }


def tree_poplar() -> dict:
    trunk = cylinder(0.15, 0.0, 1.0)
    crown = blob(0.0, 0.0, 3.4, 0.85, 0.85, 2.6)
    return {
        "id": "tree_poplar", "label": "Poplar tree", "type": "Tree",
        "height": 6.0, "radius": 0.85,
        "parts": [("trunk", BROWN, trunk),
                  ("crown", (0.30, 0.52, 0.26), crown)],
    }


def shrub() -> dict:
    # Blob centres are lifted so the crown rests on z=0 (icosphere reaches
    # ~0.851*sz below centre) rather than sinking into the road surface.
    crown = (blob(0.0, 0.0, 0.56, 1.1, 1.1, 0.6)
             + blob(0.6, 0.2, 0.42, 0.6, 0.6, 0.45))
    return {
        "id": "shrub", "label": "Shrub", "type": "Vegetation",
        "height": 1.2, "radius": 1.1,
        "parts": [("foliage", (0.28, 0.46, 0.24), crown)],
    }


TREES = [tree_pine(), tree_oak(), tree_birch(), tree_poplar(), shrub()]


# --------------------------------------------------------------------------- #
# Signal definitions — traffic light + generic sign. Same procedurally-authored
# original-work provenance as the trees (no third-party art). A signal's local
# frame faces +x (heading 0 points down +x); the mesh builder rotates it to the
# world heading derived from the road tangent + the signal's hOffset. z=0 sits
# on the road surface; the pole rises along +z. Colours are flat linear RGB.
# --------------------------------------------------------------------------- #

POLE_GREY = (0.32, 0.34, 0.36)
HOUSING_BLACK = (0.10, 0.11, 0.12)
LAMP_RED = (0.86, 0.14, 0.11)
LAMP_AMBER = (0.94, 0.66, 0.12)
LAMP_GREEN = (0.18, 0.70, 0.30)
PLATE_WHITE = (0.92, 0.92, 0.90)
PLATE_RIM = (0.74, 0.14, 0.12)


def signal_light() -> dict:
    """Three-lamp vertical traffic light on a pole. Housing faces +x; the lamps
    sit on the +x face so a light placed facing oncoming traffic shows its
    lenses. Overall height 3.9 m (pole 3.0 + housing 0.9)."""
    pole = cylinder(0.08, 0.0, 3.0)
    housing = box(0.0, 0.0, 3.42, 0.18, 0.26, 0.84)
    lamp_r = box(0.10, 0.0, 3.66, 0.05, 0.14, 0.14)
    lamp_a = box(0.10, 0.0, 3.42, 0.05, 0.14, 0.14)
    lamp_g = box(0.10, 0.0, 3.18, 0.05, 0.14, 0.14)
    return {
        "id": "signal_light", "label": "Traffic light", "type": "None",
        "height": 3.9, "radius": 0.26,
        "parts": [("pole", POLE_GREY, pole),
                  ("housing", HOUSING_BLACK, housing),
                  ("lamp_red", LAMP_RED, lamp_r),
                  ("lamp_amber", LAMP_AMBER, lamp_a),
                  ("lamp_green", LAMP_GREEN, lamp_g)],
    }


def sign_generic() -> dict:
    """A generic round-ish regulatory sign: a thin plate on a pole, plate face
    down +x. The plate is a white disc with a red rim (two coplanar boxes, the
    rim slightly larger and behind) — enough to read as a sign at scene scale
    without importing MUTCD artwork. Overall height 2.55 m."""
    pole = cylinder(0.05, 0.0, 2.2)
    rim = box(0.02, 0.0, 2.32, 0.03, 0.64, 0.64)
    face = box(0.04, 0.0, 2.32, 0.03, 0.52, 0.52)
    return {
        "id": "sign_generic", "label": "Traffic sign", "type": "None",
        "height": 2.7, "radius": 0.32,
        "parts": [("pole", POLE_GREY, pole),
                  ("rim", PLATE_RIM, rim),
                  ("face", PLATE_WHITE, face)],
    }


STOP_RED = (0.78, 0.11, 0.12)
YIELD_RED = (0.80, 0.13, 0.13)


def sign_stop() -> dict:
    """A STOP sign: a red octagonal plate with a white border on a pole, plate
    face down +x (German StVO 206 / MUTCD R1-1 silhouette). The white rim sits
    just behind the red face so the plate reads as a bordered octagon."""
    pole = cylinder(0.05, 0.0, 2.2)
    octagon = _regular_polygon(8, 0.42, math.pi / 8.0)  # flat top + bottom
    inner = _regular_polygon(8, 0.36, math.pi / 8.0)
    rim = extruded_polygon(octagon, 0.03, 0.00, cz=2.35)   # white border, behind
    face = extruded_polygon(inner, 0.06, 0.03, cz=2.35)    # red face, in front
    return {
        "id": "sign_stop", "label": "Stop sign", "type": "None",
        "height": 2.77, "radius": 0.42,
        "parts": [("pole", POLE_GREY, pole),
                  ("rim", PLATE_WHITE, rim),
                  ("face", STOP_RED, face)],
    }


def sign_yield() -> dict:
    """A YIELD sign: a downward-pointing triangular plate, white face with a red
    border, on a pole (German StVO 205 / MUTCD R1-2 silhouette). Plate face down
    +x; the red rim triangle sits just behind the white face."""
    pole = cylinder(0.05, 0.0, 2.2)
    outer = [(-0.50, 0.42), (0.50, 0.42), (0.0, -0.52)]  # red rim (down-point)
    inner = [(-0.38, 0.34), (0.38, 0.34), (0.0, -0.38)]  # white face
    rim = extruded_polygon(outer, 0.03, 0.00, cz=2.45)
    face = extruded_polygon(inner, 0.06, 0.03, cz=2.45)
    return {
        "id": "sign_yield", "label": "Yield sign", "type": "None",
        "height": 2.87, "radius": 0.52,
        "parts": [("pole", POLE_GREY, pole),
                  ("rim", YIELD_RED, rim),
                  ("face", PLATE_WHITE, face)],
    }


SIGNALS = [signal_light(), sign_generic(), sign_stop(), sign_yield()]


# --------------------------------------------------------------------------- #
# Building definitions — low-poly stacked boxes, base centre at ground. Same
# procedurally-authored original-work provenance (no fetched art). A placed
# instance is an OpenDRIVE <object type="building">; radius is the circumscribed
# footprint radius so bounding-sphere picking covers the plan silhouette.
# --------------------------------------------------------------------------- #

WALL_WARM = (0.72, 0.69, 0.63)
WALL_COOL = (0.60, 0.62, 0.66)
WALL_TAN = (0.78, 0.71, 0.60)
ROOF_GREY = (0.33, 0.34, 0.36)
ROOFTOP_UNIT = (0.46, 0.47, 0.49)


def building_low() -> dict:
    """A small commercial box: a body with a flat roof slab overhanging it."""
    body = box(0.0, 0.0, 3.5, 10.0, 8.0, 7.0)          # 0..7
    roof = box(0.0, 0.0, 7.25, 10.4, 8.4, 0.5)         # 7..7.5, slight overhang
    return {
        "id": "building_low", "label": "Low building", "type": "Building",
        "height": 7.5,
        "radius": _footprint_radius((5.2, 4.2)),
        "parts": [("body", WALL_WARM, body),
                  ("roof", ROOF_GREY, roof)],
    }


def building_mid() -> dict:
    """A mid-rise block: a tall body, a roof slab, and a rooftop plant unit."""
    body = box(0.0, 0.0, 9.0, 12.0, 12.0, 18.0)        # 0..18
    roof = box(0.0, 0.0, 18.25, 12.6, 12.6, 0.5)       # 18..18.5
    unit = box(-1.5, 1.5, 19.4, 4.0, 4.0, 1.8)         # rooftop HVAC, 18.5..20.3
    return {
        "id": "building_mid", "label": "Mid-rise building", "type": "Building",
        "height": 20.3,
        "radius": _footprint_radius((6.3, 6.3)),
        "parts": [("body", WALL_COOL, body),
                  ("roof", ROOF_GREY, roof),
                  ("rooftop_unit", ROOFTOP_UNIT, unit)],
    }


def building_tower() -> dict:
    """A stepped tower: three set-back box stages, each narrower than the last."""
    base = box(0.0, 0.0, 9.0, 14.0, 14.0, 18.0)        # 0..18
    mid = box(0.0, 0.0, 25.5, 11.0, 11.0, 15.0)        # 18..33
    cap = box(0.0, 0.0, 36.5, 8.0, 8.0, 7.0)           # 33..40
    return {
        "id": "building_tower", "label": "Tower building", "type": "Building",
        "height": 40.0,
        "radius": _footprint_radius((7.0, 7.0)),
        "parts": [("base", WALL_TAN, base),
                  ("mid", WALL_WARM, mid),
                  ("cap", WALL_COOL, cap)],
    }


BUILDINGS = [building_low(), building_mid(), building_tower()]


# --------------------------------------------------------------------------- #
# Streetlight definitions — a pole with one or two lamp arms. A placed instance
# is an OpenDRIVE <object type="pole">. The arm reaches out +x (local heading 0)
# and the lamp head hangs at its end; the mesh builder rotates the whole model
# to the world heading, so the lamp overhangs the road per the placed hdg.
# --------------------------------------------------------------------------- #

LAMP_HOUSING = (0.15, 0.16, 0.17)
LAMP_LENS = (0.98, 0.90, 0.66)


def _lamp_arm(direction: float) -> list:
    """One horizontal arm + lamp head reaching `direction` (+1 → +x, -1 → -x)
    from the pole top. Returns (arm_tris, head_tris, lens_tris)."""
    tip = 1.2 * direction
    arm = box(0.6 * direction, 0.0, 5.4, 1.2, 0.10, 0.10)
    head = box(tip, 0.0, 5.32, 0.44, 0.24, 0.18)
    lens = box(tip, 0.0, 5.21, 0.40, 0.20, 0.04)  # downward-facing lens
    return arm, head, lens


def streetlight_single() -> dict:
    """A single-arm streetlight: pole r0.12 h5.5, one arm + lamp head."""
    pole = cylinder(0.12, 0.0, 5.5)
    arm, head, lens = _lamp_arm(1.0)
    return {
        "id": "streetlight_single", "label": "Streetlight", "type": "Pole",
        "height": 5.5, "radius": 1.4,
        "parts": [("pole", POLE_GREY, pole),
                  ("arm", POLE_GREY, arm),
                  ("head", LAMP_HOUSING, head),
                  ("lens", LAMP_LENS, lens)],
    }


def streetlight_double() -> dict:
    """A double-arm streetlight: two opposed arms + lamp heads on one pole."""
    pole = cylinder(0.12, 0.0, 5.5)
    arm_a, head_a, lens_a = _lamp_arm(1.0)
    arm_b, head_b, lens_b = _lamp_arm(-1.0)
    return {
        "id": "streetlight_double", "label": "Streetlight (double)",
        "type": "Pole", "height": 5.5, "radius": 1.4,
        "parts": [("pole", POLE_GREY, pole),
                  ("arm_a", POLE_GREY, arm_a),
                  ("head_a", LAMP_HOUSING, head_a),
                  ("lens_a", LAMP_LENS, lens_a),
                  ("arm_b", POLE_GREY, arm_b),
                  ("head_b", LAMP_HOUSING, head_b),
                  ("lens_b", LAMP_LENS, lens_b)],
    }


STREETLIGHTS = [streetlight_single(), streetlight_double()]

# Everything the kernel embeds and the library/exporters resolve by id.
MODELS = TREES + SIGNALS + BUILDINGS + STREETLIGHTS


# --------------------------------------------------------------------------- #
# Flat-shaded expansion + emitters.
# --------------------------------------------------------------------------- #

def flat_arrays(tris: list[Tri]):
    """Expand triangles to unshared flat-shaded vertices (each face carries its
    own normal). Returns (positions, normals, indices)."""
    positions: list[float] = []
    normals: list[float] = []
    indices: list[int] = []
    for t in tris:
        n = _face_normal(t)
        base = len(positions) // 3
        for p in t:
            positions.extend(p)
            normals.extend(n)
        indices.extend((base, base + 1, base + 2))
    return positions, normals, indices


def write_obj(tree: dict) -> None:
    obj_path = OBJ_DIR / f"{tree['id']}.obj"
    mtl_path = OBJ_DIR / f"{tree['id']}.mtl"
    obj = [f"# {tree['label']} — procedurally authored low-poly prop.",
           f"# Original work (RoadMaker); regenerate: scripts/gen_prop_meshes.py",
           f"mtllib {tree['id']}.mtl"]
    mtl = [f"# Materials for {tree['id']} (linear RGB)."]
    voff = 0
    for name, color, tris in tree["parts"]:
        positions, normals, indices = flat_arrays(tris)
        mtl.append(f"newmtl {name}")
        mtl.append(f"Kd {color[0]:.4f} {color[1]:.4f} {color[2]:.4f}")
        obj.append(f"o {tree['id']}_{name}")
        obj.append(f"usemtl {name}")
        nverts = len(positions) // 3
        for i in range(nverts):
            obj.append(f"v {positions[3*i]:.5f} {positions[3*i+1]:.5f} "
                       f"{positions[3*i+2]:.5f}")
        for i in range(nverts):
            obj.append(f"vn {normals[3*i]:.5f} {normals[3*i+1]:.5f} "
                       f"{normals[3*i+2]:.5f}")
        for i in range(0, len(indices), 3):
            a, b, c = (indices[i] + 1 + voff, indices[i+1] + 1 + voff,
                       indices[i+2] + 1 + voff)
            obj.append(f"f {a}//{a} {b}//{b} {c}//{c}")
        voff += nverts
    obj_path.write_text("\n".join(obj) + "\n", encoding="utf-8")
    mtl_path.write_text("\n".join(mtl) + "\n", encoding="utf-8")


def _fmt_doubles(values: list[float]) -> str:
    return ", ".join(f"{v:.5f}" for v in values)


def write_cpp() -> None:
    lines = [
        "// GENERATED by scripts/gen_prop_meshes.py — DO NOT EDIT BY HAND.",
        "// Bundled low-poly prop meshes (procedurally authored original work).",
        "// Regenerate after changing any tree parameter:",
        "//   python3 scripts/gen_prop_meshes.py",
        "",
        '#include "roadmaker/assets/prop_library.hpp"',
        "",
        "#include <array>",
        "",
        "// clang-format off — this file is generated; the dense data tables are",
        "// left verbatim so CI's clang-format --Werror pass does not rewrite them.",
        "// clang-format off",
        "",
        "namespace roadmaker::props {",
        "namespace {",
        "",
    ]
    model_ids: list[str] = []
    for tree in MODELS:
        cid = tree["id"].replace("-", "_")
        model_ids.append(tree["id"])
        lines.append(f"const PropModel k_{cid} = {{")
        lines.append(f'    "{tree["id"]}",')
        lines.append("    {")
        for name, color, tris in tree["parts"]:
            positions, normals, indices = flat_arrays(tris)
            lines.append("        PropPart{")
            lines.append(f"            {{{_fmt_doubles(positions)}}},")
            lines.append(f"            {{{_fmt_doubles(normals)}}},")
            lines.append("            {"
                         + ", ".join(str(i) for i in indices) + "},")
            lines.append(f"            {{{color[0]:.4f}f, {color[1]:.4f}f, "
                         f"{color[2]:.4f}f}},")
            lines.append(f'            "{name}",')
            lines.append("        },")
        lines.append("    },")
        lines.append(f"    {tree['height']:.4f},")
        lines.append(f"    {tree['radius']:.4f},")
        lines.append(f"    ObjectType::{tree['type']},")
        lines.append("};")
        lines.append("")
    lines.append("const std::array<const PropModel*, "
                 f"{len(MODELS)}> k_models = {{")
    for tree in MODELS:
        cid = tree["id"].replace("-", "_")
        lines.append(f"    &k_{cid},")
    lines.append("};")
    lines.append("")
    lines.append("} // namespace")
    lines.append("")
    lines.append("const std::vector<std::string>& ids() {")
    lines.append("    static const std::vector<std::string> k_ids = {")
    for mid in model_ids:
        lines.append(f'        "{mid}",')
    lines.append("    };")
    lines.append("    return k_ids;")
    lines.append("}")
    lines.append("")
    lines.append("const PropModel* model(std::string_view id) {")
    lines.append("    for (const PropModel* m : k_models) {")
    lines.append("        if (m->id == id) {")
    lines.append("            return m;")
    lines.append("        }")
    lines.append("    }")
    lines.append("    return nullptr;")
    lines.append("}")
    lines.append("")
    lines.append("} // namespace roadmaker::props")
    lines.append("")
    lines.append("// clang-format on")
    GEN_CPP.write_text("\n".join(lines) + "\n", encoding="utf-8")


def main() -> int:
    OBJ_DIR.mkdir(parents=True, exist_ok=True)
    GEN_CPP.parent.mkdir(parents=True, exist_ok=True)
    for tree in MODELS:
        write_obj(tree)
    write_cpp()
    tri_total = sum(len(tris) for tree in MODELS for _, _, tris in tree["parts"])
    print(f"[gen_prop_meshes] wrote {len(MODELS)} models "
          f"({tri_total} triangles) → {OBJ_DIR} and {GEN_CPP}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
