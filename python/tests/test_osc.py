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

"""The rm.osc bindings (p8-s1, issue #245).

The kernel contract is pinned in core/tests/test_xosc_*.cpp; these cover what
only the binding layer can get wrong — that the types are reachable, that a
refusal surfaces as an exception rather than a wrong file, and that the
rule UIDs cross the boundary.
"""

from __future__ import annotations

import pytest

import roadmaker as rm


@pytest.fixture
def scenario() -> rm.osc.Scenario:
    scenario = rm.osc.Scenario()

    logic = rm.osc.FileRef()
    logic.filepath = "town.xodr"
    road_network = scenario.road_network
    road_network.logic_file = logic

    controller = rm.osc.TrafficSignalController()
    controller.name = "1"
    phase = rm.osc.Phase()
    phase.name = "go"
    phase.duration = 20.0
    phase.semantics = rm.osc.PhaseSemantics.Go
    state = rm.osc.TrafficSignalState()
    state.traffic_signal_id = "17251"
    state.state = "green"
    phase.signal_states = [state]
    controller.phases = [phase]
    road_network.traffic_signal_controllers = [controller]
    scenario.road_network = road_network

    car = rm.osc.Vehicle()
    car.name = "car"
    ego = rm.osc.ScenarioObject()
    ego.name = "Ego"
    ego.entity_object = car
    entities = scenario.entities
    entities.scenario_objects = [ego]
    scenario.entities = entities

    return scenario


def test_write_xosc_is_deterministic(scenario: rm.osc.Scenario) -> None:
    assert rm.osc.write_xosc(scenario) == rm.osc.write_xosc(scenario)


def test_default_target_declares_1_2_and_omits_semantics(scenario: rm.osc.Scenario) -> None:
    text = rm.osc.write_xosc(scenario)
    assert 'revMinor="2"' in text
    assert 'revMinor="4"' not in text
    assert "semantics=" not in text


def test_targeting_1_4_emits_semantics(scenario: rm.osc.Scenario) -> None:
    text = rm.osc.write_xosc(scenario, rm.osc.OscVersion.V1_4)
    assert 'revMinor="4"' in text
    assert 'semantics="go"' in text


def test_header_date_is_fixed_never_a_clock(scenario: rm.osc.Scenario) -> None:
    assert 'date="1970-01-01T00:00:00"' in rm.osc.write_xosc(scenario)


def test_controller_is_named_by_its_opendrive_id(scenario: rm.osc.Scenario) -> None:
    text = rm.osc.write_xosc(scenario)
    assert '<TrafficSignalController name="1"' in text
    assert 'trafficSignalId="17251"' in text


def test_a_composite_state_string_survives(scenario: rm.osc.Scenario) -> None:
    # @state is free text the engine interprets; a whole-box signal carries a
    # composite value that no colour enum could round-trip.
    road_network = scenario.road_network
    controller = road_network.traffic_signal_controllers[0]
    phase = controller.phases[0]
    states = phase.signal_states
    states[0].state = "on;off;off"
    phase.signal_states = states
    controller.phases = [phase]
    road_network.traffic_signal_controllers = [controller]
    scenario.road_network = road_network

    assert 'state="on;off;off"' in rm.osc.write_xosc(scenario)


def test_a_valid_scenario_has_no_findings(scenario: rm.osc.Scenario) -> None:
    # The counterpart the refusal tests need: without it, a validator that
    # rejected everything would pass them all.
    assert rm.osc.validate_scenario(scenario) == []


def test_duplicate_entity_names_are_refused_and_cite_the_rule(
    scenario: rm.osc.Scenario,
) -> None:
    entities = scenario.entities
    entities.scenario_objects = list(entities.scenario_objects) * 2
    scenario.entities = entities

    findings = rm.osc.validate_scenario(scenario)
    assert any(f.rule_id == rm.osc.RULE_UNIQUE_ELEMENT_NAMES for f in findings)
    with pytest.raises(ValueError, match="duplicate"):
        rm.osc.write_xosc(scenario)


