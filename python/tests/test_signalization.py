# Copyright 2026 Robomous
# SPDX-License-Identifier: Apache-2.0

"""Junction SIGNALIZATION through the Python bindings (p4-s7, #228).

Three layers meet here:

* Layer 0 — the `<controller>`/`<control>` elements OpenDRIVE 1.9.0 §14.6 defines
  and RoadMaker had no model for until this sprint. A controller is TOP-LEVEL
  (a child of `<OpenDRIVE>`, not of a road or a junction) and it names its
  signals by STRING `@signalId`, so a dangling reference from a foreign file
  survives a round trip and is *reported* rather than dropped.
* Layer 0 again — `<junction><controller>` (§12.14), a REFERENCE into a signal
  synchronization group.
* Layer 1 — the RoadMaker-only `rm:signal` / `rm:signalmount` userData records
  saying which template was applied and which props represent each head.

`junction_signals()` is the single derived read over all of it, and two commands
author it: `edit.signalize_junction` and `edit.clear_signalization`.
"""

import math

import pytest

import roadmaker as rm

NAME = "signalization"

DYNAMIC_TEMPLATES = (
    rm.edit.SignalizeTemplate.FOUR_WAY_PROTECTED_LEFT,
    rm.edit.SignalizeTemplate.TWO_PHASE,
)
STATIC_TEMPLATES = (
    rm.edit.SignalizeTemplate.ALL_WAY_STOP,
    rm.edit.SignalizeTemplate.TWO_WAY_STOP,
)
ALL_TEMPLATES = DYNAMIC_TEMPLATES + STATIC_TEMPLATES

TOKEN = {
    rm.edit.SignalizeTemplate.FOUR_WAY_PROTECTED_LEFT: "protected_left",
    rm.edit.SignalizeTemplate.TWO_PHASE: "two_phase",
    rm.edit.SignalizeTemplate.ALL_WAY_STOP: "all_way_stop",
    rm.edit.SignalizeTemplate.TWO_WAY_STOP: "two_way_stop",
}


def _junction(network, stack, builder):
    params = rm.edit.assembly.IntersectionParams()
    # Roomy on both counts: a tight junction clamps its stop-line anchors, and
    # arms longer than SIGNAL_APPROACH_WINDOW are what makes the window edge
    # testable at all.
    params.gap_m = 24.0
    params.arm_length_m = 80.0
    stack.push(network, builder(network, rm.edit.assembly.Pose(0.0, 0.0, 0.0), params))
    return network.junction_ids[0]


@pytest.fixture
def cross():
    """(network, stack, junction) for a roomy four-arm crossing."""
    network = rm.RoadNetwork()
    stack = rm.edit.EditStack()
    return network, stack, _junction(network, stack, rm.edit.assembly.x_intersection)


@pytest.fixture
def tee():
    """(network, stack, junction) for a three-arm T — one two-arm axis and one
    single-arm axis, which is what stops the templates hard-coding four arms."""
    network = rm.RoadNetwork()
    stack = rm.edit.EditStack()
    return network, stack, _junction(network, stack, rm.edit.assembly.t_intersection)


def options(tmpl, mount_model="", lateral_offset=0.5):
    value = rm.edit.SignalizeOptions()
    value.tmpl = tmpl
    value.mount_model = mount_model
    value.lateral_offset = lateral_offset
    return value


def signals(network):
    return [network.signal(signal_id) for signal_id in network.signal_ids]


def controllers(network):
    return [network.controller(controller_id) for controller_id in network.controller_ids]


# --- the query ---------------------------------------------------------------


