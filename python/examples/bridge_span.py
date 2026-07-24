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

"""Author a bridge over a grade-separated crossing (p5-s3, #233).

Two roads cross without a junction; one is raised. RoadMaker detects the
overpass, we author a <bridge> over the raised span, and the deck/pier/abutment/
guardrail solids are generated deterministically at mesh time — never written to
the .xodr, which carries only the standard <bridge> record.
"""

import roadmaker as rm


def main() -> None:
    net = rm.RoadNetwork()
    high = rm.author_clothoid_road(
        net, [(-60.0, 0.0), (60.0, 0.0)], rm.LaneProfile.two_lane_rural(), "", "high"
    )
    rm.author_clothoid_road(
        net, [(0.0, -60.0), (0.0, 60.0)], rm.LaneProfile.two_lane_rural(), "", "low"
    )

    # Raise the east-west road 6 m over the other.
    stack = rm.edit.EditStack()
    length = net.road(high).length
    stack.push(
        net,
        rm.edit.set_elevation_profile(
            net,
            high,
            [rm.edit.ElevationPoint(0.0, 6.0, 0.0), rm.edit.ElevationPoint(length, 6.0, 0.0)],
        ),
    )

    # The crossing is now a grade separation the kernel can find.
    seps = rm.find_grade_separations(net)
    print(f"grade separations found: {len(seps)}")
    sep = seps[0]
    print(f"  clearance under the deck: {sep.clearance:.1f} m")

    # Author a 24 m bridge centred on the crossing and mesh it.
    s = max(0.0, sep.s_upper - 12.0)
    stack.push(net, rm.edit.author_bridge(net, high, s, 24.0))
    mesh = rm.build_network_mesh(net)
    print(f"bridge solids: {mesh.bridge_count}  vertices: {mesh.bridge_vertex_count}")

    # Span-inflation widens the deck (and adds a pier past the free span).
    stack.push(net, rm.edit.set_bridge_span(net, high, 0, s, 44.0))
    wider = rm.build_network_mesh(net)
    print(f"after inflating to 44 m: vertices {wider.bridge_vertex_count}")

    # The record round-trips; the solids do not (they re-derive on load).
    text = rm.write_xodr(net)
    assert "<bridge" in text
    print("the <bridge> record is in the .xodr; the solids are generated, not stored")


if __name__ == "__main__":
    main()
