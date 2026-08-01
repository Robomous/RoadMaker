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

"""Authors a cut-in / traffic-light storyboard headlessly (p8-s4, issue #248).

The Python twin of what the Storyboard page of the 2D Editor pane does, and the
shape a GW-6 replay takes: every mutation is an undoable command on a
`ScenarioStack`, so the bytes this writes are the bytes the editor writes.

★ THE ONE THING TO COPY FROM THIS FILE. A
`TrafficSignalControllerAction/@phase` (or the matching condition) is authored
from `rm.osc.phase_names(controller)` and NEVER from `Phase.name`. `Phase.name`
may legally be empty — `roadmaker::SignalPhase::name` is — and the writer
synthesizes and de-duplicates names into the OUTPUT only, so a reference built
from the model matches nothing in the file it is written into. esmini v3.5.0
accepts that dangling reference in SILENCE (measured 2026-07-30), so nothing
downstream will tell you; `rm.osc.validate_scenario` is what catches it, and it
is run at the bottom of this file.

Run:  python python/examples/scenario_storyboard.py [--out <dir>]
"""

from __future__ import annotations

import argparse
from pathlib import Path

import roadmaker as rm

# Where the two cars start. Lane -1 is the inner driving lane of the right-hand
# side, -2 the one outside it — so a target in -2 moving to -1 cuts in front of
# an ego already there.
EGO_LANE = "-1"
TARGET_LANE = "-2"
START_SPEED = 13.89  # 50 km/h in m/s; speeds are m/s everywhere in the model
CUT_IN_DISTANCE = 12.0  # [m], freespace longitudinal


def build_network() -> tuple[rm.RoadNetwork, rm.JunctionId, str]:
    """A signalized crossing whose approaches carry two driving lanes each way."""
    network = rm.RoadNetwork()
    stack = rm.edit.EditStack()

    params = rm.edit.assembly.IntersectionParams()
    params.gap_m = 24.0
    params.arm_length_m = 80.0
    # NOT two_lane_default: a lane change needs somewhere to go.
    params.profile = rm.LaneProfile.arterial()
    stack.push(
        network,
        rm.edit.assembly.x_intersection(network, rm.edit.assembly.Pose(0.0, 0.0, 0.0), params),
    )
    junction = network.junction_ids[0]

    options = rm.edit.SignalizeOptions()
    options.tmpl = rm.edit.SignalizeTemplate.TWO_PHASE
    stack.push(network, rm.edit.signalize_junction(network, junction, options))

    approach = network.road(network.road_ids[0]).odr_id
    return network, junction, approach


def lane_position(road_odr_id: str, lane_odr_id: str, s: float) -> rm.osc.LanePosition:
    position = rm.osc.LanePosition()
    position.road_id = road_odr_id
    position.lane_id = lane_odr_id
    position.s = s
    position.offset = 0.0
    return position


def cut_in_group() -> rm.osc.ManeuverGroup:
    """The target cuts into the ego's lane when it gets close enough."""
    change = rm.osc.LaneChangeAction()
    dynamics = change.dynamics
    dynamics.dynamics_shape = "linear"  # NOT the "step" default: that is a teleport
    dynamics.dynamics_dimension = "time"
    dynamics.value = 2.0
    change.dynamics = dynamics
    target_lane = rm.osc.RelativeTargetLane()
    target_lane.entity_ref = "Target"
    target_lane.value = 1  # one lane toward the entity's +y, i.e. -2 -> -1
    change.target = target_lane

    lateral = rm.osc.LateralAction()
    lateral.lane_change = change
    private = rm.osc.PrivateAction()
    private.lateral = lateral
    action = rm.osc.Action()
    action.name = "cut_in"
    action.action = private

    distance = rm.osc.RelativeDistanceCondition()
    distance.entity_ref = "Ego"
    distance.freespace = True
    distance.relative_distance_type = "longitudinal"
    distance.rule = "lessThan"
    distance.value = CUT_IN_DISTANCE

    triggering = rm.osc.TriggeringEntities()
    who = rm.osc.EntityRef()
    who.entity_ref = "Target"
    triggering.entity_refs = [who]

    by_entity = rm.osc.ByEntityCondition()
    by_entity.triggering_entities = triggering
    by_entity.entity_condition = distance

    condition = rm.osc.Condition()
    condition.name = "close_enough"
    condition.by_entity = by_entity
    group = rm.osc.ConditionGroup()
    group.conditions = [condition]
    trigger = rm.osc.Trigger()
    trigger.condition_groups = [group]

    event = rm.osc.Event()
    event.name = "cut_in_event"
    event.actions = [action]
    event.start_trigger = trigger

    maneuver = rm.osc.StoryManeuver()
    maneuver.name = "cut_in_maneuver"
    maneuver.events = [event]

    actor = rm.osc.EntityRef()
    actor.entity_ref = "Target"
    maneuver_group = rm.osc.ManeuverGroup()
    maneuver_group.name = "target_group"
    maneuver_group.actors = [actor]
    maneuver_group.maneuvers = [maneuver]
    return maneuver_group


