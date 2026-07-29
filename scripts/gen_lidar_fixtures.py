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

"""Generates the committed ASPRS LAS fixtures for the lidar importer (p7-s3, #243).

A reader checked only against bytes the same test just built agrees with itself
about everything, including its own mistakes. So the fixtures are real files, on
disk, in the repository. They are generated rather than downloaded so their
provenance is unambiguously ours (no asset licence question), and they are
generated with the STANDARD LIBRARY ALONE so this script needs no lidar stack to
run -- which is the same reason the kernel does not have one (ADR-0011). This is
the sibling of `scripts/gen_gis_fixtures.py` and follows it deliberately.

The tile is a real place -- Amsterdam, UTM zone 31N (EPSG:32631) -- so that a
false-easting or central-meridian mistake cannot hide at the origin, and so the
lidar fixtures land on top of the GIS ones, which are the same place.

THE ONE FIXTURE THIS SCRIPT DOES NOT WRITE is `amsterdam_tile.laz`. LAZ is an
entropy-coded stream, so unlike the TIFF LZW encoder in gen_gis_fixtures.py it
cannot be written in a page of standard library. It is produced once, from
`amsterdam_tile.las`, by a small build target and then committed:

    cmake --build --preset dev-macos --target rm_make_laz_fixture
    ./build/dev-macos/core/tests/rm_make_laz_fixture \\
        core/tests/data/lidar/amsterdam_tile.las \\
        core/tests/data/lidar/amsterdam_tile.laz

`LazDecodesIdenticallyToLas` is what makes that honest: it decodes both committed
files and compares them point for point.

Run:  python3 scripts/gen_lidar_fixtures.py
"""

from __future__ import annotations

import pathlib
import struct

OUT = pathlib.Path(__file__).resolve().parent.parent / "core" / "tests" / "data" / "lidar"

# Amsterdam, UTM zone 31N. Far enough from the zone origin that a dropped false
# easting (500 000 m) or a wrong central meridian is a kilometres-scale error
# rather than a rounding one.
TILE_EAST = 628_000.0
TILE_NORTH = 5_803_000.0

# The tile: 100 m x 80 m, ground sloping in BOTH axes so that a row flip or a
# transposed grid shows up as the wrong corner being high rather than as a
# plausible surface.
TILE_WIDTH = 100.0
TILE_HEIGHT = 80.0
POINT_SPACING = 5.0
GROUND_Z = 2.0
SLOPE_X = 0.05
SLOPE_Y = 0.02

# ASPRS standard classifications.
CLASS_GROUND = 2
CLASS_VEGETATION_HIGH = 5
CLASS_BUILDING = 6

WKT_UTM31N = (
    'PROJCS["WGS 84 / UTM zone 31N",'
    'GEOGCS["WGS 84",DATUM["WGS_1984",SPHEROID["WGS 84",6378137,298.257223563]],'
    'PRIMEM["Greenwich",0],UNIT["degree",0.0174532925199433]],'
    'PROJECTION["Transverse_Mercator"],'
    'PARAMETER["latitude_of_origin",0],PARAMETER["central_meridian",3],'
    'PARAMETER["scale_factor",0.9996],PARAMETER["false_easting",500000],'
    'PARAMETER["false_northing",0],UNIT["metre",1]]'
)

# Out of the supported family in BOTH its projection and its datum, so a build
# that quietly ignored one of the two would still refuse this.
WKT_UNSUPPORTED = (
    'PROJCS["NAD27 / Texas North",'
    'GEOGCS["NAD27",DATUM["North_American_Datum_1927",'
    'SPHEROID["Clarke 1866",6378206.4,294.9786982]],'
    'PRIMEM["Greenwich",0],UNIT["degree",0.0174532925199433]],'
    'PROJECTION["Lambert_Conformal_Conic_2SP"],'
    'PARAMETER["standard_parallel_1",34.65],PARAMETER["standard_parallel_2",36.18],'
    'PARAMETER["latitude_of_origin",34],PARAMETER["central_meridian",-101.5],'
    'PARAMETER["false_easting",2000000],PARAMETER["false_northing",0],'
    'UNIT["US survey foot",0.3048006096012192]]'
)


# --- the tile ---------------------------------------------------------------


def ground_height(dx: float, dy: float) -> float:
    return GROUND_Z + (SLOPE_X * dx) + (SLOPE_Y * dy)


