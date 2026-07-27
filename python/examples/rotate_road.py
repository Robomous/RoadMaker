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

"""Rotate roads about a world pivot (roadmaker.edit.rotate_road / rotate_roads).

A rotation is rigid: every plan-view geometry record's start position rotates
about the pivot and the record's heading gains the angle, and authoring
waypoints rotate too. Lengths, lanes, elevation (s-relative) and shape
coefficients (defined in each record's local frame) are unchanged, so undo is
byte-identical. A road-level link to a road that does NOT rotate with it swings
that road's contacting end round too, so the joint survives.

Junctions follow one level up. Rotating a single ARM regenerates its junction
from the arm's new pose; rotating EVERY arm in one `rotate_roads` call carries
the junction rigidly, connecting roads included, so the turns are preserved
exactly rather than replanned. Only a junction's generated CONNECTING road
refuses to rotate.

Run:  python rotate_road.py out.xodr
"""

from __future__ import annotations

import math
import sys

import roadmaker as rm


def main() -> int:
    out_path = sys.argv[1] if len(sys.argv) > 1 else "rotate_road.xodr"

    network = rm.RoadNetwork()
    stack = rm.edit.EditStack()

    # A straight road from the origin along +x.
    stack.push(
        network,
        rm.edit.create_road([(0.0, 0.0), (100.0, 0.0)], rm.LaneProfile.two_lane_default(), "A"),
    )
    road = network.find_road("1")

    # Rotate it +90° about its start (0, 0): the far end swings from (100, 0)
    # to (0, 100) and the heading advances by pi/2.
    end_before = network.road(road).plan_view.evaluate(network.road(road).plan_view.length)
    stack.push(network, rm.edit.rotate_road(network, road, math.pi / 2.0, 0.0, 0.0))
    end_after = network.road(road).plan_view.evaluate(network.road(road).plan_view.length)
    print(f"end: ({end_before.x:.1f}, {end_before.y:.1f}) -> ({end_after.x:.1f}, {end_after.y:.1f})")

    # Undo is byte-identical; redo re-applies.
    stack.undo(network)
    restored = network.road(road).plan_view.evaluate(network.road(road).plan_view.length)
    assert abs(restored.x - end_before.x) < 1e-6 and abs(restored.y - end_before.y) < 1e-6
    stack.redo(network)

    # A whole junction, turned rigidly. Two arms stopping short of (200, 0) leave
    # the generator room to plan turns; passing BOTH of them to rotate_roads
    # makes the gesture a rigid body motion, so the connecting roads ride along
    # unchanged instead of being replanned (cascade-s2).
    stack.push(
        network,
        rm.edit.create_road([(300.0, 0.0), (212.0, 0.0)], rm.LaneProfile.two_lane_default(), "B"),
    )
    stack.push(
        network,
        rm.edit.create_road([(200.0, 100.0), (200.0, 12.0)], rm.LaneProfile.two_lane_default(), "C"),
    )
    arms = [network.find_road("2"), network.find_road("3")]
    ends = [rm.RoadEnd(arm, rm.ContactPoint.END) for arm in arms]
    stack.push(network, rm.edit.create_junction(network, ends))
    junction = network.junction_ids[-1]
    turns_before = len(network.junction(junction).connections)

    stack.push(network, rm.edit.rotate_roads(network, arms, math.pi / 6.0, 200.0, 0.0))
    welds = rm.edit.verify_junction_welds(network, junction)
    print(
        f"junction: {turns_before} turns -> {len(network.junction(junction).connections)}, "
        f"welds breach: {welds.breaches}"
    )

    findings = rm.validate_network(network)
    errors = [f for f in findings if f.severity == rm.Severity.ERROR]
    print(f"validation: {len(findings)} findings, {len(errors)} errors")

    rm.save_xodr(network, out_path, "rotate_road")
    print(f"wrote {out_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
