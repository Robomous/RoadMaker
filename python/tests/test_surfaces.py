"""Derived ground surfaces through the bindings (#215, p2-s7)."""

import pytest

import roadmaker as rm


def _author_square():
    """Four straight roads welded corner-to-corner into a 10 m square loop."""
    net = rm.RoadNetwork()
    corners = [(0.0, 0.0), (10.0, 0.0), (10.0, 10.0), (0.0, 10.0)]
    for i in range(4):
        rm.author_clothoid_road(
            net,
            [corners[i], corners[(i + 1) % 4]],
            rm.LaneProfile.two_lane_rural(),
            "",
            f"edge{i}",
        )
    return net


def test_enclosed_loop_derives_surface():
    net = _author_square()
    assert net.surface_count == 0

    rm.derive_surfaces(net)

    assert net.surface_count == 1
    surface_id = net.surface_ids[0]
    surface = net.surface(surface_id)
    assert surface.source == rm.BoundarySource.DERIVED
    assert len(surface.bounding_roads) == 4
    # The ring is exactly the four authored roads.
    assert set(surface.bounding_roads) == set(net.road_ids)


def test_derive_surfaces_is_idempotent():
    net = _author_square()
    rm.derive_surfaces(net)
    first = net.surface_ids[0]

    rm.derive_surfaces(net)

    assert net.surface_count == 1
    # Id-stable: the persisting loop keeps its SurfaceId.
    assert net.surface_ids[0] == first


def test_surfaces_touching_reports_ring_roads():
    net = _author_square()
    rm.derive_surfaces(net)
    surface_id = net.surface_ids[0]

    for road in net.road_ids:
        assert surface_id in net.surfaces_touching(road)


def test_surface_marker_round_trips():
    net = _author_square()
    rm.derive_surfaces(net)

    once = rm.write_xodr(net)
    assert "rm:surface" in once

    reparsed, _diagnostics = rm.parse_xodr(once)
    twice = rm.write_xodr(reparsed)
    assert once == twice


def test_set_surface_material_round_trips_and_undoes(tmp_path):
    net = _author_square()
    rm.derive_surfaces(net)
    surface_id = net.surface_ids[0]
    assert net.surface(surface_id).material == ""

    stack = rm.edit.EditStack()
    stack.push(net, rm.edit.set_surface_material(net, surface_id, "asphalt"))
    assert net.surface(surface_id).material == "asphalt"

    # Persists as a material attribute on the rm:surface userData.
    out = tmp_path / "surface.xodr"
    rm.save_xodr(net, str(out))
    reparsed, _diagnostics = rm.parse_xodr(out.read_text())
    assert reparsed.surface(reparsed.surface_ids[0]).material == "asphalt"

    # apply -> undo restores the empty default by contract.
    stack.undo(net)
    assert net.surface(surface_id).material == ""


def test_set_surface_material_stale_id_is_invalid():
    net = _author_square()
    rm.derive_surfaces(net)

    # A stale/never-valid SurfaceId yields an invalid command; pushing it raises
    # and leaves the network untouched (kernel contract).
    stack = rm.edit.EditStack()
    with pytest.raises(ValueError):
        stack.push(net, rm.edit.set_surface_material(net, rm.SurfaceId(), "asphalt"))
