#!/usr/bin/env python3

# Copyright 2026 Robomous
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

"""Generate the bundled low-poly prop meshes (trees + shrub).

The props are *procedurally authored original work* (Apache-2.0, "original work
(this repository)") — parametric trunks/cones/blobs, not fetched third-party art. The
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

Every dimension here is authored at its **true world size**: the models declare
the metres a prop actually measures, so a placed instance needs no scale
factor. The targets come from ``docs/domain/realism_defaults.md`` §1.5–1.6 and
are mirrored by ``roadmaker::defaults``; this script is stdlib-only (it must
run on a bare CI runner) so it cannot include that header, and
``core/tests/test_defaults_registry.cpp`` is what holds the two in step —
retune a prop away from the spec and CI fails there.

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


# A normalized icosahedron's extreme vertex component: its surface reaches only
# this fraction of a blob()'s per-axis scale, so a crown sized straight from a
# spec diameter would come out ~15% short. blob_scale() undoes it.
ICOSPHERE_REACH = 0.85065080835204


def blob_scale(reach: float) -> float:
    """The blob() per-axis scale whose surface sits `reach` metres from centre."""
    return reach / ICOSPHERE_REACH


def blob(cx: float, cy: float, cz: float,
         sx: float, sy: float, sz: float) -> list[Tri]:
    """A low-poly icosahedral crown, scaled per axis and centred at (cx,cy,cz).
    The surface reaches ICOSPHERE_REACH * s along each axis, not s — use
    blob_scale() when sizing from a real-world extent."""
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
    """A conifer at 8.4 m — inside §1.6's street-tree band, kept slimmer and
    taller-crowned than the oak because a pine is not a shade tree."""
    trunk = cylinder(0.30, 0.0, 2.4)
    crown = (cone(2.4, 2.0, 4.8) + cone(1.9, 3.8, 6.4)
             + cone(1.3, 5.6, 8.4))
    return {
        "id": "tree_pine", "label": "Pine tree", "type": "Tree",
        "height": 8.4, "radius": 2.4,
        "parts": [("trunk", BROWN, trunk),
                  ("crown", (0.16, 0.38, 0.22), crown)],
    }


def tree_oak() -> dict:
    """THE §1.6 default street tree: 10.0 m tall, canopy Ø 6.0 m, trunk Ø
    0.40 m. The crown starts at 4.4 m, the roadway clear-trunk rule — which
    also satisfies the 2.4 m sidewalk one, so the tree is legal on either
    side of a curb."""
    height, canopy_radius, trunk_radius = 10.0, 3.0, 0.20
    crown_bottom = 4.4
    crown_center = (height + crown_bottom) / 2.0
    trunk = cylinder(trunk_radius, 0.0, crown_center)  # top hidden in the crown
    crown = blob(0.0, 0.0, crown_center,
                 blob_scale(canopy_radius), blob_scale(canopy_radius),
                 blob_scale((height - crown_bottom) / 2.0))
    return {
        "id": "tree_oak", "label": "Oak tree", "type": "Tree",
        "height": height, "radius": canopy_radius,
        "parts": [("trunk", BROWN, trunk),
                  ("crown", (0.24, 0.50, 0.24), crown)],
    }


def tree_birch() -> dict:
    """A slender birch at 9.4 m — §1.6 street-tree band, narrow crown."""
    trunk = cylinder(0.24, 0.0, 4.8)
    crown = blob(0.0, 0.0, 6.6, 2.0, 2.0, 2.8)
    return {
        "id": "tree_birch", "label": "Birch tree", "type": "Tree",
        "height": 9.4, "radius": 2.0,
        "parts": [("trunk", BIRCH_BARK, trunk),
                  ("crown", (0.44, 0.63, 0.32), crown)],
    }


def tree_poplar() -> dict:
    """A columnar poplar at 12.0 m — taller than the §1.6 default street tree
    but below the 15–20 m mature band, which is what a planted poplar reads
    as before it matures."""
    trunk = cylinder(0.30, 0.0, 2.0)
    crown = blob(0.0, 0.0, 6.8, 1.7, 1.7, 5.2)
    return {
        "id": "tree_poplar", "label": "Poplar tree", "type": "Tree",
        "height": 12.0, "radius": 1.7,
        "parts": [("trunk", BROWN, trunk),
                  ("crown", (0.30, 0.52, 0.26), crown)],
    }


def shrub() -> dict:
    """An ornamental shrub at 2.4 m — deliberately below §1.6's 4 m small-tree
    minimum, since a shrub is not a tree."""
    # Blob centres are lifted so the crown rests on z=0 (icosphere reaches
    # ~0.851*sz below centre) rather than sinking into the road surface.
    crown = (blob(0.0, 0.0, 1.12, 2.2, 2.2, 1.2)
             + blob(1.2, 0.4, 0.84, 1.2, 1.2, 0.9))
    return {
        "id": "shrub", "label": "Shrub", "type": "Vegetation",
        "height": 2.4, "radius": 2.2,
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


# --------------------------------------------------------------------------- #
# The US sign pack (spec §1.4).
#
# A sign is DATA here, exactly as it is in the kernel's roadmaker::signs
# catalogue: one row per designation giving its silhouette, its face size and
# its colours. The mesh is always the same three pieces — post, border plate,
# field plate — assembled so the field's BOTTOM EDGE sits at the spec's
# mounting height.
#
# This script is stdlib-only (it must run on a bare CI runner) so it cannot
# include roadmaker/road/defaults.hpp. core/tests/test_defaults_registry.cpp is
# what holds these numbers to the registry — the same enforced-not-compiled
# arrangement the §1.5/§1.6 props use.
# --------------------------------------------------------------------------- #

SIGN_MOUNT = 2.10        # §1.4 urban mounting height: field bottom edge above z=0
SIGN_POST_DIAMETER = 0.06  # §1.4 breakaway single post, visual
SIGN_BORDER = 0.035      # border reveal around the field, each side
PLATE_BACK = 0.00        # plate back face x
PLATE_MID = 0.02         # border front / field back
PLATE_FRONT = 0.04       # field front face x

SIGN_WHITE = (0.93, 0.93, 0.91)
SIGN_BLACK = (0.09, 0.09, 0.10)
SIGN_RED = (0.70, 0.09, 0.14)
SIGN_YELLOW = (0.90, 0.72, 0.05)
SIGN_GREEN = (0.02, 0.35, 0.20)
SIGN_LIME = (0.72, 0.86, 0.05)   # fluorescent yellow-green (school series)


def _sign_outline(shape: str, width: float, height: float) -> list[tuple[float, float]]:
    """The silhouette of a `shape` sign spanning width × height, as (y, z)
    offsets from the field centre. Every MUTCD shape is convex, so
    extruded_polygon can prism all of them."""
    hw, hh = width / 2.0, height / 2.0
    if shape == "octagon":  # flat top and bottom, inscribed in the face box
        return _regular_polygon(8, hw / math.cos(math.pi / 8.0), math.pi / 8.0)
    if shape == "triangle_down":
        return [(-hw, hh), (hw, hh), (0.0, -hh)]
    if shape == "diamond":
        return [(0.0, hh), (hw, 0.0), (0.0, -hh), (-hw, 0.0)]
    if shape == "pentagon":  # point up (school series)
        return _regular_polygon(5, hh, math.pi / 2.0)
    if shape == "disc":
        return _regular_polygon(24, hw)
    return [(-hw, -hh), (hw, -hh), (hw, hh), (-hw, hh)]  # rectangle


def _inscribed_half_extents(shape: str, width: float, height: float) -> tuple[float, float]:
    """Half-width/half-height of the largest centred axis-aligned rectangle that
    fits inside `shape` — the area the face texture may use. A rectangle gets
    the whole field; a diamond gets half of each extent; an octagon gets what
    its diagonal edges allow. Keeping the texture INSIDE the silhouette is why
    the artwork carries no background of its own: the shape, its border and its
    field colour are all mesh, so a stop sign reads as an octagon from every
    angle instead of a rectangle wearing an octagon picture."""
    hw, hh = width / 2.0, height / 2.0
    if shape == "octagon":
        r = hw / math.cos(math.pi / 8.0)
        a = r * math.cos(math.pi / 8.0) / math.sqrt(2.0)
        return a, a
    if shape == "triangle_down":
        # A band across the wide upper part of the triangle.
        return hw * 0.58, hh * 0.30
    if shape == "diamond":
        return hw * 0.5, hh * 0.5
    if shape == "pentagon":
        return hw * 0.55, hh * 0.5
    if shape == "disc":
        return hw / math.sqrt(2.0), hh / math.sqrt(2.0)
    return hw, hh


def _street_blade_length(legend: str, height: float) -> float:
    """§1.4 gives a D3-1 blade no length: "length fits text". So derive one —
    the default legend set at the blade's letter height, with a half-letter
    margin at each end. A derivation, not a new default."""
    letter = max(height * 0.6, 0.15)  # §1.4 floor: letters ≥ 0.15 m
    return len(legend) * letter * 0.62 + letter


# One row per catalogue designation. `symbol` names the artwork in
# assets/signs/us (empty = none); `legend` is the sign's FIXED wording and
# `text` its editable default, each with its own normalised box inside the
# texture area ({left, top, width, height}, origin top-left).
SIGN_PACK = [
    dict(id="sign_us_r1_1", label="Stop sign (R1-1)", shape="octagon",
         width=0.75, height=0.75, field=SIGN_RED, border=SIGN_WHITE,
         symbol="", legend="", legend_ink=SIGN_WHITE, ink=SIGN_WHITE,
         text_box=(0.0, 0.15, 1.0, 0.70)),
    dict(id="sign_us_r1_2", label="Yield sign (R1-2)", shape="triangle_down",
         width=0.90, height=0.90, field=SIGN_WHITE, border=SIGN_RED,
         symbol="", legend="", legend_ink=SIGN_RED, ink=SIGN_RED,
         text_box=(0.0, 0.1, 1.0, 0.8)),
    dict(id="sign_us_r2_1", label="Speed limit (R2-1)", shape="rectangle",
         width=0.60, height=0.75, field=SIGN_WHITE, border=SIGN_BLACK,
         symbol="", legend="SPEED\nLIMIT", legend_ink=SIGN_BLACK, ink=SIGN_BLACK,
         legend_box=(0.05, 0.04, 0.90, 0.34), text_box=(0.1, 0.42, 0.8, 0.54)),
    dict(id="sign_us_r5_1", label="Do not enter (R5-1)", shape="disc",
         width=0.75, height=0.75, field=SIGN_WHITE, border=SIGN_RED,
         symbol="R5-1", legend="", legend_ink=SIGN_BLACK, ink=SIGN_BLACK,
         text_box=(0.0, 0.0, 1.0, 1.0)),
    dict(id="sign_us_r6_1_right", label="One way, right (R6-1)", shape="rectangle",
         width=0.90, height=0.30, field=SIGN_BLACK, border=SIGN_WHITE,
         symbol="R6-1-right", legend="ONE WAY", legend_ink=SIGN_WHITE, ink=SIGN_WHITE,
         legend_box=(0.03, 0.18, 0.38, 0.64)),
    dict(id="sign_us_r6_1_left", label="One way, left (R6-1)", shape="rectangle",
         width=0.90, height=0.30, field=SIGN_BLACK, border=SIGN_WHITE,
         symbol="R6-1-left", legend="ONE WAY", legend_ink=SIGN_WHITE, ink=SIGN_WHITE,
         legend_box=(0.59, 0.18, 0.38, 0.64)),
    dict(id="sign_us_r3_1", label="No right turn (R3-1)", shape="rectangle",
         width=0.60, height=0.60, field=SIGN_WHITE, border=SIGN_BLACK,
         symbol="R3-1", legend="", legend_ink=SIGN_BLACK, ink=SIGN_BLACK),
    dict(id="sign_us_r3_2", label="No left turn (R3-2)", shape="rectangle",
         width=0.60, height=0.60, field=SIGN_WHITE, border=SIGN_BLACK,
         symbol="R3-2", legend="", legend_ink=SIGN_BLACK, ink=SIGN_BLACK),
    dict(id="sign_us_r4_7", label="Keep right (R4-7)", shape="rectangle",
         width=0.60, height=0.75, field=SIGN_WHITE, border=SIGN_BLACK,
         symbol="R4-7", legend="", legend_ink=SIGN_BLACK, ink=SIGN_BLACK),
    dict(id="sign_us_w1_2", label="Curve ahead (W1-2)", shape="diamond",
         width=0.75, height=0.75, field=SIGN_YELLOW, border=SIGN_BLACK,
         symbol="W1-2", legend="", legend_ink=SIGN_BLACK, ink=SIGN_BLACK),
    dict(id="sign_us_w3_1", label="Stop ahead (W3-1)", shape="diamond",
         width=0.75, height=0.75, field=SIGN_YELLOW, border=SIGN_BLACK,
         symbol="W3-1", legend="", legend_ink=SIGN_BLACK, ink=SIGN_BLACK),
    dict(id="sign_us_w11_2", label="Pedestrian crossing (W11-2)", shape="diamond",
         width=0.75, height=0.75, field=SIGN_YELLOW, border=SIGN_BLACK,
         symbol="W11-2", legend="", legend_ink=SIGN_BLACK, ink=SIGN_BLACK),
    dict(id="sign_us_s1_1", label="School (S1-1)", shape="pentagon",
         width=0.90, height=0.90, field=SIGN_LIME, border=SIGN_BLACK,
         symbol="S1-1", legend="", legend_ink=SIGN_BLACK, ink=SIGN_BLACK),
    dict(id="sign_us_d3_1", label="Street name (D3-1)", shape="rectangle",
         width=_street_blade_length("MAIN ST", 0.30), height=0.30,
         field=SIGN_GREEN, border=SIGN_WHITE,
         symbol="", legend="", legend_ink=SIGN_WHITE, ink=SIGN_WHITE,
         text_box=(0.04, 0.12, 0.92, 0.76)),
]


def _sign_model(spec: dict) -> dict:
    """Assemble one pack sign: post, border plate, field plate, face plate."""
    width, height = spec["width"], spec["height"]
    shape = spec["shape"]
    # §1.4's face size is the OVERALL plate, so the border is an inset ring and
    # the coloured field sits inside it. That also makes the model's bounding
    # height exactly mount + face height, which is what the registry gate
    # asserts.
    centre_z = SIGN_MOUNT + height / 2.0   # plate bottom edge at the mounting height
    border = _sign_outline(shape, width, height)
    field = _sign_outline(shape, width - 2.0 * SIGN_BORDER, height - 2.0 * SIGN_BORDER)

    # The post rises behind the plate to the field centre, so it never pokes
    # above a short blade and always supports a tall one.
    post = cylinder(SIGN_POST_DIAMETER / 2.0, 0.0, centre_z)
    parts = [
        ("post", POLE_GREY, post),
        ("border", spec["border"], extruded_polygon(border, PLATE_MID, PLATE_BACK, cz=centre_z)),
        ("field", spec["field"], extruded_polygon(field, PLATE_FRONT, PLATE_MID, cz=centre_z)),
    ]
    half_w, half_h = _inscribed_half_extents(
        shape, width - 2.0 * SIGN_BORDER, height - 2.0 * SIGN_BORDER)
    model = {
        "id": spec["id"], "label": spec["label"], "type": "None",
        "height": SIGN_MOUNT + height,
        "radius": math.hypot(width / 2.0, height / 2.0),
        "parts": parts,
        "face_plate": {
            "x": PLATE_FRONT + 0.005, "z": centre_z, "half_w": half_w, "half_h": half_h,
            "background": spec["field"], "ink": spec["ink"],
            "symbol": spec.get("symbol", ""),
            "legend": spec.get("legend", ""), "legend_ink": spec["legend_ink"],
            "legend_box": spec.get("legend_box", (0.0, 0.0, 1.0, 1.0)),
            "text_box": spec.get("text_box", (0.0, 0.0, 1.0, 1.0)),
        },
    }
    return model


def sign_generic() -> dict:
    """The fallback silhouette for a <signal> whose (@country, @type) this build
    ships no catalogue entry for — a foreign-country sign, or the German StVO
    plates RoadMaker authored before the US pack. A plain white plate with a
    dark border, sized like a regulatory sign so it reads as one."""
    spec = dict(id="sign_generic", label="Traffic sign", shape="rectangle",
                width=0.60, height=0.60, field=SIGN_WHITE, border=SIGN_BLACK,
                symbol="", legend="", legend_ink=SIGN_BLACK, ink=SIGN_BLACK)
    return _sign_model(spec)


SIGNALS = [signal_light(), sign_generic()] + [_sign_model(spec) for spec in SIGN_PACK]


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
    """A small commercial box: a body with a flat roof slab overhanging it.
    Untouched by #415 — 7.5 m already sits in §1.6's 7–9 m two-storey house
    band and the 10.4 × 8.4 m footprint clears the ≈10 × 8 m sanity check."""
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
    """A mid-rise block on §1.6's per-floor rule: five 3.7 m floors (18.5 m)
    plus a 1 m parapet zone, total 19.5 m. The roof slab and the rooftop plant
    unit both live inside that zone, so nothing pokes above the rule."""
    body = box(0.0, 0.0, 9.25, 12.0, 12.0, 18.5)       # 0..18.5 = 5 floors
    roof = box(0.0, 0.0, 18.70, 12.6, 12.6, 0.4)       # 18.5..18.9
    unit = box(-1.5, 1.5, 19.20, 4.0, 4.0, 0.6)        # rooftop HVAC, 18.9..19.5
    return {
        "id": "building_mid", "label": "Mid-rise building", "type": "Building",
        "height": 19.5,
        "radius": _footprint_radius((6.3, 6.3)),
        "parts": [("body", WALL_COOL, body),
                  ("roof", ROOF_GREY, roof),
                  ("rooftop_unit", ROOFTOP_UNIT, unit)],
    }


def building_tower() -> dict:
    """A stepped tower: three set-back box stages, each narrower than the last,
    on §1.6's per-floor rule — 5 + 4 + 1 floors of 3.7 m (37.0 m) capped by a
    1 m parapet, total 38.0 m."""
    base = box(0.0, 0.0, 9.25, 14.0, 14.0, 18.5)       # 0..18.5 = 5 floors
    mid = box(0.0, 0.0, 25.90, 11.0, 11.0, 14.8)       # 18.5..33.3 = 4 floors
    cap = box(0.0, 0.0, 35.15, 8.0, 8.0, 3.7)          # 33.3..37.0 = 1 floor
    parapet = box(0.0, 0.0, 37.50, 8.4, 8.4, 1.0)      # 37.0..38.0
    return {
        "id": "building_tower", "label": "Tower building", "type": "Building",
        "height": 38.0,
        "radius": _footprint_radius((7.0, 7.0)),
        "parts": [("base", WALL_TAN, base),
                  ("mid", WALL_WARM, mid),
                  ("cap", WALL_COOL, cap),
                  ("parapet", ROOF_GREY, parapet)],
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


# §1.5 street lighting: the luminaire mounts at 9.0 m, so the pole rises to
# that height and the arm carries the lamp head just under the top. The arm
# reach is a typical 1.8 m overhang toward the roadway.
LAMP_MOUNTING_HEIGHT = 9.0
LAMP_ARM_REACH = 1.8


def _lamp_arm(direction: float) -> list:
    """One horizontal arm + lamp head reaching `direction` (+1 → +x, -1 → -x)
    from the pole top. Returns (arm_tris, head_tris, lens_tris)."""
    tip = LAMP_ARM_REACH * direction
    top = LAMP_MOUNTING_HEIGHT
    arm = box(tip / 2.0, 0.0, top - 0.15, LAMP_ARM_REACH, 0.12, 0.12)
    head = box(tip, 0.0, top - 0.27, 0.60, 0.30, 0.24)
    lens = box(tip, 0.0, top - 0.42, 0.54, 0.26, 0.06)  # downward-facing lens
    return arm, head, lens


def streetlight_single() -> dict:
    """A single-arm streetlight: pole r0.15 to the §1.5 mounting height of
    9.0 m, one arm + lamp head hanging just below the top."""
    pole = cylinder(0.15, 0.0, LAMP_MOUNTING_HEIGHT)
    arm, head, lens = _lamp_arm(1.0)
    return {
        "id": "streetlight_single", "label": "Streetlight", "type": "Pole",
        "height": LAMP_MOUNTING_HEIGHT, "radius": LAMP_ARM_REACH + 0.30,
        "parts": [("pole", POLE_GREY, pole),
                  ("arm", POLE_GREY, arm),
                  ("head", LAMP_HOUSING, head),
                  ("lens", LAMP_LENS, lens)],
    }


def streetlight_double() -> dict:
    """A double-arm streetlight: two opposed arms + lamp heads on one pole."""
    pole = cylinder(0.15, 0.0, LAMP_MOUNTING_HEIGHT)
    arm_a, head_a, lens_a = _lamp_arm(1.0)
    arm_b, head_b, lens_b = _lamp_arm(-1.0)
    return {
        "id": "streetlight_double", "label": "Streetlight (double)",
        "type": "Pole", "height": LAMP_MOUNTING_HEIGHT,
        "radius": LAMP_ARM_REACH + 0.30,
        "parts": [("pole", POLE_GREY, pole),
                  ("arm_a", POLE_GREY, arm_a),
                  ("head_a", LAMP_HOUSING, head_a),
                  ("lens_a", LAMP_LENS, lens_a),
                  ("arm_b", POLE_GREY, arm_b),
                  ("head_b", LAMP_HOUSING, head_b),
                  ("lens_b", LAMP_LENS, lens_b)],
    }


STREETLIGHTS = [streetlight_single(), streetlight_double()]


# --------------------------------------------------------------------------- #
# Mast-arm signal PARTS (p6-s9, #323). Unlike every other model in this file,
# these three are not meant to be placed on their own — they are the parts the
# bundled `signal_mast` ASSEMBLY pins together (core/src/assets/
# prop_assembly_registry.cpp), and the Library does not offer them individually.
#
# WHY THE ARM IS AUTHORED ALREADY-HORIZONTAL. mesh::ObjectInstance carries a
# position, a heading about +Z and one uniform scale — no pitch, no roll — so a
# part cannot be laid down at placement time. It has to arrive lying down. The
# arm therefore reaches out +x at z ≈ 0 (local heading 0) and the assembly gives
# it a quarter-turn of yaw to send it across the road, exactly the way the
# streetlight arm above reaches +x.
#
# ALL THREE HEIGHTS COME FROM §1.5, and test_defaults_registry.cpp is the join
# (this script is stdlib-only and cannot include defaults.hpp):
#   housing 1.07 m tall, 0.30 m lenses      → kSignalHousingHeight / kSignalLensDiameter
#   housing bottom 5.2 m over the roadway   → kSignalClearance
#   arm underside = 5.2 + 1.07 = 6.27 m, so the heads hang flush under it
#   arm reach = 2 lane widths (2 × 3.6 m)   → kArterialLaneWidth, one head per lane
# --------------------------------------------------------------------------- #

SIGNAL_HOUSING_HEIGHT = 1.07   # §1.5 3-section head, 42 in
SIGNAL_LENS_DIAMETER = 0.30    # §1.5 12 in lens
SIGNAL_CLEARANCE = 5.2         # §1.5 default mast-arm clearance, housing bottom
MAST_ARM_REACH = 7.2           # 2 × the §1.2 arterial lane width
MAST_ARM_DEPTH = 0.20          # visual
MAST_POLE_DIAMETER = 0.28      # visual; stouter than the §1.4 sign post


def pole_signal() -> dict:
    """The upright of a mast-arm signal. It tops out flush with the arm, so its
    height is the arm's underside plus the arm depth."""
    top = SIGNAL_CLEARANCE + SIGNAL_HOUSING_HEIGHT + MAST_ARM_DEPTH
    return {
        "id": "pole_signal", "label": "Signal pole", "type": "Pole",
        "height": top, "radius": MAST_POLE_DIAMETER / 2.0,
        "parts": [("pole", POLE_GREY, cylinder(MAST_POLE_DIAMETER / 2.0, 0.0, top))],
    }


