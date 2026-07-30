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

"""Generates the committed glTF fixtures for the prop importer (p6-s8, #322).

A reader checked only against bytes the same test just built agrees with itself
about everything, including its own mistakes. So the fixtures are real files, on
disk, in the repository -- the sibling of gen_gis_fixtures.py and
gen_lidar_fixtures.py, and deliberately in their shape.

STANDARD LIBRARY ALONE: no glTF writer, no image library. A GLB is a 12-byte
header plus two length-prefixed chunks, and a PNG is four length-prefixed chunks
around a zlib stream, so both fit in a page of code. That also keeps the
fixtures' provenance unambiguously ours, with no asset-licence question.

GEOMETRY IS DELIBERATELY OFF-ORIGIN AND ASYMMETRIC. The importer's job includes
rotating Y-up to Z-up and re-seating the model on its own base, and neither
mistake can hide on a unit cube at the origin: a 2x2x2 box centred at
(5, 6, 10) in glTF space lands at (5, -10, 6) in kernel space, so a sign error
in the frame conversion moves it somewhere a test can name. The boxes are also
1x2x3 rather than cubes, so a transposed axis is visible in the height.

THE MALFORMED FIXTURES ARE THE POINT of half this file. #322's acceptance asks
for fuzz-adjacent coverage, and these are the cases a hostile or truncated file
actually presents: a lying chunk length, an index past the vertex array, a
cyclic node graph, an image uri climbing out of the model's directory.

Run:  python3 scripts/gen_gltf_fixtures.py
"""

from __future__ import annotations

import json
import math
import struct
import zlib
from pathlib import Path

OUT_DIR = Path(__file__).resolve().parent.parent / "core" / "tests" / "data" / "gltf"

# glTF component and type constants, spelled out so this script needs no import.
FLOAT = 5126
UNSIGNED_SHORT = 5123
UNSIGNED_BYTE = 5121
UNSIGNED_INT = 5125
ARRAY_BUFFER = 34962
ELEMENT_ARRAY_BUFFER = 34963
MODE_TRIANGLES = 4
MODE_LINES = 1

GLB_MAGIC = 0x46546C67
CHUNK_JSON = 0x4E4F534A
CHUNK_BIN = 0x004E4942


# --------------------------------------------------------------------------- #
# PNG writing (stdlib zlib only)
# --------------------------------------------------------------------------- #


def _png_chunk(tag: bytes, payload: bytes) -> bytes:
    return (struct.pack(">I", len(payload)) + tag + payload
            + struct.pack(">I", zlib.crc32(tag + payload) & 0xFFFFFFFF))


def write_png(width: int, height: int, rgb: tuple[int, int, int]) -> bytes:
    """A flat RGB8 PNG. Flat because the importer only ever averages it, so a
    single known colour makes the expected average exact rather than
    approximate -- a gradient would leave the test asserting arithmetic it had
    to redo itself."""
    raw = b"".join(b"\x00" + bytes(rgb) * width for _ in range(height))
    return (b"\x89PNG\r\n\x1a\n"
            + _png_chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
            + _png_chunk(b"IDAT", zlib.compress(raw, 9))
            + _png_chunk(b"IEND", b""))


# --------------------------------------------------------------------------- #
# glTF assembly
# --------------------------------------------------------------------------- #


