"""The kernel edit layer through the bindings: EditStack + factories."""

import pytest

import roadmaker as rm


@pytest.fixture
def network():
    net = rm.RoadNetwork()
    rm.author_clothoid_road(
        net,
        [(0.0, 0.0), (60.0, 8.0), (120.0, 0.0)],
        rm.LaneProfile.two_lane_default(),
        "First",
        "1",
    )
    return net


def test_push_applies_and_undo_redo_round_trip(network):
    stack = rm.edit.EditStack()
    road = network.find_road("1")
    before = rm.write_xodr(network)

    stack.push(network, rm.edit.rename_road(network, road, "Renamed"))
    assert network.road(road).name == "Renamed"
    assert stack.can_undo and not stack.can_redo
    edited = rm.write_xodr(network)

    stack.undo(network)
    assert rm.write_xodr(network) == before  # byte-identical restore

    stack.redo(network)
    assert rm.write_xodr(network) == edited


def test_failed_push_raises_and_leaves_network_unchanged(network):
    stack = rm.edit.EditStack()
    before = rm.write_xodr(network)

    with pytest.raises(ValueError):
        stack.push(network, rm.edit.rename_road(network, rm.RoadId(), "x"))

    assert not stack.can_undo
    assert rm.write_xodr(network) == before


def test_create_road_undo_frees_and_redo_restores_same_id(network):
    stack = rm.edit.EditStack()
    stack.push(
        network,
        rm.edit.create_road(
            [(0.0, 50.0), (90.0, 60.0)], rm.LaneProfile.two_lane_default(), "Branch"
        ),
    )
    created = network.find_road("2")
    assert created
    stack.push(network, rm.edit.rename_road(network, created, "Renamed"))

    stack.undo(network)
    stack.undo(network)
    assert network.road_count == 1
    assert network.road(created) is None

    stack.redo(network)
    stack.redo(network)
    # The generational id survives undo/redo — no re-lookup needed.
    assert network.road(created).name == "Renamed"


def test_waypoint_and_lane_edits(network):
    stack = rm.edit.EditStack()
    road = network.find_road("1")
    old_length = network.road(road).length

    stack.push(network, rm.edit.move_waypoint(network, road, 1, (60.0, 30.0)))
    assert network.road(road).length != old_length
    assert network.road(road).authoring_waypoints[1].y == pytest.approx(30.0)

    section = network.road(road).sections[0]
    outer = network.lane_section(section).lanes[-1]
    stack.push(network, rm.edit.set_lane_width(network, outer, 2.5))
    assert network.lane(outer).widths[0].a == pytest.approx(2.5)

    stack.push(network, rm.edit.set_node_elevation(network, road, 1, 4.0))
    assert len(network.road(road).elevation) == 2

    while stack.can_undo:
        stack.undo(network)
    assert network.road(road).length == pytest.approx(old_length)
    assert network.road(road).elevation == []


def test_split_and_junction_commands(network):
    stack = rm.edit.EditStack()
    road = network.find_road("1")
    length = network.road(road).length

    stack.push(network, rm.edit.split_road(network, road, length * 0.5))
    assert network.road_count == 2
    tail = network.find_road("2")

    stack.push(
        network,
        rm.edit.create_junction(
            network,
            [
                rm.RoadEnd(road, rm.ContactPoint.START),
                rm.RoadEnd(tail, rm.ContactPoint.END),
            ],
        ),
    )
    assert network.junction_count == 1

    while stack.can_undo:
        stack.undo(network)
    assert network.road_count == 1
    assert network.junction_count == 0


def test_depth_limit_drops_oldest(network):
    stack = rm.edit.EditStack()
    stack.depth_limit = 2
    road = network.find_road("1")

    for name in ("a", "b", "c"):
        stack.push(network, rm.edit.rename_road(network, road, name))

    assert stack.size == 2
    stack.undo(network)
    stack.undo(network)
    assert not stack.can_undo
    assert network.road(road).name == "a"  # the dropped edit stays applied


@pytest.mark.parametrize(
    "factory",
    [
        lambda net, road: rm.edit.delete_waypoint(net, road, 0),  # min waypoints
        lambda net, road: rm.edit.move_waypoint(net, road, 99, (0, 0)),
        lambda net, road: rm.edit.split_road(net, road, 0.0),
    ],
)
def test_invalid_commands_raise_on_push(factory):
    stack = rm.edit.EditStack()
    two_point = rm.RoadNetwork()
    rm.author_clothoid_road(
        two_point, [(0.0, 0.0), (50.0, 0.0)], rm.LaneProfile.two_lane_default(), "", "1"
    )
    target = two_point.find_road("1")
    with pytest.raises(ValueError):
        stack.push(two_point, factory(two_point, target))
    assert not stack.can_undo
