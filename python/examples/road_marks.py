#!/usr/bin/env python3
"""Author road-mark completions (OpenDRIVE §11.9): color + multi-line geometry.

Puts a true double-yellow (two stripes) on the center line, a double-dashed
(broken broken, Annex A.3.4) yellow line on the left lane, and a dashed white
edge line on the right lane of a straight street, then writes OpenDRIVE and
meshes it.

Usage:
    python road_marks.py output.xodr
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
        [(0.0, 0.0), (100.0, 0.0)],
        rm.LaneProfile.two_lane_rural(),
        name="Road Marks Demo",
    )

    road = network.road(road_id)
    section = network.lane_section(road.sections[0])
    for lane_id in section.lanes:
        lane = network.lane(lane_id)
        if lane.odr_id == 0:
            # Center line: true double-yellow — two stripes 0.2 m apart.
            center = rm.RoadMark(
                type=rm.RoadMarkType.SOLID_SOLID, width=0.3, color=rm.RoadMarkColor.YELLOW
            )
            center.lines = [
                rm.RoadMarkLine(width=0.12, t_offset=0.1),
                rm.RoadMarkLine(width=0.12, t_offset=-0.1),
            ]
            lane.road_marks = [center]
        elif lane.odr_id > 0:
            # Left lane boundary: double-dashed yellow (e.g. a reversible-lane
            # divider) — the "broken broken" family member.
            lane.road_marks = [
                rm.RoadMark(
                    type=rm.RoadMarkType.BROKEN_BROKEN, width=0.24, color=rm.RoadMarkColor.YELLOW
                )
            ]
        else:
            # Edge lines: dashed white.
            lane.road_marks = [
                rm.RoadMark(type=rm.RoadMarkType.BROKEN, width=0.12, color=rm.RoadMarkColor.WHITE)
            ]

    findings = rm.validate_network(network)
    print(f"validator: {len(findings)} findings")

    mesh = rm.build_network_mesh(network)
    print(f"meshed {mesh.road_count} road(s), {mesh.vertex_count} vertices")

    rm.save_xodr(network, xodr_path, name="road_marks demo")
    print(f"wrote {xodr_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
