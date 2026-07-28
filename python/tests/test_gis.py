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

"""GIS import through the bindings (p7-s2, #242)."""

from __future__ import annotations

import pathlib

import pytest

import roadmaker as rm

FIXTURES = (
    pathlib.Path(__file__).resolve().parent.parent.parent
    / "core"
    / "tests"
    / "data"
    / "gis"
)

pytestmark = pytest.mark.skipif(
    not FIXTURES.is_dir(), reason="GIS fixtures are not present in this checkout"
)


@pytest.fixture
def scene() -> rm.RoadNetwork:
    network = rm.RoadNetwork()
    geo = rm.GeoReference()
    geo.projection = rm.tmerc_projection(52.3702, 4.8952)
    network.set_georeference(geo)
    return network


def test_parse_crs_recognises_the_bounded_family() -> None:
    assert rm.gis.parse_crs("EPSG:4326").kind == rm.gis.CrsKind.Geographic
    assert rm.gis.parse_crs("EPSG:32631").kind == rm.gis.CrsKind.TransverseMercator
    assert rm.gis.parse_crs("EPSG:3857").kind == rm.gis.CrsKind.WebMercator
    assert rm.gis.describe_crs(rm.gis.parse_crs("EPSG:32631")) == "UTM zone 31N"


def test_an_out_of_family_crs_is_opaque_not_an_exception() -> None:
    # parse_crs never raises: an unsupported description is an honest reading,
    # and refusal happens at the transform where both ends can be named.
    crs = rm.gis.parse_crs("+proj=lcc +lat_1=33 +datum=NAD27")
    assert crs.kind == rm.gis.CrsKind.Opaque
    assert crs.text == "+proj=lcc +lat_1=33 +datum=NAD27"


def test_refusal_names_the_crs_and_cites_the_follow_up(scene: rm.RoadNetwork) -> None:
    with pytest.raises(RuntimeError, match=r"#485"):
        rm.gis.crs_transform(
            rm.gis.parse_crs("+proj=lcc +lat_1=33 +datum=NAD27"),
            rm.gis.scene_crs(scene.georeference),
        )


def test_a_scene_with_no_georeference_says_what_to_do() -> None:
    with pytest.raises(RuntimeError, match="World Georeference"):
        rm.gis.crs_transform(
            rm.gis.parse_crs("EPSG:32631"), rm.gis.scene_crs(rm.GeoReference())
        )


def test_geojson_reads_as_wgs84_lonlat() -> None:
    layer, diagnostics = rm.gis.load_vector(FIXTURES / "amsterdam.geojson")
    assert len(layer.features) == 3
    assert rm.gis.parse_crs(layer.crs).kind == rm.gis.CrsKind.Geographic
    assert [f.geometry for f in layer.features] == [
        rm.gis.Geometry.Line,
        rm.gis.Geometry.Polygon,
        rm.gis.Geometry.Point,
    ]
    assert diagnostics == []


def test_shapefile_reads_geometry_names_and_prj(scene: rm.RoadNetwork) -> None:
    layer, _ = rm.gis.load_vector(FIXTURES / "utm31_roads.shp")
    assert [f.name for f in layer.features] == ["Hoofdweg", "Zijstraat"]

    transform = rm.gis.crs_transform(
        rm.gis.parse_crs(layer.crs), rm.gis.scene_crs(scene.georeference)
    )
    moved = rm.gis.reproject_vector(layer, transform)
    # A few kilometres from the scene origin, not the millions of metres a raw
    # UTM easting carries — the assertion that catches a transform that ran but
    # did nothing.
    for feature in moved.features:
        for x, y in feature.vertices:
            assert abs(x) < 20_000
            assert abs(y) < 20_000


def test_deflate_geotiff_is_refused_by_name() -> None:
    with pytest.raises(RuntimeError, match=r"Deflate.*#484"):
        rm.gis.load_raster(FIXTURES / "utm31_image_deflate.tif")


def test_lzw_and_uncompressed_decode_identically() -> None:
    plain, _ = rm.gis.load_raster(FIXTURES / "utm31_image.tif")
    lzw, _ = rm.gis.load_raster(FIXTURES / "utm31_image_lzw.tif")
    assert bytes(lzw.rgba) == bytes(plain.rgba)


def test_a_resample_is_reported_and_a_placement_is_not(scene: rm.RoadNetwork) -> None:
    raster, _ = rm.gis.load_raster(FIXTURES / "utm31_image.tif")

    # Same central meridian as the source: affine, so the pixels survive intact.
    same_meridian = rm.GeoReference()
    same_meridian.projection = (
        "+proj=tmerc +lat_0=0 +lon_0=3 +k=1 +x_0=0 +y_0=0 +datum=WGS84 +units=m"
    )
    placed, diagnostics = rm.gis.reproject_raster(
        raster,
        rm.gis.crs_transform(
            rm.gis.parse_crs(raster.crs), rm.gis.scene_crs(same_meridian)
        ),
    )
    assert placed.placement == rm.gis.RasterPlacement.Placed
    assert bytes(placed.raster.rgba) == bytes(raster.rgba)
    assert diagnostics == []

    # A different central meridian curves, so it must resample AND say so.
    resampled, diagnostics = rm.gis.reproject_raster(
        raster,
        rm.gis.crs_transform(
            rm.gis.parse_crs(raster.crs), rm.gis.scene_crs(scene.georeference)
        ),
    )
    assert resampled.placement == rm.gis.RasterPlacement.Resampled
    assert any("resampled" in d.message for d in diagnostics)


def test_elevation_raster_installs_as_terrain_and_undoes(scene: rm.RoadNetwork) -> None:
    dem, _ = rm.gis.load_raster(FIXTURES / "utm31_dem.tif")
    assert dem.elevation
    assert dem.nodata == pytest.approx(-9999.0)

    field, _ = rm.gis.raster_to_height_field(
        dem, rm.gis.crs_transform(rm.gis.parse_crs(dem.crs), rm.gis.scene_crs(scene.georeference))
    )
    assert not field.empty

    stack = rm.edit.EditStack()
    stack.push(scene, rm.edit.set_terrain_field(scene, field))
    assert not scene.terrain.empty
    assert scene.terrain.cols == field.cols

    # Terrain is real scene content, so it goes through the ordinary command
    # layer and undoes like anything else.
    stack.undo(scene)
    assert scene.terrain.empty


def test_imagery_is_refused_as_terrain(scene: rm.RoadNetwork) -> None:
    raster, _ = rm.gis.load_raster(FIXTURES / "utm31_image.tif")
    with pytest.raises(RuntimeError, match="single-band"):
        rm.gis.raster_to_height_field(
            raster,
            rm.gis.crs_transform(
                rm.gis.parse_crs(raster.crs), rm.gis.scene_crs(scene.georeference)
            ),
        )


def test_an_unsupported_format_names_its_follow_up(tmp_path: pathlib.Path) -> None:
    with pytest.raises(RuntimeError, match=r"#486"):
        rm.gis.load_vector(tmp_path / "somewhere.gpkg")
