# Copyright 2026 Robomous
# SPDX-License-Identifier: Apache-2.0

"""edit.extend_road through the bindings: curvature continuity, undo, round trip."""

import math

import pytest

import roadmaker as rm


@pytest.fixture
def network():
    net = rm.RoadNetwork()
    rm.author_clothoid_road(
        net,
        [(0.0, 0.0), (60.0, 8.0), (120.0, 0.0)],  # an S-bend, curved at the end
        rm.LaneProfile.two_lane_default(),
        "Bend",
        "1",
    )
    return net


def _ahead_of_end(network, road, distance):
    plan = network.road(road).plan_view
    end = plan.evaluate(plan.length)
    return (end.x + distance * math.cos(end.hdg), end.y + distance * math.sin(end.hdg))


def _behind_start(network, road, distance):
    plan = network.road(road).plan_view
    start = plan.evaluate(0.0)
    return (start.x - distance * math.cos(start.hdg), start.y - distance * math.sin(start.hdg))


def test_extend_is_curvature_continuous_at_join(network):
    stack = rm.edit.EditStack()
    road = network.find_road("1")
    join_s = network.road(road).plan_view.length
    to = _ahead_of_end(network, road, 40.0)

    stack.push(network, rm.edit.extend_road(network, rm.RoadEnd(road, rm.ContactPoint.END), to))

    plan = network.road(road).plan_view
    assert plan.length > join_s + 1.0  # it grew
    below = plan.evaluate(join_s - 1e-3)
    above = plan.evaluate(join_s + 1e-3)
    assert abs(above.curvature - below.curvature) < 5e-3  # tol::kWeldCurvature
    dhdg = math.remainder(above.hdg - below.hdg, 2.0 * math.pi)
    assert abs(dhdg) < 1e-3  # tol::kWeldHeading


def test_extend_is_one_command_undo(network):
    stack = rm.edit.EditStack()
    road = network.find_road("1")
    before = rm.write_xodr(network)
    to = _ahead_of_end(network, road, 40.0)

    stack.push(network, rm.edit.extend_road(network, rm.RoadEnd(road, rm.ContactPoint.END), to))
    assert rm.write_xodr(network) != before
    assert network.road_count == 1  # same road, not a new one

    stack.undo(network)  # ONE undo restores the pre-extend document byte-for-byte
    assert rm.write_xodr(network) == before


def test_extend_round_trips_through_xodr(network):
    stack = rm.edit.EditStack()
    road = network.find_road("1")
    to = _ahead_of_end(network, road, 40.0)
    stack.push(network, rm.edit.extend_road(network, rm.RoadEnd(road, rm.ContactPoint.END), to))

    written = rm.write_xodr(network)
    reparsed, _ = rm.parse_xodr(written)
    reread = reparsed.find_road("1")
    a = network.road(road).plan_view
    b = reparsed.road(reread).plan_view
    assert b.length == pytest.approx(a.length, abs=1e-3)
    for i in range(0, 41):
        s = a.length * i / 40.0
        pa = a.evaluate(s)
        pb = b.evaluate(s)
        assert pb.x == pytest.approx(pa.x, abs=1e-3)
        assert pb.y == pytest.approx(pa.y, abs=1e-3)


def test_extend_start_is_one_command_undo(network):
    stack = rm.edit.EditStack()
    road = network.find_road("1")
    before = rm.write_xodr(network)
    to = _behind_start(network, road, 40.0)

    stack.push(network, rm.edit.extend_road(network, rm.RoadEnd(road, rm.ContactPoint.START), to))
    assert rm.write_xodr(network) != before
    assert network.road_count == 1  # same road, re-based in place

    stack.undo(network)  # ONE undo restores the pre-extend document byte-for-byte
    assert rm.write_xodr(network) == before


def test_extend_start_round_trips_through_xodr(network):
    stack = rm.edit.EditStack()
    road = network.find_road("1")
    to = _behind_start(network, road, 40.0)
    stack.push(network, rm.edit.extend_road(network, rm.RoadEnd(road, rm.ContactPoint.START), to))

    written = rm.write_xodr(network)
    reparsed, _ = rm.parse_xodr(written)
    reread = reparsed.find_road("1")
    a = network.road(road).plan_view
    b = reparsed.road(reread).plan_view
    assert b.length == pytest.approx(a.length, abs=1e-3)
    for i in range(0, 41):
        s = a.length * i / 40.0
        pa = a.evaluate(s)
        pb = b.evaluate(s)
        assert pb.x == pytest.approx(pa.x, abs=1e-3)
        assert pb.y == pytest.approx(pa.y, abs=1e-3)