def tile_points() -> list[tuple[float, float, float, int]]:
    """(x, y, z, classification), in a fixed order so the fixtures are stable."""
    points: list[tuple[float, float, float, int]] = []

    steps_x = int(TILE_WIDTH / POINT_SPACING) + 1
    steps_y = int(TILE_HEIGHT / POINT_SPACING) + 1
    for iy in range(steps_y):
        for ix in range(steps_x):
            dx = ix * POINT_SPACING
            dy = iy * POINT_SPACING
            points.append(
                (TILE_EAST + dx, TILE_NORTH + dy, ground_height(dx, dy), CLASS_GROUND)
            )

    # A building: 20 m square, 8 m tall, sitting well inside the tile so that a
    # ground fit binning the LOWEST return is visibly wrong there while a fit
    # honouring the classification is not.
    for iy in range(5):
        for ix in range(5):
            dx = 30.0 + (ix * 5.0)
            dy = 25.0 + (iy * 5.0)
            points.append(
                (
                    TILE_EAST + dx,
                    TILE_NORTH + dy,
                    ground_height(dx, dy) + 8.0,
                    CLASS_BUILDING,
                )
            )

    # A row of tree canopy, for the same reason.
    for ix in range(6):
        dx = 10.0 + (ix * 5.0)
        dy = 65.0
        points.append(
            (
                TILE_EAST + dx,
                TILE_NORTH + dy,
                ground_height(dx, dy) + 6.0,
                CLASS_VEGETATION_HIGH,
            )
        )

    return points


# --- LAS writing ------------------------------------------------------------


def fixed(text: str, width: int) -> bytes:
    """A NUL-padded fixed-width character field, as LAS spells them."""
    raw = text.encode("ascii")
    if len(raw) >= width:
        return raw[:width]
    return raw + (b"\0" * (width - len(raw)))


def vlr(user_id: str, record_id: int, payload: bytes, description: str) -> bytes:
    """One variable-length record: a 54-byte header then the payload."""
    return (
        struct.pack("<H", 0)
        + fixed(user_id, 16)
        + struct.pack("<HH", record_id, len(payload))
        + fixed(description, 32)
        + payload
    )


def geokey_vlr(epsg: int) -> bytes:
    """A GeoTIFF GeoKey directory naming a projected CRS by EPSG code.

    The directory is a flat uint16 array: a 4-value header (version, revision,
    minor, key count) then 4 values per key (id, tiff_tag_location, count,
    value_or_offset). tiff_tag_location 0 means the value is inline, which is
    the only case an EPSG code ever takes.
    """
    keys = [
        1, 1, 0, 2,           # header: v1.1.0, two keys
        1024, 0, 1, 1,        # GTModelTypeGeoKey = projected
        3072, 0, 1, epsg,     # ProjectedCSTypeGeoKey
    ]
    payload = b"".join(struct.pack("<H", value) for value in keys)
    return vlr("LASF_Projection", 34735, payload, "GeoTIFF GeoKey directory")


def wkt_vlr(wkt: str) -> bytes:
    return vlr(
        "LASF_Projection", 2112, wkt.encode("ascii") + b"\0", "OGC coordinate system WKT"
    )