class Builder:
    """Accumulates a binary buffer and the accessors that address it."""

    def __init__(self) -> None:
        self.blob = bytearray()
        self.views: list[dict] = []
        self.accessors: list[dict] = []

    def _view(self, data: bytes, target: int) -> int:
        while len(self.blob) % 4:
            self.blob.append(0)
        offset = len(self.blob)
        self.blob.extend(data)
        self.views.append({"buffer": 0, "byteOffset": offset,
                           "byteLength": len(data), "target": target})
        return len(self.views) - 1

    def vec3(self, values: list[tuple[float, float, float]]) -> int:
        flat = [c for v in values for c in v]
        view = self._view(struct.pack(f"<{len(flat)}f", *flat), ARRAY_BUFFER)
        accessor = {"bufferView": view, "componentType": FLOAT,
                    "count": len(values), "type": "VEC3"}
        # POSITION requires min/max per spec; some readers rely on it. But a
        # non-finite value would be written as the bare literal `NaN`, which is
        # not valid JSON -- so the nan_position fixture would be rejected by the
        # JSON parser and never reach the importer's own non-finite check. (It
        # was, until the malformed-file assertions printed the reason they were
        # actually getting.) Omitting min/max keeps the NaN in the BINARY buffer,
        # where a broken exporter really puts it.
        if all(math.isfinite(c) for c in flat):
            cols = list(zip(*values))
            accessor["min"] = [min(c) for c in cols]
            accessor["max"] = [max(c) for c in cols]
        self.accessors.append(accessor)
        return len(self.accessors) - 1

    def vec2(self, values: list[tuple[float, float]]) -> int:
        flat = [c for v in values for c in v]
        view = self._view(struct.pack(f"<{len(flat)}f", *flat), ARRAY_BUFFER)
        self.accessors.append({"bufferView": view, "componentType": FLOAT,
                               "count": len(values), "type": "VEC2"})
        return len(self.accessors) - 1

    def indices(self, values: list[int], component: int = UNSIGNED_SHORT) -> int:
        fmt = {UNSIGNED_BYTE: "B", UNSIGNED_SHORT: "H", UNSIGNED_INT: "I"}[component]
        data = struct.pack(f"<{len(values)}{fmt}", *values)
        view = self._view(data, ELEMENT_ARRAY_BUFFER)
        self.accessors.append({"bufferView": view, "componentType": component,
                               "count": len(values), "type": "SCALAR"})
        return len(self.accessors) - 1

    def image_from_bytes(self, png: bytes, mime: str = "image/png") -> int:
        view = self._view(png, 0)
        self.views[view].pop("target")
        return view


def box(size: tuple[float, float, float],
        centre: tuple[float, float, float]) -> tuple[list, list, list, list]:
    """A closed box with per-face vertices, so every face has a real normal.

    Returns (positions, normals, uvs, indices). Winding is counter-clockwise
    viewed from outside, matching what the kernel's own prop meshes promise."""
    hx, hy, hz = (s * 0.5 for s in size)
    cx, cy, cz = centre
    faces = [
        ((0, 0, 1), [(-hx, -hy, hz), (hx, -hy, hz), (hx, hy, hz), (-hx, hy, hz)]),
        ((0, 0, -1), [(hx, -hy, -hz), (-hx, -hy, -hz), (-hx, hy, -hz), (hx, hy, -hz)]),
        ((1, 0, 0), [(hx, -hy, hz), (hx, -hy, -hz), (hx, hy, -hz), (hx, hy, hz)]),
        ((-1, 0, 0), [(-hx, -hy, -hz), (-hx, -hy, hz), (-hx, hy, hz), (-hx, hy, -hz)]),
        ((0, 1, 0), [(-hx, hy, hz), (hx, hy, hz), (hx, hy, -hz), (-hx, hy, -hz)]),
        ((0, -1, 0), [(-hx, -hy, -hz), (hx, -hy, -hz), (hx, -hy, hz), (-hx, -hy, hz)]),
    ]
    positions: list[tuple[float, float, float]] = []
    normals: list[tuple[float, float, float]] = []
    uvs: list[tuple[float, float]] = []
    indices: list[int] = []
    for normal, corners in faces:
        base = len(positions)
        for corner in corners:
            positions.append((corner[0] + cx, corner[1] + cy, corner[2] + cz))
            normals.append(normal)
        uvs.extend([(0.0, 0.0), (1.0, 0.0), (1.0, 1.0), (0.0, 1.0)])
        indices.extend([base, base + 1, base + 2, base, base + 2, base + 3])
    return positions, normals, uvs, indices


