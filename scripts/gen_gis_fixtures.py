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

"""Generates the committed GIS test fixtures in core/tests/data/gis/.

WHY THESE FILES ARE COMMITTED, AND WHY A SCRIPT MAKES THEM.

Twice now a feature has shipped with tests that would have passed on a build
that dropped the feature entirely, because no committed fixture exercised it:
p7-s5 (#324) shipped with no `.xodr` carrying a `<geoReference>`, and #390 with
no sample carrying `rm:surface`/`rm:terrain`. Reading a file format is exactly
the place that trap bites hardest — a reader tested only against bytes the same
test just built agrees with itself about everything, including its mistakes.

So the fixtures are real files, on disk, in the repository. They are generated
rather than downloaded so their provenance is unambiguously ours (no asset
licence question), and they are generated with the standard library alone so
this script needs no GIS stack to run — which is the same reason the kernel
does not have one.

Run:  python3 scripts/gen_gis_fixtures.py
"""

from __future__ import annotations

import json
import pathlib
import struct
import zlib

OUT = pathlib.Path(__file__).resolve().parent.parent / "core" / "tests" / "data" / "gis"

# A small area near Amsterdam: UTM zone 31N, which is EPSG:32631. Chosen
# because it is a REAL place in a REAL zone — a fixture at (0, 0) would hide
# every false-easting and central-meridian mistake there is.
UTM_ORIGIN_E = 628000.0
UTM_ORIGIN_N = 5804000.0
PIXEL_SIZE = 10.0

PRJ_UTM31N = (
    'PROJCS["WGS_1984_UTM_Zone_31N",GEOGCS["GCS_WGS_1984",'
    'DATUM["D_WGS_1984",SPHEROID["WGS_1984",6378137.0,298.257223563]],'
    'PRIMEM["Greenwich",0.0],UNIT["Degree",0.0174532925199433]],'
    'PROJECTION["Transverse_Mercator"],PARAMETER["False_Easting",500000.0],'
    'PARAMETER["False_Northing",0.0],PARAMETER["Central_Meridian",3.0],'
    'PARAMETER["Scale_Factor",0.9996],PARAMETER["Latitude_Of_Origin",0.0],'
    'UNIT["Meter",1.0]]'
)

# A CRS outside the supported family: a Lambert Conformal Conic on NAD27.
# Both halves are out of family — the projection AND the datum — which is what
# makes it a clean refusal fixture rather than an accidental near-miss.
PRJ_UNSUPPORTED = (
    'PROJCS["NAD27_Texas_North",GEOGCS["GCS_North_American_1927",'
    'DATUM["D_North_American_1927",SPHEROID["Clarke_1866",6378206.4,294.9786982]],'
    'PRIMEM["Greenwich",0.0],UNIT["Degree",0.0174532925199433]],'
    'PROJECTION["Lambert_Conformal_Conic"],PARAMETER["False_Easting",609601.2192],'
    'PARAMETER["Central_Meridian",-101.5],UNIT["Meter",1.0]]'
)


# --------------------------------------------------------------------------
# TIFF writing
# --------------------------------------------------------------------------

TYPE_BYTE, TYPE_ASCII, TYPE_SHORT, TYPE_LONG, TYPE_DOUBLE = 1, 2, 3, 4, 12
TYPE_SIZE = {TYPE_BYTE: 1, TYPE_ASCII: 1, TYPE_SHORT: 2, TYPE_LONG: 4, TYPE_DOUBLE: 8}
PACK = {TYPE_BYTE: "B", TYPE_ASCII: "c", TYPE_SHORT: "H", TYPE_LONG: "I", TYPE_DOUBLE: "d"}


def _pack_values(kind: int, values) -> bytes:
    if kind == TYPE_ASCII:
        return values.encode("ascii") + b"\0"
    return b"".join(struct.pack("<" + PACK[kind], v) for v in values)


