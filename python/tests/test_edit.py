"""The kernel edit layer through the bindings: EditStack + factories."""

from pathlib import Path

import pytest

import roadmaker as rm

SAMPLES = Path(__file__).resolve().parents[2] / "assets" / "samples"


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


def test_set_lane_direction_round_trips(network):
    stack = rm.edit.EditStack()
    road = network.find_road("1")
    section = network.road(road).sections[0]
    outer = network.lane_section(section).lanes[-1]  # outermost, non-center
    assert network.lane(outer).direction == rm.LaneDirection.STANDARD
    before = rm.write_xodr(network)

    stack.push(network, rm.edit.set_lane_direction(network, outer, rm.LaneDirection.REVERSED))
    assert network.lane(outer).direction == rm.LaneDirection.REVERSED

    stack.undo(network)
    assert rm.write_xodr(network) == before  # byte-identical restore
    assert network.lane(outer).direction == rm.LaneDirection.STANDARD

    # The center lane has no travel direction — the factory refuses it.
    center = next(
        lane for lane in network.lane_section(section).lanes if network.lane(lane).odr_id == 0
    )
    with pytest.raises(ValueError):
        stack.push(network, rm.edit.set_lane_direction(network, center, rm.LaneDirection.BOTH))


def test_lane_direction_survives_save_load(network, tmp_path):
    road = network.find_road("1")
    section = network.road(road).sections[0]
    outer = network.lane_section(section).lanes[-1]
    rm.edit.EditStack().push(
        network, rm.edit.set_lane_direction(network, outer, rm.LaneDirection.BOTH)
    )

    out = tmp_path / "direction.xodr"
    rm.save_xodr(network, out, "direction")
    assert 'direction="both"' in out.read_text()

    reloaded, _ = rm.load_xodr(out)
    reloaded_section = reloaded.road(reloaded.find_road("1")).sections[0]
    reloaded_outer = reloaded.lane_section(reloaded_section).lanes[-1]
    assert reloaded.lane(reloaded_outer).direction == rm.LaneDirection.BOTH


def test_fit_elevation_profile_interpolates_nodes_and_ascends():
    s = [0.0, 50.0, 100.0]
    z = [0.0, 10.0, 0.0]
    profile = rm.fit_elevation_profile(s, z)
    assert len(profile) == 2

    # Reproduces the node heights at the node stations and ascends in s.
    for station, height in zip(s, z):
        record = profile[0] if station < profile[1].s else profile[1]
        assert record.eval(station) == pytest.approx(height, abs=1e-6)
    assert [p.s for p in profile] == sorted(p.s for p in profile)

    # A constant-grade ramp fits as straight cubics (c = d = 0).
    ramp = rm.fit_elevation_profile([0.0, 50.0, 100.0], [0.0, 5.0, 10.0])
    assert ramp[0].b == pytest.approx(0.1)
    assert ramp[0].c == pytest.approx(0.0, abs=1e-6)
    assert ramp[0].d == pytest.approx(0.0, abs=1e-6)

    # Degenerate inputs return an empty profile.
    assert rm.fit_elevation_profile([], []) == []
    assert rm.fit_elevation_profile([0.0, 10.0], [1.0]) == []


def test_set_node_elevation_writes_the_cubic_fit(network):
    stack = rm.edit.EditStack()
    road = network.find_road("1")

    stack.push(network, rm.edit.set_node_elevation(network, road, 1, 4.0))
    elevation = network.road(road).elevation
    stations = rm.edit.waypoint_stations(network.road(road))

    # The written <elevation> records match the pure fit through (s, z).
    heights = [0.0, 4.0, 0.0]
    expected = rm.fit_elevation_profile(list(stations), heights)
    assert [p.s for p in elevation] == pytest.approx([p.s for p in expected])
    assert [p.a for p in elevation] == pytest.approx([p.a for p in expected])
    assert [p.d for p in elevation] == pytest.approx([p.d for p in expected])


def test_split_command_and_undo(network):
    stack = rm.edit.EditStack()
    road = network.find_road("1")
    length = network.road(road).length

    stack.push(network, rm.edit.split_road(network, road, length * 0.5))
    assert network.road_count == 2

    while stack.can_undo:
        stack.undo(network)
    assert network.road_count == 1


