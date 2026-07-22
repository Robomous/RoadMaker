# Copyright 2026 Robomous
# SPDX-License-Identifier: Apache-2.0

"""Generate a common junction from road ends (roadmaker.edit.create_junction).

Selecting two or more road ends and generating a junction is the kernel work
behind the editor's Create Junction tool: for every permitted turn the
generator fits a G1 clothoid connecting road (built in driving direction,
contactPoint="start") with a single blended-width lane, and records a
<connection>/<laneLink> per (incoming lane, outgoing lane) pair. The arm list
is stored on the junction so an incoming-road edit can regenerate it.

Run:  python create_junction.py out.xodr
"""

from __future__ import annotations

import sys

import roadmaker as rm


def main() -> int:
    out_path = sys.argv[1] if len(sys.argv) > 1 else "junction.xodr"

    network = rm.RoadNetwork()
    stack = rm.edit.EditStack()

    # Three straight two-lane arms whose ends meet near the origin (a T).
    # create_road auto-assigns ids 1, 2, 3 in creation order.
    for coords in (
        [(-40.0, 0.0), (-6.0, 0.0)],   # west arm
        [(40.0, 0.0), (6.0, 0.0)],     # east arm
        [(0.0, -40.0), (0.0, -6.0)],   # south arm
    ):
        stack.push(
            network,
            rm.edit.create_road(coords, rm.LaneProfile.two_lane_default(), ""),
        )
    ends = [
        rm.RoadEnd(network.find_road("1"), rm.ContactPoint.END),
        rm.RoadEnd(network.find_road("2"), rm.ContactPoint.END),
        rm.RoadEnd(network.find_road("3"), rm.ContactPoint.END),
    ]

    # Preview first — exactly what generation will produce (status-bar feedback
    # for an interactive tool). Turns whose fit loops are reported, not built.
    preview = rm.edit.preview_junction(network, ends)
    print(f"preview: {preview.connection_count} connections")
    for dropped in preview.dropped_turns:
        print(f"  dropped: {dropped}")

    # Generate. One undoable command creates the junction, its connecting
    # roads, the connection table and the arm links.
    stack.push(network, rm.edit.create_junction(network, ends))
    junction = network.find_junction("1")
    connections = network.junction(junction).connections
    print(f"junction '{network.junction(junction).odr_id}': {len(connections)} connections")
    for connection in connections:
        incoming = network.road(connection.incoming_road).odr_id
        connecting = network.road(connection.connecting_road).odr_id
        links = ", ".join(f"{a}->{b}" for a, b in connection.lane_links)
        print(f"  incoming {incoming} -> connecting {connecting} (laneLink {links})")

    # Editing an incoming road regenerates the junction from the recorded
    # arms: the connecting-road endpoints track the moved arm, ids preserved.
    stack.push(network, rm.edit.move_waypoint(network, network.find_road("1"), 1, (-8.0, -1.0)))
    stack.push(network, rm.edit.regenerate_junction(network, junction))
    print(f"after regenerate: still {len(network.junction(junction).connections)} connections")

    # The generated output validates with zero diagnostics, and the arm list
    # round-trips through <userData code="rm:arms">.
    assert rm.validate_network(network) == []
    rm.save_xodr(network, out_path, "create_junction_example")
    print(f"wrote {out_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
