"""Rotate a whole road about a world pivot (roadmaker.edit.rotate_road).

A rotation is rigid: every plan-view geometry record's start position rotates
about the pivot and the record's heading gains the angle, and authoring
waypoints rotate too. Lengths, lanes, elevation (s-relative) and shape
coefficients (defined in each record's local frame) are unchanged, so undo is
byte-identical. A road-level link to a road that does NOT rotate with it breaks
on both sides, and a road participating in a junction refuses to rotate.

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
    end_before = network.road(road).plan_view.evaluate(network.road(road).plan_view.length())
    stack.push(network, rm.edit.rotate_road(network, road, math.pi / 2.0, 0.0, 0.0))
    end_after = network.road(road).plan_view.evaluate(network.road(road).plan_view.length())
    print(f"end: ({end_before.x:.1f}, {end_before.y:.1f}) -> ({end_after.x:.1f}, {end_after.y:.1f})")

    # Undo is byte-identical; redo re-applies.
    stack.undo(network)
    restored = network.road(road).plan_view.evaluate(network.road(road).plan_view.length())
    assert abs(restored.x - end_before.x) < 1e-6 and abs(restored.y - end_before.y) < 1e-6
    stack.redo(network)

    findings = rm.validate_network(network)
    errors = [f for f in findings if f.severity == rm.Severity.ERROR]
    print(f"validation: {len(findings)} findings, {len(errors)} errors")

    rm.save_xodr(network, out_path, "rotate_road")
    print(f"wrote {out_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