def test_translate_road_shifts_geometry_and_undo_is_byte_identical(network):
    stack = rm.edit.EditStack()
    road = network.find_road("1")
    before = rm.write_xodr(network)
    start_before = network.road(road).plan_view.evaluate(0.0)

    stack.push(network, rm.edit.translate_road(network, road, 10.0, -4.0))
    start_after = network.road(road).plan_view.evaluate(0.0)
    assert start_after.x == pytest.approx(start_before.x + 10.0)
    assert start_after.y == pytest.approx(start_before.y - 4.0)

    stack.undo(network)
    assert rm.write_xodr(network) == before  # byte-identical restore


def test_translate_roads_moves_many_as_one_command(network):
    stack = rm.edit.EditStack()
    rm.author_clothoid_road(
        network, [(0.0, 100.0), (120.0, 100.0)], rm.LaneProfile.two_lane_default(), "Second", "2"
    )
    a = network.find_road("1")
    b = network.find_road("2")

    stack.push(network, rm.edit.translate_roads(network, [a, b], 5.0, 5.0))
    # One command moved both; a single undo puts both back.
    assert network.road(a).plan_view.evaluate(0.0).y == pytest.approx(5.0)
    assert network.road(b).plan_view.evaluate(0.0).y == pytest.approx(105.0)
    stack.undo(network)
    assert network.road(a).plan_view.evaluate(0.0).y == pytest.approx(0.0)


def test_translate_road_refuses_junction_road(network):
    stack = rm.edit.EditStack()
    # Build a junction so an incoming road touches it, then refuse to move it.
    rm.author_clothoid_road(
        network, [(200.0, 0.0), (140.0, 0.0)], rm.LaneProfile.two_lane_default(), "", "9"
    )
    ends = [
        rm.RoadEnd(network.find_road("1"), rm.ContactPoint.END),
        rm.RoadEnd(network.find_road("9"), rm.ContactPoint.END),
    ]
    stack.push(network, rm.edit.create_junction(network, ends))
    with pytest.raises(Exception):
        stack.push(network, rm.edit.translate_road(network, network.find_road("1"), 1.0, 1.0))


def test_insert_node_at_preserves_shape(network):
    stack = rm.edit.EditStack()
    road = network.find_road("1")
    length = network.road(road).length
    nodes_before = len(network.road(road).authoring_waypoints)
    sampled = [(s, network.road(road).plan_view.evaluate(s)) for s in (20.0, 60.0, 100.0)]

    stack.push(network, rm.edit.insert_node_at(network, road, length * 0.25))
    assert len(network.road(road).authoring_waypoints) == nodes_before + 1
    for s, before in sampled:
        after = network.road(road).plan_view.evaluate(s)
        assert after.x == pytest.approx(before.x, abs=1e-3)
        assert after.y == pytest.approx(before.y, abs=1e-3)

    stack.undo(network)
    assert len(network.road(road).authoring_waypoints) == nodes_before


def test_insert_node_at_rejects_near_duplicate(network):
    stack = rm.edit.EditStack()
    road = network.find_road("1")
    with pytest.raises(ValueError):
        stack.push(network, rm.edit.insert_node_at(network, road, 0.5))  # < 2 m from start


def test_merge_roads_joins_two_and_undo_restores():
    net = rm.RoadNetwork()
    stack = rm.edit.EditStack()
    stack.push(net, rm.edit.create_road(
        [(0.0, 0.0), (50.0, 0.0)], rm.LaneProfile.two_lane_default(), "A"))
    stack.push(net, rm.edit.create_road(
        [(50.0, 0.0), (100.0, 0.0)], rm.LaneProfile.two_lane_default(), "B"))
    a = net.find_road("1")
    b = net.find_road("2")
    before = rm.write_xodr(net)

    rm.edit.check_mergeable(net, a, b)  # does not raise
    stack.push(net, rm.edit.merge_roads(net, a, b))
    assert net.road_count == 1
    assert net.road(a).length == pytest.approx(100.0, abs=1e-4)

    stack.undo(net)
    assert rm.write_xodr(net) == before  # byte-identical


def test_check_mergeable_reports_reason_for_distant_roads():
    net = rm.RoadNetwork()
    rm.author_clothoid_road(net, [(0.0, 0.0), (50.0, 0.0)], rm.LaneProfile.two_lane_default(), "", "1")
    rm.author_clothoid_road(net, [(200.0, 0.0), (250.0, 0.0)], rm.LaneProfile.two_lane_default(), "", "2")
    with pytest.raises(ValueError):
        rm.edit.check_mergeable(net, net.find_road("1"), net.find_road("2"))


