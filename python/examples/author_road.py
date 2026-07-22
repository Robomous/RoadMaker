#!/usr/bin/env python3
# Copyright 2026 Robomous
# SPDX-License-Identifier: Apache-2.0

"""Author a clothoid road through waypoints, then write OpenDRIVE + glTF.

Usage:
    python author_road.py output.xodr output.glb
"""

import sys

import roadmaker as rm


def main() -> int:
    if len(sys.argv) != 3:
        print(__doc__)
        return 2
    xodr_path, glb_path = sys.argv[1], sys.argv[2]

    network = rm.RoadNetwork()

    # An S-curve through five plan-view waypoints (meters).
    waypoints = [(0.0, 0.0), (60.0, 8.0), (110.0, 45.0), (150.0, 100.0), (140.0, 160.0)]

    # Cross-section templates (the editor's Create Road dropdown offers the
    # same three): two_lane_rural, urban_sidewalk, highway.
    profile = rm.LaneProfile.two_lane_rural()

    road_id = rm.author_clothoid_road(network, waypoints, profile, name="Demo Road")
    road = network.road(road_id)
    print(f"authored {road!r} with {road.plan_view.record_count} geometry records")

    # The path is sampleable anywhere along its length:
    mid = road.plan_view.evaluate(road.length / 2)
    print(f"midpoint: {mid!r}")

    # Chain a second road G1-tangentially off the first one's end by locking
    # the fit's start heading to the end pose (what the editor's tangent
    # snapping does on a road-end click).
    end = road.plan_view.evaluate(road.length)
    chained = rm.author_clothoid_road(
        network,
        [(end.x, end.y), (end.x - 70.0, end.y + 50.0)],
        rm.LaneProfile.urban_sidewalk(),
        name="Chained Road",
        start_heading=end.hdg,
    )
    start = network.road(chained).plan_view.evaluate(0.0)
    print(f"chained road starts at heading {start.hdg:.6f} (locked to {end.hdg:.6f})")

    rm.save_xodr(network, xodr_path, name="author_road demo")
    print(f"wrote {xodr_path} (valid OpenDRIVE 1.8)")

    mesh = rm.build_network_mesh(network)
    rm.export_glb(mesh, glb_path)
    print(f"wrote {glb_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