def test_an_empty_traffic_signal_id_is_refused(scenario: rm.osc.Scenario) -> None:
    road_network = scenario.road_network
    controller = road_network.traffic_signal_controllers[0]
    phase = controller.phases[0]
    states = phase.signal_states
    states[0].traffic_signal_id = ""
    phase.signal_states = states
    controller.phases = [phase]
    road_network.traffic_signal_controllers = [controller]
    scenario.road_network = road_network

    findings = rm.osc.validate_scenario(scenario)
    assert any(f.rule_id == rm.osc.RULE_TRAFFIC_SIGNAL_STATE_REFERENCES for f in findings)
    with pytest.raises(ValueError, match="names no signal"):
        rm.osc.write_xosc(scenario)


def test_a_zero_phase_duration_is_legal(scenario: rm.osc.Scenario) -> None:
    # The rule is named phase_duration_positive but its text says NON-NEGATIVE.
    road_network = scenario.road_network
    controller = road_network.traffic_signal_controllers[0]
    phase = controller.phases[0]
    phase.duration = 0.0
    controller.phases = [phase]
    road_network.traffic_signal_controllers = [controller]
    scenario.road_network = road_network

    assert 'duration="0"' in rm.osc.write_xosc(scenario)


def test_a_negative_phase_duration_is_refused(scenario: rm.osc.Scenario) -> None:
    road_network = scenario.road_network
    controller = road_network.traffic_signal_controllers[0]
    phase = controller.phases[0]
    phase.duration = -1.0
    controller.phases = [phase]
    road_network.traffic_signal_controllers = [controller]
    scenario.road_network = road_network

    with pytest.raises(ValueError, match="negative"):
        rm.osc.write_xosc(scenario)


def test_save_xosc_writes_what_write_returns(
    scenario: rm.osc.Scenario, tmp_path
) -> None:
    path = tmp_path / "town.xosc"
    rm.osc.save_xosc(scenario, path)
    assert path.read_text() == rm.osc.write_xosc(scenario)


def test_save_xosc_writes_nothing_for_a_refused_scenario(
    scenario: rm.osc.Scenario, tmp_path
) -> None:
    entities = scenario.entities
    entities.scenario_objects = list(entities.scenario_objects) * 2
    scenario.entities = entities

    path = tmp_path / "town.xosc"
    with pytest.raises(ValueError):
        rm.osc.save_xosc(scenario, path)
    assert not path.exists()


def test_every_exported_rule_uid_is_an_openscenario_one() -> None:
    uids = [
        rm.osc.RULE_UNIQUE_ELEMENT_NAMES,
        rm.osc.RULE_NO_DOUBLE_COLON_PREFIX,
        rm.osc.RULE_PHASE_DURATION_NON_NEGATIVE,
        rm.osc.RULE_TRAFFIC_SIGNAL_STATE_REFERENCES,
        rm.osc.RULE_TRAFFIC_SIGNAL_CONTROLLER_REFERENCES,
        rm.osc.RULE_CONDITION_DELAY_NON_NEGATIVE,
        rm.osc.RULE_ROAD_NETWORK_REFERENCE,
        rm.osc.RULE_ROAD_NETWORK_AVAILABILITY,
        rm.osc.RULE_FILE_ENDING,
        rm.osc.RULE_VALID_SCHEMA,
    ]
    for uid in uids:
        emanating, standard, version, rule = uid.split(":")
        assert emanating == "asam.net"
        assert standard == "xosc", f"{uid} is not an OpenSCENARIO rule id"
        # Every citable id first appeared at 1.2.0 or earlier, which is why the
        # writer's conservative 1.2 default forfeits no rule.
        assert version in {"1.0.0", "1.1.0", "1.2.0"}
        assert len(rule.split(".")) == 2


# --- the reader (p8-s1 PR-C) -------------------------------------------------

FOREIGN = """<?xml version="1.0" encoding="UTF-8"?>
<OpenSCENARIO>
  <FileHeader revMajor="1" revMinor="2" date="2026-01-01T00:00:00"
              description="d" author="a" vendorTag="7"/>
  <CatalogLocations/>
  <RoadNetwork><LogicFile filepath="town.xodr"/></RoadNetwork>
  <Entities><ScenarioObject name="Cone"><MiscObject name="c" mass="5"
    miscObjectCategory="obstacle"/></ScenarioObject></Entities>
  <Storyboard><Init><Actions/></Init><Story name="s"><Act name="a"/></Story>
    <StopTrigger/></Storyboard>
</OpenSCENARIO>
"""


