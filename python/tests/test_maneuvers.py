# Copyright 2026 Robomous
# SPDX-License-Identifier: Apache-2.0

"""Junction maneuvers through the Python bindings (p4-s6, #227).

A *maneuver* is ONE connecting road's path through a junction. Its turn type is
DERIVED from the arm-face headings — ASAM OpenDRIVE has no carrier for it
(§12.2 Table 56 gives `<connection>` no such attribute) — so `junction_maneuvers`
reports the computed type merged over whatever a sparse `Maneuver` record
authors, and six commands author it: lock, turn type, path, reset, rebuild and
the explicit U-turn the planner never emits.
"""

import math

import pytest

import roadmaker as rm


@pytest.fixture
def cross():
    """(network, junction_id) for a roomy four-arm crossing — 12 maneuvers,
    three per arm, all of them derived."""
    network = rm.RoadNetwork()
    stack = rm.edit.EditStack()
    params = rm.edit.assembly.IntersectionParams()
    params.gap_m = 24.0
    stack.push(
        network,
        rm.edit.assembly.x_intersection(network, rm.edit.assembly.Pose(0.0, 0.0, 0.0), params),
    )
    return network, network.junction_ids[0]


def maneuver(network, junction, road):
    return next(m for m in rm.junction_maneuvers(network, junction) if m.road == road)


def test_query_reports_one_derived_maneuver_per_connecting_road(cross):
    network, junction = cross
    maneuvers = rm.junction_maneuvers(network, junction)

    turns = []
    for connection in network.junction(junction).connections:
        if connection.connecting_road not in turns:
            turns.append(connection.connecting_road)
    assert [info.road for info in maneuvers] == turns

    # A 4-arm crossing plans left/straight/right off every arm and no U-turn.
    counts = {kind: 0 for kind in (rm.TurnType.LEFT, rm.TurnType.STRAIGHT, rm.TurnType.RIGHT)}
    for info in maneuvers:
        counts[info.effective] += 1
    assert counts == {rm.TurnType.LEFT: 4, rm.TurnType.STRAIGHT: 4, rm.TurnType.RIGHT: 4}

    for info in maneuvers:
        # Every field of the record, round-tripped through the binding.
        assert info.road_odr_id == network.road(info.road).odr_id
        assert info.effective == info.computed
        assert not info.overridden
        assert not info.locked
        assert not info.authored
        assert not info.is_uturn_explicit
        assert info.from_ != info.to
        assert info.from_lane != 0 and info.to_lane != 0
        assert math.isfinite(info.start_heading) and math.isfinite(info.end_heading)
        assert info.start_offset == 0.0 and info.end_offset == 0.0
        assert info.control_points == []
        assert len(info.path) >= 2
        assert len(info.path[0]) == 3
        for slide in (info.start_slide, info.end_slide):
            assert slide.min_offset < slide.max_offset
            assert len(slide.anchor) == 2
            # The anchor IS offset 0 — where an unauthored endpoint sits.
            assert slide.min_offset <= 0.0 <= slide.max_offset
            assert math.dist(slide.min_point, slide.max_point) == pytest.approx(
                slide.max_offset - slide.min_offset
            )

    assert not network.junction(junction).maneuvers


def test_turn_type_override_is_semantic_and_reversible(cross):
    network, junction = cross
    stack = rm.edit.EditStack()
    derived = next(
        m for m in rm.junction_maneuvers(network, junction) if m.computed == rm.TurnType.LEFT
    )
    road = derived.road
    before = rm.write_xodr(network, "maneuvers")

    stack.push(network, rm.edit.set_maneuver_turn_type(network, junction, road, rm.TurnType.UTURN))
    info = maneuver(network, junction, road)
    assert info.effective == rm.TurnType.UTURN
    assert info.computed == rm.TurnType.LEFT
    assert info.overridden and info.authored and not info.locked
    # ... and the geometry did not move: the override is a label.
    assert info.control_points == []
    assert info.path == derived.path

    record = network.junction(junction).maneuvers[0]
    assert record.road == road
    assert record.turn_type == rm.TurnType.UTURN
    assert not record.locked
    assert record.start_offset is None and record.end_offset is None
    assert record.control_points == []

    # AUTHORS-NOTHING => ERASE: clearing drops the record, not merely the field.
    stack.push(network, rm.edit.set_maneuver_turn_type(network, junction, road, None))
    assert not network.junction(junction).maneuvers
    assert rm.write_xodr(network, "maneuvers") == before