def assemble(builder: Builder, meshes: list[dict], *, nodes=None, scenes=None,
             materials=None, images=None, textures=None, extras=None) -> dict:
    doc = {
        "asset": {"version": "2.0", "generator": "RoadMaker gen_gltf_fixtures.py"},
        "meshes": meshes,
        "accessors": builder.accessors,
        "bufferViews": builder.views,
        "buffers": [{"byteLength": len(builder.blob)}],
    }
    doc["nodes"] = nodes if nodes is not None else [{"mesh": 0}]
    doc["scenes"] = scenes if scenes is not None else [{"nodes": [0]}]
    doc["scene"] = 0
    if materials:
        doc["materials"] = materials
    if images:
        doc["images"] = images
    if textures:
        doc["textures"] = textures
    if extras:
        doc.update(extras)
    return doc


def glb_bytes(doc: dict, blob: bytes) -> bytes:
    js = json.dumps(doc, separators=(",", ":")).encode("utf-8")
    js += b" " * ((4 - len(js) % 4) % 4)
    bin_chunk = bytes(blob) + b"\x00" * ((4 - len(blob) % 4) % 4)
    total = 12 + 8 + len(js) + (8 + len(bin_chunk) if bin_chunk else 0)
    out = struct.pack("<III", GLB_MAGIC, 2, total)
    out += struct.pack("<II", len(js), CHUNK_JSON) + js
    if bin_chunk:
        out += struct.pack("<II", len(bin_chunk), CHUNK_BIN) + bin_chunk
    return out


def write(name: str, data: bytes) -> None:
    (OUT_DIR / name).write_bytes(data)
    print(f"[gen_gltf_fixtures] {name} ({len(data)} bytes)")


# --------------------------------------------------------------------------- #
# The fixtures
# --------------------------------------------------------------------------- #

# The one geometry every well-formed fixture shares, so a test can assert the
# same normalised extents no matter how the file wrapped it.
BOX_SIZE = (1.0, 2.0, 3.0)          # glTF axes: x right, y UP, z toward viewer
BOX_CENTRE = (5.0, 6.0, 10.0)       # off-origin so a lost translation shows up
# In the kernel frame (x, -z, y) that box is 1 wide, 3 deep, 2 TALL -- the height
# is the glTF Y extent, which is the whole point of the conversion.
TEXTURE_RGB = (204, 102, 51)        # a colour whose linear average is not 0.5