def test_parse_records_the_revision_without_putting_it_on_the_model() -> None:
    result = rm.osc.parse_xosc(FOREIGN)
    assert (result.rev_major, result.rev_minor) == (1, 2)
    # The writer re-derives revMajor/revMinor, so a preserved copy would emit
    # each attribute twice — and a duplicate attribute is not well-formed XML.
    assert [name for name, _ in result.scenario.header.preserved.attributes] == ["vendorTag"]


def test_an_unmodeled_entity_object_reads_as_none_and_survives() -> None:
    scenario = rm.osc.parse_xosc(FOREIGN).scenario
    cone = scenario.entities.scenario_objects[0]
    assert cone.entity_object is None
    assert "<MiscObject" in rm.osc.write_xosc(scenario)


def test_the_round_trip_from_the_written_form_is_byte_identical() -> None:
    first = rm.osc.write_xosc(rm.osc.parse_xosc(FOREIGN).scenario)
    assert rm.osc.write_xosc(rm.osc.parse_xosc(first).scenario) == first


def test_a_structural_problem_raises_but_a_bad_number_does_not() -> None:
    with pytest.raises(ValueError):
        rm.osc.parse_xosc("<OpenSCENARIO><Entities>")
    with pytest.raises(ValueError):
        rm.osc.parse_xosc("<OpenDRIVE/>")

    # The bad number has to be on a MODELED attribute. Corrupting the
    # MiscObject's @mass proves nothing: the whole element rides the preserved
    # tier unparsed, so no code ever looks at it — a test that would have
    # passed for a reason unrelated to what it claims to cover.
    lenient = rm.osc.parse_xosc(
        '<OpenSCENARIO><RoadNetwork><LogicFile filepath="t.xodr"/><TrafficSignals>'
        '<TrafficSignalController name="1"><Phase name="go" duration="soon"/>'
        "</TrafficSignalController></TrafficSignals></RoadNetwork></OpenSCENARIO>"
    )
    assert any("soon" in d.message for d in lenient.diagnostics)
    assert lenient.scenario.road_network.traffic_signal_controllers[0].phases[0].duration == 0.0


def test_load_cites_the_two_rules_only_a_path_can_support(tmp_path) -> None:
    path = tmp_path / "scene.xml"  # deliberately not .xosc, and no town.xodr
    path.write_text(FOREIGN, encoding="utf-8")
    cited = {d.rule_id for d in rm.osc.load_xosc(path).diagnostics}
    assert rm.osc.RULE_FILE_ENDING in cited
    assert rm.osc.RULE_ROAD_NETWORK_AVAILABILITY in cited

    good = tmp_path / "scene.xosc"
    good.write_text(FOREIGN, encoding="utf-8")
    (tmp_path / "town.xodr").write_text("<OpenDRIVE/>", encoding="utf-8")
    cleared = {d.rule_id for d in rm.osc.load_xosc(good).diagnostics}
    assert rm.osc.RULE_FILE_ENDING not in cleared
    assert rm.osc.RULE_ROAD_NETWORK_AVAILABILITY not in cleared


def test_a_negative_condition_delay_is_refused_with_its_rule(
    scenario: rm.osc.Scenario,
) -> None:
    condition = rm.osc.Condition()
    condition.name = "end"
    condition.delay = -1.0
    group = rm.osc.ConditionGroup()
    group.conditions = [condition]
    storyboard = scenario.storyboard
    stop = storyboard.stop_trigger
    stop.condition_groups = [group]
    storyboard.stop_trigger = stop
    scenario.storyboard = storyboard

    cited = {d.rule_id for d in rm.osc.validate_scenario(scenario)}
    assert rm.osc.RULE_CONDITION_DELAY_NON_NEGATIVE in cited
    with pytest.raises(ValueError):
        rm.osc.write_xosc(scenario)


