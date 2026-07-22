#!/usr/bin/env python3
# Copyright 2026 Robomous
# SPDX-License-Identifier: Apache-2.0

"""Apply a road style to an existing road (OpenDRIVE §9, §11.9).

A road style is a serializable cross-section applied to an EXISTING road: it
replaces the lane profile and boundary road marks with the style's, flattening
the road to a single lane section, while preserving everything orthogonal to
the cross section — the reference-line geometry, the elevation and
superelevation profiles, the road's name and links, and any placed objects and
signals. This is the kernel behind dropping a road style from the editor's
Library onto a road.

Usage:
    python apply_road_style.py output.xodr
"""

import sys

import roadmaker as rm


def lane_marks(network, road_id):
    """The (odr_id, mark type) of every marked lane boundary in the first
    section, leftmost first — what the style painted."""
    section = network.lane_section(network.road(road_id).sections[0])
    out = []
    for lane_id in section.lanes:
        lane = network.lane(lane_id)
        if lane.road_marks:
            out.append((lane.odr_id, lane.road_marks[0].type, lane.road_marks[0].color))
    return out


def main() -> int:
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    xodr_path = sys.argv[1]

    network = rm.RoadNetwork()
    road_id = rm.author_clothoid_road(
        network,
        [(0.0, 0.0), (60.0, 0.0), (120.0, 20.0)],
        rm.LaneProfile.two_lane_rural(),
        name="Elm Street",
    )

    before = network.road(road_id).sections
    length = network.road(road_id).length
    print(f"before: {len(before)} section(s), name={network.road(road_id).name!r}")

    stack = rm.edit.EditStack()
    stack.push(
        network,
        rm.edit.apply_road_style(network, road_id, rm.RoadStyle.urban_two_lane()),
    )

    road = network.road(road_id)
    print(f"after:  {len(road.sections)} section, name={road.name!r}")
    print(f"        reference line preserved: length {road.length:.1f} m (was {length:.1f})")
    print(f"        marked boundaries: {lane_marks(network, road_id)}")

    findings = rm.validate_network(network)
    print(f"validator: {len(findings)} findings")

    # apply -> undo is byte-identical by contract: the original profile returns.
    stack.undo(network)
    print(f"after undo: {len(network.road(road_id).sections)} section(s)")
    stack.redo(network)

    rm.save_xodr(network, xodr_path, name="apply_road_style demo")
    print(f"wrote {xodr_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
