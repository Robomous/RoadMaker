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
    ]
    for uid in uids:
        emanating, standard, version, rule = uid.split(":")
        assert emanating == "asam.net"
        assert standard == "xosc", f"{uid} is not an OpenSCENARIO rule id"
        # Every citable id first appeared at 1.2.0 or earlier, which is why the
        # writer's conservative 1.2 default forfeits no rule.
        assert version in {"1.0.0", "1.1.0", "1.2.0"}
        assert len(rule.split(".")) == 2