# --- the decomposition and the command layer (PR-D) -------------------------


@pytest.fixture
def signalized() -> tuple[rm.RoadNetwork, rm.JunctionId]:
    """A four-arm crossing with a two-phase signal group — two controllers."""
    network = rm.RoadNetwork()
    stack = rm.edit.EditStack()
    params = rm.edit.assembly.IntersectionParams()
    params.gap_m = 24.0
    params.arm_length_m = 80.0
    stack.push(
        network,
        rm.edit.assembly.x_intersection(network, rm.edit.assembly.Pose(0.0, 0.0, 0.0), params),
    )
    junction = network.junction_ids[0]
    options = rm.edit.SignalizeOptions()
    options.tmpl = rm.edit.SignalizeTemplate.TWO_PHASE
    stack.push(network, rm.edit.signalize_junction(network, junction, options))
    return network, junction


def test_state_token_is_total_and_distinct() -> None:
    tokens = {
        rm.osc.state_token(state)
        for state in (
            rm.SignalState.RED,
            rm.SignalState.YELLOW,
            rm.SignalState.GREEN,
            rm.SignalState.OFF,
        )
    }
    assert len(tokens) == 4
    assert "" not in tokens


def test_decompose_yields_one_controller_per_opendrive_controller(
    signalized: tuple[rm.RoadNetwork, rm.JunctionId],
) -> None:
    network, junction = signalized
    out = rm.osc.decompose_junction_signals(network, junction)

    live = {network.controller(cid).odr_id for cid in network.controller_ids}
    assert {c.name for c in out.controllers} == live
    # Sorted by @id at the point of construction: arena order is not creation
    # order, so an unsorted decomposition would write a different file after any
    # erase.
    assert [c.name for c in out.controllers] == sorted(c.name for c in out.controllers)


def test_every_signal_has_a_state_in_every_phase(
    signalized: tuple[rm.RoadNetwork, rm.JunctionId],
) -> None:
    # The Red-by-omission guard, per phase. A whole-controller count passes on
    # the defect because the omitted reds show up in the phases that are green.
    network, junction = signalized
    out = rm.osc.decompose_junction_signals(network, junction)
    heads = {
        network.controller(cid).odr_id: {c.signal_odr_id for c in network.controller(cid).controls}
        for cid in network.controller_ids
    }
    assert out.controllers
    for controller in out.controllers:
        assert controller.phases
        for phase in controller.phases:
            assert {s.traffic_signal_id for s in phase.signal_states} == heads[controller.name]


def test_traffic_signal_ids_are_odr_ids_never_handles(
    signalized: tuple[rm.RoadNetwork, rm.JunctionId],
) -> None:
    network, junction = signalized
    live = {network.signal(sid).odr_id for sid in network.signal_ids}
    out = rm.osc.decompose_junction_signals(network, junction)
    for controller in out.controllers:
        for phase in controller.phases:
            for state in phase.signal_states:
                assert state.traffic_signal_id in live


def test_decomposing_a_junction_with_no_cycle_reports_rather_than_returning_silence() -> None:
    network = rm.RoadNetwork()
    out = rm.osc.decompose_junction_signals(network, rm.JunctionId())
    assert not out.controllers
    assert out.findings


def test_scenario_stack_undo_redo_is_byte_identical(
    signalized: tuple[rm.RoadNetwork, rm.JunctionId],
) -> None:
    network, junction = signalized
    scenario = rm.osc.Scenario()
    stack = rm.osc.edit.ScenarioStack()

    stack.push(scenario, rm.osc.edit.set_logic_file(scenario, "crossing.xodr"))
    stack.push(scenario, rm.osc.edit.sync_traffic_signals(scenario, network, junction))
    car = rm.osc.Vehicle()
    car.name = "car"
    ego = rm.osc.ScenarioObject()
    ego.name = "Ego"
    ego.entity_object = car
    stack.push(scenario, rm.osc.edit.add_scenario_object(scenario, ego))
    position = rm.osc.WorldPosition()
    position.x = -40.0
    stack.push(scenario, rm.osc.edit.set_entity_init_position(scenario, "Ego", position))

    full = rm.osc.write_xosc(scenario)
    empty = rm.osc.write_xosc(rm.osc.Scenario())
    assert full != empty

    for _ in range(10):
        while stack.can_undo:
            stack.undo(scenario)
        assert rm.osc.write_xosc(scenario) == empty
        while stack.can_redo:
            stack.redo(scenario)
        assert rm.osc.write_xosc(scenario) == full