def test_query_derives_one_approach_per_arm_with_its_gated_movements(cross):
    network, _, junction = cross
    approaches = rm.junction_signals(network, junction)
    assert len(approaches) == 4

    maneuvers = {info.road: info for info in rm.junction_maneuvers(network, junction)}
    gated_total = 0
    for approach in approaches:
        assert approach.arm.contact in (rm.ContactPoint.START, rm.ContactPoint.END)
        assert approach.gated
        gated_total += len(approach.gated)
        # Gating is DERIVED: every listed movement really leaves this arm.
        for turn in approach.gated:
            assert maneuvers[turn].from_ == approach.arm
        # The anchor is the stop line's, not a second derivation.
        stopline = next(
            line for line in rm.junction_stoplines(network, junction) if line.arm == approach.arm
        )
        assert approach.s_stop == pytest.approx(stopline.s_center)
        assert approach.t_center == pytest.approx(stopline.t_center)
        assert math.isfinite(approach.heading)
        # Nothing is signalized yet.
        assert approach.signal_ids == []
        assert approach.controller_odr_ids == []
        assert not approach.dynamic

    assert gated_total == len(maneuvers)
    assert network.junction(junction).signalization.tmpl == ""
    assert network.junction(junction).junction_controllers == []
    assert network.junction(junction).signal_mounts == []


def test_query_is_empty_for_stale_and_span_junctions(cross):
    network, stack, _ = cross
    assert rm.junction_signals(network, rm.JunctionId()) == []

    span_network = rm.RoadNetwork()
    road = rm.author_clothoid_road(
        span_network, [(0.0, 0.0), (120.0, 0.0)], rm.LaneProfile.two_lane_rural(), name="through"
    )
    stack.push(
        span_network,
        rm.edit.create_span_junction(span_network, [rm.SpanArm(road, 50.0, 56.5)]),
    )
    # A §12.7 span junction cuts no road, so it has no connections and no
    # approaches — and it "shall not have controllers and therefore no traffic
    # lights" either.
    span_junction = span_network.junction_ids[0]
    assert rm.junction_signals(span_network, span_junction) == []
    with pytest.raises(ValueError, match="junctions.virtual.no_controllers"):
        stack.push(span_network, rm.edit.signalize_junction(span_network, span_junction))


def test_query_resolves_signals_in_the_window_and_their_controller_groups(cross):
    network, _, junction = cross
    approach = rm.junction_signals(network, junction)[0]
    road = approach.arm.road
    length = network.road(road).plan_view.length
    # The mouth is whichever end meets the junction; traffic reaching it runs
    # toward that end, and a head facing that traffic carries the matching
    # @orientation (§14.1 e_orientation).
    at_end = approach.arm.contact == rm.ContactPoint.END
    mouth = length if at_end else 0.0
    inward = 1.0 if at_end else -1.0
    facing = rm.ObjectOrientation.PLUS if at_end else rm.ObjectOrientation.MINUS
    away = rm.ObjectOrientation.MINUS if at_end else rm.ObjectOrientation.PLUS

    def place(s, orientation, odr_id):
        signal = rm.Signal()
        signal.odr_id = odr_id
        signal.s, signal.t = s, -5.0
        signal.orientation = orientation
        signal.dynamic = True
        signal.type, signal.subtype, signal.country = "1000001", "-1", "OpenDRIVE"
        return network.add_signal(road, signal)

    assert rm.SIGNAL_APPROACH_WINDOW == pytest.approx(30.0)
    assert length > rm.SIGNAL_APPROACH_WINDOW  # the window edge is on the arm

    # Exactly at the window edge: inside (the bound is inclusive).
    edge = place(mouth - inward * rm.SIGNAL_APPROACH_WINDOW, facing, "10")
    place(mouth - inward * (rm.SIGNAL_APPROACH_WINDOW + 1.0), facing, "11")  # just beyond: out
    place(mouth - inward * 1.0, away, "12")  # facing away from the junction: out
    both = place(mouth - inward * 2.0, rm.ObjectOrientation.NONE, "13")  # either way: in

    resolved = rm.junction_signals(network, junction)[0]
    assert resolved.arm == approach.arm
    # In arena creation order.
    assert resolved.signal_ids == [edge, both]
    assert resolved.dynamic
    assert resolved.controller_odr_ids == []

    # A controller naming one of them puts the approach in its group; a dangling
    # <control> matches nothing and is not an error.
    controller = rm.Controller()
    controller.odr_id = "ctrl"
    controller.controls = [rm.Control("10"), rm.Control("does-not-exist", "advance")]
    assert network.add_controller(controller)
    assert network.controller_count == 1

    grouped = rm.junction_signals(network, junction)[0]
    assert grouped.controller_odr_ids == ["ctrl"]


