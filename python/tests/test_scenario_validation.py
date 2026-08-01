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

"""`validate_scenario_against_network` through the bindings (issue #533).

The split under test is the one that matters and it is not obvious: the
document-only validator is BLIND to every reference whose other end is in the
`.xodr`, and esmini — the external gate — accepts those references in silence.
This function is the only thing that reads both documents.
"""

from __future__ import annotations

import pathlib
import subprocess
import sys

import pytest

import roadmaker as rm

SIGNAL_RULE = "asam.net:xosc:1.0.0:reference_control.traffic_signal_state_references"
BOUNDS_RULE = "asam.net:xosc:1.0.0:positioning.road_lane_offset_in_bounds"


@pytest.fixture
def scene() -> tuple[rm.RoadNetwork, rm.osc.Scenario]:
    """A signalized crossing and a scenario decomposed from it."""
    network = rm.RoadNetwork()
    net_stack = rm.edit.EditStack()
    params = rm.edit.assembly.IntersectionParams()
    params.gap_m = 24.0
    params.arm_length_m = 80.0
    net_stack.push(
        network,
        rm.edit.assembly.x_intersection(network, rm.edit.assembly.Pose(0.0, 0.0, 0.0), params),
    )
    junction = network.junction_ids[0]
    options = rm.edit.SignalizeOptions()
    options.tmpl = rm.edit.SignalizeTemplate.TWO_PHASE
    net_stack.push(network, rm.edit.signalize_junction(network, junction, options))

    scenario = rm.osc.Scenario()
    stack = rm.osc.edit.ScenarioStack()
    stack.push(scenario, rm.osc.edit.set_logic_file(scenario, "scene.xodr"))
    stack.push(scenario, rm.osc.edit.sync_traffic_signals(scenario, network, junction))

    position = rm.osc.LanePosition()
    position.road_id = network.road(network.road_ids[0]).odr_id
    position.lane_id = "-1"
    position.s = 20.0
    stack.push(
        scenario,
        rm.osc.edit.place_scenario_object(
            scenario, rm.osc.make_actor(rm.osc.ActorKind.Car, "Ego"), position
        ),
    )
    return network, scenario


def test_a_scenario_that_resolves_says_nothing(scene):
    network, scenario = scene
    assert not rm.osc.validate_scenario_against_network(scenario, network)


def test_a_dangling_signal_id_is_invisible_to_the_document_validator(scene):
    """★ The hole #533 was filed for, in one assertion pair."""
    network, scenario = scene
    road_network = scenario.road_network
    controllers = road_network.traffic_signal_controllers
    phases = controllers[0].phases
    states = phases[0].signal_states
    states[0].traffic_signal_id = "no-such-signal"
    phases[0].signal_states = states
    controllers[0].phases = phases
    road_network.traffic_signal_controllers = controllers
    scenario.road_network = road_network

    # The document-only validator sees a non-empty id and is satisfied — it has
    # no network to resolve it against, and says so in rules.hpp.
    assert not rm.osc.validate_scenario(scenario)

    findings = rm.osc.validate_scenario_against_network(scenario, network)
    assert any(f.rule_id == SIGNAL_RULE for f in findings)
    assert any("no-such-signal" in f.message for f in findings)


def test_the_upper_s_bound_needs_the_network(scene):
    network, scenario = scene
    storyboard = scenario.storyboard
    init = storyboard.init
    actions = init.actions
    privates = actions.privates
    entries = privates[0].actions
    teleport = entries[0].teleport
    position = teleport.position
    position.s = 9999.0
    teleport.position = position
    entries[0].teleport = teleport
    privates[0].actions = entries
    actions.privates = privates
    init.actions = actions
    storyboard.init = init
    scenario.storyboard = storyboard

    # A NEGATIVE s the document validator refuses on its own; the upper bound is
    # the road's length, which lives in the .xodr.
    assert not rm.osc.validate_scenario(scenario)
    findings = rm.osc.validate_scenario_against_network(scenario, network)
    assert any(f.rule_id == BOUNDS_RULE for f in findings)


def test_route_findings_arrive_through_the_one_call(scene):
    network, scenario = scene
    stack = rm.osc.edit.ScenarioStack()

    def waypoint(road_odr_id: str) -> rm.osc.RouteWaypoint:
        position = rm.osc.LanePosition()
        position.road_id = road_odr_id
        position.lane_id = "-1"
        position.s = 10.0
        point = rm.osc.RouteWaypoint()
        point.route_strategy = "shortest"
        point.position = position
        return point

    route = rm.osc.Route()
    route.name = "EgoRoute"
    route.waypoints = [
        waypoint(network.road(network.road_ids[0]).odr_id),
        waypoint("no-such-road"),
    ]
    stack.push(scenario, rm.osc.edit.assign_route(scenario, "Ego", route))

    findings = rm.osc.validate_scenario_against_network(scenario, network)
    assert findings, "no caller should have to remember to run validate_routes too"
    assert any("Ego" in f.location for f in findings)


def test_the_example_script_runs_clean():
    """python/examples/scenario_validate.py is this sprint's hand script.

    It exits non-zero when any of the five broken references goes unreported,
    so running it here IS the acceptance check rather than a smoke test.
    """
    example = pathlib.Path(__file__).resolve().parents[1] / "examples" / "scenario_validate.py"
    result = subprocess.run(
        [sys.executable, str(example)], capture_output=True, text=True, check=False
    )
    assert result.returncode == 0, result.stdout + result.stderr
    assert "clean scenario: 0 document findings, 0 cross-document findings" in result.stdout
    assert "broken references reported, each with its rule id" in result.stdout
