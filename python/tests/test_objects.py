"""OpenDRIVE <objects> bindings: authoring, iteration, round-trip (issue #67)."""

import pytest

import roadmaker as rm


@pytest.fixture
def network_with_road():
    network = rm.RoadNetwork()
    road_id = rm.author_clothoid_road(
        network,
        [(0.0, 0.0), (100.0, 0.0)],
        rm.LaneProfile.two_lane_rural(),
        name="r",
    )
    return network, road_id


def make_crosswalk() -> rm.Object:
    crosswalk = rm.Object()
    crosswalk.odr_id = "1"
    crosswalk.type = rm.ObjectType.CROSSWALK
    crosswalk.s, crosswalk.t = 50.0, 0.0
    outline = rm.ObjectOutline()
    outline.closed = True
    outline.fill_type = "paint"
    outline.corners = [
        rm.OutlineCorner(48.0, -2.0),
        rm.OutlineCorner(52.0, -2.0),
        rm.OutlineCorner(52.0, 2.0),
        rm.OutlineCorner(48.0, 2.0),
    ]
    crosswalk.outlines = [outline]
    return crosswalk


def test_add_lookup_erase(network_with_road):
    network, road_id = network_with_road
    object_id = network.add_object(road_id, make_crosswalk())
    assert object_id
    assert network.object_count == 1
    assert network.objects_of(road_id) == [object_id]

    stored = network.object(object_id)
    assert stored.type == rm.ObjectType.CROSSWALK
    assert stored.s == pytest.approx(50.0)
    assert stored.road == road_id
    assert len(stored.outlines[0].corners) == 4

    assert network.erase_object(object_id)
    assert network.object(object_id) is None
    assert network.object_count == 0


def test_add_to_stale_road_returns_invalid_id(network_with_road):
    network, road_id = network_with_road
    assert network.erase_road(road_id)
    assert not network.add_object(road_id, make_crosswalk())


def test_erase_road_cascades_objects(network_with_road):
    network, road_id = network_with_road
    object_id = network.add_object(road_id, make_crosswalk())
    assert network.erase_road(road_id)
    assert network.object(object_id) is None
    assert network.object_count == 0


def test_objects_round_trip_through_xodr(network_with_road, tmp_path):
    network, road_id = network_with_road
    network.add_object(road_id, make_crosswalk())

    tree = rm.Object()
    tree.odr_id = "2"
    tree.type = rm.ObjectType.TREE
    tree.s, tree.t = 5.0, 8.0
    tree.radius, tree.height = 0.5, 6.0
    repeat = rm.ObjectRepeat()
    repeat.s, repeat.length, repeat.distance = 5.0, 90.0, 15.0
    repeat.t_start = repeat.t_end = 8.0
    tree.repeats = [repeat]
    network.add_object(road_id, tree)

    path = tmp_path / "objects.xodr"
    rm.save_xodr(network, path, name="objects test")

    reloaded, diagnostics = rm.load_xodr(path)
    assert not [d for d in diagnostics if d.severity == rm.Severity.ERROR]
    assert reloaded.object_count == 2

    objects = {reloaded.object(i).odr_id: reloaded.object(i) for i in reloaded.object_ids}
    assert objects["1"].type == rm.ObjectType.CROSSWALK
    assert objects["1"].outlines[0].fill_type == "paint"
    assert objects["1"].outlines[0].corners[2].b == pytest.approx(2.0)
    assert objects["2"].radius == pytest.approx(0.5)
    assert objects["2"].repeats[0].distance == pytest.approx(15.0)


def test_validate_cites_object_rules(network_with_road):
    network, road_id = network_with_road
    bad = rm.Object()
    bad.odr_id = "1"
    bad.type = rm.ObjectType.OBSTACLE
    bad.radius = 1.0
    bad.length = 2.0  # circular XOR angular — both set is a rule violation
    network.add_object(road_id, bad)

    findings = rm.validate_network(network)
    assert any(
        f.rule_id == "asam.net:xodr:1.7.0:road.object.circular_vs_angular" for f in findings
    )