# --- templates ---------------------------------------------------------------


@pytest.mark.parametrize("tmpl", ALL_TEMPLATES)
def test_every_template_signalizes_and_records_what_it_applied(cross, tmpl):
    network, stack, junction = cross
    stack.push(network, rm.edit.signalize_junction(network, junction, options(tmpl)))

    assert network.signal_count > 0
    junction_value = network.junction(junction)
    assert junction_value.signalization.tmpl == TOKEN[tmpl]
    assert junction_value.signalization.mount_model == ""
    assert junction_value.signal_mounts == []

    contacts = {
        approach.arm.road: approach.arm.contact
        for approach in rm.junction_signals(network, junction)
    }
    dynamic = tmpl in DYNAMIC_TEMPLATES
    for signal in signals(network):
        assert bool(signal.dynamic) is dynamic
        # A head faces the traffic it stops: an arm met at its End is travelled
        # toward +s, so the head is @orientation="+" and sits at negative t (the
        # driver's right); an arm met at its Start is the mirror image.
        if contacts[signal.road] == rm.ContactPoint.END:
            assert signal.orientation == rm.ObjectOrientation.PLUS
            assert signal.t < 0.0
        else:
            assert signal.orientation == rm.ObjectOrientation.MINUS
            assert signal.t > 0.0
        if dynamic:
            # §14.1: the OpenDRIVE-catalog traffic light.
            assert (signal.type, signal.subtype, signal.country) == ("1000001", "-1", "OpenDRIVE")

    # Every sync-group reference names a live top-level controller, and every
    # controller drives at least one signal
    # (asam.net:xodr:1.7.0:road.signal.controller.valid_for_signals).
    live = {controller.odr_id for controller in controllers(network)}
    for reference in junction_value.junction_controllers:
        assert reference.controller_odr_id in live
    for controller in controllers(network):
        assert controller.controls
    assert not rm.validate_network(network)


def test_four_way_protected_left_groups_through_and_left_per_axis(cross):
    network, stack, junction = cross
    stack.push(
        network,
        rm.edit.signalize_junction(
            network, junction, options(rm.edit.SignalizeTemplate.FOUR_WAY_PROTECTED_LEFT)
        ),
    )
    # Every approach of a plain cross has a left turn, so each gets a through
    # head AND a protected-left head; two axes x (through + left) = 4 groups.
    assert network.signal_count == 8
    assert network.controller_count == 4
    assert len(network.junction(junction).junction_controllers) == 4


def test_two_phase_groups_one_controller_per_axis(cross):
    network, stack, junction = cross
    stack.push(
        network,
        rm.edit.signalize_junction(network, junction, options(rm.edit.SignalizeTemplate.TWO_PHASE)),
    )
    assert network.signal_count == 4  # permissive lefts: one head per approach
    assert network.controller_count == 2
    assert [len(controller.controls) for controller in controllers(network)] == [2, 2]

    for approach in rm.junction_signals(network, junction):
        assert len(approach.signal_ids) == 1
        assert approach.dynamic
        assert len(approach.controller_odr_ids) == 1


@pytest.mark.parametrize("tmpl", STATIC_TEMPLATES)
def test_static_templates_create_zero_controllers(cross, tmpl):
    """GW-4 step 3: a stop-controlled junction has no phases, so no phase data
    is created — not an empty controller, none at all."""
    network, stack, junction = cross
    stack.push(network, rm.edit.signalize_junction(network, junction, options(tmpl)))

    assert network.controller_count == 0
    assert network.controller_ids == []
    assert network.junction(junction).junction_controllers == []
    for approach in rm.junction_signals(network, junction):
        assert approach.controller_odr_ids == []
        assert not approach.dynamic


