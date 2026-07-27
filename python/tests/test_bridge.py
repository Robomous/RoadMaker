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

"""Bridges through the bindings (p5-s3, #233)."""

import pytest

import roadmaker as rm


def _crossing(raise_high: float = 6.0):
    """Two roads crossing at the origin; the east-west one raised by `raise_high`."""
    net = rm.RoadNetwork()
    high = rm.author_clothoid_road(
        net, [(-60.0, 0.0), (60.0, 0.0)], rm.LaneProfile.two_lane_rural(), "", "high"
    )
    low = rm.author_clothoid_road(
        net, [(0.0, -60.0), (0.0, 60.0)], rm.LaneProfile.two_lane_rural(), "", "low"
    )
    stack = rm.edit.EditStack()
    length = net.road(high).length
    stack.push(
        net,
        rm.edit.set_elevation_profile(
            net,
            high,
            [
                rm.edit.ElevationPoint(0.0, raise_high, 0.0),
                rm.edit.ElevationPoint(length, raise_high, 0.0),
            ],
        ),
    )
    return net, high, low, stack


def test_find_grade_separations_detects_the_overpass():
    net, high, low, _stack = _crossing()
    seps = rm.find_grade_separations(net)
    assert len(seps) == 1
    assert seps[0].upper == high
    assert seps[0].lower == low
    assert seps[0].clearance >= 3.0


def test_low_clearance_is_not_a_grade_separation():
    net, _high, _low, _stack = _crossing(raise_high=1.0)
    assert rm.find_grade_separations(net) == []


def test_author_bridge_generates_a_solid():
    net, high, _low, stack = _crossing()
    s = max(0.0, rm.find_grade_separations(net)[0].s_upper - 12.0)
    stack.push(net, rm.edit.author_bridge(net, high, s, 24.0))
    mesh = rm.build_network_mesh(net)
    assert mesh.bridge_count == 1
    assert mesh.bridge_vertex_count > 0


def test_set_bridge_span_inflates_the_extent():
    net, high, _low, stack = _crossing()
    s = max(0.0, rm.find_grade_separations(net)[0].s_upper - 12.0)
    stack.push(net, rm.edit.author_bridge(net, high, s, 24.0))
    narrow = rm.build_network_mesh(net).bridge_vertex_count
    stack.push(net, rm.edit.set_bridge_span(net, high, 0, s, 44.0))
    wide = rm.build_network_mesh(net).bridge_vertex_count
    assert wide != narrow


def test_bridge_undo_removes_the_solid():
    net, high, _low, stack = _crossing()
    s = max(0.0, rm.find_grade_separations(net)[0].s_upper - 12.0)
    stack.push(net, rm.edit.author_bridge(net, high, s, 24.0))
    assert rm.build_network_mesh(net).bridge_count == 1
    stack.undo(net)
    assert rm.build_network_mesh(net).bridge_count == 0


def test_bridge_record_round_trips_but_no_bridge_writes_nothing():
    net, high, _low, stack = _crossing()
    # No bridge yet: the text has no <bridge>.
    assert "<bridge" not in rm.write_xodr(net)
    s = max(0.0, rm.find_grade_separations(net)[0].s_upper - 12.0)
    stack.push(net, rm.edit.author_bridge(net, high, s, 24.0))
    once = rm.write_xodr(net)
    assert "<bridge" in once
    reparsed, _diags = rm.parse_xodr(once)
    assert rm.write_xodr(reparsed) == once


# --- cascade-s3 (#463): a move re-anchors a span, or reports it orphaned ------


def _bridged():
    """A crossing with a 24 m deck already over it."""
    net, high, low, stack = _crossing()
    s = max(0.0, rm.find_grade_separations(net)[0].s_upper - 12.0)
    stack.push(net, rm.edit.author_bridge(net, high, s, 24.0))
    return net, high, low, stack


def test_moving_a_road_takes_the_bridge_span_with_the_crossing():
    net, high, low, stack = _bridged()
    s_before = net.road(high).bridges[0].s

    stack.push(net, rm.edit.translate_road(net, low, 25.0, 0.0))

    assert net.road(high).bridges[0].s == pytest.approx(s_before + 25.0)
    sep = rm.find_grade_separations(net)[0]
    assert rm.bridge_covering(net.road(high), sep.s_upper) == 0
    changes = [r.change for r in stack.last_derived_records]
    assert rm.edit.DerivedChange.BRIDGE_RELOCATED in changes


def test_a_span_whose_crossing_is_gone_is_reported_not_deleted():
    net, high, low, stack = _bridged()
    before = net.road(high).bridges[0].s

    stack.push(net, rm.edit.translate_road(net, low, 400.0, 0.0))

    assert rm.find_grade_separations(net) == []
    assert len(net.road(high).bridges) == 1
    assert net.road(high).bridges[0].s == pytest.approx(before)
    changes = [r.change for r in stack.last_derived_records]
    assert rm.edit.DerivedChange.BRIDGE_ORPHANED in changes


def test_remove_orphaned_bridges_is_the_explicit_counterpart():
    net, high, low, stack = _bridged()
    stack.push(net, rm.edit.translate_road(net, low, 400.0, 0.0))

    stack.push(net, rm.edit.remove_orphaned_bridges(net))
    assert net.road(high).bridges == []

    stack.undo(net)
    assert len(net.road(high).bridges) == 1

    # Nothing orphaned left once the sweep has run: the command refuses.
    stack.redo(net)
    with pytest.raises(ValueError):
        stack.push(net, rm.edit.remove_orphaned_bridges(net))
