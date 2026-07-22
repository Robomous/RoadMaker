# Copyright 2026 Robomous
# SPDX-License-Identifier: Apache-2.0

"""edit.assembly.cross_roads + road_intersections through the bindings."""

import pytest

import roadmaker as rm


def _crossing_pair():
    net = rm.RoadNetwork()
    rm.author_clothoid_road(
        net,
        [(-100.0, 0.0), (100.0, 0.0)],  # horizontal, id "1"
        rm.LaneProfile.two_lane_rural(),
        "H",
        "1",
    )
    rm.author_clothoid_road(
        net,
        [(0.0, -100.0), (0.0, 100.0)],  # vertical, id "2"
        rm.LaneProfile.two_lane_rural(),
        "V",
        "2",
    )
    return net


def test_cross_roads_forms_a_four_way_junction():
    net = _crossing_pair()
    stack = rm.edit.EditStack()
    a = net.find_road("1")
    b = net.find_road("2")

    assert net.junction_count == 0
    stack.push(net, rm.edit.assembly.cross_roads(net, a, b))
    assert net.junction_count == 1


def test_cross_roads_is_one_command_undo():
    net = _crossing_pair()
    stack = rm.edit.EditStack()
    before = rm.write_xodr(net)

    stack.push(net, rm.edit.assembly.cross_roads(net, net.find_road("1"), net.find_road("2")))
    assert rm.write_xodr(net) != before
    assert net.junction_count == 1

    stack.undo(net)  # ONE undo restores the pre-cross document byte-for-byte
    assert rm.write_xodr(net) == before


def test_cross_roads_round_trips_through_xodr():
    net = _crossing_pair()
    stack = rm.edit.EditStack()
    stack.push(net, rm.edit.assembly.cross_roads(net, net.find_road("1"), net.find_road("2")))

    written = rm.write_xodr(net)
    reparsed, _ = rm.parse_xodr(written)
    assert reparsed.junction_count == 1
    assert rm.write_xodr(reparsed) == written  # deterministic write survives the round trip