def mast_arm() -> dict:
    """The horizontal arm, lying down along +x from the pole. Authored flat: it
    spans z in [0, MAST_ARM_DEPTH] so the base-on-origin invariant holds, and
    the assembly lifts it to the arm underside."""
    arm = box(MAST_ARM_REACH / 2.0, 0.0, MAST_ARM_DEPTH / 2.0,
              MAST_ARM_REACH, 0.16, MAST_ARM_DEPTH)
    return {
        "id": "mast_arm", "label": "Signal mast arm", "type": "Pole",
        "height": MAST_ARM_DEPTH, "radius": MAST_ARM_REACH + 0.10,
        "parts": [("arm", POLE_GREY, arm)],
    }


def signal_head() -> dict:
    """A 3-section head on its own — housing plus three lenses on the +x face,
    no pole. Sits with its housing bottom at z=0; the assembly lifts it to the
    §1.5 clearance."""
    lens = SIGNAL_LENS_DIAMETER
    pitch = lens + 0.06  # lens-to-lens spacing inside the housing
    mid = SIGNAL_HOUSING_HEIGHT / 2.0
    housing = box(0.0, 0.0, mid, 0.22, lens + 0.10, SIGNAL_HOUSING_HEIGHT)
    lens_r = box(0.12, 0.0, mid + pitch, 0.05, lens, lens)
    lens_a = box(0.12, 0.0, mid, 0.05, lens, lens)
    lens_g = box(0.12, 0.0, mid - pitch, 0.05, lens, lens)
    return {
        "id": "signal_head", "label": "Signal head", "type": "Pole",
        "height": SIGNAL_HOUSING_HEIGHT, "radius": (lens + 0.10) / 2.0,
        "parts": [("housing", HOUSING_BLACK, housing),
                  ("lamp_red", LAMP_RED, lens_r),
                  ("lamp_amber", LAMP_AMBER, lens_a),
                  ("lamp_green", LAMP_GREEN, lens_g)],
    }


