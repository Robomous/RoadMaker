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

"""Merge two adjacent roads into one (roadmaker.edit.merge_roads).

Merge welds road a's END to road b's START into a single road that keeps a's
id (b is erased). The joining ends must already meet (within a centimetre and a
milliradian) with matching lane profiles and elevation — check_mergeable spells
out any reason it can't. Splitting then merging is geometry-identical (the
section boundary survives), so it is a lossless round-trip of the shape.

Run:  python merge_roads.py out.xodr
"""

from __future__ import annotations

import sys

import roadmaker as rm


def main() -> int:
    out_path = sys.argv[1] if len(sys.argv) > 1 else "merge_roads.xodr"

    network = rm.RoadNetwork()
    stack = rm.edit.EditStack()

    # Two straight two-lane roads meeting end-to-start at (100, 0).
    stack.push(
        network,
        rm.edit.create_road([(0.0, 0.0), (100.0, 0.0)], rm.LaneProfile.two_lane_default(), "A"),
    )
    stack.push(
        network,
        rm.edit.create_road([(100.0, 0.0), (200.0, 0.0)], rm.LaneProfile.two_lane_default(), "B"),
    )
    # A distant road (id "3") — kept around to show a refusal.
    stack.push(
        network,
        rm.edit.create_road([(500.0, 0.0), (560.0, 0.0)], rm.LaneProfile.two_lane_default(), "D"),
    )
    a = network.find_road("1")
    b = network.find_road("2")
    d = network.find_road("3")

    # A distant road can't merge — check_mergeable raises with the reason.
    try:
        rm.edit.check_mergeable(network, a, d)
    except ValueError as exc:
        print(f"refused: {exc}")

    # A and B meet end-to-start with matching profiles — this passes and merges.
    rm.edit.check_mergeable(network, a, b)
    stack.push(network, rm.edit.merge_roads(network, a, b))
    print(f"merged: {network.road_count} roads, A is now {network.road(a).length:.1f} m")

    # Undo is byte-identical — B comes back.
    stack.undo(network)
    assert network.road_count == 3

    rm.save_xodr(network, out_path, "merge_roads")
    print(f"wrote {out_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
