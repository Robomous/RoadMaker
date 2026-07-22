#!/usr/bin/env python3
"""Generate the Library panel's item thumbnails (p6-s2, #236).

Every catalogue item in ``assets/library/manifest.json`` names a 96x96 PNG under
``assets/library/thumbnails/``; this script draws all of them. They are
*original work* (MIT, "original work (this repository)") — a tiny software
rasterizer here, no third-party art:

  * Props and signals are rendered from the SAME procedurally-authored geometry
    the kernel embeds (``gen_prop_meshes.MODELS``): an orthographic three-quarter
    view, painter's-sorted, flat-Lambert shaded per part colour — so the preview
    matches what the scene draws.
  * Road templates, the road style, the T/X assemblies, the lane markings, and
    the ground materials are stylised top-down swatches built from the same fill
    primitives (drawn, not texture crops, so the licence stays original work).

Regenerate after adding or renaming a catalogue item:

    python3 scripts/gen_library_thumbnails.py

Stdlib only (zlib/struct/math/random) — runs on any CI runner. Output is
committed; there is no byte-identity gate (zlib output varies across builds).
"""

from __future__ import annotations

import math
import random
import struct
import zlib
from pathlib import Path

import gen_prop_meshes as props

REPO_ROOT = Path(__file__).resolve().parent.parent
OUT_DIR = REPO_ROOT / "assets" / "library" / "thumbnails"

SIZE = 96  # 2x the 48px Library icon size


# --------------------------------------------------------------------------- #
# Canvas + PNG writer (RGBA8, filter 0).
# --------------------------------------------------------------------------- #

def new_canvas() -> bytearray:
    return bytearray(SIZE * SIZE * 4)  # transparent black


def blend(buf: bytearray, x: int, y: int, color, alpha: float) -> None:
    """Source-over one pixel; color is linear RGB in 0..1, alpha in 0..1."""
    if x < 0 or y < 0 or x >= SIZE or y >= SIZE or alpha <= 0.0:
        return
    i = (y * SIZE + x) * 4
    da = buf[i + 3] / 255.0
    out_a = alpha + da * (1.0 - alpha)
    if out_a <= 0.0:
        return
    for k in range(3):
        d = buf[i + k] / 255.0
        s = color[k]
        o = (s * alpha + d * da * (1.0 - alpha)) / out_a
        buf[i + k] = max(0, min(255, int(o * 255.0 + 0.5)))
    buf[i + 3] = max(0, min(255, int(out_a * 255.0 + 0.5)))


def fill_rect(buf, x0, y0, x1, y1, color, alpha=1.0) -> None:
    for py in range(int(y0), int(y1)):
        for px in range(int(x0), int(x1)):
            blend(buf, px, py, color, alpha)


def fill_tri(buf, p0, p1, p2, color, alpha=1.0) -> None:
    (x0, y0), (x1, y1), (x2, y2) = p0, p1, p2
    minx = max(0, int(math.floor(min(x0, x1, x2))))
    maxx = min(SIZE - 1, int(math.ceil(max(x0, x1, x2))))
    miny = max(0, int(math.floor(min(y0, y1, y2))))
    maxy = min(SIZE - 1, int(math.ceil(max(y0, y1, y2))))
    denom = (y1 - y2) * (x0 - x2) + (x2 - x1) * (y0 - y2)
    if abs(denom) < 1e-9:
        return
    for py in range(miny, maxy + 1):
        for px in range(minx, maxx + 1):
            cx, cy = px + 0.5, py + 0.5
            a = ((y1 - y2) * (cx - x2) + (x2 - x1) * (cy - y2)) / denom
            b = ((y2 - y0) * (cx - x2) + (x0 - x2) * (cy - y2)) / denom
            c = 1.0 - a - b
            if a >= -0.001 and b >= -0.001 and c >= -0.001:
                blend(buf, px, py, color, alpha)


def write_png(path: Path, buf: bytearray) -> None:
    def chunk(tag: bytes, data: bytes) -> bytes:
        return (struct.pack(">I", len(data)) + tag + data
                + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF))

    raw = bytearray()
    for y in range(SIZE):
        raw.append(0)  # filter type 0 (None)
        raw.extend(buf[y * SIZE * 4:(y + 1) * SIZE * 4])
    ihdr = struct.pack(">IIBBBBB", SIZE, SIZE, 8, 6, 0, 0, 0)  # 8-bit RGBA
    png = (b"\x89PNG\r\n\x1a\n"
           + chunk(b"IHDR", ihdr)
           + chunk(b"IDAT", zlib.compress(bytes(raw), 9))
           + chunk(b"IEND", b""))
    path.write_bytes(png)


# --------------------------------------------------------------------------- #
# Palette — mid-tone so every swatch reads on both the light and dark theme.
# --------------------------------------------------------------------------- #