def test_setting_the_computed_type_clears_the_override(cross):
    network, junction = cross
    stack = rm.edit.EditStack()
    info = rm.junction_maneuvers(network, junction)[0]
    stack.push(
        network, rm.edit.set_maneuver_turn_type(network, junction, info.road, rm.TurnType.UTURN)
    )
    # Storing the COMPUTED value is a change (it drops the override) but never
    # pins what the derivation already says.
    stack.push(
        network, rm.edit.set_maneuver_turn_type(network, junction, info.road, info.computed)
    )
    assert not network.junction(junction).maneuvers
    assert not maneuver(network, junction, info.road).overridden


def test_set_maneuver_path_locks_in_the_same_undo_step(cross):
    network, junction = cross
    stack = rm.edit.EditStack()
    info = rm.junction_maneuvers(network, junction)[0]
    mid = info.path[len(info.path) // 2]

    stack.push(
        network,
        rm.edit.set_maneuver_path(
            network, junction, info.road, [(mid[0] + 1.5, mid[1] + 1.5)], 0.8
        ),
    )
    shaped = maneuver(network, junction, info.road)
    assert shaped.locked and shaped.authored
    assert len(shaped.control_points) == 1
    assert shaped.control_points[0].x == pytest.approx(mid[0] + 1.5)
    assert shaped.start_offset == pytest.approx(0.8)
    assert shaped.end_offset == 0.0
    assert shaped.path != info.path

    # ONE command: the implicit lock is one undo away, not two.
    stack.undo(network)
    assert not network.junction(junction).maneuvers
    assert not maneuver(network, junction, info.road).locked


def test_lock_and_hand_shaped_geometry_survive_regeneration(cross):
    network, junction = cross
    stack = rm.edit.EditStack()
    info = rm.junction_maneuvers(network, junction)[0]
    mid = info.path[len(info.path) // 2]
    stack.push(
        network,
        rm.edit.set_maneuver_path(network, junction, info.road, [(mid[0] + 1.5, mid[1] + 1.5)]),
    )
    shaped = maneuver(network, junction, info.road).path

    stack.push(network, rm.edit.regenerate_junction(network, junction))
    kept = maneuver(network, junction, info.road)
    assert kept.locked
    assert len(kept.control_points) == 1
    assert kept.path == shaped

    # An unlocked sibling was replanned all the same.
    other = rm.junction_maneuvers(network, junction)[1]
    assert not other.locked and not other.authored


def test_explicit_uturn_survives_regeneration_and_cannot_be_reset(cross):
    network, junction = cross
    stack = rm.edit.EditStack()
    arm = network.junction(junction).arms[0]
    derived = len(rm.junction_maneuvers(network, junction))

    stack.push(network, rm.edit.add_uturn_maneuver(network, junction, arm))
    maneuvers = rm.junction_maneuvers(network, junction)
    assert len(maneuvers) == derived + 1
    uturn = next(m for m in maneuvers if m.is_uturn_explicit)
    assert uturn.from_ == uturn.to == arm
    assert uturn.effective == rm.TurnType.UTURN
    # The lock is what keeps a turn no plan contains alive.
    assert uturn.locked and uturn.authored

    stack.push(network, rm.edit.regenerate_junction(network, junction))
    assert any(m.is_uturn_explicit for m in rm.junction_maneuvers(network, junction))

    # It has no derived path to fall back on, so reset is refused outright.
    with pytest.raises(ValueError):
        stack.push(network, rm.edit.reset_maneuver(network, junction, uturn.road))

    # A second U-turn on the same arm is a duplicate connection.
    with pytest.raises(ValueError):
        stack.push(network, rm.edit.add_uturn_maneuver(network, junction, arm))


def test_rebuild_clears_geometry_but_keeps_turn_type_overrides(cross):
    network, junction = cross
    stack = rm.edit.EditStack()
    infos = rm.junction_maneuvers(network, junction)
    shaped, labelled = infos[0], infos[1]
    mid = shaped.path[len(shaped.path) // 2]

    stack.push(
        network,
        rm.edit.set_maneuver_path(
            network, junction, shaped.road, [(mid[0] + 1.5, mid[1] + 1.5)], 0.7
        ),
    )
    stack.push(
        network,
        rm.edit.set_maneuver_turn_type(network, junction, labelled.road, rm.TurnType.UTURN),
    )
    stack.push(network, rm.edit.add_uturn_maneuver(network, junction, network.junction(
        junction).arms[0]))

    stack.push(network, rm.edit.rebuild_maneuvers(network, junction))

    rebuilt = maneuver(network, junction, shaped.road)
    assert not rebuilt.locked
    assert rebuilt.control_points == []
    assert rebuilt.start_offset == 0.0 and rebuilt.end_offset == 0.0

    # SEMANTIC, so it survives: the rebuild only clears geometry.
    kept = maneuver(network, junction, labelled.road)
    assert kept.overridden and kept.effective == rm.TurnType.UTURN
    assert [record.road for record in network.junction(junction).maneuvers] == [labelled.road]

    # The explicit U-turn is a turn no plan contains, so it goes.
    assert not any(m.is_uturn_explicit for m in rm.junction_maneuvers(network, junction))

    # Nothing left to rebuild is a no-op, and a no-op is refused.
    with pytest.raises(ValueError):
        stack.push(network, rm.edit.rebuild_maneuvers(network, junction))


def test_reset_returns_one_maneuver_to_the_derivation(cross):
    network, junction = cross
    stack = rm.edit.EditStack()
    info = rm.junction_maneuvers(network, junction)[0]
    mid = info.path[len(info.path) // 2]
    stack.push(
        network,
        rm.edit.set_maneuver_path(network, junction, info.road, [(mid[0] + 1.5, mid[1] + 1.5)]),
    )

    stack.push(network, rm.edit.reset_maneuver(network, junction, info.road))
    reset = maneuver(network, junction, info.road)
    assert not reset.authored and not reset.locked
    assert reset.control_points == []
    assert not network.junction(junction).maneuvers

    # Nothing authored left to reset.
    with pytest.raises(ValueError):
        stack.push(network, rm.edit.reset_maneuver(network, junction, info.road))


def test_records_round_trip_through_xodr(tmp_path, cross):
    network, junction = cross
    stack = rm.edit.EditStack()
    infos = rm.junction_maneuvers(network, junction)
    mid = infos[0].path[len(infos[0].path) // 2]
    stack.push(
        network,
        rm.edit.set_maneuver_path(
            network, junction, infos[0].road, [(mid[0] + 1.5, mid[1] + 1.5)], 0.6, -0.4
        ),
    )
    stack.push(
        network, rm.edit.set_maneuver_turn_type(network, junction, infos[1].road, rm.TurnType.UTURN)
    )

    path = tmp_path / "maneuvers.xodr"
    rm.save_xodr(network, path, "maneuvers")
    assert 'code="rm:maneuver"' in path.read_text()

    reloaded_network, diagnostics = rm.load_xodr(path)
    assert not [d for d in diagnostics if d.severity == rm.Severity.ERROR]
    reloaded = rm.junction_maneuvers(reloaded_network, reloaded_network.junction_ids[0])
    assert [
        (m.locked, m.effective, m.overridden, m.start_offset, m.end_offset,
         [(p.x, p.y) for p in m.control_points])
        for m in reloaded
    ] == [
        (m.locked, m.effective, m.overridden, m.start_offset, m.end_offset,
         [(p.x, p.y) for p in m.control_points])
        for m in rm.junction_maneuvers(network, junction)
    ]
    # save -> reload -> save is byte-identical.
    assert rm.write_xodr(reloaded_network, "maneuvers") == rm.write_xodr(network, "maneuvers")


@pytest.mark.parametrize("command", ["locked", "turn_type", "path", "reset", "rebuild"])
def test_undo_is_byte_identical(cross, command):
    network, junction = cross
    stack = rm.edit.EditStack()
    info = rm.junction_maneuvers(network, junction)[0]
    mid = info.path[len(info.path) // 2]

    if command == "rebuild":
        # rebuild needs something to rebuild, so lock first and take THAT as
        # the baseline.
        stack.push(network, rm.edit.set_maneuver_locked(network, junction, info.road, True))
    before = rm.write_xodr(network, "maneuvers")

    if command == "locked":
        push = rm.edit.set_maneuver_locked(network, junction, info.road, True)
    elif command == "turn_type":
        push = rm.edit.set_maneuver_turn_type(network, junction, info.road, rm.TurnType.UTURN)
    elif command == "path":
        push = rm.edit.set_maneuver_path(
            network, junction, info.road, [(mid[0] + 1.5, mid[1] + 1.5)], 0.5
        )
    elif command == "reset":
        stack.push(network, rm.edit.set_maneuver_locked(network, junction, info.road, True))
        before = rm.write_xodr(network, "maneuvers")
        push = rm.edit.reset_maneuver(network, junction, info.road)
    else:
        push = rm.edit.rebuild_maneuvers(network, junction)

    stack.push(network, push)
    assert rm.write_xodr(network, "maneuvers") != before
    stack.undo(network)
    assert rm.write_xodr(network, "maneuvers") == before


def test_no_ops_and_bad_targets_raise(cross):
    network, junction = cross
    stack = rm.edit.EditStack()
    info = rm.junction_maneuvers(network, junction)[0]

    # A no-op is judged against the EFFECTIVE value, with no record to compare.
    with pytest.raises(ValueError):
        stack.push(network, rm.edit.set_maneuver_locked(network, junction, info.road, False))
    with pytest.raises(ValueError):
        stack.push(
            network, rm.edit.set_maneuver_turn_type(network, junction, info.road, info.computed)
        )
    # Clearing an override that does not exist.
    with pytest.raises(ValueError):
        stack.push(network, rm.edit.set_maneuver_turn_type(network, junction, info.road, None))
    # An empty path with no offsets is NOT a no-op on a derived maneuver — it
    # still flips the implicit lock. Repeating it is.
    stack.push(network, rm.edit.set_maneuver_path(network, junction, info.road, []))
    assert maneuver(network, junction, info.road).locked
    with pytest.raises(ValueError):
        stack.push(network, rm.edit.set_maneuver_path(network, junction, info.road, []))
    stack.undo(network)
    assert not network.junction(junction).maneuvers

    # An ARM road is not a connecting road, so it is not a maneuver.
    arm = network.junction(junction).arms[0].road
    with pytest.raises(ValueError):
        stack.push(network, rm.edit.set_maneuver_locked(network, junction, arm, True))

    # Offsets outside the anchor lane's span, and a non-finite coordinate.
    with pytest.raises(ValueError):
        stack.push(
            network,
            rm.edit.set_maneuver_path(
                network, junction, info.road, [], info.start_slide.max_offset + 5.0
            ),
        )
    with pytest.raises(ValueError):
        stack.push(
            network, rm.edit.set_maneuver_path(network, junction, info.road, [(math.nan, 0.0)])
        )
    # More than kMaxManeuverControlPoints.
    with pytest.raises(ValueError):
        stack.push(
            network,
            rm.edit.set_maneuver_path(
                network, junction, info.road, [(float(i), 0.0) for i in range(65)]
            ),
        )
    assert not network.junction(junction).maneuvers


def test_a_span_junction_has_no_maneuvers():
    network = rm.RoadNetwork()
    road = rm.author_clothoid_road(
        network, [(0.0, 0.0), (120.0, 0.0)], rm.LaneProfile.two_lane_default(), "", "1"
    )
    stack = rm.edit.EditStack()
    stack.push(network, rm.edit.create_span_junction(network, [rm.SpanArm(road, 50.0, 56.5)]))
    span_junction = network.junction_ids[0]

    # §12.7: nothing is cut, so there are no connections to turn through.
    assert rm.junction_maneuvers(network, span_junction) == []
    with pytest.raises(ValueError):
        stack.push(network, rm.edit.rebuild_maneuvers(network, span_junction))
    with pytest.raises(ValueError):
        stack.push(
            network,
            rm.edit.add_uturn_maneuver(
                network, span_junction, rm.RoadEnd(road, rm.ContactPoint.END)
            ),
        )