def test_a_refused_factory_returns_a_command_not_none(
    scenario: rm.osc.Scenario,
) -> None:
    # Never None: the caller pushes it and reads the refusal as an exception,
    # exactly as with a command that failed for any other reason.
    duplicate = rm.osc.edit.add_scenario_object(scenario, scenario.entities.scenario_objects[0])
    assert duplicate is not None
    assert duplicate.name

    stack = rm.osc.edit.ScenarioStack()
    before = rm.osc.write_xosc(scenario)
    with pytest.raises(ValueError):
        stack.push(scenario, duplicate)
    assert stack.size == 0
    assert rm.osc.write_xosc(scenario) == before


def test_syncing_a_second_junction_keeps_the_first_controllers(
    signalized: tuple[rm.RoadNetwork, rm.JunctionId],
) -> None:
    network, junction = signalized
    scenario = rm.osc.Scenario()
    foreign = rm.osc.TrafficSignalController()
    foreign.name = "zzz_elsewhere"
    road_network = scenario.road_network
    road_network.traffic_signal_controllers = [foreign]
    scenario.road_network = road_network

    stack = rm.osc.edit.ScenarioStack()
    stack.push(scenario, rm.osc.edit.sync_traffic_signals(scenario, network, junction))
    names = {c.name for c in scenario.road_network.traffic_signal_controllers}
    assert "zzz_elsewhere" in names
    assert len(names) > 1


def test_sync_refuses_a_junction_with_no_cycle() -> None:
    network = rm.RoadNetwork()
    scenario = rm.osc.Scenario()
    stack = rm.osc.edit.ScenarioStack()
    with pytest.raises(ValueError):
        stack.push(scenario, rm.osc.edit.sync_traffic_signals(scenario, network, rm.JunctionId()))


def test_removing_an_actor_takes_its_init_private_with_it(
    signalized: tuple[rm.RoadNetwork, rm.JunctionId],
) -> None:
    # Left behind, the <Private> is a dangling entityRef and write_xosc refuses
    # the whole document — a removal that reported success would make the
    # scenario unsavable.
    scenario = rm.osc.Scenario()
    stack = rm.osc.edit.ScenarioStack()
    car = rm.osc.Vehicle()
    car.name = "car"
    ego = rm.osc.ScenarioObject()
    ego.name = "Ego"
    ego.entity_object = car
    stack.push(scenario, rm.osc.edit.add_scenario_object(scenario, ego))
    stack.push(scenario, rm.osc.edit.set_entity_init_position(scenario, "Ego", rm.osc.WorldPosition()))
    assert scenario.storyboard.init.actions.privates

    stack.push(scenario, rm.osc.edit.remove_scenario_object(scenario, "Ego"))
    assert not scenario.storyboard.init.actions.privates
    rm.osc.write_xosc(scenario)  # must not raise

    stack.undo(scenario)
    assert len(scenario.storyboard.init.actions.privates) == 1


# --- p8-s2 (#246): actors on lanes -------------------------------------------


def networked_scenario() -> tuple[rm.osc.Scenario, rm.osc.edit.ScenarioStack]:
    """A scenario with a <LogicFile>, which every lane position requires.

    Without it, ``asam.net:xosc:1.0.0:scenario_logic.invalid_elements_if_no_road_network``
    fires: a <LanePosition> naming a road network the document never links is a
    roadId nothing can resolve, and write_xosc refuses the whole file.
    """
    scenario = rm.osc.Scenario()
    stack = rm.osc.edit.ScenarioStack()
    stack.push(scenario, rm.osc.edit.set_logic_file(scenario, "town.xodr"))
    return scenario, stack


def lane(road: str = "1", lane_id: str = "-1", s: float = 10.0) -> rm.osc.LanePosition:
    position = rm.osc.LanePosition()
    position.road_id = road
    position.lane_id = lane_id
    position.s = s
    return position


