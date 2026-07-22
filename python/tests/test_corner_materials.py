# Copyright 2026 Robomous
# SPDX-License-Identifier: Apache-2.0

"""Per-corner and junction-wide materials through the bindings (#226, p4-s2).

`edit.set_corner_sidewalk_material` / `edit.set_corner_median_material` author
the overlay materials of ONE corner; `edit.set_junction_material` and
`edit.set_junction_default_corner_radius` author the junction-wide carriageway
material and the fallback fillet radius. All four persist as `<userData>` on
`<junction>` (`rm:corners` for the per-corner fields, `rm:junction` for the
junction-wide ones) — ASAM OpenDRIVE 1.9.0 §12.10 gives `<boundary>` no
material or radius carrier.
"""

import pytest

import roadmaker as rm


@pytest.fixture
def crossing():
    """(network, stack, junction_id) for a roomy four-arm crossing — wide
    enough that an authored radius can move in both directions."""
    net = rm.RoadNetwork()
    stack = rm.edit.EditStack()
    params = rm.edit.assembly.IntersectionParams()
    params.gap_m = 24.0
    pose = rm.edit.assembly.Pose(0.0, 0.0, 0.0)
    stack.push(net, rm.edit.assembly.x_intersection(net, pose, params))
    return net, stack, net.junction_ids[0]


# --- per-corner materials ----------------------------------------------------


@pytest.mark.parametrize(
    ("setter", "corner_field", "info_field"),
    [
        pytest.param(
            "set_corner_sidewalk_material", "sidewalk_material", "sidewalk_material", id="sidewalk"
        ),
        pytest.param(
            "set_corner_median_material", "median_material", "median_material", id="median"
        ),
    ],
)
def test_corner_material_set_clear_roundtrip(crossing, setter, corner_field, info_field):
    net, stack, jid = crossing
    first = rm.junction_corners(net, jid)[0]
    assert getattr(first, info_field) == ""
    before = rm.write_xodr(net)
    push = getattr(rm.edit, setter)

    stack.push(net, push(net, jid, first.arm_a, first.arm_b, "concrete"))

    overrides = net.junction(jid).corners
    assert len(overrides) == 1
    assert getattr(overrides[0], corner_field) == "concrete"
    solved = rm.junction_corners(net, jid)[0]
    assert getattr(solved, info_field) == "concrete"
    # A material is pure pass-through: the corner geometry is untouched.
    assert solved.radius == pytest.approx(first.radius)
    assert not solved.radius_authored

    # Undo is byte-identical, redo re-applies.
    stack.undo(net)
    assert rm.write_xodr(net) == before
    assert getattr(rm.junction_corners(net, jid)[0], info_field) == ""
    stack.redo(net)
    assert getattr(rm.junction_corners(net, jid)[0], info_field) == "concrete"

    # An empty material clears the slot, and with nothing else authored the
    # whole sparse entry goes away.
    stack.push(net, push(net, jid, first.arm_a, first.arm_b, ""))
    assert net.junction(jid).corners == []
    assert getattr(rm.junction_corners(net, jid)[0], info_field) == ""


def test_corner_materials_are_independent_slots(crossing):
    net, stack, jid = crossing
    first = rm.junction_corners(net, jid)[0]

    stack.push(
        net, rm.edit.set_corner_sidewalk_material(net, jid, first.arm_a, first.arm_b, "concrete")
    )
    stack.push(
        net, rm.edit.set_corner_median_material(net, jid, first.arm_a, first.arm_b, "grass")
    )

    override = net.junction(jid).corners[0]
    assert override.sidewalk_material == "concrete"
    assert override.median_material == "grass"

    # Clearing one leaves the other (and the entry) alive.
    stack.push(net, rm.edit.set_corner_sidewalk_material(net, jid, first.arm_a, first.arm_b, ""))
    override = net.junction(jid).corners[0]
    assert override.sidewalk_material is None
    assert override.median_material == "grass"


@pytest.mark.parametrize("token", ["with space", "a:b", "a;b", "bad/slash"])
def test_illegal_material_tokens_raise_value_error(crossing, token):
    net, stack, jid = crossing
    first = rm.junction_corners(net, jid)[0]
    before = rm.write_xodr(net)
    depth = stack.size

    with pytest.raises(ValueError):
        stack.push(
            net, rm.edit.set_corner_sidewalk_material(net, jid, first.arm_a, first.arm_b, token)
        )

    # A failed apply is not recorded and leaves the network untouched.
    assert stack.size == depth
    assert rm.write_xodr(net) == before


# --- junction-wide default corner radius -------------------------------------


def test_junction_default_radius_resolution(crossing):
    net, stack, jid = crossing
    infos = rm.junction_corners(net, jid)
    derived = [info.radius for info in infos]
    assert all(not info.radius_from_junction_default for info in infos)
    assert net.junction(jid).default_corner_radius is None

    default = min(info.max_radius for info in infos) * 0.5
    stack.push(net, rm.edit.set_junction_default_corner_radius(net, jid, default))

    assert net.junction(jid).default_corner_radius == pytest.approx(default)
    for info in rm.junction_corners(net, jid):
        assert info.radius == pytest.approx(default)
        assert info.radius_from_junction_default
        assert not info.radius_authored

    # A per-corner override BEATS the junction default, and only on its corner.
    first = infos[0]
    per_corner = default * 0.5
    stack.push(net, rm.edit.set_corner_radius(net, jid, first.arm_a, first.arm_b, per_corner))

    solved = rm.junction_corners(net, jid)
    assert solved[0].radius == pytest.approx(per_corner)
    assert solved[0].radius_authored
    # Never both at once.
    assert not solved[0].radius_from_junction_default
    for info in solved[1:]:
        assert info.radius == pytest.approx(default)
        assert info.radius_from_junction_default

    # Dropping the per-corner override falls back to the default, not derived.
    stack.push(net, rm.edit.set_corner_radius(net, jid, first.arm_a, first.arm_b, 0.0))
    fallback = rm.junction_corners(net, jid)[0]
    assert fallback.radius == pytest.approx(default)
    assert fallback.radius_from_junction_default

    # Clearing the default returns every corner to its derived radius.
    stack.push(net, rm.edit.set_junction_default_corner_radius(net, jid, 0.0))
    assert net.junction(jid).default_corner_radius is None
    for info, want in zip(rm.junction_corners(net, jid), derived):
        assert info.radius == pytest.approx(want)
        assert not info.radius_from_junction_default


