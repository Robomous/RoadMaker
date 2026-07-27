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

"""Move whole roads in plan view (roadmaker.edit.translate_road / translate_roads).

A move shifts a road's plan-view geometry and authoring waypoints by (dx, dy);
headings, lengths, lanes, elevation and marks are untouched, so undo is
byte-identical. Moving several roads together is ONE command.

A linked neighbour FOLLOWS: its contacting end is re-fit onto the moved pose so
the joint still satisfies the connection contract (position, heading, elevation
and grade). When the re-fit is impossible the link is cut instead, and the
command names the joint in `follow_records` — a move never leaves a link that
no longer describes the geometry. Roads that participate in a junction have
generated poses and refuse to move.

Run:  python move_road.py out.xodr
"""

from __future__ import annotations

import sys

import roadmaker as rm


def main() -> int:
    out_path = sys.argv[1] if len(sys.argv) > 1 else "move_road.xodr"

    network = rm.RoadNetwork()
    stack = rm.edit.EditStack()

    # Two roads meeting end-to-start at (100, 0) — a chain.
    stack.push(
        network,
        rm.edit.create_road([(0.0, 0.0), (100.0, 0.0)], rm.LaneProfile.two_lane_default(), "A"),
    )
    stack.push(
        network,
        rm.edit.create_road([(100.0, 0.0), (200.0, 0.0)], rm.LaneProfile.two_lane_default(), "B"),
    )
    road_a = network.find_road("1")
    road_b = network.find_road("2")

    # Weld them, so the chain is a real joint rather than two adjacent roads.
    a_end = rm.RoadEnd(road_a, rm.ContactPoint.END)
    b_start = rm.RoadEnd(road_b, rm.ContactPoint.START)
    stack.push(network, rm.edit.close_gap(network, a_end, b_start))

    # Move road A alone by (0, 20). Its start shifts to (0, 20); the road stays
    # the same length and shape — only translated.
    before = network.road(road_a).plan_view.evaluate(0.0)
    stack.push(network, rm.edit.translate_road(network, road_a, 0.0, 20.0))
    after = network.road(road_a).plan_view.evaluate(0.0)
    print(f"A start: ({before.x:.1f}, {before.y:.1f}) -> ({after.x:.1f}, {after.y:.1f})")

    # B was NOT moved, yet the joint survived: its start was re-fit onto A's new
    # end. verify_link_weld measures what the joint actually delivers.
    weld = rm.edit.verify_link_weld(network, a_end)
    print(f"joint after the move: {weld}")
    assert not weld.breaches, "the neighbour should have followed"
    # push() takes ownership of the command, so its report is read off the stack.
    assert not [
        r for r in stack.last_follow_records if r.outcome == rm.edit.FollowOutcome.SEVERED
    ]

    # Undo is byte-identical; redo re-applies.
    stack.undo(network)
    assert network.road(road_a).plan_view.evaluate(0.0).y == before.y
    stack.redo(network)

    # Move BOTH roads together by (10, -5) — one command, one undo step.
    stack.push(network, rm.edit.translate_roads(network, [road_a, road_b], 10.0, -5.0))
    print(f"moved 2 roads together: {network.road_count} roads")

    findings = rm.validate_network(network)
    errors = [f for f in findings if f.severity == rm.Severity.ERROR]
    print(f"validation: {len(findings)} findings, {len(errors)} errors")

    rm.save_xodr(network, out_path, "move_road")
    print(f"wrote {out_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