def write_las(
    path: pathlib.Path,
    points: list[tuple[float, float, float, int]],
    *,
    version_minor: int,
    point_format: int,
    vlrs: bytes,
    wkt_bit: bool = False,
    extra_bytes: int = 0,
) -> None:
    """Writes one LAS file. Everything on disk is little-endian, at every version."""
    natural_length = {
        0: 20, 1: 28, 2: 26, 3: 34, 4: 57, 5: 63,
        6: 30, 7: 36, 8: 38, 9: 59, 10: 67,
    }[point_format]
    record_length = natural_length + extra_bytes

    header_size = {0: 227, 1: 227, 2: 227, 3: 235, 4: 375}[version_minor]

    scale = 0.001
    offset = (TILE_EAST, TILE_NORTH, 0.0)

    body = bytearray()
    for x, y, z, classification in points:
        record = bytearray(record_length)
        struct.pack_into(
            "<iii",
            record,
            0,
            round((x - offset[0]) / scale),
            round((y - offset[1]) / scale),
            round((z - offset[2]) / scale),
        )
        struct.pack_into("<H", record, 12, 100)  # intensity
        if point_format >= 6:
            record[14] = 0x11  # return 1 of 1, in the widened 4+4-bit field
            record[15] = 0x00  # classification flags / scanner channel
            record[16] = classification
        else:
            record[14] = 0x09  # return 1 of 1, in the legacy 3+3-bit field
            # Bits 0-4 are the class; 5-7 are synthetic/key-point/withheld. A
            # reader that forgets the mask reports class 34 for a withheld
            # ground point, so one point per file carries the withheld flag.
            record[15] = classification
        body += record

    if points:
        record_15 = 15 if point_format < 6 else 16
        # Set the withheld bit (0x80) on the LAST point of a legacy-format file,
        # so an unmasked read of byte 15 is provably wrong rather than merely
        # unproven. Formats 6-10 have no such bit in the class byte.
        if point_format < 6:
            last = len(body) - record_length
            body[last + record_15] = body[last + record_15] | 0x80

    xs = [p[0] for p in points]
    ys = [p[1] for p in points]
    zs = [p[2] for p in points]

    global_encoding = 0x10 if wkt_bit else 0

    header = bytearray(header_size)
    header[0:4] = b"LASF"
    struct.pack_into("<H", header, 4, 0)  # file source id
    struct.pack_into("<H", header, 6, global_encoding)
    header[24] = 1
    header[25] = version_minor
    header[26:58] = fixed("RoadMaker fixtures", 32)
    header[58:90] = fixed("gen_lidar_fixtures.py", 32)
    struct.pack_into("<HH", header, 90, 1, 2026)  # day of year, year
    struct.pack_into("<H", header, 94, header_size)
    struct.pack_into("<I", header, 96, header_size + len(vlrs))
    struct.pack_into("<I", header, 100, 1 if vlrs else 0)
    header[104] = point_format
    struct.pack_into("<H", header, 105, record_length)
    # The legacy 32-bit count. LAS 1.4 requires it to be zero for point formats
    # 6-10, and the 64-bit field at 247 carries the real number.
    legacy_count = 0 if (version_minor >= 4 and point_format >= 6) else len(points)
    struct.pack_into("<I", header, 107, legacy_count)
    for axis in range(3):
        struct.pack_into("<d", header, 131 + (axis * 8), scale)
        struct.pack_into("<d", header, 155 + (axis * 8), offset[axis])
    for axis, values in enumerate((xs, ys, zs)):
        struct.pack_into("<d", header, 179 + (axis * 16), max(values))
        struct.pack_into("<d", header, 187 + (axis * 16), min(values))
    if version_minor >= 4:
        struct.pack_into("<Q", header, 227, 0)  # start of waveform data
        struct.pack_into("<Q", header, 235, 0)  # start of first EVLR
        struct.pack_into("<I", header, 243, 0)  # number of EVLRs
        struct.pack_into("<Q", header, 247, len(points))

    path.write_bytes(bytes(header) + vlrs + bytes(body))
    print(f"  {path.name}  ({len(points)} points, {path.stat().st_size} bytes)")


def main() -> None:
    OUT.mkdir(parents=True, exist_ok=True)
    points = tile_points()
    print(f"writing LAS fixtures to {OUT}")

    # The base tile: LAS 1.2, point format 3, CRS as GeoTIFF keys.
    write_las(
        OUT / "amsterdam_tile.las",
        points,
        version_minor=2,
        point_format=3,
        vlrs=geokey_vlr(32631),
    )

    # The same tile as LAS 1.4 with point format 6 and the CRS as WKT: the
    # 375-byte header, the 64-bit point count, the widened return field and the
    # promoted classification byte, all in one file.
    write_las(
        OUT / "amsterdam_tile_14.las",
        points,
        version_minor=4,
        point_format=6,
        vlrs=wkt_vlr(WKT_UTM31N),
        wkt_bit=True,
    )

    # Records LONGER than the format's natural length are legal and common (LAS
    # 1.4 extra bytes). A reader that steps by the format's own size instead of
    # the header's reads garbage from the second point onward.
    write_las(
        OUT / "amsterdam_tile_extra.las",
        points,
        version_minor=2,
        point_format=1,
        vlrs=geokey_vlr(32631),
        extra_bytes=4,
    )

    # Out of the supported CRS family, so the refusal can be asserted against a
    # real file rather than a hypothetical one.
    write_las(
        OUT / "unsupported_crs.las",
        points[:40],
        version_minor=2,
        point_format=1,
        vlrs=wkt_vlr(WKT_UNSUPPORTED),
        wkt_bit=True,
    )

    # No LASF_Projection record at all: a legitimate state (a raw scan), read as
    # "already in the scene's frame" with a warning.
    write_las(
        OUT / "no_crs.las",
        points[:40],
        version_minor=2,
        point_format=1,
        vlrs=b"",
    )

    print("done. amsterdam_tile.laz is built separately -- see this file's docstring.")


if __name__ == "__main__":
    main()
