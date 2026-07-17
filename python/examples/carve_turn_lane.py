#!/usr/bin/env python3
"""Carve a tapering turn lane approaching the road's end (OpenDRIVE §11.7.1).

Builds a straight two-lane street and carves a right-hand turn lane through
`carve_lane`: the lane's width ramps 0 -> full over the DRAGGED span
[s_start, s_end] and then holds full width to the road terminus, where — in the
editor — junction regeneration absorbs it. In kernel terms a carve is

    split_lane_section(s_start) + insert_lane(final section) + a 0 -> full
    width ramp,

the same p2-s1 primitives Lane Add and Lane Form compose.

`lane_boundary_offsets` reports the lateral t of every lane edge at a station —
the same routine the mesher uses — which is how an editor resolves "which lane
boundary is under the cursor" before a carve.

Usage:
    python carve_turn_lane.py output.xodr
"""

import sys

import roadmaker as rm


def lane_by_odr_id(network, section_id, odr_id):
    """The lane with this OpenDRIVE id in `section_id`, or None."""
    for lane_id in network.lane_section(section_id).lanes:
        if network.lane(lane_id).odr_id == odr_id:
            return lane_id
    return None


def width_at(lane, local_s):
    """Piecewise width: the last record starting at or before local_s, evaluated
    in that record's own frame (ds measured from its sOffset)."""
    record = None
    for w in lane.widths:
        if w.s <= local_s + 1e-9:
            record = w
    return record.eval(local_s - record.s) if record is not None else 0.0


def main() -> int:
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    xodr_path = sys.argv[1]

    network = rm.RoadNetwork()
    road_id = rm.author_clothoid_road(
        network,
        [(0.0, 0.0), (120.0, 0.0)],
        rm.LaneProfile.two_lane_rural(),
        name="Turn Lane Demo",
    )

    # The lane edges across the cross section, leftmost first. For two_lane_rural
    # (+1 | centre | -1 | -2 shoulder) at lane offset 0 these are 3.5, 0, -3.5,
    # -4.5 — exactly where an editor would snap a boundary pick.
    print("lane boundaries at s = 60:", [round(t, 2) for t in network.lane_boundary_offsets(road_id, 60.0)])

    stack = rm.edit.EditStack()

    # Carve a right turn lane: taper it in over [60, 100], holding full width to
    # the terminus at 120. `at_odr_id = -1` inserts it at the -1 position on the
    # right side (the through lane and shoulder step outward).
    stack.push(
        network,
        rm.edit.carve_lane(network, road_id, -1, 60.0, 100.0, -1, rm.LaneType.DRIVING),
    )

    sections = network.road(road_id).sections
    print(f"lane sections after the carve: {len(sections)}")  # one split at s_start
    turn = lane_by_odr_id(network, sections[-1], -1)
    s0 = network.lane_section(sections[-1]).s0
    for s in (60.0, 80.0, 100.0, 119.0):
        lane = network.lane(turn)
        print(f"s = {s:5.1f} m  ->  turn lane width = {width_at(lane, s - s0):.3f} m")

    findings = rm.validate_network(network)
    print(f"validator: {len(findings)} findings")

    # apply -> revert is byte-identical by contract.
    while stack.can_undo:
        stack.undo(network)
    print(f"after undo, lane sections: {len(network.road(road_id).sections)}")
    while stack.can_redo:
        stack.redo(network)

    rm.save_xodr(network, xodr_path, name="carve_turn_lane demo")
    print(f"wrote {xodr_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
