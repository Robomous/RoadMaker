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

"""The scene height field and elevation-terrain coupling (p5-s2, #232, GW-2 step 7).

A scene has no ground DATA until a height field is created: the terrain mesh
channel is empty and the ground is a flat render plane. `create_terrain_field`
lays a flat zero field over the network bounds — visually a no-op, since a flat
field samples 0 everywhere, exactly like no field. What it buys is a real
surface the roads shape: raise a road's elevation profile and the ground within
a skirt band of it follows, while ground far away stays at the field. The field
persists to an ESRI ASCII `.asc` sidecar referenced from the `.xodr`.

Run:  python terrain_field.py [out.xodr]
"""

import sys
import tempfile
from pathlib import Path

import roadmaker as rm


def main() -> None:
    net = rm.RoadNetwork()
    road = rm.author_clothoid_road(
        net, [(0.0, 0.0), (120.0, 0.0)], rm.LaneProfile.two_lane_rural(), "", "r0"
    )

    # No field yet: the terrain channel is empty and the ground is presentation.
    assert net.terrain.empty
    assert rm.build_network_mesh(net).terrain_vertex_count == 0

    # Create a flat field over the network. A flat field is visually the old
    # plate — sample_height is 0 everywhere — but the ground is now real data.
    stack = rm.edit.EditStack()
    stack.push(net, rm.edit.create_terrain_field(net))
    assert not net.terrain.empty
    print(f"created a {net.terrain.cols}x{net.terrain.rows} field, "
          f"spacing {net.terrain.spacing} m")
    print(f"  sample at the origin: {rm.sample_height(net.terrain, 0.0, 0.0):.3f} m (flat)")

    # Raise the road into a hill and re-mesh: the coupling is that the ground
    # beside it rises with it.
    length = net.road(road).length
    stack.push(
        net,
        rm.edit.set_elevation_profile(
            net,
            road,
            [
                rm.edit.ElevationPoint(0.0, 0.0, 0.0),
                rm.edit.ElevationPoint(length / 2.0, 8.0, 0.0),
                rm.edit.ElevationPoint(length, 0.0, 0.0),
            ],
        ),
    )
    mesh = rm.build_network_mesh(net)
    print(f"  raised the road into an 8 m hill; terrain re-meshed "
          f"{mesh.terrain_vertex_count} vertices that skirt it")

    # Persist: the field writes an .asc sidecar and the .xodr references it.
    out = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(tempfile.mkdtemp()) / "scene.xodr"
    rm.save_xodr(net, str(out), out.stem)
    sidecar = out.with_name(f"{out.stem}.terrain.asc")
    print(f"wrote {out} + sidecar {sidecar.name} ({'exists' if sidecar.exists() else 'MISSING'})")

    reloaded, _diagnostics = rm.load_xodr(str(out))
    assert not reloaded.terrain.empty, "the field did not survive the round-trip"
    # The grid round-trips exactly; only `sidecar` differs (it was unset in
    # memory and is filled in from the reference on load).
    assert reloaded.terrain.heights == net.terrain.heights, "the reloaded grid differs"
    assert reloaded.terrain.cols == net.terrain.cols
    assert reloaded.terrain.rows == net.terrain.rows
    print(f"reloaded the scene; its {reloaded.terrain.cols}x{reloaded.terrain.rows} "
          f"height field round-tripped, sidecar='{reloaded.terrain.sidecar}'")


if __name__ == "__main__":
    main()
