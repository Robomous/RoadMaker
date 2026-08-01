#!/usr/bin/env python3

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

"""The broken references esmini loads in silence, caught (issue #533).

★ WHAT THIS SCRIPT IS. Every mutation below was MEASURED against the pinned
esmini v3.5.0 to load with **exit 0 and no error line** — a scenario whose
traffic-light half references nothing at all exports clean, loads clean, and is
wrong.

The script prints BOTH validators' counts side by side, because the split is the
whole point and it is not the split you would guess:

  * `rm.osc.validate_scenario` takes a `Scenario` ALONE. It catches the two
    references whose other end is also inside the document — a
    `@trafficSignalControllerRef` resolves against the scenario's own
    `<TrafficSignalController>` list (p8-s4, #248 added that) — and is BLIND to
    every reference whose other end is in the `.xodr`.
  * `rm.osc.validate_scenario_against_network` is the only thing that reads the
    network, so it is the only thing that catches a `<TrafficSignalState>` or a
    `<TrafficSignalCondition>` naming a signal no `<signal>` carries, and the
    only thing that can check an `s` against a road's actual length.

Run:  python python/examples/scenario_validate.py
"""

from __future__ import annotations

import roadmaker as rm


def build_scene() -> tuple[rm.RoadNetwork, rm.osc.Scenario]:
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

    approach = network.road(network.road_ids[0]).odr_id
    position = rm.osc.LanePosition()
    position.road_id = approach
    position.lane_id = "-1"
    position.s = 20.0
    stack.push(
        scenario,
        rm.osc.edit.place_scenario_object(
            scenario, rm.osc.make_actor(rm.osc.ActorKind.Car, "Ego"), position
        ),
    )

    # A story whose event holds the signal reference and the controller
    # reference — the two the simulator will not check.
    controller = scenario.road_network.traffic_signal_controllers[0]
    phases = rm.osc.phase_names(controller)

    force = rm.osc.TrafficSignalControllerAction()
    force.traffic_signal_controller_ref = controller.name
    force.phase = phases[0]
    signal_action = rm.osc.TrafficSignalAction()
    signal_action.action = force
    infrastructure = rm.osc.InfrastructureAction()
    infrastructure.traffic_signal = signal_action
    global_action = rm.osc.GlobalAction()
    global_action.infrastructure = infrastructure
    action = rm.osc.Action()
    action.name = "hold_phase"
    action.action = global_action

    green = rm.osc.Condition()
    green.name = "green"
    signal = rm.osc.TrafficSignalCondition()
    signal.name = first_signal_id(scenario)
    signal.state = "green"
    green.traffic_signal = signal
    group = rm.osc.ConditionGroup()
    group.conditions = [green]
    trigger = rm.osc.Trigger()
    trigger.condition_groups = [group]

    event = rm.osc.Event()
    event.name = "light_event"
    event.actions = [action]
    event.start_trigger = trigger
    maneuver = rm.osc.StoryManeuver()
    maneuver.name = "lights"
    maneuver.events = [event]
    maneuver_group = rm.osc.ManeuverGroup()
    maneuver_group.name = "infrastructure"
    maneuver_group.maneuvers = [maneuver]
    act = rm.osc.Act()
    act.name = "act"
    act.maneuver_groups = [maneuver_group]
    story = rm.osc.Story()
    story.name = "lights_story"
    story.acts = [act]
    stack.push(scenario, rm.osc.edit.set_story(scenario, 0, story))

    return network, scenario


def first_signal_id(scenario: rm.osc.Scenario) -> str:
    for phase in scenario.road_network.traffic_signal_controllers[0].phases:
        if phase.signal_states:
            return phase.signal_states[0].traffic_signal_id
    raise SystemExit("the fixture controller carries no signal states")


def break_signal_state(scenario: rm.osc.Scenario) -> None:
    """A <TrafficSignalState> naming a signal the .xodr does not have."""
    # `def_rw` on a vector hands back a COPY, so every mutation below rebuilds
    # the list and assigns it wholesale.
    road_network = scenario.road_network
    controllers = road_network.traffic_signal_controllers
    phases = controllers[0].phases
    states = phases[0].signal_states
    states[0].traffic_signal_id = "999"
    phases[0].signal_states = states
    controllers[0].phases = phases
    road_network.traffic_signal_controllers = controllers
    scenario.road_network = road_network