def _t_junction_arms(net):
    """Three straight two-lane arms whose ends meet near the origin."""
    for coords, odr in (
        ([(-40.0, 0.0), (-6.0, 0.0)], "1"),
        ([(40.0, 0.0), (6.0, 0.0)], "2"),
        ([(0.0, -40.0), (0.0, -6.0)], "3"),
    ):
        rm.author_clothoid_road(net, coords, rm.LaneProfile.two_lane_default(), "", odr)
    return [
        rm.RoadEnd(net.find_road("1"), rm.ContactPoint.END),
        rm.RoadEnd(net.find_road("2"), rm.ContactPoint.END),
        rm.RoadEnd(net.find_road("3"), rm.ContactPoint.END),
    ]


def test_create_junction_generates_connecting_roads_and_persists_arms(tmp_path):
    net = rm.RoadNetwork()
    ends = _t_junction_arms(net)

    # Preview mirrors what generation produces: six turns, none dropped.
    preview = rm.edit.preview_junction(net, ends)
    assert preview.connection_count == 6
    assert preview.dropped_turns == []

    stack = rm.edit.EditStack()
    stack.push(net, rm.edit.create_junction(net, ends))
    assert net.junction_count == 1
    junction = net.find_junction("1")
    assert len(net.junction(junction).connections) == 6
    assert len(net.junction(junction).arms) == 3

    # A no-op regeneration keeps the junction valid and is undoable.
    stack.push(net, rm.edit.regenerate_junction(net, junction))

    # Arms round-trip through <userData code="rm:arms"> so the saved project
    # can regenerate after reload.
    out = tmp_path / "junction.xodr"
    rm.save_xodr(net, out, "junction_example")
    assert "rm:arms" in out.read_text()
    # No structural errors; the only finding is the intentional boundary-omitted
    # warning (M2 writes the surface without <boundary>).
    findings = rm.validate_network(net)
    assert all(f.severity == rm.Severity.WARNING for f in findings)
    assert all(
        f.rule_id == "asam.net:xodr:1.8.0:junctions.boundary.close_gap_with_new_roads"
        for f in findings
    )

    while stack.can_undo:
        stack.undo(net)
    assert net.junction_count == 0
    assert net.road_count == 3


def test_delete_road_closure_takes_connecting_roads_and_undo_restores():
    # t_junction.xodr: incoming road "1" feeds connecting roads "10"/"11"
    # inside junction "100". Deleting "1" carries the referential closure.
    network, _ = rm.load_xodr(SAMPLES / "t_junction.xodr")
    stack = rm.edit.EditStack()
    incoming = network.find_road("1")
    junction = network.find_junction("100")
    before = rm.write_xodr(network)

    stack.push(network, rm.edit.delete_road(network, incoming))
    assert network.road(incoming) is None
    assert not network.find_road("10")
    assert not network.find_road("11")
    assert network.junction(junction).connections == []
    assert network.find_road("2")  # surviving leg

    stack.undo(network)
    assert rm.write_xodr(network) == before


def test_delete_junction_closure_and_undo_restores():
    network, _ = rm.load_xodr(SAMPLES / "t_junction.xodr")
    stack = rm.edit.EditStack()
    junction = network.find_junction("100")
    incoming = network.find_road("1")
    before = rm.write_xodr(network)

    stack.push(network, rm.edit.delete_junction(network, junction))
    # The junction takes its connecting roads along; arms survive with their
    # links into it cleared.
    assert network.junction(junction) is None
    assert not network.find_road("10")
    assert not network.find_road("11")
    assert network.road(incoming) is not None
    assert network.road(incoming).successor is None

    stack.undo(network)
    assert rm.write_xodr(network) == before


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


# --- issue #13: lane-profile templates + endpoint-locked fit -----------------


def test_lane_profile_templates_contents():
    rural = rm.LaneProfile.two_lane_rural()
    assert [lane.type for lane in rural.left] == [rm.LaneType.DRIVING]
    assert [lane.type for lane in rural.right] == [rm.LaneType.DRIVING, rm.LaneType.SHOULDER]
    assert rural.center_marking

    urban = rm.LaneProfile.urban_sidewalk()
    for side in (urban.left, urban.right):
        assert [lane.type for lane in side] == [rm.LaneType.DRIVING, rm.LaneType.SIDEWALK]
        assert [lane.width for lane in side] == [pytest.approx(3.5), pytest.approx(2.0)]

    highway = rm.LaneProfile.highway()
    for side in (highway.left, highway.right):
        assert [lane.type for lane in side] == [
            rm.LaneType.DRIVING,
            rm.LaneType.DRIVING,
            rm.LaneType.SHOULDER,
        ]
    assert not highway.center_marking

    # two_lane_default stays as an alias of the rural template.
    alias = rm.LaneProfile.two_lane_default()
    assert [lane.type for lane in alias.right] == [lane.type for lane in rural.right]


