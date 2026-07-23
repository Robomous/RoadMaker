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


# --- authored boundaries (p5-s1, #231) ---------------------------------------


def _author_block():
    """A 60 m block — roomy enough that the enclosed hole survives the union."""
    net = rm.RoadNetwork()
    corners = [(0.0, 0.0), (60.0, 0.0), (60.0, 60.0), (0.0, 60.0)]
    for i in range(4):
        rm.author_clothoid_road(
            net,
            [corners[i], corners[(i + 1) % 4]],
            rm.LaneProfile.two_lane_rural(),
            "",
            f"edge{i}",
        )
    rm.derive_surfaces(net)
    return net, net.surface_ids[0]


def _square(half=15.0):
    """A plain square loop centred on the block, with zero tangents."""
    nodes = []
    for dx, dy in ((-half, -half), (half, -half), (half, half), (-half, half)):
        node = rm.SurfaceNode()
        node.x, node.y = 30.0 + dx, 30.0 + dy
        nodes.append(node)
    return nodes


def test_derived_surface_seeds_boundary_nodes_without_storing_them():
    net, surface_id = _author_block()

    seed = rm.surface_boundary_nodes(net, surface_id)

    assert 3 <= len(seed) <= 24
    surface = net.surface(surface_id)
    assert surface.source == rm.BoundarySource.DERIVED
    assert surface.nodes == []


def test_sample_surface_boundary_passes_through_every_node():
    nodes = _square()
    ring = rm.sample_surface_boundary(nodes)

    assert len(ring) >= len(nodes)
    for node in nodes:
        assert any(
            abs(x - node.x) < 1e-9 and abs(y - node.y) < 1e-9 for x, y in ring
        ), f"({node.x}, {node.y}) is not on the sampled loop"


def test_self_intersecting_boundary_is_detected_and_refused():
    net, surface_id = _author_block()
    bowtie = _square()
    bowtie[2], bowtie[3] = bowtie[3], bowtie[2]

    assert rm.surface_boundary_self_intersects(bowtie)
    stack = rm.edit.EditStack()
    with pytest.raises(ValueError):
        stack.push(net, rm.edit.set_surface_boundary(net, surface_id, bowtie))
    assert net.surface(surface_id).source == rm.BoundarySource.DERIVED


def test_editing_a_derived_boundary_detaches_it_and_undo_reattaches():
    net, surface_id = _author_block()
    nodes = _square()

    stack = rm.edit.EditStack()
    stack.push(net, rm.edit.set_surface_boundary(net, surface_id, nodes))

    surface = net.surface(surface_id)
    assert surface.source == rm.BoundarySource.AUTHORED
    assert surface.nodes == nodes
    # Provenance survives the detach — it is what the elevation comes from.
    assert len(surface.bounding_roads) == 4

    # And derive_surfaces leaves it alone rather than laying a duplicate on it.
    rm.derive_surfaces(net)
    assert net.surface_count == 1
    assert net.surface(surface_id).source == rm.BoundarySource.AUTHORED

    stack.undo(net)
    assert net.surface(surface_id).source == rm.BoundarySource.DERIVED
    assert net.surface(surface_id).nodes == []


def test_revert_surface_to_derived_reclaims_the_surface():
    net, surface_id = _author_block()
    stack = rm.edit.EditStack()
    stack.push(net, rm.edit.set_surface_boundary(net, surface_id, _square()))

    stack.push(net, rm.edit.revert_surface_to_derived(net, surface_id))
    assert net.surface(surface_id).source == rm.BoundarySource.DERIVED
    assert net.surface(surface_id).nodes == []

    rm.derive_surfaces(net)
    assert net.surface_count == 1
    # The ring still matches, so the id survives rather than churning.
    assert net.surface_ids[0] == surface_id


def test_revert_on_a_derived_surface_is_invalid():
    net, surface_id = _author_block()
    stack = rm.edit.EditStack()
    with pytest.raises(ValueError):
        stack.push(net, rm.edit.revert_surface_to_derived(net, surface_id))


def test_authored_boundary_round_trips_byte_identical(tmp_path):
    net, surface_id = _author_block()
    stack = rm.edit.EditStack()
    stack.push(net, rm.edit.set_surface_boundary(net, surface_id, _square()))

    once = rm.write_xodr(net)
    assert "nodes=" in once

    reparsed, _diagnostics = rm.parse_xodr(once)
    assert rm.write_xodr(reparsed) == once

    loaded = reparsed.surface(reparsed.surface_ids[0])
    assert loaded.source == rm.BoundarySource.AUTHORED
    assert loaded.nodes == _square()
    assert len(loaded.bounding_roads) == 4


def test_derived_surface_still_writes_no_nodes_attribute():
    net, _surface_id = _author_block()
    xml = rm.write_xodr(net)
    assert "rm:surface" in xml
    assert "nodes=" not in xml
