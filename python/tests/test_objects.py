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


def test_expand_repeat_rounds_down_and_keeps_endpoint():
    # §13.4: floor(length / distance) + 1 instances, no incomplete trailing one.
    repeat = rm.ObjectRepeat()
    repeat.s, repeat.length, repeat.distance = 5.0, 110.0, 15.0
    repeat.t_start = repeat.t_end = 8.0

    instances = rm.expand_repeat(repeat)
    assert len(instances) == 8  # floor(110/15)=7 -> 8 instances at ds 0..105
    assert instances[0].s == pytest.approx(5.0)
    assert instances[-1].s == pytest.approx(5.0 + 105.0)  # last origin inside section
    assert all(inst.t == pytest.approx(8.0) for inst in instances)

    # Exact fit keeps the endpoint instance (length == k*distance).
    repeat.length = 30.0
    exact = rm.expand_repeat(repeat)
    assert len(exact) == 3  # ds = 0, 15, 30
    assert exact[-1].s == pytest.approx(5.0 + 30.0)

    # A continuous object (distance == 0) is extruded, not instanced.
    repeat.distance = 0.0
    assert rm.expand_repeat(repeat) == []


def test_expand_repeat_cubic_t_overrides_linear():
    # §13.4: any of bT/cT/dT present selects the cubic t(ds); 1.8.1 has no cubic.
    repeat = rm.ObjectRepeat()
    repeat.s, repeat.length, repeat.distance = 0.0, 20.0, 10.0
    repeat.t_start, repeat.t_end = 1.0, 99.0  # tEnd is ignored under the cubic
    repeat.b_t = 0.5

    instances = rm.expand_repeat(repeat)
    assert len(instances) == 3  # ds = 0, 10, 20
    assert instances[0].t == pytest.approx(1.0)  # t_start + 0.5*0
    assert instances[1].t == pytest.approx(1.0 + 0.5 * 10.0)
    assert instances[2].t == pytest.approx(1.0 + 0.5 * 20.0)


def test_object_markings_round_trip(network_with_road, tmp_path):
    network, road_id = network_with_road
    crosswalk = rm.Object()
    crosswalk.odr_id = "1"
    crosswalk.type = rm.ObjectType.CROSSWALK
    crosswalk.s, crosswalk.t = 50.0, 0.0
    corners = []
    for i, (a, b) in enumerate([(48.0, -2.0), (52.0, -2.0), (52.0, 2.0), (48.0, 2.0)]):
        corner = rm.OutlineCorner(a, b)
        corner.id = i
        corners.append(corner)
    marking = rm.ObjectMarking()
    marking.color = "white"
    marking.line_length = 0.2
    marking.space_length = 0.05
    marking.corner_refs = [0, 1, 2, 3]
    outline = rm.ObjectOutline()
    outline.closed = True
    outline.fill_type = "paint"
    outline.corners = corners
    outline.markings = [marking]
    crosswalk.outlines = [outline]
    network.add_object(road_id, crosswalk)

    path = tmp_path / "markings.xodr"
    rm.save_xodr(network, path, name="markings")
    reloaded, diagnostics = rm.load_xodr(path)
    assert not [d for d in diagnostics if d.severity == rm.Severity.ERROR]
    obj = reloaded.object(reloaded.object_ids[0])
    # The default writer targets 1.8.1, which demotes the outline-nested marking
    # to object level; either placement is a faithful round-trip.
    markings = list(obj.markings) + list(obj.outlines[0].markings)
    assert len(markings) == 1
    assert markings[0].color == "white"
    assert markings[0].corner_refs == [0, 1, 2, 3]


def test_crosswalk_data_round_trip_and_override(network_with_road, tmp_path):
    network, road_id = network_with_road
    crosswalk = make_crosswalk()
    data = rm.CrosswalkData()
    data.asset = "crosswalk.zebra"
    data.dash_length = 0.4
    data.dash_gap = 0.6
    data.material = "material.paint_white"
    data.material_override = True
    crosswalk.crosswalk = data
    network.add_object(road_id, crosswalk)

    path = tmp_path / "crosswalk.xodr"
    rm.save_xodr(network, path, name="crosswalk")
    reloaded, diagnostics = rm.load_xodr(path)
    assert not [d for d in diagnostics if d.severity == rm.Severity.ERROR]
    obj = reloaded.object(reloaded.object_ids[0])
    assert obj.crosswalk is not None
    assert obj.crosswalk.asset == "crosswalk.zebra"
    assert obj.crosswalk.dash_length == pytest.approx(0.4)
    assert obj.crosswalk.material_override is True


