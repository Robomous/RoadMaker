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

"""Insert a bend node without reshaping the road (roadmaker.edit.insert_node_at).

Adding a node at a station pins the heading at every node from the current
curve, so the re-fit reproduces the road's shape (the covering record just
splits in two) — unlike a naive waypoint insert, which reflows the whole
chain. The new node then gives you a handle to bend the road at that point.

Run:  python insert_bend.py out.xodr
"""

from __future__ import annotations

import sys

import roadmaker as rm


def main() -> int:
    out_path = sys.argv[1] if len(sys.argv) > 1 else "insert_bend.xodr"

    network = rm.RoadNetwork()
    stack = rm.edit.EditStack()

    # A gently curved road with two authored nodes.
    stack.push(
        network,
        rm.edit.create_road(
            [(0.0, 0.0), (60.0, 20.0), (120.0, 0.0)],
            rm.LaneProfile.two_lane_default(),
            "Curved",
        ),
    )
    road = network.find_road("1")

    # Sample the shape before inserting.
    length = network.road(road).length
    sampled_before = [network.road(road).plan_view.evaluate(s) for s in (10.0, 40.0, 90.0)]
    nodes_before = len(network.road(road).authoring_waypoints)

    # Insert a bend node a quarter of the way along. The shape is preserved.
    stack.push(network, rm.edit.insert_node_at(network, road, length * 0.25))
    nodes_after = len(network.road(road).authoring_waypoints)
    print(f"nodes: {nodes_before} -> {nodes_after}")

    for before, s in zip(sampled_before, (10.0, 40.0, 90.0)):
        after = network.road(road).plan_view.evaluate(s)
        assert abs(after.x - before.x) < 1e-3 and abs(after.y - before.y) < 1e-3, "shape drifted"
    print("shape preserved at every sampled station")

    # A node too close to an existing one (< 2 m) is refused on push.
    try:
        stack.push(network, rm.edit.insert_node_at(network, road, 0.5))
    except ValueError as exc:
        print(f"refused near-duplicate node: {exc}")

    rm.save_xodr(network, out_path, "insert_bend")
    print(f"wrote {out_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