def test_the_catalog_covers_every_kind_and_the_car_is_the_reference_vehicle() -> None:
    catalog = rm.osc.actor_catalog()
    assert {a.key for a in catalog} == {
        "car",
        "truck",
        "bus",
        "motorbike",
        "bicycle",
        "pedestrian",
    }
    car = rm.osc.actor_archetype(rm.osc.ActorKind.Car)
    # docs/domain/realism_defaults.md §1.1, the AASHTO P design vehicle every
    # other default in that document is measured against.
    assert (car.width, car.length, car.height) == pytest.approx((2.13, 5.79, 1.45))


def test_make_actor_is_writable_without_further_assembly() -> None:
    # <Performance> and <Axles> are required children of <Vehicle> in every
    # revision. The point of make_actor is that a caller never has to know that.
    for archetype in rm.osc.actor_catalog():
        scenario = rm.osc.Scenario()
        scenario.entities.scenario_objects = [
            rm.osc.make_actor(archetype.kind, f"{archetype.key}1")
        ]
        rm.osc.write_xosc(scenario)  # must not raise


def test_placing_an_actor_on_a_lane_is_one_undo_entry() -> None:
    scenario, stack = networked_scenario()
    before = rm.osc.write_xosc(scenario)

    stack.push(
        scenario,
        rm.osc.edit.place_scenario_object(
            scenario, rm.osc.make_actor(rm.osc.ActorKind.Car, "Car1"), lane(s=42.5)
        ),
    )
    assert len(scenario.entities.scenario_objects) == 1
    assert len(scenario.storyboard.init.actions.privates) == 1
    assert '<LanePosition roadId="1" laneId="-1" s="42.5"' in rm.osc.write_xosc(scenario)

    # ONE undo, not two — placing an actor is one gesture.
    stack.undo(scenario)
    assert rm.osc.write_xosc(scenario) == before


def test_a_lane_position_round_trips_through_a_file(tmp_path) -> None:
    scenario, stack = networked_scenario()
    stack.push(
        scenario,
        rm.osc.edit.place_scenario_object(
            scenario, rm.osc.make_actor(rm.osc.ActorKind.Car, "Car1"), lane(s=42.5)
        ),
    )
    path = tmp_path / "actors.xosc"
    rm.osc.save_xosc(scenario, path)

    parsed = rm.osc.parse_xosc(path.read_text())
    position = parsed.scenario.storyboard.init.actions.privates[0].actions[0].teleport.position
    assert isinstance(position, rm.osc.LanePosition)
    # The ids are STRINGS all the way through — never parsed to an int and
    # re-rendered, which is how a leading zero or a temporary-layer id would
    # quietly change on a round trip.
    assert position.road_id == "1"
    assert position.lane_id == "-1"
    assert position.s == pytest.approx(42.5)
    assert rm.osc.write_xosc(parsed.scenario) == rm.osc.write_xosc(scenario)


def test_an_initial_speed_is_a_second_private_action() -> None:
    # <PrivateAction> is a per-element CHOICE, so a file holds one action per
    # arm. Sharing one element would produce a document no parser accepts.
    scenario, stack = networked_scenario()
    stack.push(
        scenario,
        rm.osc.edit.place_scenario_object(
            scenario, rm.osc.make_actor(rm.osc.ActorKind.Car, "Car1"), lane()
        ),
    )
    stack.push(scenario, rm.osc.edit.set_entity_init_speed(scenario, "Car1", 13.89))

    actions = scenario.storyboard.init.actions.privates[0].actions
    assert len(actions) == 2
    assert actions[0].teleport is not None
    assert actions[0].longitudinal is None
    assert actions[1].longitudinal is not None
    assert '<AbsoluteTargetSpeed value="13.89" />' in rm.osc.write_xosc(scenario)


