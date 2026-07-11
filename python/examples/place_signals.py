#!/usr/bin/env python3
"""Place road signals (OpenDRIVE <signal>, spec chapter 14) on an authored road.

Authors the GS-1 signal set on a straight street — a dynamic traffic light, a
static speed-limit sign, and a pedestrian-crossing warning sign — then
validates and writes OpenDRIVE.

Usage:
    python place_signals.py output.xodr
"""

import sys

import roadmaker as rm


def main() -> int:
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    xodr_path = sys.argv[1]

    network = rm.RoadNetwork()
    road_id = rm.author_clothoid_road(
        network,
        [(0.0, 0.0), (120.0, 0.0)],
        rm.LaneProfile.urban_sidewalk(),
        name="Signal Demo Street",
    )

    # A static speed-limit sign (GS-1: DE type 274, subtype 50). Value + unit
    # travel together — @unit is mandatory whenever @value is set (§14.1).
    speed_limit = rm.Signal()
    speed_limit.odr_id = "1"
    speed_limit.name = "SpeedLimit50"
    speed_limit.s, speed_limit.t = 40.0, -6.0
    speed_limit.z_offset = 1.9
    speed_limit.dynamic = False
    speed_limit.orientation = rm.ObjectOrientation.PLUS
    speed_limit.country, speed_limit.country_revision = "DE", "2017"
    speed_limit.type, speed_limit.subtype = "274", "50"
    speed_limit.value, speed_limit.unit = 50.0, "km/h"
    speed_limit.height, speed_limit.width = 0.77, 0.77
    network.add_signal(road_id, speed_limit)

    # A dynamic traffic light (GS-1: catalog signal, country="OpenDRIVE").
    light = rm.Signal()
    light.odr_id = "2"
    light.name = "TrafficLight"
    light.s, light.t = 110.0, -6.0
    light.z_offset = 3.0
    light.dynamic = True
    light.orientation = rm.ObjectOrientation.PLUS
    light.country, light.country_revision = "OpenDRIVE", "2023"
    light.type, light.subtype = "1000001", "-1"
    light.height, light.width = 1.0, 0.3
    network.add_signal(road_id, light)

    # A pedestrian-crossing warning sign (GS-1: DE type 101, subtype 11).
    crossing = rm.Signal()
    crossing.odr_id = "3"
    crossing.name = "PedestrianCrossing"
    crossing.s, crossing.t = 58.0, -6.0
    crossing.z_offset = 2.0
    crossing.dynamic = False
    crossing.orientation = rm.ObjectOrientation.PLUS
    crossing.country = "DE"
    crossing.type, crossing.subtype = "101", "11"
    network.add_signal(road_id, crossing)

    print(f"placed {network.signal_count} signals on {network.road(road_id)!r}")
    for signal_id in network.signals_of(road_id):
        print(f"  {network.signal(signal_id)!r}")

    findings = rm.validate_network(network)
    signal_findings = [f for f in findings if "signal" in f.rule_id]
    print(f"validator: {len(signal_findings)} signal findings")

    rm.save_xodr(network, xodr_path, name="place_signals demo")
    print(f"wrote {xodr_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
