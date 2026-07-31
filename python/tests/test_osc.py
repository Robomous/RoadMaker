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
