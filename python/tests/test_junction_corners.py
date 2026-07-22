# Copyright 2026 Robomous
# SPDX-License-Identifier: Apache-2.0

"""Junction corner overrides through the bindings (#225, p4-s1).

`junction_corners` is the shared corner solve (mesher + editor tool + panel);
`edit.set_corner_radius` / `edit.set_corner_extents` author the sparse
`Junction.corners` overrides that persist as `<userData code="rm:corners">`.
"""

import math

import pytest

import roadmaker as rm


def _x_junction(gap_m=24.0):
    """A 4-way crossing wide enough that the derived fillet is not already
    against its `max_radius` — so an authored radius can move in both
    directions."""
    net = rm.RoadNetwork()
    stack = rm.edit.EditStack()
    params = rm.edit.assembly.IntersectionParams()
    params.gap_m = gap_m
    pose = rm.edit.assembly.Pose(0.0, 0.0, 0.0)
    stack.push(net, rm.edit.assembly.x_intersection(net, pose, params))
    return net, stack, net.junction_ids[0]


@pytest.fixture
def crossing():
    """(network, stack, junction_id) for a four-arm crossing."""
    return _x_junction()


def _dist(a, b):
    return math.hypot(a[0] - b[0], a[1] - b[1])


def test_four_arm_junction_solves_four_corners(crossing):
    net, _stack, jid = crossing
    infos = rm.junction_corners(net, jid)

    assert len(infos) == 4
    for info in infos:
        assert 0.0 < info.phi < math.pi
        assert info.radius > 0.0
        assert info.radius <= info.max_radius + 1e-9
        assert info.extent_a > 0.0 and info.extent_b > 0.0
        assert info.extent_a <= info.max_extent_a + 1e-9
        assert info.extent_b <= info.max_extent_b + 1e-9
        assert len(info.curve) >= 2
        assert _dist(info.tangent_a, info.tangent_b) > 1e-6
        # The curve runs tangency -> tangency and its apex lies between them.
        assert info.curve[0] == pytest.approx(info.tangent_a)
        assert info.curve[-1] == pytest.approx(info.tangent_b)
        legs = _dist(info.tangent_a, info.corner) + _dist(info.tangent_b, info.corner)
        assert _dist(info.apex(), info.corner) < legs
        # Nothing authored yet.
        assert not info.radius_authored
        assert not info.extents_authored
    assert net.junction(jid).corners == []


def test_stale_junction_id_solves_no_corners(crossing):
    net, _stack, _jid = crossing
    assert rm.junction_corners(net, rm.JunctionId()) == []


def test_set_corner_radius_round_trips_through_the_stack(crossing):
    net, stack, jid = crossing
    first = rm.junction_corners(net, jid)[0]
    derived = first.radius
    target = 0.5 * (derived + first.max_radius)
    assert target > derived

    stack.push(net, rm.edit.set_corner_radius(net, jid, first.arm_a, first.arm_b, target))

    solved = rm.junction_corners(net, jid)[0]
    assert solved.radius == pytest.approx(target)
    assert solved.radius_authored
    assert not solved.extents_authored
    assert solved.extent_a == pytest.approx(solved.extent_b)
    overrides = net.junction(jid).corners
    assert len(overrides) == 1
    assert overrides[0].radius == pytest.approx(target)
    assert overrides[0].extent_a is None and overrides[0].extent_b is None

    stack.undo(net)
    undone = rm.junction_corners(net, jid)[0]
    assert undone.radius == pytest.approx(derived)
    assert not undone.radius_authored
    assert net.junction(jid).corners == []

    stack.redo(net)
    assert rm.junction_corners(net, jid)[0].radius == pytest.approx(target)


def test_undo_is_byte_identical(crossing):
    net, stack, jid = crossing
    before = rm.write_xodr(net)
    first = rm.junction_corners(net, jid)[0]

    stack.push(net, rm.edit.set_corner_radius(net, jid, first.arm_a, first.arm_b, 8.0))
    assert rm.write_xodr(net) != before

    stack.undo(net)
    assert rm.write_xodr(net) == before


def test_non_positive_radius_clears_the_override(crossing):
    net, stack, jid = crossing
    first = rm.junction_corners(net, jid)[0]
    derived = first.radius

    stack.push(net, rm.edit.set_corner_extents(net, jid, first.arm_a, first.arm_b, 6.0, 9.0))
    stack.push(net, rm.edit.set_corner_radius(net, jid, first.arm_a, first.arm_b, -1.0))

    assert net.junction(jid).corners == []
    cleared = rm.junction_corners(net, jid)[0]
    assert cleared.radius == pytest.approx(derived)
    assert not cleared.radius_authored and not cleared.extents_authored