def write_tiff(path: pathlib.Path, tags: dict[int, tuple[int, list]], strip: bytes) -> None:
    """Writes a little-endian TIFF with one strip.

    The IFD is emitted with entries sorted by tag, as the specification
    requires. libtiff tolerates unsorted entries, so getting this right buys
    nothing at read time — it is here so the fixture is a valid TIFF and not
    merely one libtiff happens to accept.
    """
    header = b"II" + struct.pack("<HI", 42, 8)

    # Layout: header, IFD, out-of-line tag values, then the pixel strip. Values
    # of four bytes or fewer live inside the entry itself.
    entries = sorted(tags.items())
    ifd_size = 2 + (12 * (len(entries) + 2)) + 4  # +2 for StripOffsets/ByteCounts
    value_base = 8 + ifd_size

    payload = bytearray()
    resolved: list[tuple[int, int, int, bytes]] = []

    def add(tag: int, kind: int, values) -> None:
        raw = _pack_values(kind, values)
        count = len(values) + 1 if kind == TYPE_ASCII else len(values)
        if len(raw) <= 4:
            inline = raw + b"\0" * (4 - len(raw))
            resolved.append((tag, kind, count, inline))
        else:
            offset = value_base + len(payload)
            payload.extend(raw)
            if len(payload) % 2:  # keep everything word aligned
                payload.append(0)
            resolved.append((tag, kind, count, struct.pack("<I", offset)))

    for tag, (kind, values) in entries:
        add(tag, kind, values)

    strip_offset = value_base + len(payload)
    add(273, TYPE_LONG, [strip_offset])          # StripOffsets
    add(279, TYPE_LONG, [len(strip)])            # StripByteCounts
    resolved.sort(key=lambda e: e[0])

    ifd = struct.pack("<H", len(resolved))
    for tag, kind, count, value in resolved:
        ifd += struct.pack("<HHI", tag, kind, count) + value
    ifd += struct.pack("<I", 0)  # no next IFD

    assert len(ifd) <= ifd_size, (len(ifd), ifd_size)
    ifd += b"\0" * (ifd_size - len(ifd))

    path.write_bytes(header + ifd + bytes(payload) + strip)


def geo_tags(epsg: int, origin_e: float, origin_n: float, scale: float) -> dict:
    """ModelPixelScale + ModelTiepoint + a GeoKey directory naming an EPSG code."""
    geo_keys = [
        1, 1, 1, 3,        # version 1.1.1, three keys
        1024, 0, 1, 1,     # GTModelType = projected
        1025, 0, 1, 1,     # GTRasterType = PixelIsArea
        3072, 0, 1, epsg,  # ProjectedCSType
    ]
    return {
        33550: (TYPE_DOUBLE, [scale, scale, 0.0]),
        33922: (TYPE_DOUBLE, [0.0, 0.0, 0.0, origin_e, origin_n, 0.0]),
        34735: (TYPE_SHORT, geo_keys),
    }


def packbits(data: bytes) -> bytes:
    """PackBits run-length encoding (TIFF compression 32773)."""
    out = bytearray()
    i = 0
    while i < len(data):
        run = 1
        while i + run < len(data) and run < 128 and data[i + run] == data[i]:
            run += 1
        if run > 1:
            out.append(257 - run)
            out.append(data[i])
            i += run
            continue
        literal = 1
        while (
            i + literal < len(data)
            and literal < 128
            and not (i + literal + 1 < len(data) and data[i + literal] == data[i + literal + 1])
        ):
            literal += 1
        out.append(literal - 1)
        out.extend(data[i : i + literal])
        i += literal
    return bytes(out)


def tiff_lzw(data: bytes) -> bytes:
    """TIFF-flavoured LZW (compression 5).

    Variable-width MSB-first codes with TIFF's "early change": the width grows
    one code sooner than a naive reading of the algorithm suggests. Getting that
    off by one produces a stream that decodes to plausible garbage rather than
    failing, so the fixture test compares these pixels against the uncompressed
    fixture's — if this encoder is wrong, that comparison is what says so.
    """
    CLEAR, EOI = 256, 257
    out = bytearray()
    bit_buffer = 0
    bit_count = 0

    def emit(code: int, width: int) -> None:
        nonlocal bit_buffer, bit_count
        bit_buffer = (bit_buffer << width) | code
        bit_count += width
        while bit_count >= 8:
            bit_count -= 8
            out.append((bit_buffer >> bit_count) & 0xFF)

    table: dict[bytes, int] = {bytes([i]): i for i in range(256)}
    next_code = 258
    width = 9
    emit(CLEAR, width)

    prefix = b""
    for byte in data:
        candidate = prefix + bytes([byte])
        if candidate in table:
            prefix = candidate
            continue
        emit(table[prefix], width)
        table[candidate] = next_code
        next_code += 1
        # Early change: switch at 511/1023/2047, not 512/1024/2048.
        if next_code + 1 > (1 << width) and width < 12:
            width += 1
        if next_code + 1 >= 4096:
            emit(CLEAR, width)
            table = {bytes([i]): i for i in range(256)}
            next_code = 258
            width = 9
        prefix = bytes([byte])

    if prefix:
        emit(table[prefix], width)
    emit(EOI, width)
    if bit_count:
        out.append((bit_buffer << (8 - bit_count)) & 0xFF)
    return bytes(out)


# --------------------------------------------------------------------------
# PNG writing
# --------------------------------------------------------------------------


def write_png(path: pathlib.Path, width: int, height: int, rgb: bytes) -> None:
    raw = b"".join(
        b"\0" + rgb[y * width * 3 : (y + 1) * width * 3] for y in range(height)
    )

    def chunk(kind: bytes, payload: bytes) -> bytes:
        return (
            struct.pack(">I", len(payload))
            + kind
            + payload
            + struct.pack(">I", zlib.crc32(kind + payload) & 0xFFFFFFFF)
        )

    path.write_bytes(
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
        + chunk(b"IDAT", zlib.compress(raw, 9))
        + chunk(b"IEND", b"")
    )