def test_all_way_stop_signs_every_approach_and_two_way_stop_signs_one_axis(cross):
    network, stack, junction = cross
    stack.push(
        network,
        rm.edit.signalize_junction(
            network, junction, options(rm.edit.SignalizeTemplate.ALL_WAY_STOP)
        ),
    )
    assert network.signal_count == 4

    stack.push(
        network,
        rm.edit.signalize_junction(
            network, junction, options(rm.edit.SignalizeTemplate.TWO_WAY_STOP)
        ),
    )
    # Replaces rather than accumulates, and signs only the minor axis.
    assert network.signal_count == 2
    signed = [signal.road for signal in signals(network)]
    assert len(set(signed)) == 2
    headings = {
        approach.arm.road: approach.heading for approach in rm.junction_signals(network, junction)
    }
    # The two signed arms are opposite: one axis.
    delta = abs(math.remainder(headings[signed[0]] - headings[signed[1]], 2.0 * math.pi))
    assert delta == pytest.approx(math.pi, abs=0.6)


# --- the three-arm T ---------------------------------------------------------


def test_three_arm_tee_gets_a_two_arm_axis_and_a_single_arm_axis(tee):
    network, stack, junction = tee
    assert len(rm.junction_signals(network, junction)) == 3

    stack.push(
        network,
        rm.edit.signalize_junction(network, junction, options(rm.edit.SignalizeTemplate.TWO_PHASE)),
    )
    assert network.signal_count == 3
    # One axis for the through road, one for the stem — two groups, not one and
    # not three.
    assert network.controller_count == 2
    assert sorted(len(controller.controls) for controller in controllers(network)) == [1, 2]


def test_three_arm_tee_takes_an_all_way_stop(tee):
    network, stack, junction = tee
    stack.push(
        network,
        rm.edit.signalize_junction(
            network, junction, options(rm.edit.SignalizeTemplate.ALL_WAY_STOP)
        ),
    )
    assert network.signal_count == 3
    assert network.controller_count == 0


# --- rejections --------------------------------------------------------------


def test_reapplying_the_same_template_is_refused(cross):
    network, stack, junction = cross
    stack.push(
        network,
        rm.edit.signalize_junction(network, junction, options(rm.edit.SignalizeTemplate.TWO_PHASE)),
    )
    before = rm.write_xodr(network, NAME)

    # The command layer's round-trip oracle forbids a command that changes
    # nothing, so the factory refuses instead of authoring a duplicate.
    with pytest.raises(ValueError):
        stack.push(
            network,
            rm.edit.signalize_junction(
                network, junction, options(rm.edit.SignalizeTemplate.TWO_PHASE)
            ),
        )
    # A failed apply leaves the network untouched.
    assert rm.write_xodr(network, NAME) == before


def test_stale_unknown_mount_and_unsignalized_clear_are_refused(cross):
    network, stack, junction = cross
    with pytest.raises(ValueError):
        stack.push(network, rm.edit.signalize_junction(network, rm.JunctionId()))
    with pytest.raises(ValueError):
        stack.push(
            network,
            rm.edit.signalize_junction(
                network, junction, options(rm.edit.SignalizeTemplate.TWO_PHASE, "not_a_prop")
            ),
        )
    # Nothing is signalized, so clearing would be a no-op.
    with pytest.raises(ValueError):
        stack.push(network, rm.edit.clear_signalization(network, junction))


# --- mounts, clearing, persistence -------------------------------------------