# --- clearing what is not authored is an error -------------------------------


@pytest.mark.parametrize(
    "factory",
    [
        pytest.param(
            lambda net, jid, a, b: rm.edit.set_corner_sidewalk_material(net, jid, a, b, ""),
            id="corner_sidewalk",
        ),
        pytest.param(
            lambda net, jid, a, b: rm.edit.set_corner_median_material(net, jid, a, b, ""),
            id="corner_median",
        ),
        pytest.param(
            lambda net, jid, a, b: rm.edit.set_junction_default_corner_radius(net, jid, 0.0),
            id="junction_default_radius",
        ),
        pytest.param(
            lambda net, jid, a, b: rm.edit.set_junction_material(net, jid, ""),
            id="junction_material",
        ),
    ],
)
def test_clear_without_override_raises(crossing, factory):
    net, stack, jid = crossing
    first = rm.junction_corners(net, jid)[0]
    before = rm.write_xodr(net)
    depth = stack.size

    with pytest.raises(ValueError):
        stack.push(net, factory(net, jid, first.arm_a, first.arm_b))

    assert stack.size == depth
    assert rm.write_xodr(net) == before


@pytest.mark.parametrize(
    "factory",
    [
        pytest.param(
            lambda net, jid, a, b: rm.edit.set_corner_sidewalk_material(
                net, jid, a, b, "concrete"
            ),
            id="corner_sidewalk",
        ),
        pytest.param(
            lambda net, jid, a, b: rm.edit.set_corner_median_material(net, jid, a, b, "grass"),
            id="corner_median",
        ),
        pytest.param(
            lambda net, jid, a, b: rm.edit.set_junction_default_corner_radius(net, jid, 6.0),
            id="junction_default_radius",
        ),
        pytest.param(
            lambda net, jid, a, b: rm.edit.set_junction_material(net, jid, "asphalt"),
            id="junction_material",
        ),
    ],
)
def test_stale_junction_id_raises(crossing, factory):
    net, stack, jid = crossing
    first = rm.junction_corners(net, jid)[0]

    with pytest.raises(ValueError):
        stack.push(net, factory(net, rm.JunctionId(), first.arm_a, first.arm_b))


# --- junction-wide material + persistence ------------------------------------


def test_junction_material_xodr_roundtrip(crossing, tmp_path):
    net, stack, jid = crossing
    first = rm.junction_corners(net, jid)[0]
    assert net.junction(jid).material == ""

    stack.push(net, rm.edit.set_junction_material(net, jid, "cobblestone"))
    stack.push(net, rm.edit.set_junction_default_corner_radius(net, jid, 7.5))
    stack.push(
        net, rm.edit.set_corner_sidewalk_material(net, jid, first.arm_a, first.arm_b, "concrete")
    )
    stack.push(
        net, rm.edit.set_corner_median_material(net, jid, first.arm_a, first.arm_b, "grass")
    )
    assert net.junction(jid).material == "cobblestone"

    path = tmp_path / "corner_materials.xodr"
    rm.save_xodr(net, path)
    text = path.read_text()
    assert "rm:junction" in text
    assert "rm:corners" in text

    reloaded, diagnostics = rm.load_xodr(path)
    assert [d for d in diagnostics if d.severity == rm.Severity.ERROR] == []

    reloaded_jid = reloaded.junction_ids[0]
    junction = reloaded.junction(reloaded_jid)
    assert junction.material == "cobblestone"
    assert junction.default_corner_radius == pytest.approx(7.5)
    assert len(junction.corners) == 1
    assert junction.corners[0].sidewalk_material == "concrete"
    assert junction.corners[0].median_material == "grass"

    solved = rm.junction_corners(reloaded, reloaded_jid)
    assert solved[0].sidewalk_material == "concrete"
    assert solved[0].median_material == "grass"
    for info in solved:
        assert info.radius == pytest.approx(min(7.5, info.max_radius))
        assert info.radius_from_junction_default


def test_junction_wide_fields_are_read_only(crossing):
    net, stack, jid = crossing
    stack.push(net, rm.edit.set_junction_material(net, jid, "cobblestone"))
    junction = net.junction(jid)

    with pytest.raises(AttributeError):
        junction.material = "asphalt"
    with pytest.raises(AttributeError):
        junction.default_corner_radius = 4.0


@pytest.mark.parametrize("field", ["sidewalk_material", "median_material"])
def test_corner_material_fields_are_read_only(crossing, field):
    net, stack, jid = crossing
    first = rm.junction_corners(net, jid)[0]
    stack.push(
        net, rm.edit.set_corner_sidewalk_material(net, jid, first.arm_a, first.arm_b, "concrete")
    )

    with pytest.raises(AttributeError):
        setattr(net.junction(jid).corners[0], field, "grass")
