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

"""Lidar import through the Python bindings (p7-s3, #243)."""

from __future__ import annotations

import math
import pathlib

import pytest

import roadmaker as rm

FIXTURES = (
    pathlib.Path(__file__).resolve().parent.parent.parent / "core" / "tests" / "data" / "lidar"
)


def scene_georeference() -> rm.GeoReference:
    """A scene framed on the tile's own area, as the editor's form produces."""
    geo = rm.GeoReference()
    geo.projection = rm.tmerc_projection(52.3702, 4.8952)
    return geo


def test_fixtures_are_present():
    # If this fails every other test here is vacuous.
    for name in (
        "amsterdam_tile.las",
        "amsterdam_tile_14.las",
        "amsterdam_tile.laz",
        "unsupported_crs.las",
        "no_crs.las",
    ):
        assert (FIXTURES / name).is_file(), name


def test_reads_a_tile_and_its_crs():
    cloud, diagnostics = rm.lidar.load(FIXTURES / "amsterdam_tile.las")
    assert len(cloud) == 388
    assert cloud.source_count == 388
    assert cloud.stride == 1
    assert cloud.crs == "EPSG:32631"
    assert not any(d.severity == rm.Severity.Error for d in diagnostics)

    east, north, _, east_max, north_max, _ = cloud.bounds
    assert east == pytest.approx(628_000.0, abs=1e-3)
    assert north == pytest.approx(5_803_000.0, abs=1e-3)
    assert east_max - east == pytest.approx(100.0, abs=1e-3)
    assert north_max - north == pytest.approx(80.0, abs=1e-3)


def test_laz_decodes_identically_to_las():
    plain, _ = rm.lidar.load(FIXTURES / "amsterdam_tile.las")
    packed, _ = rm.lidar.load(FIXTURES / "amsterdam_tile.laz")

    assert len(packed) == len(plain)
    # LAZ is lossless, so this is exact rather than approximate — a tolerance
    # here would hide a scale/offset mistake.
    assert sorted(plain.point(i) for i in range(len(plain))) == sorted(
        packed.point(i) for i in range(len(packed))
    )
    # And the CRS: laz-perf's writer emits only its own VLRs, so a .laz can
    # silently lose its coordinate system while every point still matches.
    assert packed.crs == plain.crs != ""


def test_decimation_is_reported_and_deterministic():
    options = rm.lidar.ReadOptions()
    options.max_points = 100

    first, diagnostics = rm.lidar.load(FIXTURES / "amsterdam_tile.las", options)
    second, _ = rm.lidar.load(FIXTURES / "amsterdam_tile.las", options)

    assert len(first) <= 100
    assert first.stride > 1
    assert first.source_count == 388
    # A cloud silently reduced looks like a sparse survey rather than a
    # decimated one.
    assert any("kept 1 point in" in d.message for d in diagnostics)
    # Two reads must agree, or a scene reopened tomorrow is not the one saved.
    assert [first.point(i) for i in range(len(first))] == [
        second.point(i) for i in range(len(second))
    ]


def test_an_unsupported_crs_is_refused_by_name():
    cloud, _ = rm.lidar.load(FIXTURES / "unsupported_crs.las")
    source = rm.gis.parse_crs(cloud.crs)
    scene = rm.gis.scene_crs(scene_georeference())

    with pytest.raises(RuntimeError) as refusal:
        rm.gis.crs_transform(source, scene)
    # The refusal names where the limitation is tracked, not merely that it
    # exists.
    assert "#485" in str(refusal.value)
    assert "NAD27" in rm.gis.describe_crs(source)


def test_a_tile_with_no_crs_warns_rather_than_guessing():
    cloud, diagnostics = rm.lidar.load(FIXTURES / "no_crs.las")
    assert cloud.crs == ""
    assert any("states no coordinate reference system" in d.message for d in diagnostics)


def test_reprojection_lands_the_tile_in_scene_metres():
    cloud, _ = rm.lidar.load(FIXTURES / "amsterdam_tile.las")
    transform = rm.gis.crs_transform(rm.gis.parse_crs(cloud.crs), rm.gis.scene_crs(scene_georeference()))
    before_z = cloud.point(0)[2]

    placed = rm.lidar.reproject(cloud, transform)

    assert abs(placed.bounds[0]) < 2000.0
    assert abs(placed.bounds[1]) < 2000.0
    # Z is untouched: the supported family shares one ellipsoid and none of it
    # is a vertical datum, so moving heights would be a datum shift.
    assert placed.point(0)[2] == pytest.approx(before_z)


def test_ground_fit_installs_as_terrain_and_undoes():
    cloud, _ = rm.lidar.load(FIXTURES / "amsterdam_tile.las")
    options = rm.lidar.GroundFitOptions()
    options.spacing = 10.0
    field, diagnostics = rm.lidar.to_height_field(cloud, options)

    assert field.cols == 11
    assert field.rows == 9
    assert any("classified as bare ground" in d.message for d in diagnostics)
    assert all(math.isfinite(h) for h in field.heights)

    # The whole point of producing a HeightField: it goes in by the same door a
    # .asc DEM and an elevation GeoTIFF use, as one undoable edit.
    network = rm.RoadNetwork()
    stack = rm.edit.EditStack()
    assert network.terrain.empty
    stack.push(network, rm.edit.set_terrain_field(network, field))
    assert not network.terrain.empty
    assert network.terrain.cols == 11
    stack.undo(network)
    assert network.terrain.empty


def test_empty_cells_are_interpolated_never_zero():
    # The tile's own gaps are what a ground fit meets in the field. Zero is not
    # a missing value — it is a claim that the ground is at the vertical datum.
    cloud, _ = rm.lidar.load(FIXTURES / "amsterdam_tile.las")
    options = rm.lidar.GroundFitOptions()
    options.spacing = 10.0
    field, _ = rm.lidar.to_height_field(cloud, options)
    assert all(h > 1.0 for h in field.heights)


def test_the_extension_predicate_agrees_with_the_loader():
    assert rm.lidar.is_point_cloud("tile.las")
    assert rm.lidar.is_point_cloud("tile.LAZ")
    assert not rm.lidar.is_point_cloud("tile.e57")

    with pytest.raises(RuntimeError) as refusal:
        rm.lidar.load(FIXTURES / "tile.e57")
    assert ".e57" in str(refusal.value)


def test_a_fit_too_fine_for_the_tile_is_refused_by_name():
    cloud, _ = rm.lidar.load(FIXTURES / "amsterdam_tile.las")
    options = rm.lidar.GroundFitOptions()
    options.spacing = 0.01
    with pytest.raises(RuntimeError) as refusal:
        rm.lidar.to_height_field(cloud, options)
    assert "2048" in str(refusal.value)