def test_mount_records_round_trip_and_are_cleared_with_the_signalization(cross):
    network, stack, junction = cross
    stack.push(
        network,
        rm.edit.signalize_junction(
            network,
            junction,
            options(rm.edit.SignalizeTemplate.TWO_PHASE, "streetlight_single"),
        ),
    )
    assert network.signal_count == 4
    assert network.object_count == 4

    mounts = network.junction(junction).signal_mounts
    assert len(mounts) == 4
    signal_ids = {signal.odr_id for signal in signals(network)}
    object_ids = {network.object(o).odr_id for o in network.object_ids}
    for mount in mounts:
        assert mount.signal_odr_id in signal_ids
        assert len(mount.object_odr_ids) == 1  # a list: #323 assembly parts slot in
        assert set(mount.object_odr_ids) <= object_ids

    # rm:signal + rm:signalmount survive a save -> reload -> save unchanged.
    text = rm.write_xodr(network, NAME)
    reloaded, diagnostics = rm.parse_xodr(text)
    assert not diagnostics
    assert rm.write_xodr(reloaded, NAME) == text
    assert "rm:signalmount" in text
    assert "<controller" in text

    reloaded_junction = reloaded.junction(reloaded.junction_ids[0])
    assert reloaded_junction.signalization.mount_model == "streetlight_single"
    assert [mount.signal_odr_id for mount in reloaded_junction.signal_mounts] == [
        mount.signal_odr_id for mount in mounts
    ]

    stack.push(network, rm.edit.clear_signalization(network, junction))
    assert network.signal_count == 0
    assert network.object_count == 0
    assert network.controller_count == 0
    cleared = network.junction(junction)
    assert cleared.signalization.tmpl == ""
    assert cleared.signal_mounts == []
    assert cleared.junction_controllers == []


def test_clear_leaves_a_hand_placed_sign_of_another_type_alone(cross):
    network, stack, junction = cross
    approach = rm.junction_signals(network, junction)[0]
    plate = rm.Signal()
    plate.odr_id = "speed"
    plate.s = approach.s_stop
    plate.t = -6.0
    plate.orientation = rm.ObjectOrientation.PLUS
    plate.dynamic = False
    plate.type, plate.subtype, plate.country = "274", "50", "DE"
    network.add_signal(approach.arm.road, plate)

    stack.push(
        network,
        rm.edit.signalize_junction(
            network, junction, options(rm.edit.SignalizeTemplate.ALL_WAY_STOP)
        ),
    )
    stack.push(network, rm.edit.clear_signalization(network, junction))
    assert [signal.odr_id for signal in signals(network)] == ["speed"]


@pytest.mark.parametrize("tmpl", ALL_TEMPLATES)
def test_signalize_and_clear_are_byte_identical_under_undo(cross, tmpl):
    network, stack, junction = cross
    pristine = rm.write_xodr(network, NAME)

    stack.push(
        network, rm.edit.signalize_junction(network, junction, options(tmpl, "streetlight_single"))
    )
    signalized = rm.write_xodr(network, NAME)
    assert signalized != pristine

    stack.push(network, rm.edit.clear_signalization(network, junction))
    assert rm.write_xodr(network, NAME) == pristine

    stack.undo(network)
    assert rm.write_xodr(network, NAME) == signalized
    stack.undo(network)
    # Undoing the signalization writes the original bytes: no orphaned
    # <controller>, no empty rm:signal element on a junction that has none.
    assert rm.write_xodr(network, NAME) == pristine
    assert network.controller_count == 0
    assert "rm:signal" not in pristine
    assert "<controller" not in pristine


@pytest.mark.parametrize("tmpl", ALL_TEMPLATES)
def test_save_reload_save_is_stable_and_silent(cross, tmpl, tmp_path):
    network, stack, junction = cross
    stack.push(network, rm.edit.signalize_junction(network, junction, options(tmpl)))

    path = tmp_path / f"{TOKEN[tmpl]}.xodr"
    rm.save_xodr(network, path)
    reloaded, diagnostics = rm.load_xodr(path)
    assert not diagnostics

    round_tripped = tmp_path / f"{TOKEN[tmpl]}-again.xodr"
    rm.save_xodr(reloaded, round_tripped)
    assert round_tripped.read_bytes() == path.read_bytes()

    # Layer 0 survives as elements, not as a RoadMaker-only record.
    assert reloaded.signal_count == network.signal_count
    assert reloaded.controller_count == network.controller_count
    reloaded_junction = reloaded.junction(reloaded.junction_ids[0])
    assert reloaded_junction.signalization.tmpl == TOKEN[tmpl]
    assert len(reloaded_junction.junction_controllers) == len(
        network.junction(junction).junction_controllers
    )
    assert not rm.validate_network(reloaded)