def well_formed() -> None:
    # 1. A textured box in a GLB: the mainline case, and the one that exercises
    #    the flatten-to-average-colour decision (ADR-0013).
    b = Builder()
    pos, nrm, uv, idx = box(BOX_SIZE, BOX_CENTRE)
    p, n, t, i = b.vec3(pos), b.vec3(nrm), b.vec2(uv), b.indices(idx)
    image_view = b.image_from_bytes(write_png(8, 8, TEXTURE_RGB))
    doc = assemble(
        b,
        [{"name": "crate", "primitives": [{
            "attributes": {"POSITION": p, "NORMAL": n, "TEXCOORD_0": t},
            "indices": i, "material": 0, "mode": MODE_TRIANGLES}]}],
        materials=[{"name": "crate_paint", "pbrMetallicRoughness": {
            "baseColorFactor": [1.0, 1.0, 1.0, 1.0],
            "baseColorTexture": {"index": 0}}}],
        images=[{"bufferView": image_view, "mimeType": "image/png"}],
        textures=[{"source": 0}],
    )
    write("textured_box.glb", glb_bytes(doc, b.blob))

    # 2. The same box with a plain baseColorFactor and no texture at all, so a
    #    test can tell "flattened a texture" from "read a factor".
    b = Builder()
    p, n, i = b.vec3(pos), b.vec3(nrm), b.indices(idx)
    doc = assemble(
        b,
        [{"name": "plain", "primitives": [{
            "attributes": {"POSITION": p, "NORMAL": n},
            "indices": i, "material": 0, "mode": MODE_TRIANGLES}]}],
        materials=[{"name": "plain_paint", "pbrMetallicRoughness": {
            "baseColorFactor": [0.25, 0.5, 0.75, 1.0]}}],
    )
    write("factor_box.glb", glb_bytes(doc, b.blob))

    # 3. A .gltf with an EXTERNAL image beside it: the path tinygltf refuses to
    #    walk (TINYGLTF_NO_EXTERNAL_IMAGE), so the reader resolves it itself.
    b = Builder()
    p, n, t, i = b.vec3(pos), b.vec3(nrm), b.vec2(uv), b.indices(idx)
    doc = assemble(
        b,
        [{"name": "external", "primitives": [{
            "attributes": {"POSITION": p, "NORMAL": n, "TEXCOORD_0": t},
            "indices": i, "material": 0, "mode": MODE_TRIANGLES}]}],
        materials=[{"name": "external_paint", "pbrMetallicRoughness": {
            "baseColorFactor": [1.0, 1.0, 1.0, 1.0],
            "baseColorTexture": {"index": 0}}}],
        images=[{"uri": "external_albedo.png"}],
        textures=[{"source": 0}],
    )
    doc["buffers"] = [{"byteLength": len(b.blob), "uri": "external_box.bin"}]
    write("external_box.gltf", json.dumps(doc, indent=1).encode("utf-8"))
    write("external_box.bin", bytes(b.blob))
    write("external_albedo.png", write_png(4, 4, TEXTURE_RGB))

    # 4. A nested node hierarchy whose composed transform reproduces the same
    #    world box from a UNIT box at the origin. If transform flattening is
    #    wrong, this fixture's extents differ from every other fixture's, which
    #    is exactly the assertion a test can make.
    b = Builder()
    unit_pos, unit_nrm, _, unit_idx = box((1.0, 1.0, 1.0), (0.0, 0.0, 0.0))
    p, n, i = b.vec3(unit_pos), b.vec3(unit_nrm), b.indices(unit_idx)
    doc = assemble(
        b,
        [{"name": "unit", "primitives": [{
            "attributes": {"POSITION": p, "NORMAL": n},
            "indices": i, "mode": MODE_TRIANGLES}]}],
        # root translates, child scales -- so the composition order matters.
        nodes=[{"name": "root", "translation": list(BOX_CENTRE), "children": [1]},
               {"name": "leaf", "scale": list(BOX_SIZE), "mesh": 0}],
        scenes=[{"nodes": [0]}],
    )
    write("nested_box.glb", glb_bytes(doc, b.blob))

    # 5. No NORMAL attribute: the reader must compute them, or the prop renders
    #    black. Also uses UNSIGNED_BYTE indices, the narrowest encoding.
    b = Builder()
    small_pos, _, _, small_idx = box(BOX_SIZE, BOX_CENTRE)
    p = b.vec3(small_pos)
    i = b.indices(small_idx, UNSIGNED_BYTE)
    doc = assemble(b, [{"name": "no_normals", "primitives": [{
        "attributes": {"POSITION": p}, "indices": i, "mode": MODE_TRIANGLES}]}])
    write("no_normals_box.glb", glb_bytes(doc, b.blob))

    # 6. A NON-INDEXED draw: vertices already in triangle order.
    b = Builder()
    expanded = [small_pos[k] for k in small_idx]
    p = b.vec3(expanded)
    doc = assemble(b, [{"name": "nonindexed", "primitives": [{
        "attributes": {"POSITION": p}, "mode": MODE_TRIANGLES}]}])
    write("nonindexed_box.glb", glb_bytes(doc, b.blob))

    # 7. Two primitives -> two parts, with a LINES primitive between them that
    #    must be skipped by name rather than silently, and a mesh-only file with
    #    no nodes at all (legal, and some exporters emit it).
    b = Builder()
    p, n, i = b.vec3(pos), b.vec3(nrm), b.indices(idx)
    doc = assemble(
        b,
        [{"name": "mixed", "primitives": [
            {"attributes": {"POSITION": p, "NORMAL": n}, "indices": i,
             "material": 0, "mode": MODE_TRIANGLES},
            {"attributes": {"POSITION": p}, "indices": i, "mode": MODE_LINES},
            {"attributes": {"POSITION": p, "NORMAL": n}, "indices": i,
             "material": 1, "mode": MODE_TRIANGLES},
        ]}],
        materials=[{"name": "first", "pbrMetallicRoughness": {
                       "baseColorFactor": [1.0, 0.0, 0.0, 1.0]}},
                   {"name": "second", "pbrMetallicRoughness": {
                       "baseColorFactor": [0.0, 0.0, 1.0, 1.0]}}],
    )
    write("two_parts_and_lines.glb", glb_bytes(doc, b.blob))


