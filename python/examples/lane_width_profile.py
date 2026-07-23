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

"""Author width that varies along s (OpenDRIVE §11.7.1) and split a lane section.

Builds a straight street, cuts its single lane section in two, and tapers a
lane's width across the cut with cubic <width> records:

    width(ds) = a + b*ds + c*ds^2 + d*ds^3      (ds is SECTION-local)

This is the kernel shape a turn lane is made of: `split_lane_section` decides
where the width is allowed to change, `set_lane_width_profile` decides how.

Note `set_lane_width` is the constant-width convenience and deliberately
REFUSES a lane whose width already varies — flattening an authored taper to a
single number is data loss.

Usage:
    python lane_width_profile.py output.xodr
"""

import sys

import roadmaker as rm


def lane_by_odr_id(network, section_id, odr_id):
    """The lane with this OpenDRIVE id in `section_id`, or None."""
    for lane_id in network.lane_section(section_id).lanes:
        if network.lane(lane_id).odr_id == odr_id:
            return lane_id
    return None


def main() -> int:
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    xodr_path = sys.argv[1]

    network = rm.RoadNetwork()
    road_id = rm.author_clothoid_road(
        network,
        [(0.0, 0.0), (120.0, 0.0)],
        rm.LaneProfile.two_lane_rural(),
        name="Width Profile Demo",
    )

    stack = rm.edit.EditStack()

    # An authored road has exactly one lane section spanning its whole length,
    # so the cross section cannot change. Cut it at s = 60 m: from here the two
    # halves carry independent width profiles.
    stack.push(network, rm.edit.split_lane_section(network, road_id, 60.0))
    sections = network.road(road_id).sections
    print(f"lane sections after the split: {len(sections)}")
    for section_id in sections:
        print(f"  s0 = {network.lane_section(section_id).s0:.1f} m")

    # Widen lane -1 from 3.5 m to 5.0 m across the second section: a linear
    # ramp is b = 1.5 / 60 = 0.025 per metre.
    tail = lane_by_odr_id(network, sections[1], -1)
    stack.push(
        network,
        rm.edit.set_lane_width_profile(network, tail, [rm.Poly3(s=0.0, a=3.5, b=0.025)]),
    )

    # Zero width is legal (width >= 0 is the only rule) and is how a lane
    # tapers in from nothing — give the head section's -2 shoulder a ramp that
    # opens up over its first 20 m.
    head = lane_by_odr_id(network, sections[0], -2)
    if head is not None:
        stack.push(
            network,
            rm.edit.set_lane_width_profile(
                network,
                head,
                [rm.Poly3(s=0.0, a=0.0, b=0.05), rm.Poly3(s=20.0, a=1.0)],
            ),
        )

    for s in (0.0, 30.0, 60.0, 90.0, 119.0):
        section_id = network.section_at(road_id, s)
        local = s - network.lane_section(section_id).s0
        lane_id = lane_by_odr_id(network, section_id, -1)
        widths = network.lane(lane_id).widths
        end = network.section_end(section_id)
        print(
            f"s = {s:5.1f} m  (section [{network.lane_section(section_id).s0:.0f}, {end:.0f}))"
            f"  ->  lane -1 width = {widths[0].eval(local):.3f} m"
        )

    # set_lane_width would flatten the ramp, so the kernel refuses it.
    try:
        stack.push(network, rm.edit.set_lane_width(network, tail, 4.0))
        print("BUG: set_lane_width flattened a tapered lane")
    except ValueError as exc:
        print(f"set_lane_width on a tapered lane refused: {exc}")

    findings = rm.validate_network(network)
    print(f"validator: {len(findings)} findings")

    # Undo everything: apply -> revert is byte-identical by contract.
    while stack.can_undo:
        stack.undo(network)
    print(f"after undo, lane sections: {len(network.road(road_id).sections)}")
    while stack.can_redo:
        stack.redo(network)

    rm.save_xodr(network, xodr_path, name="lane_width_profile demo")
    print(f"wrote {xodr_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
