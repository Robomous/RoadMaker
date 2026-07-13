"""Move whole roads in plan view (roadmaker.edit.translate_road / translate_roads).

A move shifts a road's plan-view geometry and authoring waypoints by (dx, dy);
headings, lengths, lanes, elevation and marks are untouched, so undo is
byte-identical. Moving several roads together is ONE command: links *between*
the moved roads survive, while a link leaving the moved set breaks on both
sides. Roads that participate in a junction have generated poses and refuse to
move.

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

    # Move road A alone by (0, 20). Its start shifts to (0, 20); the road stays
    # the same length and shape — only translated.
    before = network.road(road_a).plan_view.evaluate(0.0)
    stack.push(network, rm.edit.translate_road(network, road_a, 0.0, 20.0))
    after = network.road(road_a).plan_view.evaluate(0.0)
    print(f"A start: ({before.x:.1f}, {before.y:.1f}) -> ({after.x:.1f}, {after.y:.1f})")

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
