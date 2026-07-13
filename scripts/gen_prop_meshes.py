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
        "id": "tree_pine", "label": "Pine tree", "otype": "Tree",
        "height": 4.2, "radius": 1.2,
        "parts": [("trunk", BROWN, trunk),
                  ("crown", (0.16, 0.38, 0.22), crown)],
    }


def tree_oak() -> dict:
    trunk = cylinder(0.22, 0.0, 1.9)
    crown = blob(0.0, 0.0, 3.0, 1.8, 1.8, 1.6)
    return {
        "id": "tree_oak", "label": "Oak tree", "otype": "Tree",
        "height": 4.6, "radius": 1.8,
        "parts": [("trunk", BROWN, trunk),
                  ("crown", (0.24, 0.50, 0.24), crown)],
    }


def tree_birch() -> dict:
    trunk = cylinder(0.12, 0.0, 2.4)
    crown = blob(0.0, 0.0, 3.3, 1.0, 1.0, 1.4)
    return {
        "id": "tree_birch", "label": "Birch tree", "otype": "Tree",
        "height": 4.7, "radius": 1.0,
        "parts": [("trunk", BIRCH_BARK, trunk),
                  ("crown", (0.44, 0.63, 0.32), crown)],
    }


def tree_poplar() -> dict:
    trunk = cylinder(0.15, 0.0, 1.0)
    crown = blob(0.0, 0.0, 3.4, 0.85, 0.85, 2.6)
    return {
        "id": "tree_poplar", "label": "Poplar tree", "otype": "Tree",
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
        "id": "shrub", "label": "Shrub", "otype": "Vegetation",
        "height": 1.2, "radius": 1.1,
        "parts": [("foliage", (0.28, 0.46, 0.24), crown)],
    }


TREES = [tree_pine(), tree_oak(), tree_birch(), tree_poplar(), shrub()]


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
    for tree in TREES:
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
        lines.append("};")
        lines.append("")
    lines.append("const std::array<const PropModel*, "
                 f"{len(TREES)}> k_models = {{")
    for tree in TREES:
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
    for tree in TREES:
        write_obj(tree)
    write_cpp()
    tri_total = sum(len(tris) for tree in TREES for _, _, tris in tree["parts"])
    print(f"[gen_prop_meshes] wrote {len(TREES)} props "
          f"({tri_total} triangles) → {OBJ_DIR} and {GEN_CPP}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
