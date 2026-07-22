# Copyright 2026 Robomous
# SPDX-License-Identifier: Apache-2.0

"""Junction floor surface spans through the Python bindings (p4-s5, #320).

A junction's floor is a union of one contribution — a *surface span* — per
connecting road. `junction_surface_spans` exposes them with the exact samples
the mesher used, and two commands control how each takes part: Include Samples
(samples-only) and a free Sort Index (higher wins on overlap).

Not to be confused with `SpanArm`, a VIRTUAL junction's s-interval — a span
junction has no floor at all, which these tests pin down explicitly.
"""

import pytest

import roadmaker as rm


@pytest.fixture
def cross():
    """(network, junction_id) for a roomy four-arm crossing — wide enough that
    every turn contributes a real ribbon to the floor union."""
    network = rm.RoadNetwork()
    stack = rm.edit.EditStack()
    params = rm.edit.assembly.IntersectionParams()
    params.gap_m = 24.0
    stack.push(
        network,
        rm.edit.assembly.x_intersection(network, rm.edit.assembly.Pose(0.0, 0.0, 0.0), params),
    )
    return network, network.junction_ids[0]


def test_query_returns_one_span_per_connecting_road(cross):
    network, junction = cross
    spans = rm.junction_surface_spans(network, junction)
    assert spans

    turns = []
    for connection in network.junction(junction).connections:
        if connection.connecting_road not in turns:
            turns.append(connection.connecting_road)
    assert [info.road for info in spans] == turns

    for info in spans:
        assert info.included
        assert info.sort_index == 0
        assert not info.authored
        assert info.road_odr_id == network.road(info.road).odr_id
        # The exact samples the floor was built from, not a re-derivation.
        assert len(info.footprint) >= 3
        assert len(info.border) == len(info.footprint)
        assert len(info.centerline) == len(info.footprint) // 2


def test_commands_are_one_undo_step_each(cross):
    network, junction = cross
    stack = rm.edit.EditStack()
    road = rm.junction_surface_spans(network, junction)[0].road

    stack.push(network, rm.edit.set_surface_span_included(network, junction, road, False))
    stack.push(network, rm.edit.set_surface_span_sort_index(network, junction, road, 3))
    info = next(s for s in rm.junction_surface_spans(network, junction) if s.road == road)
    assert info.authored and not info.included and info.sort_index == 3

    stack.undo(network)
    info = next(s for s in rm.junction_surface_spans(network, junction) if s.road == road)
    assert not info.included and info.sort_index == 0

    stack.undo(network)
    assert not network.junction(junction).surface_spans

    stack.redo(network)
    stack.redo(network)
    record = network.junction(junction).surface_spans[0]
    assert record.road == road
    assert not record.included
    assert record.sort_index == 3


def test_returning_both_fields_to_default_erases_the_record(cross):
    network, junction = cross
    stack = rm.edit.EditStack()
    road = rm.junction_surface_spans(network, junction)[0].road
    before = rm.write_xodr(network, "spans")

    stack.push(network, rm.edit.set_surface_span_included(network, junction, road, False))
    stack.push(network, rm.edit.set_surface_span_sort_index(network, junction, road, 2))
    assert rm.write_xodr(network, "spans") != before

    stack.push(network, rm.edit.set_surface_span_sort_index(network, junction, road, 0))
    stack.push(network, rm.edit.set_surface_span_included(network, junction, road, True))
    assert not network.junction(junction).surface_spans
    assert rm.write_xodr(network, "spans") == before


def test_records_round_trip_through_xodr(tmp_path, cross):
    network, junction = cross
    stack = rm.edit.EditStack()
    spans = rm.junction_surface_spans(network, junction)
    stack.push(network, rm.edit.set_surface_span_included(network, junction, spans[0].road, False))
    stack.push(network, rm.edit.set_surface_span_sort_index(network, junction, spans[1].road, -2))

    path = tmp_path / "spans.xodr"
    rm.save_xodr(network, path, "spans")
    assert 'code="rm:floor"' in path.read_text()

    reloaded_network, diagnostics = rm.load_xodr(path)
    assert not [d for d in diagnostics if d.severity == rm.Severity.ERROR]
    reloaded = rm.junction_surface_spans(reloaded_network, reloaded_network.junction_ids[0])
    assert [(s.included, s.sort_index) for s in reloaded] == [
        (s.included, s.sort_index) for s in rm.junction_surface_spans(network, junction)
    ]


def test_no_ops_and_stale_targets_raise(cross):
    network, junction = cross
    stack = rm.edit.EditStack()
    road = rm.junction_surface_spans(network, junction)[0].road

    # A no-op is judged against the EFFECTIVE value, so "already included" is
    # refused even though no record exists to compare against.
    with pytest.raises(ValueError):
        stack.push(network, rm.edit.set_surface_span_included(network, junction, road, True))
    with pytest.raises(ValueError):
        stack.push(network, rm.edit.set_surface_span_sort_index(network, junction, road, 0))
    assert not network.junction(junction).surface_spans

    # An ARM road is not a connecting road, so it is not a span of the floor.
    arm = network.junction(junction).arms[0].road
    with pytest.raises(ValueError):
        stack.push(network, rm.edit.set_surface_span_included(network, junction, arm, False))

    with pytest.raises(ValueError):
        stack.push(network, rm.edit.set_surface_span_sort_index(network, junction, road, 100_000))


def test_a_span_junction_has_no_floor_to_control():
    network = rm.RoadNetwork()
    road = rm.author_clothoid_road(
        network, [(0.0, 0.0), (120.0, 0.0)], rm.LaneProfile.two_lane_default(), "", "1"
    )
    stack = rm.edit.EditStack()
    stack.push(network, rm.edit.create_span_junction(network, [rm.SpanArm(road, 50.0, 56.5)]))
    span_junction = network.junction_ids[0]

    assert rm.junction_surface_spans(network, span_junction) == []
    with pytest.raises(ValueError):
        stack.push(network, rm.edit.set_surface_span_included(network, span_junction, road, False))
