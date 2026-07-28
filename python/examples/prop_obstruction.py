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

"""Props that a road move drove somewhere they do not belong (cascade-s4, #464).

A placed prop stores no world pose: its transform is derived from its anchor
road's frame every time. So a prop goes where its road goes — which is correct,
and which is exactly what can drive one into another road, a junction floor, or
another prop.

The move REPORTS that and never corrects it. Two fixes are offered, and both
have to be asked for: relocate the prop somewhere clear on its own road, or
re-anchor it to the road it now sits beside.
"""

import math

import roadmaker as rm


def tree(net, road, s, t, odr_id):
    obj = rm.Object()
    obj.odr_id = odr_id
    obj.name = "tree_pine"
    obj.type = rm.ObjectType.TREE
    obj.s = s
    obj.t = t
    obj.radius = 1.5
    obj.height = 4.0
    return net.add_object(road, obj)


def main() -> None:
    net = rm.RoadNetwork()
    anchor = rm.author_clothoid_road(
        net, [(-50.0, 40.0), (50.0, 40.0)], rm.LaneProfile.two_lane_rural(), "", "anchor"
    )
    rm.author_clothoid_road(
        net, [(-50.0, 0.0), (50.0, 0.0)], rm.LaneProfile.two_lane_rural(), "", "crossed"
    )
    # 20 m south of its anchor road, i.e. world (0, 20) — well clear of the
    # road at y = 0.
    prop = tree(net, anchor, 50.0, -20.0, "1")
    print("obstructions at rest:", rm.find_prop_obstructions(net))

    stack = rm.edit.EditStack()

    # Carry the anchor road 20 m south. Not one datum of the object changes —
    # the prop rides the road's frame straight into the other carriageway.
    stack.push(net, rm.edit.translate_road(net, anchor, 0.0, -20.0))
    for record in stack.last_obstruction_records:
        print(f"prop {record.object_odr_id}: {record.detail}")

    # The move fixed nothing, on purpose.
    print("still obstructed:", len(rm.find_prop_obstructions(net)))

    # Fix 1 — relocate. One undoable command; a prop with nowhere clear to go is
    # left exactly as authored, and the whole thing is REFUSED when there is
    # nothing to do, so a caller can say so rather than push an empty entry.
    stack.push(net, rm.edit.relocate_obstructed_props(net))
    print("after relocating:", rm.find_prop_obstructions(net))
    stack.undo(net)

    # Fix 2 — re-anchor. The prop does not move a millimetre: it keeps its world
    # position, heading and height, and changes which road carries it. Note the
    # honest caveat — a prop re-anchored INTO the road it is standing in stops
    # being reported, because a prop is never flagged against its own anchor
    # road. That is a placement fix, not an obstruction fix.
    crossed = net.find_road("crossed")
    stack.push(net, rm.edit.reanchor_object(net, prop, crossed))
    print("after re-anchoring:", rm.find_prop_obstructions(net))
    print("the object id is unchanged:", net.object(prop) is not None)

    # The rotation arc #338 deferred here by name: a prop at large |t| sweeps a
    # circle about the pivot, so a half turn can carry it far further than the
    # road itself moves.
    arc = rm.RoadNetwork()
    spun = rm.author_clothoid_road(
        arc, [(-50.0, 40.0), (50.0, 40.0)], rm.LaneProfile.two_lane_rural(), "", "spun"
    )
    rm.author_clothoid_road(
        arc, [(-50.0, 15.0), (50.0, 15.0)], rm.LaneProfile.two_lane_rural(), "", "below"
    )
    tree(arc, spun, 50.0, 25.0, "1")  # world (0, 65)
    arc_stack = rm.edit.EditStack()
    arc_stack.push(arc, rm.edit.rotate_road(arc, spun, math.pi, 0.0, 40.0))
    for record in arc_stack.last_obstruction_records:
        print(f"#338 arc — prop {record.object_odr_id}: {record.detail}")


if __name__ == "__main__":
    main()
