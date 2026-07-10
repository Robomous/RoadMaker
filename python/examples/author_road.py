#!/usr/bin/env python3
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

    # One driving lane each way, right shoulder, edge + center markings.
    profile = rm.LaneProfile.two_lane_default()

    road_id = rm.author_clothoid_road(network, waypoints, profile, name="Demo Road")
    road = network.road(road_id)
    print(f"authored {road!r} with {road.plan_view.record_count} geometry records")

    # The path is sampleable anywhere along its length:
    mid = road.plan_view.evaluate(road.length / 2)
    print(f"midpoint: {mid!r}")

    rm.save_xodr(network, xodr_path, name="author_road demo")
    print(f"wrote {xodr_path} (valid OpenDRIVE 1.7)")

    mesh = rm.build_network_mesh(network)
    rm.export_glb(mesh, glb_path)
    print(f"wrote {glb_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
