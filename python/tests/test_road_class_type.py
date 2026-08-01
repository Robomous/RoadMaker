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

"""Authoring stamps a road's OpenDRIVE ``<type>`` from its class (#454).

``realism_defaults.md`` §1.7 binds each of the four road classes to an
``e_roadType`` literal, but nothing applied that binding at authoring time — so
a road drawn as a freeway exported indistinguishable from a local street.
"""

from __future__ import annotations

import pytest
import roadmaker as rm

STRAIGHT = [(0.0, 0.0), (150.0, 0.0)]


def _author(profile, odr_id="1"):
    network = rm.RoadNetwork()
    road = rm.author_clothoid_road(network, STRAIGHT, profile, "", odr_id)
    return network, road


@pytest.mark.parametrize(
    ("profile", "expected"),
    [
        (rm.LaneProfile.freeway(), "motorway"),
        (rm.LaneProfile.arterial(), "townArterial"),
        (rm.LaneProfile.collector(), "townCollector"),
        (rm.LaneProfile.local_road(), "townLocal"),
    ],
)
def test_each_template_stamps_its_classes_type(profile, expected):
    network, road = _author(profile)
    types = network.road(road).types
    assert len(types) == 1
    assert types[0].type == expected
    assert types[0].s == 0.0
    # §1.7: no per-class default speed — inventing one would be a guess.
    assert types[0].speed is None
    assert f'type="{expected}"' in rm.write_xodr(network, "authored")


@pytest.mark.parametrize(
    "road_class",
    [rm.RoadClass.FREEWAY, rm.RoadClass.ARTERIAL, rm.RoadClass.COLLECTOR, rm.RoadClass.LOCAL],
)
def test_every_class_names_a_standard_road_type(road_class):
    assert rm.is_known_road_type(rm.road_type_name(road_class))


def test_a_hand_built_profile_claims_no_class_and_stamps_nothing():
    bespoke = rm.LaneProfile()
    bespoke.right = [rm.LaneSpec()]
    assert bespoke.road_class is None

    network, road = _author(bespoke)
    assert network.road(road).types == []
    assert "<type" not in rm.write_xodr(network, "bespoke")


def test_restyling_rewrites_the_type_and_undo_restores_it():
    network, road = _author(rm.LaneProfile.local_road())
    assert network.road(road).types[0].type == "townLocal"

    stack = rm.edit.EditStack()
    stack.push(network, rm.edit.apply_road_style(network, road, rm.RoadStyle.freeway()))
    assert network.road(road).types[0].type == "motorway"

    stack.undo(network)
    assert network.road(road).types[0].type == "townLocal"


def test_a_classless_style_leaves_the_type_alone():
    """A hand-built style must not erase what the road already declared."""
    network, road = _author(rm.LaneProfile.arterial())
    before = network.road(road).types[0].type

    bespoke = rm.RoadStyle()
    bespoke.right = [rm.StyleLane()]
    assert bespoke.road_class is None

    stack = rm.edit.EditStack()
    stack.push(network, rm.edit.apply_road_style(network, road, bespoke))
    assert network.road(road).types[0].type == before
