"""The junction signal PHASE cycle through the Python bindings (p4-s8, #229).

OpenDRIVE §14.6 excludes signal timing on purpose, so the cycle is a RoadMaker
Layer-1 record (`<userData code="rm:phases">`, ADR-0008). It is DERIVED until the
first edit MATERIALIZES it, and `mesh.junction_phases()` reports the derived and
the authored cycle identically — only `plan.authored` tells them apart.
"""

import math

import pytest

import roadmaker as rm

NAME = "signal_phases"


def _junction(network, stack, builder):
    params = rm.edit.assembly.IntersectionParams()
    params.gap_m = 24.0
    params.arm_length_m = 80.0
    stack.push(network, builder(network, rm.edit.assembly.Pose(0.0, 0.0, 0.0), params))
    return network.junction_ids[0]


def _signalize(network, stack, junction, tmpl=None):
    options = rm.edit.SignalizeOptions()
    options.tmpl = tmpl or rm.edit.SignalizeTemplate.TWO_PHASE
    stack.push(network, rm.edit.signalize_junction(network, junction, options))


@pytest.fixture
def cross():
    """(network, stack, junction) for a roomy four-arm crossing, two-phase signalized."""
    network = rm.RoadNetwork()
    stack = rm.edit.EditStack()
    junction = _junction(network, stack, rm.edit.assembly.x_intersection)
    _signalize(network, stack, junction)
    return network, stack, junction


# --- the derived cycle -------------------------------------------------------


def test_two_phase_four_way_derives_four_phases_and_a_46s_cycle(cross):
    network, _, junction = cross
    plan = rm.junction_phases(network, junction)

    assert not plan.authored  # DERIVED — nothing stored yet
    assert network.junction(junction).phases == []  # empty ⇔ derived
    # green + yellow clearance per axis.
    assert len(plan.phases) == 4
    assert len(plan.controller_odr_ids) == 2  # one timeline row per axis
    assert plan.dormant_controller_odr_ids == []

    expected_cycle = rm.DEFAULT_PHASE_GREEN_SECONDS * 2 + rm.DEFAULT_PHASE_YELLOW_SECONDS * 2
    assert plan.cycle_duration == pytest.approx(expected_cycle)  # 46 s

    # start offsets accumulate the durations in order.
    start = 0.0
    for phase in plan.phases:
        assert phase.start == pytest.approx(start)
        assert phase.duration > 0.0
        # every member controller appears (Red-filled), matching the row count.
        assert len(phase.states) == len(plan.controller_odr_ids)
        assert phase.signal_states  # heads resolve on a dynamic junction
        start += phase.duration

    # A green phase moves traffic; a yellow clearance names its controller yellow.
    greens = [p for p in plan.phases if any(c.state == rm.SignalState.GREEN for c in p.states)]
    yellows = [p for p in plan.phases if any(c.state == rm.SignalState.YELLOW for c in p.states)]
    assert len(greens) == 2 and len(yellows) == 2
    assert all(phase.moving for phase in greens)


def test_phase_index_at_wraps_over_the_cycle(cross):
    network, _, junction = cross
    plan = rm.junction_phases(network, junction)
    cycle = plan.cycle_duration

    assert rm.phase_index_at(plan, 0.0) == 0
    assert rm.phase_index_at(plan, plan.phases[0].duration + 0.1) == 1
    # wraps: t == cycle is phase 0 again, and negatives fold forward.
    assert rm.phase_index_at(plan, cycle) == 0
    assert rm.phase_index_at(plan, cycle + 1.0) == rm.phase_index_at(plan, 1.0)
    assert rm.phase_index_at(plan, -1.0) == len(plan.phases) - 1


def test_empty_for_stale_static_and_span_junctions():
    network = rm.RoadNetwork()
    stack = rm.edit.EditStack()

    # stale id
    empty = rm.junction_phases(network, rm.JunctionId())
    assert not empty.authored and empty.phases == []
    assert rm.phase_index_at(empty, 0.0) == 2**64 - 1  # SIZE_MAX: nothing to index

    # a STATIC template makes no controllers, so no cycle.
    junction = _junction(network, stack, rm.edit.assembly.x_intersection)
    _signalize(network, stack, junction, rm.edit.SignalizeTemplate.ALL_WAY_STOP)
    static = rm.junction_phases(network, junction)
    assert not static.authored and static.phases == []


# --- the first edit materializes --------------------------------------------


def test_first_edit_materializes_the_derived_cycle_sparsely(cross):
    network, stack, junction = cross
    derived = rm.junction_phases(network, junction)

    stack.push(network, rm.edit.set_phase_duration(network, junction, 0, 30.0))

    stored = network.junction(junction).phases
    assert stored, "the first edit materialized the cycle onto the junction"
    assert len(stored) == len(derived.phases)
    # sparse: a green phase stores only its one non-Red controller pair.
    assert all(len(phase.states) <= len(derived.controller_odr_ids) for phase in stored)

    authored = rm.junction_phases(network, junction)
    assert authored.authored
    assert authored.phases[0].duration == pytest.approx(30.0)
    # the three untouched phases kept their derived durations verbatim.
    for i in range(1, len(authored.phases)):
        assert authored.phases[i].duration == pytest.approx(derived.phases[i].duration)
    assert authored.cycle_duration == pytest.approx(derived.cycle_duration + (30.0 - derived.phases[0].duration))