ASPHALT = (0.27, 0.28, 0.30)
WORN_ASPHALT = (0.36, 0.35, 0.33)  # lighter, browner — sun-bleached and dusty
CONCRETE = (0.60, 0.60, 0.58)
WHITE_PAINT = (0.93, 0.93, 0.91)
YELLOW_PAINT = (0.91, 0.73, 0.17)
GRASS = (0.30, 0.45, 0.22)
CHIP_A = (0.86, 0.42, 0.30)  # library-brand coral accent
CHIP_B = (0.35, 0.55, 0.72)


# --------------------------------------------------------------------------- #
# Prop / signal previews — rasterise the embedded mesh geometry.
# --------------------------------------------------------------------------- #

def _norm3(v):
    length = math.sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]) or 1.0
    return (v[0] / length, v[1] / length, v[2] / length)


def render_model(model: dict) -> bytearray:
    """Orthographic three-quarter view of a bundled prop/signal model."""
    buf = new_canvas()
    yaw, pitch = math.radians(35.0), math.radians(28.0)
    cz, sz = math.cos(yaw), math.sin(yaw)
    cp, sp = math.cos(pitch), math.sin(pitch)

    def project(p):
        # Yaw about Z (up), then pitch about the screen X axis (look down).
        ax = p[0] * cz - p[1] * sz
        ay = p[0] * sz + p[1] * cz
        az = p[2]
        return (ax, ay * sp + az * cp, ay * cp - az * sp)  # (sx, sy_up, depth)

    light = _norm3((0.35, -0.5, 0.8))  # from front-left, above
    tris = []
    for _name, color, triangles in model["parts"]:
        for t in triangles:
            n = props._face_normal(t)
            lam = max(0.0, n[0] * light[0] + n[1] * light[1] + n[2] * light[2])
            shade = 0.35 + 0.65 * lam
            col = tuple(min(1.0, c * shade) for c in color)
            proj = [project(v) for v in t]
            tris.append((sum(p[2] for p in proj) / 3.0, proj, col))

    xs = [p[0] for _, proj, _ in tris for p in proj]
    ys = [p[1] for _, proj, _ in tris for p in proj]
    minx, maxx, miny, maxy = min(xs), max(xs), min(ys), max(ys)
    margin = 9.0
    span = max(maxx - minx, maxy - miny) or 1.0
    scale = (SIZE - 2.0 * margin) / span
    width_p = (maxx - minx) * scale
    height_p = (maxy - miny) * scale
    ox = (SIZE - width_p) / 2.0 - minx * scale
    oy = (SIZE - height_p) / 2.0

    def to_pixel(sx, sy):
        return (ox + sx * scale, oy + (maxy - sy) * scale)  # sy up -> py down

    for _depth, proj, col in sorted(tris, key=lambda e: e[0], reverse=True):
        fill_tri(buf, to_pixel(*proj[0][:2]), to_pixel(*proj[1][:2]),
                 to_pixel(*proj[2][:2]), col)
    return buf


# --------------------------------------------------------------------------- #
# Top-down swatches.
# --------------------------------------------------------------------------- #

def _dashes(buf, x, y0, y1, w, color, dash=7, gap=6):
    y = y0
    while y < y1:
        fill_rect(buf, x - w / 2, y, x + w / 2, min(y + dash, y1), color)
        y += dash + gap


def road_rural() -> bytearray:
    buf = new_canvas()
    fill_rect(buf, 28, 6, 68, 90, ASPHALT)
    fill_rect(buf, 30, 6, 32, 90, WHITE_PAINT)   # left edge line
    fill_rect(buf, 64, 6, 66, 90, WHITE_PAINT)   # right edge line
    _dashes(buf, 48, 6, 90, 2.4, YELLOW_PAINT)   # dashed centre line
    return buf


def road_urban() -> bytearray:
    buf = new_canvas()
    fill_rect(buf, 12, 6, 22, 90, CONCRETE)      # left sidewalk
    fill_rect(buf, 74, 6, 84, 90, CONCRETE)      # right sidewalk
    fill_rect(buf, 24, 6, 72, 90, ASPHALT)
    fill_rect(buf, 26, 6, 28, 90, WHITE_PAINT)
    fill_rect(buf, 68, 6, 70, 90, WHITE_PAINT)
    _dashes(buf, 48, 6, 90, 2.4, WHITE_PAINT)
    return buf


def road_highway() -> bytearray:
    buf = new_canvas()
    fill_rect(buf, 18, 6, 44, 90, ASPHALT)       # carriageway A
    fill_rect(buf, 52, 6, 78, 90, ASPHALT)       # carriageway B
    fill_rect(buf, 44, 6, 52, 90, GRASS)         # median
    for x in (31, 65):
        _dashes(buf, x, 6, 90, 2.0, WHITE_PAINT)
    fill_rect(buf, 18, 6, 20, 90, WHITE_PAINT)
    fill_rect(buf, 76, 6, 78, 90, WHITE_PAINT)
    return buf


def style_urban() -> bytearray:
    buf = road_urban()
    # Accent chips mark this as a saved *style*, not a bare template.
    fill_rect(buf, 62, 10, 78, 26, CHIP_A)
    fill_rect(buf, 62, 30, 78, 46, CHIP_B)
    return buf