def traffic_light_group(controller: rm.osc.TrafficSignalController) -> rm.osc.ManeuverGroup:
    """Holds the crossing's first phase — the half esmini does not check."""
    phases = rm.osc.phase_names(controller)
    if not phases:
        raise SystemExit("the controller has no phases to reference")

    force = rm.osc.TrafficSignalControllerAction()
    force.traffic_signal_controller_ref = controller.name
    # ★ phase_names, NOT controller.phases[0].name — see this file's docstring.
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

    event = rm.osc.Event()
    event.name = "light_event"
    event.actions = [action]

    maneuver = rm.osc.StoryManeuver()
    maneuver.name = "light_maneuver"
    maneuver.events = [event]

    group = rm.osc.ManeuverGroup()
    group.name = "infrastructure_group"
    # No actors: "allowed for situations where the maneuvers ... are not related
    # to instances of Entity" (§7.3.1) — an infrastructure action has no actor.
    group.maneuvers = [maneuver]
    return group


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", type=Path, default=Path("."), help="output directory")
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)

    network, junction, approach = build_network()
    xodr = args.out / "storyboard.xodr"
    rm.save_xodr(network, xodr)

    scenario = rm.osc.Scenario()
    stack = rm.osc.edit.ScenarioStack()

    header = scenario.header
    header.description = "RoadMaker cut-in and traffic-light example"
    scenario.header = header

    stack.push(scenario, rm.osc.edit.set_logic_file(scenario, xodr.name))
    stack.push(scenario, rm.osc.edit.sync_traffic_signals(scenario, network, junction))

    stack.push(
        scenario,
        rm.osc.edit.place_scenario_object(
            scenario,
            rm.osc.make_actor(rm.osc.ActorKind.Car, "Ego"),
            lane_position(approach, EGO_LANE, 20.0),
        ),
    )
    stack.push(scenario, rm.osc.edit.set_entity_init_speed(scenario, "Ego", START_SPEED))
    stack.push(
        scenario,
        rm.osc.edit.place_scenario_object(
            scenario,
            rm.osc.make_actor(rm.osc.ActorKind.Car, "Target"),
            lane_position(approach, TARGET_LANE, 10.0),
        ),
    )
    stack.push(scenario, rm.osc.edit.set_entity_init_speed(scenario, "Target", START_SPEED))

    act = rm.osc.Act()
    act.name = "act"
    act.maneuver_groups = [
        cut_in_group(),
        traffic_light_group(scenario.road_network.traffic_signal_controllers[0]),
    ]
    story = rm.osc.Story()
    story.name = "cut_in_story"
    story.acts = [act]

    # ONE command for the whole story — the panel's granularity too: a
    # storyboard is a six-level tree and a per-node API would be thirty
    # factories carrying a five-deep index path.
    stack.push(scenario, rm.osc.edit.set_story(scenario, 0, story))

    # The scenario ends when the cut-in event completes, rather than on a
    # wall-clock timeout that has nothing to do with the story.
    done = rm.osc.Condition()
    done.name = "cut_in_done"
    element = rm.osc.StoryboardElementStateCondition()
    element.storyboard_element_ref = "cut_in_event"
    element.state = "completeState"
    element.storyboard_element_type = "event"
    done.storyboard_element_state = element
    done_group = rm.osc.ConditionGroup()
    done_group.conditions = [done]

    # ★ AND A DEADLINE BESIDE IT, in its OWN condition group — groups are OR'd,
    # conditions within a group are AND'd (§7.6.1). Without this the scenario
    # never stops when the target's cut-in never triggers, and a simulator run
    # that has to be killed is not a run anyone can measure.
    deadline = rm.osc.Condition()
    deadline.name = "deadline"
    timing = rm.osc.SimulationTimeCondition()
    timing.value = 30.0
    timing.rule = "greaterThan"
    deadline.simulation_time = timing
    deadline_group = rm.osc.ConditionGroup()
    deadline_group.conditions = [deadline]

    stop = rm.osc.Trigger()
    stop.condition_groups = [done_group, deadline_group]
    stack.push(scenario, rm.osc.edit.set_stop_trigger(scenario, stop))

    # The undo/redo fixed point, asserted on the way past: if these commands do
    # not round-trip, nothing GW-6 claims about a headless replay is worth
    # anything.
    full = rm.osc.write_xosc(scenario)
    for _ in range(10):
        while stack.can_undo:
            stack.undo(scenario)
        while stack.can_redo:
            stack.redo(scenario)
    assert rm.osc.write_xosc(scenario) == full, "undo x10 / redo x10 changed the document"

    # ★ THE CHECK ESMINI CANNOT MAKE. A @phase or a controller reference that
    # resolves to nothing loads silently in the simulator; this is what reports
    # it.
    findings = rm.osc.validate_scenario(scenario)
    for finding in findings:
        print(f"  {finding.severity}: {finding.location}: {finding.message}")
    if findings:
        return 1

    xosc = args.out / "storyboard.xosc"
    rm.osc.save_xosc(scenario, xosc)
    print(f"wrote {xodr}")
    print(f"wrote {xosc}")
    print(f"  1 story, {len(act.maneuver_groups)} maneuver groups, no findings")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