def test_set_corner_extents_stores_both_and_moves_the_tangencies(crossing):
    net, stack, jid = crossing
    first = rm.junction_corners(net, jid)[0]
    before_a, before_b = first.tangent_a, first.tangent_b

    stack.push(net, rm.edit.set_corner_extents(net, jid, first.arm_a, first.arm_b, 4.0, 9.0))

    solved = rm.junction_corners(net, jid)[0]
    assert solved.extent_a == pytest.approx(4.0)
    assert solved.extent_b == pytest.approx(9.0)
    assert solved.extents_authored
    assert _dist(solved.tangent_a, before_a) > 1e-6
    assert _dist(solved.tangent_b, before_b) > 1e-6
    # Tangency points sit exactly `extent` back along each edge from `corner`.
    for extent, direction, tangent in (
        (solved.extent_a, solved.dir_a, solved.tangent_a),
        (solved.extent_b, solved.dir_b, solved.tangent_b),
    ):
        expected = (
            solved.corner[0] - extent * direction[0],
            solved.corner[1] - extent * direction[1],
        )
        assert tangent == pytest.approx(expected, abs=1e-9)

    override = net.junction(jid).corners[0]
    assert override.extent_a == pytest.approx(4.0)
    assert override.extent_b == pytest.approx(9.0)

    stack.undo(net)
    assert not rm.junction_corners(net, jid)[0].extents_authored


def test_override_survives_an_xodr_round_trip(crossing, tmp_path):
    net, stack, jid = crossing
    first = rm.junction_corners(net, jid)[0]
    stack.push(net, rm.edit.set_corner_radius(net, jid, first.arm_a, first.arm_b, 8.0))
    stack.push(net, rm.edit.set_corner_extents(net, jid, first.arm_a, first.arm_b, 4.0, 9.0))

    path = tmp_path / "corners.xodr"
    rm.save_xodr(net, path)
    assert "rm:corners" in path.read_text()

    reloaded, diagnostics = rm.load_xodr(path)
    assert [d for d in diagnostics if d.severity == rm.Severity.ERROR] == []

    junction = reloaded.junction(reloaded.junction_ids[0])
    assert len(junction.corners) == 1
    override = junction.corners[0]
    assert override.radius == pytest.approx(8.0)
    assert override.extent_a == pytest.approx(4.0)
    assert override.extent_b == pytest.approx(9.0)

    solved = rm.junction_corners(reloaded, reloaded.junction_ids[0])[0]
    assert solved.radius_authored and solved.extents_authored
    assert solved.extent_a == pytest.approx(4.0)


@pytest.mark.parametrize(
    "factory",
    [
        pytest.param(
            lambda net, jid, a, b: rm.edit.set_corner_radius(net, jid, a, b, 5.0), id="radius"
        ),
        pytest.param(
            lambda net, jid, a, b: rm.edit.set_corner_extents(net, jid, a, b, 5.0, 5.0),
            id="extents",
        ),
    ],
)
def test_non_adjacent_arm_pair_raises_value_error(crossing, factory):
    net, stack, jid = crossing
    infos = rm.junction_corners(net, jid)
    # Opposite arms of a 4-way crossing never share a corner.
    arm_a, arm_b = infos[0].arm_a, infos[2].arm_a
    before = rm.write_xodr(net)
    depth = stack.size

    with pytest.raises(ValueError):
        stack.push(net, factory(net, jid, arm_a, arm_b))

    # A failed apply is not recorded and leaves the network untouched.
    assert stack.size == depth
    assert rm.write_xodr(net) == before


def test_stale_junction_id_raises_value_error(crossing):
    net, stack, jid = crossing
    first = rm.junction_corners(net, jid)[0]

    with pytest.raises(ValueError):
        stack.push(
            net, rm.edit.set_corner_radius(net, rm.JunctionId(), first.arm_a, first.arm_b, 5.0)
        )


@pytest.mark.parametrize("extents", [(-1.0, 5.0), (5.0, 0.0)])
def test_non_positive_extents_raise_value_error(crossing, extents):
    net, stack, jid = crossing
    first = rm.junction_corners(net, jid)[0]

    with pytest.raises(ValueError):
        stack.push(net, rm.edit.set_corner_extents(net, jid, first.arm_a, first.arm_b, *extents))


def test_clearing_a_corner_with_no_override_raises_value_error(crossing):
    net, stack, jid = crossing
    first = rm.junction_corners(net, jid)[0]

    with pytest.raises(ValueError):
        stack.push(net, rm.edit.set_corner_radius(net, jid, first.arm_a, first.arm_b, 0.0))


@pytest.mark.parametrize("field", ["arm_a", "arm_b", "radius", "extent_a", "extent_b"])
def test_junction_corner_fields_are_read_only(crossing, field):
    net, stack, jid = crossing
    first = rm.junction_corners(net, jid)[0]
    stack.push(net, rm.edit.set_corner_radius(net, jid, first.arm_a, first.arm_b, 8.0))
    corner = net.junction(jid).corners[0]

    with pytest.raises(AttributeError):
        setattr(corner, field, 1.0)


def test_junction_corners_list_is_read_only(crossing):
    net, _stack, jid = crossing
    with pytest.raises(AttributeError):
        net.junction(jid).corners = []


def test_corner_info_reprs_are_informative(crossing):
    net, stack, jid = crossing
    first = rm.junction_corners(net, jid)[0]
    assert repr(first).startswith("JunctionCornerInfo(")

    stack.push(net, rm.edit.set_corner_radius(net, jid, first.arm_a, first.arm_b, 8.0))
    assert "radius" in repr(net.junction(jid).corners[0])