def test_update_objects_undo_parity(network_with_road):
    network, road_id = network_with_road
    crosswalk = make_crosswalk()
    data = rm.CrosswalkData()
    data.asset = "crosswalk.zebra"
    data.dash_length = 0.5
    crosswalk.crosswalk = data
    object_id = network.add_object(road_id, crosswalk)
    before = rm.write_xodr(network, name="cw")

    # Copy the live object (keeps its read-only road), edit the copy, and pass it
    # as the new value — the live object is untouched until the command applies.
    updated = rm.Object(network.object(object_id))
    new_data = updated.crosswalk
    new_data.dash_length = 0.3
    updated.crosswalk = new_data
    stack = rm.edit.EditStack()
    stack.push(network, rm.edit.update_objects(network, [(object_id, updated)], "Edit Asset"))
    assert network.object(object_id).crosswalk.dash_length == pytest.approx(0.3)

    stack.undo(network)
    assert rm.write_xodr(network, name="cw") == before
    stack.redo(network)
    assert network.object(object_id).crosswalk.dash_length == pytest.approx(0.3)


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


# --- free-form marking curves + arrow stencils (p3-s4) -----------------------


def test_marking_curve_authoring_round_trip(network_with_road, tmp_path):
    network, road_id = network_with_road
    centerline = [(10.0, 0.0), (12.0, 0.5), (14.0, 1.0), (16.0, 1.5)]
    params = rm.edit.MarkingCurveParams()
    params.width_m = 0.2
    params.asset = "marking.solid_white"
    params.material = "material.paint_white"

    curve = rm.Object()
    curve.odr_id = "1"
    curve = rm.edit.apply_marking_curve_asset(curve, centerline, params)
    assert curve.marking_curve is not None
    assert curve.marking_curve.asset == "marking.solid_white"
    assert len(curve.marking_curve.samples) == len(centerline)
    network.add_object(road_id, curve)

    path = tmp_path / "curve.xodr"
    rm.save_xodr(network, str(path), name="curve")
    reloaded, _ = rm.load_xodr(str(path))
    obj = next(iter(reloaded.objects_of(reloaded.road_ids[0])))
    data = reloaded.object(obj).marking_curve
    assert data is not None
    assert data.width == pytest.approx(0.2)
    assert len(data.samples) == len(centerline)


def test_marking_curve_rejects_degenerate():
    params = rm.edit.MarkingCurveParams()
    with pytest.raises(ValueError):
        rm.edit.apply_marking_curve_asset(rm.Object(), [(0.0, 0.0)], params)


def test_stencil_authoring_and_round_trip(network_with_road, tmp_path):
    network, road_id = network_with_road
    params = rm.edit.StencilParams()
    params.subtype = "arrowLeft"
    params.material = "material.paint_white"
    params.asset = "stencil.arrow_left"

    stencil = rm.Object()
    stencil.odr_id = "1"
    stencil = rm.edit.apply_stencil_asset(stencil, params)
    stencil.s, stencil.t = 40.0, -1.75
    assert stencil.stencil is not None
    assert stencil.stencil.asset == "stencil.arrow_left"
    assert not stencil.outlines[0].road_coords  # cornerLocal
    network.add_object(road_id, stencil)

    path = tmp_path / "stencil.xodr"
    rm.save_xodr(network, str(path), name="stencil")
    reloaded, _ = rm.load_xodr(str(path))
    obj = next(iter(reloaded.objects_of(reloaded.road_ids[0])))
    assert reloaded.object(obj).stencil.asset == "stencil.arrow_left"


def test_arrow_glyph_outline_unknown_is_empty():
    assert rm.edit.arrow_glyph_outline("arrowStraight", 4.0, 1.75)
    assert rm.edit.arrow_glyph_outline("arrowMergeLeft", 4.0, 1.75) == []


def test_stencil_rejects_unknown_subtype():
    params = rm.edit.StencilParams()
    params.subtype = "arrowMergeLeft"
    with pytest.raises(ValueError):
        rm.edit.apply_stencil_asset(rm.Object(), params)