def test_add_duplicate_and_clear(cross):
    network, stack, junction = cross
    base = len(rm.junction_phases(network, junction).phases)

    stack.push(network, rm.edit.add_signal_phase(network, junction, base))  # append
    assert len(rm.junction_phases(network, junction).phases) == base + 1

    stack.push(network, rm.edit.duplicate_signal_phase(network, junction, 0))
    assert len(rm.junction_phases(network, junction).phases) == base + 2

    stack.push(network, rm.edit.clear_signal_phases(network, junction))
    back = rm.junction_phases(network, junction)
    assert not back.authored  # returned to the derived cycle
    assert network.junction(junction).phases == []
    assert len(back.phases) == base


# --- undo / redo -------------------------------------------------------------


def test_undo_redo_round_trips_authoring(cross):
    network, stack, junction = cross
    pristine = rm.write_xodr(network, NAME)
    assert "rm:phases" not in pristine  # derived writes no bytes

    stack.push(network, rm.edit.set_phase_duration(network, junction, 0, 30.0))
    authored = rm.write_xodr(network, NAME)
    assert "rm:phases" in authored

    stack.undo(network)
    assert rm.write_xodr(network, NAME) == pristine
    assert not rm.junction_phases(network, junction).authored

    stack.redo(network)
    assert rm.write_xodr(network, NAME) == authored
    assert rm.junction_phases(network, junction).phases[0].duration == pytest.approx(30.0)


# --- file round trip ---------------------------------------------------------


def test_authored_phases_survive_save_reload_save(cross, tmp_path):
    network, stack, junction = cross
    stack.push(network, rm.edit.set_phase_duration(network, junction, 0, 30.0))
    stack.push(
        network, rm.edit.set_phase_state(network, junction, 0, _green_controller(network, junction), rm.SignalState.YELLOW)
    )
    authored = rm.junction_phases(network, junction)

    path = tmp_path / "phases.xodr"
    rm.save_xodr(network, path)
    reloaded, diagnostics = rm.load_xodr(path)
    assert not diagnostics

    reloaded_junction = reloaded.junction(reloaded.junction_ids[0])
    assert reloaded_junction.phases, "authored phases came back"
    reloaded_plan = rm.junction_phases(reloaded, reloaded.junction_ids[0])
    assert reloaded_plan.authored
    assert [p.duration for p in reloaded_plan.phases] == pytest.approx(
        [p.duration for p in authored.phases]
    )

    again = tmp_path / "again.xodr"
    rm.save_xodr(reloaded, again)
    assert again.read_bytes() == path.read_bytes(), "byte-identical round trip"


# --- rejections --------------------------------------------------------------


def test_no_op_duration_raises(cross):
    network, stack, junction = cross
    plan = rm.junction_phases(network, junction)
    before = rm.write_xodr(network, NAME)

    # re-typing a phase's effective (derived) duration changes nothing.
    with pytest.raises(ValueError):
        stack.push(
            network,
            rm.edit.set_phase_duration(network, junction, 0, plan.phases[0].duration),
        )
    assert rm.write_xodr(network, NAME) == before  # a failed apply leaves it untouched


def test_out_of_band_duration_and_index_raise(cross):
    network, stack, junction = cross
    with pytest.raises(ValueError):
        stack.push(network, rm.edit.set_phase_duration(network, junction, 0, 0.0))
    with pytest.raises(ValueError):
        stack.push(
            network,
            rm.edit.set_phase_duration(network, junction, 0, rm.MAX_SIGNAL_PHASE_DURATION + 1.0),
        )
    with pytest.raises(ValueError):
        stack.push(network, rm.edit.set_phase_duration(network, junction, 99, 10.0))


def test_removing_the_last_phase_is_rejected(cross):
    network, stack, junction = cross
    count = len(rm.junction_phases(network, junction).phases)
    # remove down to one phase...
    for _ in range(count - 1):
        stack.push(network, rm.edit.remove_signal_phase(network, junction, 0))
    assert len(rm.junction_phases(network, junction).phases) == 1
    # ...and the last removal is refused (a zero-phase authored cycle is
    # unrepresentable — clear_signal_phases returns to the derived cycle instead).
    with pytest.raises(ValueError):
        stack.push(network, rm.edit.remove_signal_phase(network, junction, 0))


def test_clear_on_a_derived_cycle_is_rejected(cross):
    network, stack, junction = cross
    # nothing authored yet: there is nothing to clear.
    with pytest.raises(ValueError):
        stack.push(network, rm.edit.clear_signal_phases(network, junction))


# --- dormancy ----------------------------------------------------------------


def _green_controller(network, junction):
    """odr id of the controller shown Green in the first green phase."""
    for phase in rm.junction_phases(network, junction).phases:
        for cell in phase.states:
            if cell.state == rm.SignalState.GREEN:
                return cell.controller_odr_id
    raise AssertionError("no green controller found")


def test_deleting_a_controller_reports_it_dormant(cross):
    network, stack, junction = cross
    # materialize the cycle so its states name the controllers by string.
    stack.push(network, rm.edit.set_phase_duration(network, junction, 0, 30.0))
    doomed = _green_controller(network, junction)

    # erase the top-level <controller> — the authored string reference stays.
    victim = next(
        cid for cid in network.controller_ids if network.controller(cid).odr_id == doomed
    )
    assert network.erase_controller(victim)

    plan = rm.junction_phases(network, junction)
    assert doomed not in plan.controller_odr_ids  # no longer a live row
    assert doomed in plan.dormant_controller_odr_ids  # but reported, not dropped
    assert math.isfinite(plan.cycle_duration)