def assembly_t() -> bytearray:
    buf = new_canvas()
    fill_rect(buf, 8, 12, 88, 40, ASPHALT)       # crossing arm (top)
    fill_rect(buf, 34, 12, 62, 90, ASPHALT)      # stem (down)
    return buf


def assembly_x() -> bytearray:
    buf = new_canvas()
    fill_rect(buf, 8, 34, 88, 62, ASPHALT)       # horizontal arm
    fill_rect(buf, 34, 8, 62, 88, ASPHALT)       # vertical arm
    return buf


def marking_swatch(stripes, color, half_width=3) -> bytearray:
    """Asphalt background with one or more vertical paint stripes.

    Each stripe is (cx, dashed): a solid column, or a dashed one drawn as
    segmented rects (3 m paint / 6 m gap, scaled). `half_width` widens every
    stripe for the wide edge line.
    """
    buf = new_canvas()
    fill_rect(buf, 10, 10, 86, 86, ASPHALT)
    for cx, dashed in stripes:
        if dashed:
            y = 14
            while y < 82:
                fill_rect(buf, cx - half_width, y, cx + half_width, min(y + 10, 82), color)
                y += 24  # 10 px paint + 14 px gap, echoing the 3 m / 6 m cadence
        else:
            fill_rect(buf, cx - half_width, 14, cx + half_width, 82, color)
    return buf


def material_swatch(base) -> bytearray:
    """A paved tile with a light deterministic speckle so it reads as texture."""
    buf = new_canvas()
    fill_rect(buf, 8, 8, 88, 88, base)
    rng = random.Random(0x5EED)
    for _ in range(260):
        px = rng.randint(10, 85)
        py = rng.randint(10, 85)
        d = rng.uniform(-0.06, 0.06)
        blend(buf, px, py, tuple(min(1.0, max(0.0, c + d)) for c in base), 0.5)
    return buf


# --------------------------------------------------------------------------- #
# Manifest name -> generator.
# --------------------------------------------------------------------------- #

def _model(model_id: str):
    for m in props.MODELS:
        if m["id"] == model_id:
            return m
    raise KeyError(model_id)


THUMBNAILS = {
    # Props + signals: real previews of the embedded geometry.
    "prop_tree_pine": lambda: render_model(_model("tree_pine")),
    "prop_tree_oak": lambda: render_model(_model("tree_oak")),
    "prop_tree_birch": lambda: render_model(_model("tree_birch")),
    "prop_tree_poplar": lambda: render_model(_model("tree_poplar")),
    "prop_shrub": lambda: render_model(_model("shrub")),
    "signal_traffic_light": lambda: render_model(_model("signal_light")),
    "signal_sign": lambda: render_model(_model("sign_generic")),
    "sign_stop": lambda: render_model(_model("sign_stop")),
    "sign_yield": lambda: render_model(_model("sign_yield")),
    "sign_text": lambda: render_model(_model("sign_plate")),
    "streetlight_single": lambda: render_model(_model("streetlight_single")),
    "streetlight_double": lambda: render_model(_model("streetlight_double")),
    "building_low": lambda: render_model(_model("building_low")),
    "building_mid": lambda: render_model(_model("building_mid")),
    "building_tower": lambda: render_model(_model("building_tower")),
    # Stylised swatches.
    "road_rural": road_rural,
    "road_urban": road_urban,
    "road_highway": road_highway,
    "style_urban": style_urban,
    "assembly_t": assembly_t,
    "assembly_x": assembly_x,
    "marking_solid_white": lambda: marking_swatch([(48, False)], WHITE_PAINT),
    "marking_double_yellow": lambda: marking_swatch([(42, False), (54, False)], YELLOW_PAINT),
    "marking_dashed_white": lambda: marking_swatch([(48, True)], WHITE_PAINT),
    "marking_dashed_yellow": lambda: marking_swatch([(48, True)], YELLOW_PAINT),
    "marking_double_white": lambda: marking_swatch([(42, False), (54, False)], WHITE_PAINT),
    "marking_solid_broken_yellow": lambda: marking_swatch([(42, False), (54, True)], YELLOW_PAINT),
    "marking_broken_solid_yellow": lambda: marking_swatch([(42, True), (54, False)], YELLOW_PAINT),
    "marking_double_dashed_yellow": lambda: marking_swatch([(42, True), (54, True)], YELLOW_PAINT),
    "marking_edge_white": lambda: marking_swatch([(48, False)], WHITE_PAINT, half_width=6),
    "material_asphalt": lambda: material_swatch(ASPHALT),
    "material_asphalt_worn": lambda: material_swatch(WORN_ASPHALT),
    "material_concrete": lambda: material_swatch(CONCRETE),
    "material_paint_white": lambda: material_swatch(WHITE_PAINT),
    "material_paint_yellow": lambda: material_swatch(YELLOW_PAINT),
}


def main() -> int:
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    for name, make in THUMBNAILS.items():
        write_png(OUT_DIR / f"{name}.png", make())
    print(f"[gen_library_thumbnails] wrote {len(THUMBNAILS)} thumbnails -> {OUT_DIR}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
