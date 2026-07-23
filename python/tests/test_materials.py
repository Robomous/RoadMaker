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

"""Lane <material> (§11.8.2) through the bindings: assign, round-trip, refusals."""

import pytest

import roadmaker as rm


@pytest.fixture
def network():
    net = rm.RoadNetwork()
    rm.author_clothoid_road(
        net,
        [(0.0, 0.0), (60.0, 0.0), (120.0, 0.0)],
        rm.LaneProfile.two_lane_default(),
        "Straight",
        "1",
    )
    return net


def lane_by_odr_id(net, section_id, odr_id):
    for lane_id in net.lane_section(section_id).lanes:
        if net.lane(lane_id).odr_id == odr_id:
            return lane_id
    return None


def test_lane_material_defaults_and_keywords():
    material = rm.LaneMaterial(s_offset=5.0, friction=0.8, surface="rm:asphalt")
    assert material.s_offset == pytest.approx(5.0)
    assert material.friction == pytest.approx(0.8)
    assert material.roughness is None
    assert material.surface == "rm:asphalt"


def test_set_lane_material_assigns_and_round_trips(network):
    road = network.find_road("1")
    section = network.road(road).sections[0]
    lane = lane_by_odr_id(network, section, -1)

    stack = rm.edit.EditStack()
    stack.push(
        network,
        rm.edit.set_lane_material(
            network,
            lane,
            [
                rm.LaneMaterial(s_offset=0.0, friction=0.9, roughness=0.012, surface="rm:asphalt"),
                rm.LaneMaterial(s_offset=60.0, friction=0.7, surface="rm:asphalt_worn"),
            ],
        ),
    )

    materials = network.lane(lane).materials
    assert len(materials) == 2
    assert materials[0].surface == "rm:asphalt"
    assert materials[1].surface == "rm:asphalt_worn"
    assert materials[1].roughness is None

    # Save -> reload keeps both records.
    xodr = rm.write_xodr(network)
    reloaded, _ = rm.parse_xodr(xodr)
    reloaded_lane = lane_by_odr_id(reloaded, reloaded.road(reloaded.find_road("1")).sections[0], -1)
    reloaded_materials = reloaded.lane(reloaded_lane).materials
    assert [m.surface for m in reloaded_materials] == ["rm:asphalt", "rm:asphalt_worn"]

    # apply -> revert is byte-identical: undo clears the records.
    before = rm.write_xodr(network)
    stack.undo(network)
    assert not network.lane(lane).materials
    stack.redo(network)
    assert rm.write_xodr(network) == before


def test_set_lane_material_refuses_center_lane(network):
    road = network.find_road("1")
    section = network.road(road).sections[0]
    center = lane_by_odr_id(network, section, 0)
    with pytest.raises(ValueError):
        rm.edit.EditStack().push(
            network,
            rm.edit.set_lane_material(network, center, [rm.LaneMaterial(friction=0.9)]),
        )


def test_set_lane_material_refuses_non_ascending(network):
    road = network.find_road("1")
    section = network.road(road).sections[0]
    lane = lane_by_odr_id(network, section, -1)
    with pytest.raises(ValueError):
        rm.edit.EditStack().push(
            network,
            rm.edit.set_lane_material(
                network,
                lane,
                [
                    rm.LaneMaterial(s_offset=40.0, friction=0.8),
                    rm.LaneMaterial(s_offset=10.0, friction=0.8),
                ],
            ),
        )


def test_road_mark_material_round_trips(network):
    road = network.find_road("1")
    section = network.road(road).sections[0]
    lane = lane_by_odr_id(network, section, -1)

    marks = network.lane(lane).road_marks
    assert marks  # the default profile carries an outer marking
    marks[0].material = "rm:paint_white"
    network.lane(lane).road_marks = marks

    reloaded, _ = rm.parse_xodr(rm.write_xodr(network))
    reloaded_lane = lane_by_odr_id(reloaded, reloaded.road(reloaded.find_road("1")).sections[0], -1)
    assert reloaded_lane is not None
    assert reloaded.lane(reloaded_lane).road_marks[0].material == "rm:paint_white"
