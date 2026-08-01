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

"""The modeled storyboard through the bindings (p8-s4, issue #248).

The properties worth a headless test are the ones a GW-6 replay rests on: the
whole tree round-trips through the bindings, `set_story` is one undoable
command, and a `@phase` authored from `phase_names` resolves while one authored
from `Phase.name` does not.
"""

from __future__ import annotations

import pathlib
import subprocess
import sys

import pytest

import roadmaker as rm


def _entity(name: str) -> rm.osc.ScenarioObject:
    return rm.osc.make_actor(rm.osc.ActorKind.Car, name)


@pytest.fixture
def scenario() -> rm.osc.Scenario:
    """Two actors and a linked network — everything a story can reference."""
    scenario = rm.osc.Scenario()
    stack = rm.osc.edit.ScenarioStack()
    stack.push(scenario, rm.osc.edit.set_logic_file(scenario, "n.xodr"))
    position = rm.osc.WorldPosition()
    stack.push(scenario, rm.osc.edit.place_scenario_object(scenario, _entity("Ego"), position))
    stack.push(scenario, rm.osc.edit.place_scenario_object(scenario, _entity("Target"), position))
    return scenario


def _minimal_story(name: str = "story") -> rm.osc.Story:
    """The smallest story the schema admits, and no smaller."""
    change = rm.osc.LaneChangeAction()
    target = rm.osc.RelativeTargetLane()
    target.entity_ref = "Target"
    target.value = 1
    change.target = target

    lateral = rm.osc.LateralAction()
    lateral.lane_change = change
    private = rm.osc.PrivateAction()
    private.lateral = lateral
    action = rm.osc.Action()
    action.name = "cut_in"
    action.action = private

    event = rm.osc.Event()
    event.name = "event"
    event.actions = [action]

    maneuver = rm.osc.StoryManeuver()
    maneuver.name = "maneuver"
    maneuver.events = [event]

    actor = rm.osc.EntityRef()
    actor.entity_ref = "Target"
    group = rm.osc.ManeuverGroup()
    group.name = "group"
    group.actors = [actor]
    group.maneuvers = [maneuver]

    act = rm.osc.Act()
    act.name = "act"
    act.maneuver_groups = [group]

    story = rm.osc.Story()
    story.name = name
    story.acts = [act]
    return story


def test_a_story_round_trips_through_the_reader(scenario):
    stack = rm.osc.edit.ScenarioStack()
    stack.push(scenario, rm.osc.edit.set_story(scenario, 0, _minimal_story()))

    written = rm.osc.write_xosc(scenario)
    reloaded = rm.osc.parse_xosc(written, "storyboard.xosc")

    # THE BYTES ARE THE ORACLE: a field-by-field comparison passes on a model
    # that normalizes something the writer then emits differently.
    assert rm.osc.write_xosc(reloaded.scenario) == written

    stories = reloaded.scenario.storyboard.stories
    assert len(stories) == 1
    assert stories[0].name == "story"
    assert len(stories[0].acts[0].maneuver_groups[0].maneuvers[0].events[0].actions) == 1


def test_set_story_appends_then_replaces_and_undo_is_byte_identical(scenario):
    before = rm.osc.write_xosc(scenario)
    stack = rm.osc.edit.ScenarioStack()

    stack.push(scenario, rm.osc.edit.set_story(scenario, 0, _minimal_story()))
    appended = rm.osc.write_xosc(scenario)
    assert len(scenario.storyboard.stories) == 1

    stack.push(scenario, rm.osc.edit.set_story(scenario, 0, _minimal_story("renamed")))
    assert len(scenario.storyboard.stories) == 1, "a replace must not append"
    assert scenario.storyboard.stories[0].name == "renamed"

    stack.undo(scenario)
    assert rm.osc.write_xosc(scenario) == appended
    stack.undo(scenario)
    assert rm.osc.write_xosc(scenario) == before
    stack.redo(scenario)
    assert rm.osc.write_xosc(scenario) == appended


def test_set_story_refuses_what_would_make_the_document_unwritable(scenario):
    stack = rm.osc.edit.ScenarioStack()
    stack.push(scenario, rm.osc.edit.set_story(scenario, 0, _minimal_story()))
    before = rm.osc.write_xosc(scenario)

    actless = _minimal_story("other")
    actless.acts = []
    with pytest.raises(ValueError, match="at least one act"):
        stack.push(scenario, rm.osc.edit.set_story(scenario, 0, actless))

    unnamed = _minimal_story("")
    with pytest.raises(ValueError, match="needs a name"):
        stack.push(scenario, rm.osc.edit.set_story(scenario, 0, unnamed))

    # Never clamped: an index past the end is refused, because a clamped index
    # rewrites a story the caller did not name.
    with pytest.raises(ValueError, match="past the end"):
        stack.push(scenario, rm.osc.edit.set_story(scenario, 99, _minimal_story("far")))

    assert rm.osc.write_xosc(scenario) == before


