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

"""Importing GIS vector and raster data into a scene's world frame (p7-s2, #242).

RoadMaker reprojects a BOUNDED family of coordinate reference systems — WGS 84
geographic, UTM, Transverse Mercator, Web Mercator — computed in closed form,
and refuses everything else by name. There is no PROJ dependency and no datum
shift; the reasoning is ADR-0010 and the escape hatch is issue #485.

Run:  python3 python/examples/gis_import.py
"""

from __future__ import annotations

import pathlib

import roadmaker as rm

FIXTURES = pathlib.Path(__file__).resolve().parent.parent.parent / "core" / "tests" / "data" / "gis"


def main() -> None:
    # 1. A scene has to say where on the earth it is before anything can be
    #    imported into it. This is exactly what Edit > World Georeference does
    #    in the editor (p7-s5, #324).
    network = rm.RoadNetwork()
    geo = rm.GeoReference()
    geo.projection = rm.tmerc_projection(52.3702, 4.8952)  # Amsterdam
    network.set_georeference(geo)

    scene = rm.gis.scene_crs(network.georeference)
    print(f"scene frame : {rm.gis.describe_crs(scene)}")

    # 2. Vectors. A shapefile states its CRS in a sibling .prj.
    layer, diagnostics = rm.gis.load_vector(FIXTURES / "utm31_roads.shp")
    source = rm.gis.parse_crs(layer.crs)
    print(f"vector source: {rm.gis.describe_crs(source)}  ({len(layer.features)} features)")
    for d in diagnostics:
        print(f"  [{d.severity}] {d.message}")

    transform = rm.gis.crs_transform(source, scene)
    layer = rm.gis.reproject_vector(layer, transform)
    for feature in layer.features:
        x, y = feature.vertices[0]
        print(f"  {feature.name or '(unnamed)'} starts at {x:>10.2f}, {y:>10.2f} m from origin")

    # 3. Imagery. Note `placement`: an affine mapping keeps the source pixels,
    #    anything else resamples them — and a resampled image must not be
    #    mistaken for the original file.
    raster, diagnostics = rm.gis.load_raster(FIXTURES / "utm31_image.tif")
    placed, more = rm.gis.reproject_raster(raster, rm.gis.crs_transform(
        rm.gis.parse_crs(raster.crs), scene))
    west, south, east, north = placed.extent
    print(f"raster      : {placed.placement}, covering "
          f"{east - west:.0f} x {north - south:.0f} m at the origin")
    for d in diagnostics + more:
        print(f"  [{d.severity}] {d.message}")

    # 4. An elevation raster becomes the scene's terrain, through the SAME
    #    undoable command a .asc DEM import uses. Imagery is refused here —
    #    heights are not a picture.
    dem, _ = rm.gis.load_raster(FIXTURES / "utm31_dem.tif")
    print(f"dem         : elevation={dem.elevation}, nodata={dem.nodata}")
    field, dem_diagnostics = rm.gis.raster_to_height_field(
        dem, rm.gis.crs_transform(rm.gis.parse_crs(dem.crs), scene))
    for d in dem_diagnostics:
        print(f"  [{d.severity}] {d.message}")

    stack = rm.edit.EditStack()
    stack.push(network, rm.edit.set_terrain_field(network, field))
    print(f"terrain     : {network.terrain.cols} x {network.terrain.rows} posts at "
          f"{network.terrain.spacing} m spacing")
    stack.undo(network)
    print(f"after undo  : terrain empty = {network.terrain.empty}")

    # 5. What a refusal looks like. It names the coordinate system it read and
    #    the issue that would lift the limitation, rather than merely failing.
    unsupported, _ = rm.gis.load_vector(FIXTURES / "unsupported_crs_roads.shp")
    try:
        rm.gis.crs_transform(rm.gis.parse_crs(unsupported.crs), scene)
    except RuntimeError as error:
        print(f"refused     : {error}")


if __name__ == "__main__":
    main()
