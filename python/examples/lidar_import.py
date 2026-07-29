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

"""Importing a lidar tile and turning it into terrain (p7-s3, #243).

Reads an ASPRS LAS/LAZ tile, reprojects it into a scene's world frame, and fits
the ground into the scene's height field as one undoable edit.

LAS is read by RoadMaker itself rather than through PDAL: PDAL hard-requires
PROJ and GDAL, the dependency ADR-0010 declined, so ADR-0011 keeps the reader
in-house and takes only laz-perf for the LAZ codec. The CRS a tile states goes
through the same bounded family the GIS importer uses, which is why a point
cloud and an orthophoto of the same place land on top of each other.

Run:  python3 python/examples/lidar_import.py
"""

from __future__ import annotations

import pathlib

import roadmaker as rm

FIXTURES = (
    pathlib.Path(__file__).resolve().parent.parent.parent / "core" / "tests" / "data" / "lidar"
)


def main() -> None:
    # 1. A scene that knows where on the earth it is. Without this there is
    #    nowhere for georeferenced data to land, and the import says so.
    network = rm.RoadNetwork()
    stack = rm.edit.EditStack()
    geo = rm.GeoReference()
    geo.projection = rm.tmerc_projection(52.3702, 4.8952)  # Amsterdam
    stack.push(network, rm.edit.set_georeference(network, geo))

    # 2. Read the tile. It arrives in ITS OWN frame — nothing is moved yet.
    cloud, diagnostics = rm.lidar.load(FIXTURES / "amsterdam_tile.laz")
    print(f"read {len(cloud)} of {cloud.source_count} points, 1 in {cloud.stride}")
    print(f"  stated CRS: {rm.gis.describe_crs(rm.gis.parse_crs(cloud.crs))}")
    for diagnostic in diagnostics:
        print(f"  {diagnostic.severity.name}: {diagnostic.message}")

    # 3. Move it into the scene's frame. A CRS outside the supported family is
    #    refused here BY NAME, citing the issue that would lift the limitation —
    #    never silently mis-placed.
    transform = rm.gis.crs_transform(
        rm.gis.parse_crs(cloud.crs), rm.gis.scene_crs(network.georeference)
    )
    cloud = rm.lidar.reproject(cloud, transform)
    print(f"  in scene metres: x {cloud.bounds[0]:.1f}..{cloud.bounds[3]:.1f}, "
          f"y {cloud.bounds[1]:.1f}..{cloud.bounds[4]:.1f}")

    # 4. Fit the ground. The estimator that ran is always named: "classified as
    #    bare ground" and "lowest return" disagree under a bridge or a canopy,
    #    and you have to know which answer you are looking at.
    options = rm.lidar.GroundFitOptions()
    options.spacing = 10.0
    field, fit_diagnostics = rm.lidar.to_height_field(cloud, options)
    for diagnostic in fit_diagnostics:
        print(f"  {diagnostic.severity.name}: {diagnostic.message}")

    # 5. Install it as real, undoable scene content — the same door a .asc DEM
    #    and an elevation GeoTIFF go through.
    stack.push(network, rm.edit.set_terrain_field(network, field))
    print(f"terrain is now {network.terrain.cols}×{network.terrain.rows} "
          f"at {network.terrain.spacing:g} m")
    # Sample INSIDE the tile — the scene origin is a kilometre away from it, and
    # sampling outside the field only reports its clamped edge.
    low = rm.sample_height(network.terrain, cloud.bounds[0], cloud.bounds[1])
    high = rm.sample_height(network.terrain, cloud.bounds[3], cloud.bounds[4])
    print(f"  ground across the tile: {low:.2f} m at its low corner, {high:.2f} m at its high one")

    stack.undo(network)
    print(f"after undo, the scene has terrain: {not network.terrain.empty}")


if __name__ == "__main__":
    main()