# --------------------------------------------------------------------------
# Shapefile writing
# --------------------------------------------------------------------------


def write_shapefile(stem: pathlib.Path, shapes: list, names: list[str], prj: str) -> None:
    """Writes .shp, .shx, .dbf and .prj.

    `shapes` is a list of (type, parts) where type is 1 (Point), 3 (PolyLine)
    or 5 (Polygon) and parts is a list of rings, each a list of (x, y).
    """
    records = bytearray()
    index = bytearray()
    min_x = min_y = float("inf")
    max_x = max_y = float("-inf")

    for number, (kind, parts) in enumerate(shapes, start=1):
        points = [p for ring in parts for p in ring]
        for x, y in points:
            min_x, min_y = min(min_x, x), min(min_y, y)
            max_x, max_y = max(max_x, x), max(max_y, y)

        if kind == 1:
            body = struct.pack("<idd", 1, points[0][0], points[0][1])
        else:
            box = (
                min(p[0] for p in points),
                min(p[1] for p in points),
                max(p[0] for p in points),
                max(p[1] for p in points),
            )
            starts, at = [], 0
            for ring in parts:
                starts.append(at)
                at += len(ring)
            body = struct.pack("<i4d2i", kind, *box, len(parts), len(points))
            body += b"".join(struct.pack("<i", s) for s in starts)
            body += b"".join(struct.pack("<dd", x, y) for x, y in points)

        offset_words = (100 + len(records)) // 2
        index += struct.pack(">ii", offset_words, len(body) // 2)
        # Record header is BIG-endian; the body is little-endian. This mixed
        # order is the format's, not a mistake.
        records += struct.pack(">ii", number, len(body) // 2) + body

    shape_type = shapes[0][0] if shapes else 0

    def header(content_words: int) -> bytes:
        return (
            struct.pack(">iiiiiii", 9994, 0, 0, 0, 0, 0, content_words)
            + struct.pack("<ii", 1000, shape_type)
            + struct.pack("<8d", min_x, min_y, max_x, max_y, 0, 0, 0, 0)
        )

    stem.with_suffix(".shp").write_bytes(header((100 + len(records)) // 2) + bytes(records))
    stem.with_suffix(".shx").write_bytes(header((100 + len(index)) // 2) + bytes(index))
    stem.with_suffix(".prj").write_text(prj, encoding="ascii")

    # dBASE III with a single 32-char NAME field.
    width = 32
    header_size = 32 + 32 + 1
    record_size = 1 + width
    dbf = bytearray()
    dbf += struct.pack("<BBBBIHH", 3, 126, 1, 1, len(shapes), header_size, record_size)
    dbf += b"\0" * 20
    dbf += b"NAME".ljust(11, b"\0") + b"C" + struct.pack("<I", 0)
    dbf += struct.pack("<BB", width, 0) + b"\0" * 14
    dbf += b"\r"
    for name in names:
        dbf += b" " + name.encode("ascii", "replace").ljust(width)[:width]
    dbf += b"\x1a"
    stem.with_suffix(".dbf").write_bytes(bytes(dbf))


# --------------------------------------------------------------------------


def checkerboard(width: int, height: int) -> bytes:
    """A pattern with no symmetry in x or y, so a transposed or mirrored read is
    visible rather than plausible."""
    out = bytearray()
    for y in range(height):
        for x in range(width):
            out += bytes(((x * 16) % 256, (y * 32) % 256, ((x + y) * 8) % 256))
    return bytes(out)


def main() -> None:
    OUT.mkdir(parents=True, exist_ok=True)
    width, height = 8, 6
    rgb = checkerboard(width, height)
    # libtiff hands back RGBA; the fixtures are RGB and the alpha is synthesised.
    common = {
        256: (TYPE_LONG, [width]),
        257: (TYPE_LONG, [height]),
        258: (TYPE_SHORT, [8, 8, 8]),
        262: (TYPE_SHORT, [2]),
        277: (TYPE_SHORT, [3]),
        278: (TYPE_LONG, [height]),
        284: (TYPE_SHORT, [1]),
        339: (TYPE_SHORT, [1, 1, 1]),
        **geo_tags(32631, UTM_ORIGIN_E, UTM_ORIGIN_N, PIXEL_SIZE),
    }

    write_tiff(OUT / "utm31_image.tif", {**common, 259: (TYPE_SHORT, [1])}, rgb)
    write_tiff(OUT / "utm31_image_lzw.tif", {**common, 259: (TYPE_SHORT, [5])}, tiff_lzw(rgb))
    write_tiff(
        OUT / "utm31_image_packbits.tif", {**common, 259: (TYPE_SHORT, [32773])}, packbits(rgb)
    )
    # Deflate is REFUSED by this build (no zlib in libtiff — ADR-0010, #484).
    # The fixture exists so that refusal is asserted against a real file rather
    # than a hypothesis about one.
    write_tiff(
        OUT / "utm31_image_deflate.tif",
        {**common, 259: (TYPE_SHORT, [8])},
        zlib.compress(rgb, 9),
    )

    # A geographic (EPSG:4326) image, to exercise the non-affine resample path
    # into a UTM or tmerc scene.
    write_tiff(
        OUT / "wgs84_image.tif",
        {
            **common,
            259: (TYPE_SHORT, [1]),
            **geo_tags(4326, 4.85, 52.40, 0.001),
        },
        rgb,
    )

    # Single-band float32 elevation. A visible slope in both axes, so an
    # inverted row order or a transposed read shows up as the wrong corner
    # being high rather than as noise.
    dem = b"".join(
        struct.pack("<f", 10.0 + (x * 2.0) + (y * 5.0))
        for y in range(height)
        for x in range(width)
    )
    write_tiff(
        OUT / "utm31_dem.tif",
        {
            256: (TYPE_LONG, [width]),
            257: (TYPE_LONG, [height]),
            258: (TYPE_SHORT, [32]),
            259: (TYPE_SHORT, [1]),
            262: (TYPE_SHORT, [1]),
            277: (TYPE_SHORT, [1]),
            278: (TYPE_LONG, [height]),
            284: (TYPE_SHORT, [1]),
            339: (TYPE_SHORT, [3]),
            42113: (TYPE_ASCII, "-9999"),
            **geo_tags(32631, UTM_ORIGIN_E, UTM_ORIGIN_N, PIXEL_SIZE),
        },
        dem,
    )

    # World-filed PNG. The world file's C/F name the CENTRE of the top-left
    # pixel, and its y scale is negative because rows run north to south.
    write_png(OUT / "utm31_image.png", width, height, rgb)
    (OUT / "utm31_image.pgw").write_text(
        f"{PIXEL_SIZE}\n0.0\n0.0\n-{PIXEL_SIZE}\n{UTM_ORIGIN_E}\n{UTM_ORIGIN_N}\n",
        encoding="ascii",
    )
    (OUT / "utm31_image.prj").write_text(PRJ_UTM31N, encoding="ascii")

    # Vectors. GeoJSON is WGS84 lon/lat by RFC 7946 — there is no CRS to state.
    (OUT / "amsterdam.geojson").write_text(
        json.dumps(
            {
                "type": "FeatureCollection",
                "features": [
                    {
                        "type": "Feature",
                        "properties": {"name": "centre line"},
                        "geometry": {
                            "type": "LineString",
                            "coordinates": [[4.8500, 52.4000], [4.8515, 52.4008]],
                        },
                    },
                    {
                        "type": "Feature",
                        "properties": {"name": "block"},
                        "geometry": {
                            "type": "Polygon",
                            "coordinates": [
                                [
                                    [4.8520, 52.4000],
                                    [4.8530, 52.4000],
                                    [4.8530, 52.4006],
                                    [4.8520, 52.4006],
                                    [4.8520, 52.4000],
                                ]
                            ],
                        },
                    },
                    {
                        "type": "Feature",
                        "properties": {"name": "marker"},
                        "geometry": {"type": "Point", "coordinates": [4.8505, 52.4003]},
                    },
                ],
            },
            indent=2,
        )
        + "\n",
        encoding="utf-8",
    )

    write_shapefile(
        OUT / "utm31_roads",
        [
            (
                3,
                [
                    [
                        (UTM_ORIGIN_E, UTM_ORIGIN_N),
                        (UTM_ORIGIN_E + 100.0, UTM_ORIGIN_N + 50.0),
                        (UTM_ORIGIN_E + 200.0, UTM_ORIGIN_N + 50.0),
                    ]
                ],
            ),
            (
                3,
                [
                    [
                        (UTM_ORIGIN_E + 50.0, UTM_ORIGIN_N - 80.0),
                        (UTM_ORIGIN_E + 150.0, UTM_ORIGIN_N - 20.0),
                    ]
                ],
            ),
        ],
        ["Hoofdweg", "Zijstraat"],
        PRJ_UTM31N,
    )

    write_shapefile(
        OUT / "unsupported_crs_roads",
        [(3, [[(2000000.0, 400000.0), (2000100.0, 400050.0)]])],
        ["Refused Road"],
        PRJ_UNSUPPORTED,
    )

    for path in sorted(OUT.iterdir()):
        print(f"  {path.name}  ({path.stat().st_size} bytes)")


if __name__ == "__main__":
    main()