def test_create_road_locked_start_heading_chains_g1(network):
    first = network.road(network.find_road("1"))
    end = first.plan_view.evaluate(first.length)

    stack = rm.edit.EditStack()
    stack.push(
        network,
        rm.edit.create_road(
            [(end.x, end.y), (end.x + 70.0, end.y - 25.0)],
            rm.LaneProfile.two_lane_rural(),
            "Chained",
            start_heading=end.hdg,
        ),
    )
    chained = network.road(network.find_road("2"))
    start = chained.plan_view.evaluate(0.0)
    assert start.x == pytest.approx(end.x)
    assert start.y == pytest.approx(end.y)
    assert start.hdg == pytest.approx(end.hdg, abs=1e-9)


def test_create_road_auto_names_from_the_assigned_id(network):
    stack = rm.edit.EditStack()
    stack.push(
        network,
        rm.edit.create_road([(0.0, 50.0), (80.0, 60.0)], rm.LaneProfile.urban_sidewalk(), ""),
    )
    assert network.road(network.find_road("2")).name == "Road 2"


def test_author_clothoid_road_accepts_locked_headings():
    net = rm.RoadNetwork()
    rm.author_clothoid_road(
        net,
        [(0.0, 0.0), (60.0, 10.0), (120.0, 0.0)],
        rm.LaneProfile.two_lane_rural(),
        "Locked",
        "1",
        start_heading=0.4,
        end_heading=-0.3,
    )
    line = net.road(net.find_road("1")).plan_view
    assert line.evaluate(0.0).hdg == pytest.approx(0.4, abs=1e-9)
    assert line.evaluate(line.length).hdg == pytest.approx(-0.3, abs=1e-9)



def test_remove_lane_drops_junction_lane_links_and_undo_restores():
    network, _ = rm.load_xodr(SAMPLES / "t_junction.xodr")
    stack = rm.edit.EditStack()
    west = network.find_road("1")
    junction = network.find_junction("100")
    outer_right = network.lane_section(network.road(west).sections[0]).lanes[-1]
    before = rm.write_xodr(network)

    # Both connections link West Approach's lane -1 — removal must not leave
    # a dangling laneLink (issue #14 integrity criterion).
    stack.push(network, rm.edit.remove_lane(network, outer_right))
    for connection in network.junction(junction).connections:
        assert connection.lane_links == []

    stack.undo(network)
    assert rm.write_xodr(network) == before
    for connection in network.junction(junction).connections:
        assert connection.lane_links == [(-1, -1)]


def test_set_road_mark_edits_the_first_record_only(network):
    road = network.find_road("1")
    outer_right = network.lane_section(network.road(road).sections[0]).lanes[-1]
    network.lane(outer_right).road_marks = [
        rm.RoadMark(s_offset=0.0, type=rm.RoadMarkType.BROKEN, width=0.12),
        rm.RoadMark(s_offset=40.0, type=rm.RoadMarkType.SOLID, width=0.12),
    ]

    stack = rm.edit.EditStack()
    stack.push(
        network,
        rm.edit.set_road_mark(
            network,
            outer_right,
            rm.RoadMark(s_offset=0.0, type=rm.RoadMarkType.SOLID, width=0.25),
        ),
    )
    marks = network.lane(outer_right).road_marks
    assert len(marks) == 2
    assert marks[0].type == rm.RoadMarkType.SOLID
    assert marks[0].width == pytest.approx(0.25)
    assert marks[1].s_offset == pytest.approx(40.0)  # tail untouched

    # An sOffset at or past the next record would break ascending order.
    with pytest.raises(ValueError):
        stack.push(
            network,
            rm.edit.set_road_mark(
                network,
                outer_right,
                rm.RoadMark(s_offset=40.0, type=rm.RoadMarkType.NONE, width=0.12),
            ),
        )


def test_t_intersection_generates_a_valid_junction():
    net = rm.RoadNetwork()
    stack = rm.edit.EditStack()
    stack.push(net, rm.edit.assembly.t_intersection(net, rm.edit.assembly.Pose(0.0, 0.0, 0.0)))
    assert net.junction_count == 1
    assert net.road_count >= 3  # 3 arms + generated connecting roads
    errors = [d for d in rm.validate_network(net) if d.severity == rm.Severity.ERROR]
    assert not errors


