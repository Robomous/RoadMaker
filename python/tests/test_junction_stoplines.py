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

"""Junction stop lines through the bindings (p4-s3, issue #318).

Stop lines are DERIVED: every arm has one without anything being authored, and
the three edit commands layer an override on top of the solve. These cases pin
the query, the commands' error behaviour, and the save/load round trip that
carries the authoring through `rm:stopline`.
"""

from __future__ import annotations

import pytest

import roadmaker as rm


@pytest.fixture
def cross():
    """A four-arm crossing with roomy arms, plus its junction id."""
    network = rm.RoadNetwork()
    stack = rm.edit.EditStack()
    for coords in (
        [(-80.0, 0.0), (-20.0, 0.0)],
        [(80.0, 0.0), (20.0, 0.0)],
        [(0.0, -80.0), (0.0, -20.0)],
        [(0.0, 80.0), (0.0, 20.0)],
    ):
        stack.push(network, rm.edit.create_road(coords, rm.LaneProfile.two_lane_default(), ""))
    ends = [
        rm.RoadEnd(network.find_road(odr), rm.ContactPoint.END) for odr in ("1", "2", "3", "4")
    ]
    stack.push(network, rm.edit.create_junction(network, ends))
    return network, network.find_junction("1"), stack


def test_four_arms_expose_four_derived_defaults(cross):
    network, junction, _ = cross
    lines = rm.junction_stoplines(network, junction)
    assert len(lines) == 4
    for info in lines:
        assert not info.authored
        assert not info.distance_authored
        assert not info.flipped
        assert info.distance == pytest.approx(4.0)
        assert info.thickness == pytest.approx(0.3)
        assert info.span > 2.0
        assert info.max_distance > info.distance
        assert info.crosswalk_odr_id == ""


def test_set_distance_is_visible_in_the_query(cross):
    network, junction, stack = cross
    arm = rm.junction_stoplines(network, junction)[0].arm
    stack.push(network, rm.edit.set_stopline_distance(network, junction, arm, 9.0))

    info = next(i for i in rm.junction_stoplines(network, junction) if i.arm == arm)
    assert info.authored
    assert info.distance_authored
    assert info.distance == pytest.approx(9.0)
    # The other three arms stay derived — the record is per-arm.
    assert sum(1 for i in rm.junction_stoplines(network, junction) if i.authored) == 1


def test_distance_is_stored_unclamped_and_clamped_only_when_solved(cross):
    network, junction, stack = cross
    arm = rm.junction_stoplines(network, junction)[0].arm
    stack.push(network, rm.edit.set_stopline_distance(network, junction, arm, 5000.0))

    info = next(i for i in rm.junction_stoplines(network, junction) if i.arm == arm)
    assert info.distance == pytest.approx(info.max_distance)


def test_flip_switches_direction_and_flipping_twice_normalizes_away(cross):
    network, junction, stack = cross
    arm = rm.junction_stoplines(network, junction)[0].arm
    before = rm.write_xodr(network)

    stack.push(network, rm.edit.flip_stopline(network, junction, arm))
    info = next(i for i in rm.junction_stoplines(network, junction) if i.arm == arm)
    assert info.flipped
    assert info.authored
    assert not info.distance_authored, "flipping alone must not fabricate a setback"

    stack.push(network, rm.edit.flip_stopline(network, junction, arm))
    assert rm.write_xodr(network) == before, "flip-twice is byte-identical to no edit"


def test_reset_returns_the_arm_to_the_derived_default(cross):
    network, junction, stack = cross
    arm = rm.junction_stoplines(network, junction)[0].arm
    stack.push(network, rm.edit.set_stopline_distance(network, junction, arm, 9.0))
    stack.push(network, rm.edit.reset_stopline(network, junction, arm))

    info = next(i for i in rm.junction_stoplines(network, junction) if i.arm == arm)
    assert not info.authored
    assert info.distance == pytest.approx(4.0)


def test_undo_redo_round_trips_an_edit(cross):
    network, junction, stack = cross
    arm = rm.junction_stoplines(network, junction)[0].arm
    before = rm.write_xodr(network)

    stack.push(network, rm.edit.set_stopline_distance(network, junction, arm, 9.0))
    after = rm.write_xodr(network)
    assert after != before

    stack.undo(network)
    assert rm.write_xodr(network) == before
    stack.redo(network)
    assert rm.write_xodr(network) == after


def test_bad_arm_raises(cross):
    network, junction, stack = cross
    stranger = rm.RoadEnd(network.find_road("1"), rm.ContactPoint.START)
    with pytest.raises(ValueError):
        stack.push(network, rm.edit.set_stopline_distance(network, junction, stranger, 3.0))


def test_negative_distance_raises(cross):
    network, junction, stack = cross
    arm = rm.junction_stoplines(network, junction)[0].arm
    with pytest.raises(ValueError):
        stack.push(network, rm.edit.set_stopline_distance(network, junction, arm, -1.0))


def test_reset_on_an_unauthored_arm_raises(cross):
    network, junction, stack = cross
    arm = rm.junction_stoplines(network, junction)[0].arm
    with pytest.raises(ValueError):
        stack.push(network, rm.edit.reset_stopline(network, junction, arm))


def test_save_load_round_trip_keeps_the_authoring(cross, tmp_path):
    network, junction, stack = cross
    arm = rm.junction_stoplines(network, junction)[0].arm
    stack.push(
        network, rm.edit.set_stopline_distance(network, junction, arm, 2.5, crosswalk_link="7")
    )
    stack.push(network, rm.edit.flip_stopline(network, junction, arm))

    path = tmp_path / "stoplines.xodr"
    rm.save_xodr(network, str(path), "stoplines")
    text = path.read_text()
    # Layer 0: a plain, self-contained signalLines object per arm.
    assert text.count('subtype="signalLines"') == 4
    # Layer 1: the parametric record beside it.
    assert text.count('code="rm:stopline"') == 4
    assert 'distance="2.5"' in text
    assert 'crosswalk="7"' in text

    reloaded, diagnostics = rm.parse_xodr(text)
    assert not diagnostics
    reloaded_junction = reloaded.find_junction("1")
    lines = rm.junction_stoplines(reloaded, reloaded_junction)
    assert len(lines) == 4, "the absorbed objects are not duplicated"

    authored = [i for i in lines if i.authored]
    assert len(authored) == 1
    assert authored[0].distance == pytest.approx(2.5)
    assert authored[0].flipped
    assert authored[0].crosswalk_odr_id == "7"

    # Writing again reproduces the file byte for byte.
    assert rm.write_xodr(reloaded, "stoplines") == rm.write_xodr(network, "stoplines")


def test_a_stale_junction_yields_nothing(cross):
    network, junction, _ = cross
    network.erase_junction(junction)
    assert rm.junction_stoplines(network, junction) == []
