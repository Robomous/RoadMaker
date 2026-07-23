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

"""Undoable editing with the kernel command layer (roadmaker.edit).

Every mutation is a command pushed onto an EditStack — pushing applies it,
undo/redo replay it, and undoing a delete resurrects objects under their
original ids. This is the same command layer the RoadMaker editor's
undo/redo uses, so a headless pipeline gets identical semantics.

Run:  python edit_network.py out.xodr
"""

from __future__ import annotations

import sys

import roadmaker as rm


def main() -> int:
    out_path = sys.argv[1] if len(sys.argv) > 1 else "edited.xodr"

    network = rm.RoadNetwork()
    stack = rm.edit.EditStack()

    # Author a road through a command so it is undoable from the start.
    stack.push(
        network,
        rm.edit.create_road(
            [(0.0, 0.0), (80.0, 10.0), (160.0, 0.0)],
            rm.LaneProfile.two_lane_default(),
            "Main Street",
        ),
    )
    road = network.find_road("1")
    print(f"created: {network.road(road)}")

    # Bend the road by moving its middle waypoint; the clothoid path is
    # re-fitted through the updated points.
    stack.push(network, rm.edit.move_waypoint(network, road, 1, (80.0, 40.0)))
    print(f"after move: length={network.road(road).length:.2f} m")

    # The editing nodes and their stations along the fitted reference line —
    # what an interactive editor renders as node handles. For roads loaded
    # without rm:waypoints metadata this derives nodes from the geometry
    # records instead.
    nodes = rm.edit.effective_waypoints(network.road(road))
    stations = rm.edit.waypoint_stations(network.road(road))
    print(f"editing nodes: {nodes} at s={[f'{s:.1f}' for s in stations]}")

    # Raise the middle waypoint 3 m — the elevation profile is re-fitted as a
    # smooth C1 cubic through the node heights. The same pure fit is exposed
    # for inspection: fit_elevation_profile(stations, heights) returns the
    # <elevation> records (ascending in s) the command writes.
    heights = [0.0, 3.0, 0.0]
    profile = rm.fit_elevation_profile(list(stations), heights)
    crest = profile[1]
    print(f"elevation records: {len(profile)}; crest z={crest.eval(crest.s):.2f} m")
    stack.push(network, rm.edit.set_node_elevation(network, road, 1, 3.0))

    # Widen the outermost right lane and paint its outer boundary solid at
    # the bold convention width (0.25 m; the standard-weight convention is
    # 0.12 m — OpenDRIVE itself sets no numeric values).
    section = network.road(road).sections[0]
    outer_right = network.lane_section(section).lanes[-1]
    stack.push(network, rm.edit.set_lane_width(network, outer_right, 2.5))
    stack.push(
        network,
        rm.edit.set_road_mark(
            network,
            outer_right,
            rm.RoadMark(s_offset=0.0, type=rm.RoadMarkType.SOLID, width=0.25),
        ),
    )

    print(f"{stack.size} commands recorded")

    # Everything unwinds...
    while stack.can_undo:
        stack.undo(network)
    assert network.road_count == 0

    # ...and replays, byte-identically.
    while stack.can_redo:
        stack.redo(network)
    assert network.road(road).name == "Main Street"

    # Deletion is one undoable command carrying the full referential closure:
    # junction connections referencing the road are removed (and, where it
    # was the incoming road, their connecting roads deleted with it), so a
    # single undo restores every object and link under the original ids.
    stack.push(network, rm.edit.delete_road(network, road))
    assert network.road(road) is None
    stack.undo(network)
    assert network.road(road).name == "Main Street"

    # Snapping queries (kernel-side, pure): interactive tools call this per
    # cursor move. Near a road end the endpoint outranks tangent and grid.
    snap = rm.edit.snap_point(network, (161.0, 0.5), rm.edit.SnapOptions(radius=2.0, grid=1.0))
    assert snap.kind == rm.edit.SnapKind.RoadEndpoint
    print(f"snap near road end: {snap}")

    # While dragging a node of `road`, exclude it so its own (moving)
    # endpoint cannot mask other roads' snap candidates.
    drag_options = rm.edit.SnapOptions(radius=2.0, grid=1.0, exclude_road=road)
    assert rm.edit.snap_point(network, (161.0, 0.5), drag_options).kind == rm.edit.SnapKind.Grid

    # Before shipping a file, run the checker validation. Findings cite
    # normative ASAM rule UIDs; rules that exist only in one version's
    # catalog are cited only when targeting that version — and they never
    # block the save.
    for finding in rm.validate_network(network, target_version=rm.XodrVersion.V1_9_0):
        print(f"validator: {finding}")

    # The writer targets OpenDRIVE 1.8.1 by default; 1.9.0 is selectable.
    rm.save_xodr(network, out_path, "edit_network_example",
                 target_version=rm.XodrVersion.V1_8_1)
    print(f"wrote {out_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
