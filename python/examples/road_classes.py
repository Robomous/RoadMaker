#!/usr/bin/env python3

# Copyright 2026 Robomous
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

"""The four road-class presets (#413, docs/domain/realism_defaults.md §1.2).

Creates one road per class — freeway, arterial, collector, local street —
from the LaneProfile templates, then re-dresses the collector with the
arterial RoadStyle (sidewalks, dashed white lane lines, double yellow
center) to show that templates create and styles re-dress. Every width
comes from the realism-defaults registry; the pre-#413 names
(two_lane_rural, urban_sidewalk, highway, urban_two_lane) remain as
aliases of their nearest class.

Usage:
    python road_classes.py output.xodr
"""

import sys

import roadmaker as rm


def main() -> int:
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    xodr_path = sys.argv[1]

    network = rm.RoadNetwork()
    stack = rm.edit.EditStack()

    classes = [
        ("Freeway", rm.LaneProfile.freeway()),
        ("Arterial", rm.LaneProfile.arterial()),
        ("Collector", rm.LaneProfile.collector()),
        ("Local Street", rm.LaneProfile.local_road()),
    ]
    roads = []
    for row, (name, profile) in enumerate(classes):
        y = 40.0 * row
        stack.push(network, rm.edit.create_road([(0.0, y), (150.0, y)], profile, name))
        road_id = network.find_road(str(row + 1))  # auto odr ids count up from 1
        roads.append(road_id)
        section = network.road(road_id).sections[0]
        lanes = network.lane_section(section).lanes
        print(f"{name}: {len(lanes) - 1} lanes (plus center)")

    # A style is a delta applied to an existing road: promote the collector
    # to the arterial cross section and markings in one undoable command.
    stack.push(network, rm.edit.apply_road_style(network, roads[2], rm.RoadStyle.arterial()))

    rm.write_xodr(network, xodr_path)
    print(f"wrote {xodr_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