def asymmetric() -> None:
    """The fixture that pins the frame conversion, and the reason it exists.

    EVERY BUNDLED PROP MODEL IS SYMMETRIC ABOUT ITS Y AXIS. So a round-trip
    through export_glb and back -- which is otherwise the best oracle available,
    because it checks the importer against the exporter rather than against
    arithmetic done twice -- CANNOT catch a sign error on the axis the conversion
    negates: reflecting a y-symmetric model leaves it unchanged. That is a
    vacuous test waiting to happen.

    This tetrahedron is asymmetric on all three axes, and its expected kernel
    coordinates are short enough to write out by hand in the test from the
    spec's (x, y, z) -> (x, -z, y), which is what makes them an independent
    check rather than a restatement of the code."""
    b = Builder()
    positions = [(0.0, 0.0, 0.0), (3.0, 0.0, 0.0), (0.0, 5.0, 0.0), (0.0, 0.0, 7.0)]
    indices = [0, 2, 1, 0, 3, 2, 0, 1, 3, 1, 2, 3]
    p = b.vec3(positions)
    i = b.indices(indices)
    doc = assemble(b, [{"name": "wedge", "primitives": [{
        "attributes": {"POSITION": p}, "indices": i, "mode": MODE_TRIANGLES}]}])
    write("asymmetric_wedge.glb", glb_bytes(doc, b.blob))


