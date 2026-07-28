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

"""Prop obstruction through the bindings (cascade-s4, #464)."""

import pytest

import roadmaker as rm


def _tree(net, road, s, t, odr_id="1", radius=1.5):
    obj = rm.Object()
    obj.odr_id = odr_id
    obj.name = "tree_pine"
    obj.type = rm.ObjectType.TREE
    obj.s = s
    obj.t = t
    obj.radius = radius
    obj.height = 4.0
    return net.add_object(road, obj)


def _two_roads_and_a_tree():
    """An anchor road carrying a tree 20 m south of it, and a road 40 m south."""
    net = rm.RoadNetwork()
    anchor = rm.author_clothoid_road(
        net, [(-50.0, 40.0), (50.0, 40.0)], rm.LaneProfile.two_lane_rural(), "", "anchor"
    )
    crossed = rm.author_clothoid_road(
        net, [(-50.0, 0.0), (50.0, 0.0)], rm.LaneProfile.two_lane_rural(), "", "crossed"
    )
    prop = _tree(net, anchor, 50.0, -20.0)  # world (0, 20): clear of everything
    return net, anchor, crossed, prop


def test_a_clear_scene_reports_nothing():
    net, _anchor, _crossed, _prop = _two_roads_and_a_tree()
    assert rm.find_prop_obstructions(net) == []


def test_a_prop_driven_into_a_road_is_flagged_and_names_what_it_hit():
    net, anchor, crossed, prop = _two_roads_and_a_tree()
    stack = rm.edit.EditStack()
    stack.push(net, rm.edit.translate_road(net, anchor, 0.0, -20.0))

    found = rm.find_prop_obstructions(net)
    assert len(found) == 1
    assert found[0].object == prop
    assert found[0].kind == rm.ObstructionKind.ROAD_SURFACE
    assert found[0].road == crossed

    records = stack.last_obstruction_records
    assert len(records) == 1
    assert records[0].object_odr_id == "1"
    assert "crossed" in records[0].detail


def test_a_prop_is_never_flagged_against_its_own_anchor_road():
    """A median tree, a bollard and a kerbside streetlight are placements."""
    net = rm.RoadNetwork()
    road = rm.author_clothoid_road(
        net, [(-50.0, 0.0), (50.0, 0.0)], rm.LaneProfile.two_lane_rural(), "", "own"
    )
    _tree(net, road, 50.0, 0.0, radius=0.6)  # dead centre of its own carriageway
    assert rm.find_prop_obstructions(net) == []


def test_a_prop_with_no_bounding_volume_is_not_checked_and_nothing_is_invented():
    net, anchor, _crossed, prop = _two_roads_and_a_tree()
    net.object(prop).t = -40.0  # world (0, 0): in the crossed road
    assert len(rm.find_prop_obstructions(net)) == 1

    before = rm.write_xodr(net)
    net.object(prop).radius = None
    net.object(prop).height = None
    after_clearing = rm.write_xodr(net)

    assert rm.find_prop_obstructions(net) == []
    assert rm.write_xodr(net) == after_clearing, "the query writes nothing anywhere"
    assert before != after_clearing, "the fixture really did clear the dimensions"


def test_an_already_obstructed_prop_is_not_reported_again_by_a_later_nudge():
    """Only obstructions the gesture CREATED are its doing."""
    net, anchor, _crossed, prop = _two_roads_and_a_tree()
    net.object(prop).t = -40.0
    assert len(rm.find_prop_obstructions(net)) == 1

    stack = rm.edit.EditStack()
    stack.push(net, rm.edit.translate_road(net, anchor, 0.01, 0.0))
    assert stack.last_obstruction_records == []
    assert len(rm.find_prop_obstructions(net)) == 1, "still obstructed, just not newly"


def test_the_rotation_arc_of_338_is_reported():
    """A prop at large |t| sweeps a wide arc when its anchor road rotates."""
    net = rm.RoadNetwork()
    anchor = rm.author_clothoid_road(
        net, [(-50.0, 40.0), (50.0, 40.0)], rm.LaneProfile.two_lane_rural(), "", "anchor"
    )
    rm.author_clothoid_road(
        net, [(-50.0, 15.0), (50.0, 15.0)], rm.LaneProfile.two_lane_rural(), "", "crossed"
    )
    _tree(net, anchor, 50.0, 25.0)  # world (0, 65)
    assert rm.find_prop_obstructions(net) == []

    # Half a turn about the road's own midpoint: the road maps onto itself, the
    # prop sweeps a 25 m arc down onto the crossed road.
    stack = rm.edit.EditStack()
    stack.push(net, rm.edit.rotate_road(net, anchor, 3.141592653589793, 0.0, 40.0))
    assert len(stack.last_obstruction_records) == 1


def test_relocating_is_the_offered_fix_and_undo_is_byte_identical():
    net, anchor, _crossed, _prop = _two_roads_and_a_tree()
    stack = rm.edit.EditStack()
    stack.push(net, rm.edit.translate_road(net, anchor, 0.0, -20.0))
    assert len(rm.find_prop_obstructions(net)) == 1

    before = rm.write_xodr(net)
    stack.push(net, rm.edit.relocate_obstructed_props(net))
    assert rm.find_prop_obstructions(net) == []

    stack.undo(net)
    assert rm.write_xodr(net) == before


def test_relocate_refuses_when_nothing_is_obstructed():
    """A refusal, not an empty undo entry — remove_orphaned_bridges' contract."""
    net, _anchor, _crossed, _prop = _two_roads_and_a_tree()
    stack = rm.edit.EditStack()
    with pytest.raises(ValueError):
        stack.push(net, rm.edit.relocate_obstructed_props(net))


def test_reanchor_preserves_the_object_id_and_refuses_the_owner():
    net, anchor, crossed, prop = _two_roads_and_a_tree()

    stack = rm.edit.EditStack()
    with pytest.raises(ValueError):
        stack.push(net, rm.edit.reanchor_object(net, prop, anchor))

    before = rm.write_xodr(net)
    stack.push(net, rm.edit.reanchor_object(net, prop, crossed))
    assert net.object(prop) is not None, "the id survives — that is the whole point"
    assert net.object(prop).road == crossed

    stack.undo(net)
    assert rm.write_xodr(net) == before


def test_the_narrowed_query_sees_both_directions_of_a_move():
    net, anchor, crossed, _prop = _two_roads_and_a_tree()
    net.object(_prop).t = -40.0
    everything = rm.find_prop_obstructions(net)
    assert len(everything) == 1

    assert rm.find_prop_obstructions_touching(net, [anchor]) == everything
    assert rm.find_prop_obstructions_touching(net, [crossed]) == everything
