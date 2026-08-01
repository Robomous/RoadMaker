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

"""Lane links are lists, and carry ``@layer`` (§11.6, #536).

``<predecessor>``/``<successor>`` are multiplicity 0..* and the standard
*mandates* several where a lane splits abruptly. The reader kept only the first
of each, silently, so a spec-required split was halved on every round trip.
"""

from __future__ import annotations

import pathlib

import pytest
import roadmaker as rm

CORPUS = (
    pathlib.Path(__file__).resolve().parents[2]
    / "core"
    / "tests"
    / "fuzz"
    / "corpus"
    / "lane_link_multi.xodr"
)


@pytest.fixture
def network():
    loaded, _ = rm.load_xodr(str(CORPUS))
    return loaded


def _lane(network, section_index, lane_odr_id):
    road = network.road(network.find_road("1"))
    section = network.lane_section(road.sections[section_index])
    for lane_id in section.lanes:
        lane = network.lane(lane_id)
        if lane.odr_id == lane_odr_id:
            return lane
    raise AssertionError(f"lane {lane_odr_id} missing from section {section_index}")


def test_a_splitting_lane_keeps_both_successors(network):
    splitting = _lane(network, 0, -1)
    assert [link.id for link in splitting.successors] == [-1, -2]


def test_the_scalar_accessor_still_answers_the_first(network):
    """What the old field held, so existing consumers read unchanged."""
    splitting = _lane(network, 0, -1)
    assert splitting.successor == -1
    assert splitting.predecessor is None


def test_layer_is_kept_verbatim_and_an_absent_one_stays_absent(network):
    layered = _lane(network, 0, -3)
    assert [link.layer for link in layered.successors] == ["temporary", "permanent"]
    assert _lane(network, 0, -1).successors[0].layer == ""


def test_the_round_trip_reproduces_every_link(network):
    written = rm.write_xodr(network, "lane_link_multi")
    assert written.count("<successor") == 4
    assert written.count("<predecessor") == 4
    assert 'layer="temporary"' in written

    reloaded, _ = rm.parse_xodr(written)
    assert [link.id for link in _lane(reloaded, 0, -1).successors] == [-1, -2]
    assert rm.write_xodr(reloaded, "lane_link_multi") == written