def test_x_intersection_undo_is_byte_identical():
    net = rm.RoadNetwork()
    stack = rm.edit.EditStack()
    before = rm.write_xodr(net)
    stack.push(net, rm.edit.assembly.x_intersection(net, rm.edit.assembly.Pose(5.0, -2.0, 0.4)))
    assert net.junction_count == 1
    after = rm.write_xodr(net)
    stack.undo(net)
    assert rm.write_xodr(net) == before  # undo restores the empty network exactly
    stack.redo(net)
    assert rm.write_xodr(net) == after


def test_intersection_params_honored_and_bad_arm_length_raises():
    net = rm.RoadNetwork()
    stack = rm.edit.EditStack()
    params = rm.edit.assembly.IntersectionParams()
    params.arm_length_m = 25.0
    params.gap_m = 10.0
    params.profile = rm.LaneProfile.highway()
    stack.push(net, rm.edit.assembly.x_intersection(net, rm.edit.assembly.Pose(), params))
    assert net.junction_count == 1

    bad = rm.edit.assembly.IntersectionParams()
    bad.arm_length_m = 0.0
    with pytest.raises(ValueError):
        stack.push(net, rm.edit.assembly.t_intersection(net, rm.edit.assembly.Pose(), bad))


def _tree(odr_id: str, s: float, t: float) -> rm.Object:
    tree = rm.Object()
    tree.odr_id = odr_id
    tree.name = "tree_pine"
    tree.type = rm.ObjectType.TREE
    tree.s, tree.t = s, t
    tree.radius, tree.height = 1.2, 4.2
    return tree


def test_add_object_undo_redo_round_trip(network):
    stack = rm.edit.EditStack()
    road = network.find_road("1")
    before = rm.write_xodr(network)

    stack.push(network, rm.edit.add_object(network, road, _tree("1", 20.0, 5.0)))
    assert network.object_count == 1
    added = rm.write_xodr(network)

    stack.undo(network)
    assert network.object_count == 0
    assert rm.write_xodr(network) == before  # byte-identical restore

    stack.redo(network)
    assert rm.write_xodr(network) == added


def test_move_object_then_undo_is_byte_identical(network):
    stack = rm.edit.EditStack()
    road = network.find_road("1")
    stack.push(network, rm.edit.add_object(network, road, _tree("1", 20.0, 5.0)))
    obj = network.objects_of(road)[-1]
    placed = rm.write_xodr(network)

    stack.push(network, rm.edit.move_object(network, obj, 40.0, -5.0, 0.3))
    assert network.object(obj).s == 40.0
    assert network.object(obj).hdg == pytest.approx(0.3)

    stack.undo(network)
    assert rm.write_xodr(network) == placed


def test_set_object_model_retargets_the_prop_and_undoes_exactly(network):
    stack = rm.edit.EditStack()
    road = network.find_road("1")
    stack.push(network, rm.edit.add_object(network, road, _tree("1", 20.0, 5.0)))
    obj = network.objects_of(road)[-1]
    placed = rm.write_xodr(network)

    stack.push(network, rm.edit.set_object_model(network, obj, "shrub"))
    assert network.object(obj).name == "shrub"
    # The bounding volume follows the model instead of describing the pine it
    # was: the fixture's hand-set 1.2 m radius must not survive the swap.
    assert network.object(obj).radius != pytest.approx(1.2)
    assert network.object(obj).radius > 0.0
    assert network.object(obj).s == 20.0  # pose untouched

    stack.undo(network)
    assert rm.write_xodr(network) == placed


def test_set_object_model_rejects_an_unknown_model(network):
    road = network.find_road("1")
    obj = network.add_object(road, _tree("1", 20.0, 5.0))
    before = rm.write_xodr(network)

    stack = rm.edit.EditStack()
    with pytest.raises(ValueError):
        stack.push(network, rm.edit.set_object_model(network, obj, "no_such_model"))
    assert rm.write_xodr(network) == before


def test_delete_object_undo_restores_it(network):
    stack = rm.edit.EditStack()
    road = network.find_road("1")
    obj = network.add_object(road, _tree("1", 20.0, 5.0))
    with_object = rm.write_xodr(network)

    stack.push(network, rm.edit.delete_object(network, obj))
    assert network.object(obj) is None

    stack.undo(network)
    assert network.object(obj) is not None
    assert rm.write_xodr(network) == with_object


def test_add_object_bad_station_raises(network):
    stack = rm.edit.EditStack()
    road = network.find_road("1")
    with pytest.raises(ValueError):
        stack.push(network, rm.edit.add_object(network, road, _tree("1", 9999.0, 5.0)))
