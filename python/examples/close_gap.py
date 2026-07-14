"""Close the gap between two free road ends (roadmaker.edit.close_gap).

close_gap welds two free ends together in one undoable command: a pure link
when they nearly coincide, or a single-lane G1 connector road when a real gap
separates them. check_linkable spells out any reason it can't (an end already
linked or owned by a junction, or the ends too far apart). This is the kernel
side of the editor's "Link Ends" affordance (gate-extension WS-2).

Run:  python close_gap.py out.xodr
"""

from __future__ import annotations

import sys

import roadmaker as rm


def main() -> int:
    out_path = sys.argv[1] if len(sys.argv) > 1 else "close_gap.xodr"

    network = rm.RoadNetwork()
    stack = rm.edit.EditStack()

    # Two straight roads with a 30 m gap between A's END (100, 0) and B's START
    # (130, 0), plus a distant road to show a refusal.
    stack.push(
        network,
        rm.edit.create_road([(0.0, 0.0), (100.0, 0.0)], rm.LaneProfile.two_lane_default(), "A"),
    )
    stack.push(
        network,
        rm.edit.create_road([(130.0, 0.0), (230.0, 0.0)], rm.LaneProfile.two_lane_default(), "B"),
    )
    stack.push(
        network,
        rm.edit.create_road([(0.0, 500.0), (100.0, 500.0)], rm.LaneProfile.two_lane_default(), "F"),
    )
    a = network.find_road("1")
    b = network.find_road("2")
    far = network.find_road("3")

    a_end = rm.RoadEnd(a, rm.ContactPoint.END)
    b_start = rm.RoadEnd(b, rm.ContactPoint.START)
    far_start = rm.RoadEnd(far, rm.ContactPoint.START)

    # A distant end can't be linked — check_linkable raises with the reason.
    try:
        rm.edit.check_linkable(network, a_end, far_start)
    except ValueError as exc:
        print(f"refused: {exc}")

    # A and B have a real gap — close_gap bridges it with a connector road.
    rm.edit.check_linkable(network, a_end, b_start)
    before = network.road_count
    stack.push(network, rm.edit.close_gap(network, a_end, b_start))
    print(f"closed: {network.road_count} roads (added {network.road_count - before} connector)")

    # Undo is byte-identical — the connector road disappears again.
    stack.undo(network)
    assert network.road_count == before

    # Re-close and save.
    stack.push(network, rm.edit.close_gap(network, a_end, b_start))

    # create_linked_road authors a road AND welds its start to a free end in one
    # command — the Create Road tangent-continuation snap. Here a new road
    # continues from B's END, linked in a single undo step.
    b_end = rm.RoadEnd(b, rm.ContactPoint.END)
    stack.push(
        network,
        rm.edit.create_linked_road(
            network, [(230.0, 0.0), (330.0, 0.0)], rm.LaneProfile.two_lane_rural(), "C", b_end
        ),
    )
    print(f"linked road: {network.road_count} roads")

    rm.save_xodr(network, out_path, "close_gap")
    print(f"wrote {out_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