# --- validation --------------------------------------------------------------


def test_dangling_control_reference_is_a_finding_not_a_drop(cross):
    """Erasing a signal does NOT cascade into controllers — that keeps
    delete_signal a leaf op — so a dangling <control> is an expected state the
    author is told about and which survives the round trip verbatim."""
    network, stack, junction = cross
    stack.push(
        network,
        rm.edit.signalize_junction(network, junction, options(rm.edit.SignalizeTemplate.TWO_PHASE)),
    )
    assert not rm.validate_network(network)

    victim = network.signal_ids[0]
    driven = network.signal(victim).odr_id
    assert network.erase_signal(victim)

    findings = rm.validate_network(network)
    dangling = [d for d in findings if "does not exist" in d.message]
    assert len(dangling) == 1
    assert driven in dangling[0].message
    # ASAM has no rule id for a dangling @signalId, so none is invented.
    assert dangling[0].rule_id == ""
    assert dangling[0].severity == rm.Severity.WARNING

    # ...and the reference is written out, not silently dropped.
    text = rm.write_xodr(network, NAME)
    reloaded, _ = rm.parse_xodr(text)
    assert rm.write_xodr(reloaded, NAME) == text
    assert any(
        control.signal_odr_id == driven
        for controller in controllers(reloaded)
        for control in controller.controls
    )


def test_controller_with_no_controls_is_a_finding():
    """"Controllers shall be valid for one or more signals" (§14.6; <control>
    has multiplicity 1..*)."""
    network = rm.RoadNetwork()
    empty = rm.Controller()
    empty.odr_id = "lonely"
    empty.name = "no signals"
    empty.sequence = 3
    assert network.add_controller(empty)

    findings = rm.validate_network(network)
    assert [d.rule_id for d in findings] == [
        "asam.net:xodr:1.7.0:road.signal.controller.valid_for_signals"
    ]

    stored = network.controller(network.controller_ids[0])
    assert stored.odr_id == "lonely"
    assert stored.sequence == 3
    assert stored.controls == []
    assert network.erase_controller(network.controller_ids[0])
    assert network.controller_count == 0
    assert network.controller(rm.ControllerId()) is None


def test_top_level_controller_round_trips_from_a_hand_built_network():
    """A top-level <controller> used to be warned about and DROPPED on read.
    It is a child of <OpenDRIVE>: no road and no junction owns it."""
    network = rm.RoadNetwork()
    road = rm.author_clothoid_road(
        network, [(0.0, 0.0), (100.0, 0.0)], rm.LaneProfile.two_lane_rural(), name="r"
    )
    head = rm.Signal()
    head.odr_id = "7"
    head.s, head.t = 90.0, -5.0
    head.dynamic = True
    head.orientation = rm.ObjectOrientation.PLUS
    head.type, head.subtype, head.country = "1000001", "-1", "OpenDRIVE"
    network.add_signal(road, head)

    controller = rm.Controller()
    controller.odr_id = "c1"
    controller.name = "north group"
    controller.sequence = 0
    controller.controls = [rm.Control("7", "phase-a")]
    network.add_controller(controller)

    text = rm.write_xodr(network, NAME)
    assert 'signalId="7"' in text
    reloaded, diagnostics = rm.parse_xodr(text)
    assert not diagnostics
    assert rm.write_xodr(reloaded, NAME) == text

    assert reloaded.controller_count == 1
    stored = reloaded.controller(reloaded.controller_ids[0])
    assert stored.odr_id == "c1"
    assert stored.name == "north group"
    assert stored.sequence == 0
    assert [(c.signal_odr_id, c.type) for c in stored.controls] == [("7", "phase-a")]
    assert not rm.validate_network(reloaded)