def malformed() -> None:
    b = Builder()
    pos, nrm, _, idx = box(BOX_SIZE, BOX_CENTRE)
    p, n, i = b.vec3(pos), b.vec3(nrm), b.indices(idx)
    good_doc = assemble(b, [{"name": "box", "primitives": [{
        "attributes": {"POSITION": p, "NORMAL": n}, "indices": i,
        "mode": MODE_TRIANGLES}]}])
    good = glb_bytes(good_doc, b.blob)

    # Truncated mid-BIN-chunk: the header promises more than the file holds.
    write("truncated.glb", good[: len(good) // 2])

    # Wrong magic: a PNG renamed .glb, which is what a mis-drag produces.
    write("bad_magic.glb", write_png(4, 4, (0, 0, 0)))

    # A JSON chunk length far past the end of the file.
    lying = bytearray(good)
    struct.pack_into("<I", lying, 12, 0x00FF_FFFF)
    write("lying_chunk_length.glb", bytes(lying))

    # An index pointing past the vertex array -- the classic case, and the one
    # that reads out of bounds if the importer trusts it.
    b = Builder()
    p, n = b.vec3(pos), b.vec3(nrm)
    i = b.indices([0, 1, 9999] + idx[3:])
    doc = assemble(b, [{"name": "bad_index", "primitives": [{
        "attributes": {"POSITION": p, "NORMAL": n}, "indices": i,
        "mode": MODE_TRIANGLES}]}])
    write("index_out_of_range.glb", glb_bytes(doc, b.blob))

    # A non-finite vertex. NaN survives a float round-trip, so this is a real
    # file a broken exporter can write, not a synthetic impossibility.
    b = Builder()
    nan_pos = [(float("nan"), 0.0, 0.0)] + list(pos[1:])
    p, n = b.vec3(nan_pos), b.vec3(nrm)
    i = b.indices(idx)
    doc = assemble(b, [{"name": "nan", "primitives": [{
        "attributes": {"POSITION": p, "NORMAL": n}, "indices": i,
        "mode": MODE_TRIANGLES}]}])
    # json.dumps writes NaN unquoted by default, which is invalid JSON; the
    # values live in the BINARY buffer here, so the document stays clean.
    write("nan_position.glb", glb_bytes(doc, b.blob))

    # A cyclic node graph. The spec says the nodes form a forest; a reader that
    # believes it recurses forever.
    b = Builder()
    p, n, i = b.vec3(pos), b.vec3(nrm), b.indices(idx)
    doc = assemble(
        b,
        [{"name": "box", "primitives": [{
            "attributes": {"POSITION": p, "NORMAL": n}, "indices": i,
            "mode": MODE_TRIANGLES}]}],
        nodes=[{"name": "a", "children": [1]},
               {"name": "b", "children": [0], "mesh": 0}],
        scenes=[{"nodes": [0]}],
    )
    # A .gltf must name its buffer, or tinygltf refuses the document before the
    # node walk ever runs -- which would make this fixture test the buffer check
    # instead of the cycle check, and leave the cycle guard unexercised. (It did,
    # until the assertion printed the message it was actually getting.)
    doc["buffers"] = [{"byteLength": len(b.blob), "uri": "cyclic_nodes.bin"}]
    write("cyclic_nodes.gltf", json.dumps(doc, indent=1).encode("utf-8"))
    write("cyclic_nodes.bin", bytes(b.blob))

    # An image uri climbing out of the model's own directory. Nothing but the
    # importer's own check stands between this and the filesystem, because
    # TINYGLTF_NO_EXTERNAL_IMAGE means tinygltf never touches it (ADR-0013).
    b = Builder()
    p, n, i = b.vec3(pos), b.vec3(nrm), b.indices(idx)
    doc = assemble(
        b,
        [{"name": "box", "primitives": [{
            "attributes": {"POSITION": p, "NORMAL": n}, "indices": i,
            "material": 0, "mode": MODE_TRIANGLES}]}],
        materials=[{"name": "escape", "pbrMetallicRoughness": {
            "baseColorFactor": [1.0, 1.0, 1.0, 1.0],
            "baseColorTexture": {"index": 0}}}],
        images=[{"uri": "../../../../../../etc/passwd"}],
        textures=[{"source": 0}],
    )
    doc["buffers"] = [{"byteLength": len(b.blob), "uri": "path_traversal.bin"}]
    write("path_traversal.gltf", json.dumps(doc, indent=1).encode("utf-8"))
    write("path_traversal.bin", bytes(b.blob))

    # Geometry-free: a valid document with nothing to make a prop out of.
    b = Builder()
    doc = {
        "asset": {"version": "2.0"},
        "scenes": [{"nodes": []}],
        "scene": 0,
        "nodes": [],
    }
    write("no_geometry.gltf", json.dumps(doc, indent=1).encode("utf-8"))

    # A flat model: every vertex at z = 0 in kernel space, so height is zero and
    # per-instance scaling would divide by it.
    b = Builder()
    flat_pos, flat_nrm, _, flat_idx = box((2.0, 0.0, 2.0), (0.0, 0.0, 0.0))
    p, n, i = b.vec3(flat_pos), b.vec3(flat_nrm), b.indices(flat_idx)
    doc = assemble(b, [{"name": "flat", "primitives": [{
        "attributes": {"POSITION": p, "NORMAL": n}, "indices": i,
        "mode": MODE_TRIANGLES}]}])
    write("zero_height.glb", glb_bytes(doc, b.blob))


def main() -> int:
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    well_formed()
    asymmetric()
    malformed()
    print(f"[gen_gltf_fixtures] wrote to {OUT_DIR}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