def test_renaming_an_actor_rewrites_its_entity_ref() -> None:
    # Renaming the entity alone leaves a dangling entityRef, which write_xosc
    # refuses — so the rename would make the document unsavable.
    scenario, stack = networked_scenario()
    stack.push(
        scenario,
        rm.osc.edit.place_scenario_object(
            scenario, rm.osc.make_actor(rm.osc.ActorKind.Car, "Car1"), lane()
        ),
    )
    stack.push(scenario, rm.osc.edit.rename_scenario_object(scenario, "Car1", "Hero"))

    assert scenario.entities.scenario_objects[0].name == "Hero"
    assert scenario.storyboard.init.actions.privates[0].entity_ref == "Hero"
    rm.osc.write_xosc(scenario)  # must not raise


def test_a_lane_position_with_no_logic_file_is_refused() -> None:
    scenario = rm.osc.Scenario()  # deliberately WITHOUT set_logic_file
    stack = rm.osc.edit.ScenarioStack()
    stack.push(
        scenario,
        rm.osc.edit.place_scenario_object(
            scenario, rm.osc.make_actor(rm.osc.ActorKind.Car, "Car1"), lane()
        ),
    )
    with pytest.raises(ValueError):
        rm.osc.write_xosc(scenario)


@pytest.mark.parametrize(
    ("position", "reason"),
    [
        (lane(road=""), "no road"),
        (lane(lane_id=""), "no lane"),
        (lane(s=-1.0), "negative s"),
    ],
)
def test_an_unresolvable_lane_position_is_refused_at_placement(
    position: rm.osc.LanePosition, reason: str
) -> None:
    # validate_scenario would catch these too, but only at SAVE time — an hour
    # after the placement that caused them.
    scenario, stack = networked_scenario()
    scenario.entities.scenario_objects = [rm.osc.make_actor(rm.osc.ActorKind.Car, "Car1")]
    with pytest.raises(ValueError):
        stack.push(scenario, rm.osc.edit.set_entity_init_pose(scenario, "Car1", position))


def test_a_negative_speed_is_refused_and_not_clamped() -> None:
    scenario, stack = networked_scenario()
    scenario.entities.scenario_objects = [rm.osc.make_actor(rm.osc.ActorKind.Car, "Car1")]
    with pytest.raises(ValueError):
        stack.push(scenario, rm.osc.edit.set_entity_init_speed(scenario, "Car1", -5.0))
    assert not scenario.storyboard.init.actions.privates


def test_retyping_a_position_reports_the_preserved_tier_it_drops() -> None:
    # A <WorldPosition>'s foreign attributes name a DIFFERENT element and cannot
    # ride onto a <LanePosition>. Dropping them is correct; dropping them in
    # silence is what ADR-0014 §6 forbids.
    #
    # The preserved tier is built by PARSING rather than by assignment: RawXml
    # is deliberately read-only from Python (it is a preservation carrier, not
    # something a caller authors), and parsing is the only way one is ever
    # populated in practice anyway.
    document = """<?xml version="1.0" encoding="UTF-8"?>
<OpenSCENARIO>
  <FileHeader revMajor="1" revMinor="2" date="2026-01-01T00:00:00"
              description="d" author="a"/>
  <CatalogLocations/>
  <RoadNetwork><LogicFile filepath="town.xodr"/></RoadNetwork>
  <Entities><ScenarioObject name="Ego"><MiscObject name="c" mass="5"
    miscObjectCategory="obstacle"/></ScenarioObject></Entities>
  <Storyboard><Init><Actions><Private entityRef="Ego"><PrivateAction>
    <TeleportAction><Position>
      <WorldPosition x="1" y="2" z="0" vendorFlag="1"/>
    </Position></TeleportAction></PrivateAction></Private></Actions></Init>
    <StopTrigger/></Storyboard>
</OpenSCENARIO>
"""
    scenario = rm.osc.parse_xosc(document).scenario
    assert "vendorFlag" in rm.osc.write_xosc(scenario)

    stack = rm.osc.edit.ScenarioStack()
    stack.push(scenario, rm.osc.edit.set_entity_init_pose(scenario, "Ego", lane()))
    assert stack.last_findings, "a preserved tier was dropped and nothing said so"
    assert "WorldPosition" in stack.last_findings[0].message
    assert "vendorFlag" not in rm.osc.write_xosc(scenario)