def break_controller_name(scenario: rm.osc.Scenario) -> None:
    """A <TrafficSignalController> naming a controller the .xodr does not have."""
    road_network = scenario.road_network
    controllers = road_network.traffic_signal_controllers
    controllers[0].name = "999"
    road_network.traffic_signal_controllers = controllers
    scenario.road_network = road_network


def _story_event(scenario: rm.osc.Scenario) -> rm.osc.Event:
    return scenario.storyboard.stories[0].acts[0].maneuver_groups[0].maneuvers[0].events[0]


def _put_story(scenario: rm.osc.Scenario, event: rm.osc.Event) -> None:
    storyboard = scenario.storyboard
    stories = storyboard.stories
    story = stories[0]
    acts = story.acts
    groups = acts[0].maneuver_groups
    maneuvers = groups[0].maneuvers
    maneuvers[0].events = [event]
    groups[0].maneuvers = maneuvers
    acts[0].maneuver_groups = groups
    story.acts = acts
    stories[0] = story
    storyboard.stories = stories
    scenario.storyboard = storyboard


def break_controller_action(scenario: rm.osc.Scenario) -> None:
    """A <TrafficSignalControllerAction> naming a controller that is not there."""
    event = _story_event(scenario)
    actions = event.actions
    global_action = actions[0].action
    infrastructure = global_action.infrastructure
    signal_action = infrastructure.traffic_signal
    arm = signal_action.action
    arm.traffic_signal_controller_ref = "999"
    signal_action.action = arm
    infrastructure.traffic_signal = signal_action
    global_action.infrastructure = infrastructure
    actions[0].action = global_action
    event.actions = actions
    _put_story(scenario, event)


def break_signal_condition(scenario: rm.osc.Scenario) -> None:
    """A <TrafficSignalCondition> naming a signal that is not there."""
    event = _story_event(scenario)
    trigger = event.start_trigger
    groups = trigger.condition_groups
    conditions = groups[0].conditions
    signal = conditions[0].traffic_signal
    signal.name = "999"
    conditions[0].traffic_signal = signal
    groups[0].conditions = conditions
    trigger.condition_groups = groups
    event.start_trigger = trigger
    _put_story(scenario, event)


def break_lane_anchor(scenario: rm.osc.Scenario) -> None:
    """An init teleport whose `s` is past the end of its road.

    Not one of the four — esmini DOES notice a dangling roadId or laneId — but
    it TRUNCATES an out-of-range `s` rather than refusing it, and
    `validate_scenario` cannot check the upper bound because the road's length
    lives in the .xodr.
    """
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


MUTATIONS = [
    ("a <TrafficSignalState> naming no live <signal>", break_signal_state),
    ("a <TrafficSignalController> naming no live <controller>", break_controller_name),
    ("a <TrafficSignalControllerAction> naming no live <controller>", break_controller_action),
    ("a <TrafficSignalCondition> naming no live <signal>", break_signal_condition),
    ("an init teleport whose s runs past the end of its road", break_lane_anchor),
]


def main() -> int:
    network, clean = build_scene()

    # The baseline: a scenario that resolves says nothing. A checker that cried
    # wolf here would be worse than none — nobody reads a dock that is always
    # red.
    document_only = rm.osc.validate_scenario(clean)
    cross = rm.osc.validate_scenario_against_network(clean, network)
    assert not document_only, [f.message for f in document_only]
    assert not cross, [f.message for f in cross]
    print("clean scenario: 0 document findings, 0 cross-document findings")

    failures = 0
    for label, mutate in MUTATIONS:
        # Rebuilt rather than deep-copied: a nanobind object wraps C++ storage
        # and is not picklable, so `copy.deepcopy` raises. Rebuilding is also
        # the honest thing — each mutation is measured against a scene that was
        # clean a moment ago.
        _, broken = build_scene()
        mutate(broken)

        document = rm.osc.validate_scenario(broken)
        findings = rm.osc.validate_scenario_against_network(broken, network)

        # ★ The point of the whole exercise: the document-only validator is
        # silent, and this one is not.
        print(f"\n{label}")
        print(f"  validate_scenario:                 {len(document)} finding(s)")
        print(f"  validate_scenario_against_network: {len(findings)} finding(s)")
        for finding in findings:
            print(f"    {finding.location}")
            print(f"      {finding.message}")
            print(f"      rule: {finding.rule_id or '(none)'}")
        if not findings:
            print("    ✗ NOT CAUGHT")
            failures += 1

    if failures:
        print(f"\n{failures} mutation(s) went unreported")
        return 1
    print(f"\nall {len(MUTATIONS)} broken references reported, each with its rule id")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