ASSEMBLY_PARTS = [pole_signal(), mast_arm(), signal_head()]

# Everything the kernel embeds and the library/exporters resolve by id.
MODELS = TREES + SIGNALS + BUILDINGS + STREETLIGHTS + ASSEMBLY_PARTS


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


def _cpp_string(text: str) -> str:
    """A C++ string literal for `text`, escaping the few characters the sign
    tables can carry (backslash, quote, newline)."""
    escaped = (text.replace("\\", "\\\\")
                   .replace('"', '\\"')
                   .replace("\n", "\\n"))
    return f'"{escaped}"'


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
        fp = tree.get("face_plate")
        if fp is not None:
            # POSITIONAL aggregate init — this must stay in lockstep with the
            # field order of props::FacePlate (prop_library.hpp), which says so
            # too. A reorder there without one here mis-assigns silently.
            bg, ink = fp["background"], fp["ink"]
            lg_ink = fp["legend_ink"]
            lg_box, tx_box = fp["legend_box"], fp["text_box"]
            rgb = lambda c: f"{{{c[0]:.4f}f, {c[1]:.4f}f, {c[2]:.4f}f}}"
            box = lambda b: "{" + ", ".join(f"{v:.4f}" for v in b) + "}"
            lines.append(
                f"    FacePlate{{{fp['x']:.4f}, {fp['z']:.4f}, "
                f"{fp['half_w']:.4f}, {fp['half_h']:.4f}, "
                f"{rgb(bg)}, {rgb(ink)}, "
                f"{_cpp_string(fp['symbol'])}, {_cpp_string(fp['legend'])}, "
                f"{rgb(lg_ink)}, {box(lg_box)}, {box(tx_box)}}},")
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
    lines.append("// The BUILT-IN half of the catalogue. The public ids()/model()")
    lines.append("// live in src/assets/prop_registry.cpp, which layers a project's")
    lines.append("// imported models over these (p6-s8, #322) — so this generated")
    lines.append("// file stays pure data and knows nothing about projects.")
    lines.append("namespace detail {")
    lines.append("")
    lines.append("const std::vector<std::string>& builtin_ids() {")
    lines.append("    static const std::vector<std::string> k_ids = {")
    for mid in model_ids:
        lines.append(f'        "{mid}",')
    lines.append("    };")
    lines.append("    return k_ids;")
    lines.append("}")
    lines.append("")
    lines.append("const PropModel* builtin_model(std::string_view id) {")
    lines.append("    for (const PropModel* m : k_models) {")
    lines.append("        if (m->id == id) {")
    lines.append("            return m;")
    lines.append("        }")
    lines.append("    }")
    lines.append("    return nullptr;")
    lines.append("}")
    lines.append("")
    lines.append("} // namespace detail")
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