def test_remove_story_restores_it_in_place_on_undo(scenario):
    stack = rm.osc.edit.ScenarioStack()
    stack.push(scenario, rm.osc.edit.set_story(scenario, 0, _minimal_story("first")))
    stack.push(scenario, rm.osc.edit.set_story(scenario, 1, _minimal_story("second")))
    before = rm.osc.write_xosc(scenario)

    stack.push(scenario, rm.osc.edit.remove_story(scenario, 0))
    assert [s.name for s in scenario.storyboard.stories] == ["second"]

    stack.undo(scenario)
    assert rm.osc.write_xosc(scenario) == before


def _signalized_scenario() -> tuple[rm.osc.Scenario, rm.osc.TrafficSignalController]:
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
    stack.push(scenario, rm.osc.edit.set_logic_file(scenario, "n.xodr"))
    stack.push(scenario, rm.osc.edit.sync_traffic_signals(scenario, network, junction))
    position = rm.osc.WorldPosition()
    stack.push(scenario, rm.osc.edit.place_scenario_object(scenario, _entity("Ego"), position))
    stack.push(scenario, rm.osc.edit.place_scenario_object(scenario, _entity("Target"), position))
    return scenario, scenario.road_network.traffic_signal_controllers[0]


def _story_referencing_phase(controller_name: str, phase: str) -> rm.osc.Story:
    force = rm.osc.TrafficSignalControllerAction()
    force.traffic_signal_controller_ref = controller_name
    force.phase = phase
    signal_action = rm.osc.TrafficSignalAction()
    signal_action.action = force
    infrastructure = rm.osc.InfrastructureAction()
    infrastructure.traffic_signal = signal_action
    global_action = rm.osc.GlobalAction()
    global_action.infrastructure = infrastructure

    action = rm.osc.Action()
    action.name = "hold_phase"
    action.action = global_action

    story = _minimal_story()
    story.acts[0].maneuver_groups[0].maneuvers[0].events[0].actions = [action]
    return story


def test_phase_names_are_what_the_file_carries_not_phase_name():
    """★ The trap #248 was filed with, in the form a Python author meets it."""
    scenario, controller = _signalized_scenario()
    names = rm.osc.phase_names(controller)
    assert names

    stack = rm.osc.edit.ScenarioStack()
    stack.push(scenario, rm.osc.edit.set_story(scenario, 0, _story_referencing_phase(controller.name, names[0])))
    written = rm.osc.write_xosc(scenario)
    assert not rm.osc.validate_scenario(scenario)
    assert f'<Phase name="{names[0]}"' in written

    # A reference that names no written phase is refused — the check esmini was
    # measured NOT to make (it loads the broken file in silence).
    stack.push(
        scenario,
        rm.osc.edit.set_story(scenario, 0, _story_referencing_phase(controller.name, "nope")),
    )
    findings = rm.osc.validate_scenario(scenario)
    assert any("has no phase of that name" in f.message for f in findings)
    with pytest.raises(ValueError):
        rm.osc.write_xosc(scenario)


def test_a_dangling_controller_reference_is_refused():
    scenario, controller = _signalized_scenario()
    stack = rm.osc.edit.ScenarioStack()
    stack.push(
        scenario,
        rm.osc.edit.set_story(scenario, 0, _story_referencing_phase("999", "whatever")),
    )
    findings = rm.osc.validate_scenario(scenario)
    assert any(
        f.rule_id == "asam.net:xosc:1.0.0:reference_control.traffic_signal_controller_references"
        for f in findings
    )
    assert controller.name != "999"


def test_the_example_script_runs_clean(tmp_path):
    """python/examples/scenario_storyboard.py is this sprint's hand script.

    An example nobody runs is wrong in ways review does not catch — the p6-s9
    lesson, and the reason every sprint's example is executed here.
    """
    example = (
        pathlib.Path(__file__).resolve().parents[1] / "examples" / "scenario_storyboard.py"
    )
    result = subprocess.run(
        [sys.executable, str(example), "--out", str(tmp_path)],
        capture_output=True,
        text=True,
        check=False,
    )
    assert result.returncode == 0, result.stdout + result.stderr
    assert "1 story, 2 maneuver groups, no findings" in result.stdout
    assert (tmp_path / "storyboard.xosc").exists()
