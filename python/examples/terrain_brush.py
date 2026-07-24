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

"""Terrain sculpting brushes and DEM import (p5-s4, #234, GW-2 step 9).

Building on the height field (#232), this shows the two things p5-s4 adds:
sculpting the field with raise/lower/smooth brushes, and importing an ESRI ASCII
grid (.asc) DEM. Each brush stroke is ONE undoable command; the sculpted (or
imported) field persists to the same .asc sidecar the created field does.

Run:  python terrain_brush.py [out.xodr]
"""

import sys
import tempfile
from pathlib import Path

import roadmaker as rm


def main() -> None:
    net = rm.RoadNetwork()
    rm.author_clothoid_road(
        net, [(0.0, 0.0), (120.0, 0.0)], rm.LaneProfile.two_lane_rural(), "", "r0"
    )

    stack = rm.edit.EditStack()
    stack.push(net, rm.edit.create_terrain_field(net))
    assert not net.terrain.empty

    lo_x, lo_y, hi_x, hi_y = rm.field_extent(net.terrain)
    cx, cy = (lo_x + hi_x) / 2.0, (lo_y + hi_y) / 2.0

    # Raise a hill: a stroke is a list of BrushStamp dabs, replayed as ONE
    # region-delta command (so undo is a single step).
    stroke = [
        rm.BrushStamp(cx, cy, 30.0, 5.0, rm.BrushMode.RAISE),
        rm.BrushStamp(cx + 10.0, cy, 30.0, 5.0, rm.BrushMode.RAISE),
    ]
    stack.push(net, rm.edit.stamp_terrain(net, stroke))
    peak = rm.sample_height(net.terrain, cx, cy)
    print(f"raised a hill; the ground under the brush is now {peak:.2f} m")
    assert peak > 0.0

    # Smooth it back down a little.
    stack.push(
        net,
        rm.edit.stamp_terrain(net, [rm.BrushStamp(cx, cy, 30.0, 0.5, rm.BrushMode.SMOOTH)]),
    )
    print(f"smoothed the peak to {rm.sample_height(net.terrain, cx, cy):.2f} m")

    # Persist and reload: the sculpted grid survives the .asc sidecar round-trip.
    out = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(tempfile.mkdtemp()) / "sculpted.xodr"
    rm.save_xodr(net, str(out), out.stem)
    reloaded, _ = rm.load_xodr(str(out))
    assert reloaded.terrain.heights == net.terrain.heights, "the sculpted grid did not round-trip"
    print(f"wrote {out.name} + sidecar; the sculpted field reloaded byte-for-byte")

    # DEM import path: write the field out as a .asc, then load_terrain_asc reads
    # it back as a HeightField (this is exactly what Edit ▸ Terrain ▸ Import DEM
    # does with a real-world DEM).
    dem_path = out.with_name("dem.asc")
    dem_path.write_text(rm.write_terrain_asc(net.terrain))
    imported = rm.load_terrain_asc(str(dem_path))
    print(f"imported a {imported.cols}x{imported.rows} DEM from {dem_path.name}")
    # Import replaces the scene field: clear the current one, then install the DEM
    # (Edit ▸ Import DEM does this onto a scene with no terrain of its own).
    stack.push(net, rm.edit.remove_terrain_field(net))
    stack.push(net, rm.edit.set_terrain_field(net, imported))
    assert net.terrain.heights == imported.heights
    print("installed the imported DEM as the scene field")


if __name__ == "__main__":
    main()
