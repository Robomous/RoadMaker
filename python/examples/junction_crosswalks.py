"""Add crosswalks and stop lines to every arm of a junction.

Uses edit.junction_crosswalks and edit.junction_stop_lines.

This is the kernel geometry behind the editor's junction "Add crosswalks to all
arms" action: for each distinct arm road, derive one <object type="crosswalk">
spanning the arm's driving lanes just inside the junction, then add them as
<object>s. junction_crosswalks is pure (no mutation) — the caller adds the
objects, so an editor can group the adds into one undo step.

Run:  python junction_crosswalks.py out.xodr
"""

from __future__ import annotations

import sys

import roadmaker as rm


def main() -> int:
    out_path = sys.argv[1] if len(sys.argv) > 1 else "junction_crosswalks.xodr"

    network = rm.RoadNetwork()
    stack = rm.edit.EditStack()

    # Three straight two-lane arms meeting near the origin (a T), then a junction.
    for coords in (
        [(-40.0, 0.0), (-6.0, 0.0)],
        [(40.0, 0.0), (6.0, 0.0)],
        [(0.0, -40.0), (0.0, -6.0)],
    ):
        stack.push(network, rm.edit.create_road(coords, rm.LaneProfile.two_lane_default(), ""))
    ends = [
        rm.RoadEnd(network.find_road("1"), rm.ContactPoint.END),
        rm.RoadEnd(network.find_road("2"), rm.ContactPoint.END),
        rm.RoadEnd(network.find_road("3"), rm.ContactPoint.END),
    ]
    stack.push(network, rm.edit.create_junction(network, ends))
    junction = network.find_junction("1")

    # One crosswalk per arm, spanning its driving lanes. Add each as an object;
    # an editor wraps these in a single undo macro.
    crosswalks = rm.edit.junction_crosswalks(network, junction)
    print(f"authoring {len(crosswalks)} crosswalks")
    for road, crosswalk in crosswalks:
        print(
            f"  road {network.road(road).odr_id}: "
            f"s={crosswalk.s:.1f} width(across)={crosswalk.length:.1f} depth={crosswalk.width:.1f}"
        )
        stack.push(network, rm.edit.add_object(network, road, crosswalk))

    # A stop line behind each arm's crosswalk, spanning the approach lanes.
    stop_lines = rm.edit.junction_stop_lines(network, junction)
    print(f"authoring {len(stop_lines)} stop lines")
    for road, stop_line in stop_lines:
        stack.push(network, rm.edit.add_object(network, road, stop_line))

    # A straight arrow on each approach lane, pointing into the junction.
    arrows = rm.edit.junction_lane_arrows(network, junction)
    print(f"authoring {len(arrows)} lane arrows")
    for road, arrow in arrows:
        stack.push(network, rm.edit.add_object(network, road, arrow))

    assert rm.validate_network(network) == []
    rm.save_xodr(network, out_path, "junction_crosswalks_example")
    print(f"wrote {out_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
