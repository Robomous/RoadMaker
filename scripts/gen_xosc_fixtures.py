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

"""Generates the tracked OpenSCENARIO fixture the esmini CI gate loads (p8-s1, #245).

WHY A GENERATED FIXTURE AND NOT A HAND-WRITTEN ONE. The whole point of
tests/esmini/signalized.xosc is to prove that a file THIS BUILD WRITES is
accepted by a shipping simulator — which is what "validation" means for this
project (the maintainer ruling on #257: there is no XSD in the tree and CI
cannot carry one). A hand-authored file would prove that a human can write valid
OpenSCENARIO, which was never in doubt.

The pair is a pair on purpose. No sample .xodr in this repository contains a
single `<controller>` element, so nothing here could reference one — and the
traffic-signal half is exactly what needs a simulator's opinion:
`TrafficSignalController/@name` carries the OpenDRIVE controller `@id`, and
`TrafficSignalState/@state`'s spelling is an ENGINE-DIRECTED choice the
specification explicitly leaves open (§10.10).

★ EVERY MUTATION HERE NOW GOES THROUGH THE KERNEL, AND THAT IS THE POINT (PR-D).
This script used to hand-assemble the junction-timeline decomposition in Python.
It no longer contains any: the decomposition is
`rm.osc.edit.sync_traffic_signals` over `rm.osc.decompose_junction_signals`, and
every edit below is an undoable command pushed onto a `ScenarioStack` — exactly
as a GW-6 replay drives them. So the tracked output stopped being merely a
fixture: `XoscFixture.TheTrackedScenarioIsExactlyWhatTheWriterEmitsToday`
(core/tests/test_xosc_reader.cpp) pins these bytes, so regenerating them is a
deliberate act with a red test in front of it, and those bytes are now a test of
the kernel factories that produced them.

The SignalState -> @state table moved to the kernel too (`rm.osc.state_token`,
core/src/osc/decompose.cpp), where its "measured, and validated by nothing"
provenance is recorded in full.

Run:  python scripts/gen_xosc_fixtures.py [--out tests/esmini]
"""

from __future__ import annotations

import argparse
from pathlib import Path

import roadmaker as rm

# The teleport target: on an approach arm, well clear of the junction box, so
# the ego is on a real lane rather than in the middle of the intersection.
#
# ★ A LANE POSITION, NOT A WORLD ONE (p8-s2, #246). The ego now names the road
# and the lane by their OpenDRIVE `@id` STRINGS, which is what the editor's
# Actor tool authors and what makes an actor follow its road through later
# edits. It also puts a <LanePosition> in front of esmini, which is the only way
# to learn whether a shipping simulator accepts the one RoadMaker writes —
# the whole reason this fixture is generated rather than hand-authored.
#
# Road "3" is the west arm (reference line running from x=-24 out to x=-104), so
# s=16 is 40 m from the junction centre, matching the world position this
# replaced. Lane "-1" is the first driving lane right of the reference line.
EGO_ROAD_ODR_ID = "3"
EGO_LANE_ODR_ID = "-1"
EGO_START_S = 16.0

# 50 km/h in m/s. Speeds are m/s everywhere in the model; km/h and mph are a
# display concern that never reaches the file.
EGO_START_SPEED = 13.89


def build_network() -> tuple[rm.RoadNetwork, rm.JunctionId]:
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

    assert not rm.validate_network(network), "the fixture network must itself be clean"
    return network, junction


def build_scenario(
    network: rm.RoadNetwork, junction: rm.JunctionId, xodr_name: str
) -> rm.osc.Scenario:
    """Authors the scenario the way the editor will: one command at a time."""
    scenario = rm.osc.Scenario()
    stack = rm.osc.edit.ScenarioStack()

    header = scenario.header
    header.description = "RoadMaker signalized-junction fixture"
    scenario.header = header

    # The .xodr link. Relative: esmini resolves it against the .xosc.
    stack.push(scenario, rm.osc.edit.set_logic_file(scenario, xodr_name))

    # The traffic-signal half — one TrafficSignalController per OpenDRIVE
    # <controller>, read off the Red-filled plan (ADR-0014 §8). This one call
    # replaces every line of decomposition this script used to carry.
    stack.push(scenario, rm.osc.edit.sync_traffic_signals(scenario, network, junction))
    for finding in stack.last_findings:
        print(f"  note: {finding.location}: {finding.message}")

    # The ego, from the kernel's actor catalogue — <Performance> and <Axles>
    # included, which are required children of <Vehicle> in every revision and
    # which this script used to assemble by hand and half-omit.
    position = rm.osc.LanePosition()
    position.road_id = EGO_ROAD_ODR_ID
    position.lane_id = EGO_LANE_ODR_ID
    position.s = EGO_START_S
    position.offset = 0.0  # the lane centre

    # ONE command: placing an actor is one gesture, and it is one undo entry.
    stack.push(
        scenario,
        rm.osc.edit.place_scenario_object(
            scenario, rm.osc.make_actor(rm.osc.ActorKind.Car, "Ego"), position
        ),
    )
    stack.push(scenario, rm.osc.edit.set_entity_init_speed(scenario, "Ego", EGO_START_SPEED))

    # The stop trigger stays hand-assembled: the storyboard model is p8-s4's,
    # and inventing a factory for it here would be scope this sprint did not
    # take. Assigned wholesale — `def_rw` on a vector hands back a COPY.
    end = rm.osc.Condition()
    end.name = "end"
    timing = rm.osc.SimulationTimeCondition()
    timing.value = 0.5
    timing.rule = "greaterThan"
    end.simulation_time = timing
    group = rm.osc.ConditionGroup()
    group.conditions = [end]
    storyboard = scenario.storyboard
    stop = storyboard.stop_trigger
    stop.condition_groups = [group]
    storyboard.stop_trigger = stop
    scenario.storyboard = storyboard

    # ★ The undo/redo fixed point, asserted on the way past. If the commands
    # above do not round-trip, the fixture rests on a broken stack and every
    # claim GW-6 makes about headless replay is worth nothing. Note this runs
    # BEFORE the hand-assembled stop trigger would be affected: the stack knows
    # nothing about it, so undoing to the bottom leaves it in place.
    full = rm.osc.write_xosc(scenario)
    for _ in range(10):
        while stack.can_undo:
            stack.undo(scenario)
        while stack.can_redo:
            stack.redo(scenario)
    assert rm.osc.write_xosc(scenario) == full, "undo x10 / redo x10 changed the document"

    findings = rm.osc.validate_scenario(scenario)
    assert not findings, [f.message for f in findings]
    return scenario


def build_routed_network() -> rm.RoadNetwork:
    """Two straight roads welded end-to-start along +x, 100 m each (p8-s3, #247).

    Deliberately the SMALLEST network whose route resolution crosses a road
    boundary: RoadMaker's own authoring never emits `<lane><link>` across a
    plain weld (the resolver's documented same-`@id` fallback carries it), so
    this pair is exactly the shape whose acceptance by a shipping simulator is
    worth knowing — a junction would re-test what signalized.* already covers.
    """
    network = rm.RoadNetwork()
    stack = rm.edit.EditStack()

    profile = rm.LaneProfile.two_lane_default()
    stack.push(network, rm.edit.create_road([(0.0, 0.0), (100.0, 0.0)], profile, "west"))
    first = network.road_ids[0]
    end = rm.RoadEnd()
    end.road = first
    end.contact = rm.ContactPoint.END
    stack.push(
        network,
        rm.edit.create_linked_road(network, [(100.0, 0.0), (200.0, 0.0)], profile, "east", end),
    )

    assert not rm.validate_network(network), "the fixture network must itself be clean"
    return network


def _route_waypoint(road_odr_id: str, lane_odr_id: str, s: float) -> rm.osc.RouteWaypoint:
    position = rm.osc.LanePosition()
    position.road_id = road_odr_id
    position.lane_id = lane_odr_id
    position.s = s
    waypoint = rm.osc.RouteWaypoint()
    waypoint.route_strategy = "shortest"
    waypoint.position = position
    return waypoint


def build_routed_scenario(network: rm.RoadNetwork, xodr_name: str) -> rm.osc.Scenario:
    """An ego with a lane-anchored route across the weld, one command at a time."""
    scenario = rm.osc.Scenario()
    stack = rm.osc.edit.ScenarioStack()

    header = scenario.header
    header.description = "RoadMaker routed fixture"
    scenario.header = header

    stack.push(scenario, rm.osc.edit.set_logic_file(scenario, xodr_name))

    roads = {network.road(i).name: network.road(i).odr_id for i in network.road_ids}

    position = rm.osc.LanePosition()
    position.road_id = roads["west"]
    position.lane_id = "-1"
    position.s = 10.0
    position.offset = 0.0
    stack.push(
        scenario,
        rm.osc.edit.place_scenario_object(
            scenario, rm.osc.make_actor(rm.osc.ActorKind.Car, "Ego"), position
        ),
    )
    stack.push(scenario, rm.osc.edit.set_entity_init_speed(scenario, "Ego", EGO_START_SPEED))

    # The route: one waypoint per road, lane -1 both times, joined across the
    # weld by the resolver. Authored through the same factory the editor's
    # Route tool pushes.
    route = rm.osc.Route()
    route.name = "EgoRoute"
    route.waypoints = [
        _route_waypoint(roads["west"], "-1", 20.0),
        _route_waypoint(roads["east"], "-1", 80.0),
    ]
    stack.push(scenario, rm.osc.edit.assign_route(scenario, "Ego", route))

    # The same 0.5 s stop trigger signalized.xosc carries, for the same reason:
    # a scenario with no stop trigger runs in esmini until something kills it,
    # and a smoke gate that has to time out to pass is not a gate.
    end = rm.osc.Condition()
    end.name = "end"
    timing = rm.osc.SimulationTimeCondition()
    timing.value = 0.5
    timing.rule = "greaterThan"
    end.simulation_time = timing
    group = rm.osc.ConditionGroup()
    group.conditions = [end]
    storyboard = scenario.storyboard
    stop = storyboard.stop_trigger
    stop.condition_groups = [group]
    storyboard.stop_trigger = stop
    scenario.storyboard = storyboard

    # ★ The cross-document gate, run at generation time: the route must resolve
    # COMPLETELY against the network the pair ships with. A fixture whose route
    # did not drive would put a broken example in front of esmini and call
    # whatever it says a measurement.
    assert not rm.osc.validate_routes(network, scenario), "the fixture route must resolve"
    resolved = rm.osc.resolve_route(network, route)
    assert resolved.complete, [f.message for f in resolved.findings]
    assert len(resolved.legs) >= 2, "the route must actually cross the weld"

    full = rm.osc.write_xosc(scenario)
    for _ in range(10):
        while stack.can_undo:
            stack.undo(scenario)
        while stack.can_redo:
            stack.redo(scenario)
    assert rm.osc.write_xosc(scenario) == full, "undo x10 / redo x10 changed the document"

    findings = rm.osc.validate_scenario(scenario)
    assert not findings, [f.message for f in findings]
    return scenario


# --- the cut-in / traffic-light pair (p8-s4, #248) ----------------------------
#
# The acceptance of #248 is "a cut-in / traffic-light scenario is authorable end
# to end", and the two halves are validated by DIFFERENT things: esmini was
# measured to resolve a lane anchor against the .xodr, and measured NOT to check
# a signal or controller reference at all (#257, #533). So this pair carries
# both, and each half is gated by the thing that can actually see it — esmini
# here, `validate_scenario` in core/tests/test_xosc_storyboard.cpp.
#
# A MULTI-LANE APPROACH, which is why this is its own network and not the
# signalized one above: `two_lane_default` gives one driving lane per direction,
# and a lane change with nowhere to go is a fixture that says nothing. The
# arterial profile gives lanes -1 and -2 driving plus a sidewalk, so the cut-in
# is a real manoeuvre between two real lanes.
CUTIN_ROAD_HINT = "the west approach"
CUTIN_EGO_LANE = "-1"  # the inner driving lane
CUTIN_TARGET_LANE = "-2"  # the outer one, from which the target cuts in
CUTIN_TRIGGER_DISTANCE = 12.0  # [m], freespace longitudinal


def build_cutin_network() -> tuple[rm.RoadNetwork, rm.JunctionId]:
    """A signalized four-arm crossing whose approaches carry TWO driving lanes."""
    network = rm.RoadNetwork()
    stack = rm.edit.EditStack()

    params = rm.edit.assembly.IntersectionParams()
    params.gap_m = 24.0
    params.arm_length_m = 80.0
    params.profile = rm.LaneProfile.arterial()
    stack.push(
        network,
        rm.edit.assembly.x_intersection(network, rm.edit.assembly.Pose(0.0, 0.0, 0.0), params),
    )
    junction = network.junction_ids[0]

    options = rm.edit.SignalizeOptions()
    options.tmpl = rm.edit.SignalizeTemplate.TWO_PHASE
    stack.push(network, rm.edit.signalize_junction(network, junction, options))

    assert not rm.validate_network(network), "the fixture network must itself be clean"
    return network, junction


def _lane_position(road_odr_id: str, lane_odr_id: str, s: float) -> rm.osc.LanePosition:
    position = rm.osc.LanePosition()
    position.road_id = road_odr_id
    position.lane_id = lane_odr_id
    position.s = s
    position.offset = 0.0
    return position


def build_cutin_scenario(
    network: rm.RoadNetwork, junction: rm.JunctionId, xodr_name: str
) -> rm.osc.Scenario:
    """Ego and a target on one approach, a cut-in story, and a traffic-light story."""
    scenario = rm.osc.Scenario()
    stack = rm.osc.edit.ScenarioStack()

    header = scenario.header
    header.description = "RoadMaker cut-in and traffic-light fixture"
    scenario.header = header

    stack.push(scenario, rm.osc.edit.set_logic_file(scenario, xodr_name))
    stack.push(scenario, rm.osc.edit.sync_traffic_signals(scenario, network, junction))

    stack.push(
        scenario,
        rm.osc.edit.place_scenario_object(
            scenario,
            rm.osc.make_actor(rm.osc.ActorKind.Car, "Ego"),
            _lane_position(EGO_ROAD_ODR_ID, CUTIN_EGO_LANE, 20.0),
        ),
    )
    stack.push(scenario, rm.osc.edit.set_entity_init_speed(scenario, "Ego", EGO_START_SPEED))
    stack.push(
        scenario,
        rm.osc.edit.place_scenario_object(
            scenario,
            rm.osc.make_actor(rm.osc.ActorKind.Car, "Target"),
            _lane_position(EGO_ROAD_ODR_ID, CUTIN_TARGET_LANE, 10.0),
        ),
    )
    stack.push(scenario, rm.osc.edit.set_entity_init_speed(scenario, "Target", EGO_START_SPEED))

    # --- the cut-in half ------------------------------------------------------
    change = rm.osc.LaneChangeAction()
    dynamics = change.dynamics
    dynamics.dynamics_shape = "linear"
    dynamics.dynamics_dimension = "time"
    dynamics.value = 2.0
    change.dynamics = dynamics
    target_lane = rm.osc.RelativeTargetLane()
    target_lane.entity_ref = "Target"
    # +1: one lane toward the reference entity's positive y — from -2 to -1,
    # which is the lane Ego is in. That is what "cuts in" means.
    target_lane.value = 1
    change.target = target_lane

    lateral = rm.osc.LateralAction()
    lateral.lane_change = change
    private = rm.osc.PrivateAction()
    private.lateral = lateral
    cut_in = rm.osc.Action()
    cut_in.name = "cut_in"
    cut_in.action = private

    close = rm.osc.Condition()
    close.name = "close_enough"
    by_entity = rm.osc.ByEntityCondition()
    triggering = by_entity.triggering_entities
    entity_ref = rm.osc.EntityRef()
    entity_ref.entity_ref = "Target"
    triggering.entity_refs = [entity_ref]
    by_entity.triggering_entities = triggering
    distance = rm.osc.RelativeDistanceCondition()
    distance.entity_ref = "Ego"
    distance.freespace = True
    distance.relative_distance_type = "longitudinal"
    distance.rule = "lessThan"
    distance.value = CUTIN_TRIGGER_DISTANCE
    by_entity.entity_condition = distance
    close.by_entity = by_entity

    group = rm.osc.ConditionGroup()
    group.conditions = [close]
    cut_in_trigger = rm.osc.Trigger()
    cut_in_trigger.condition_groups = [group]

    cut_in_event = rm.osc.Event()
    cut_in_event.name = "cut_in_event"
    cut_in_event.actions = [cut_in]
    cut_in_event.start_trigger = cut_in_trigger

    cut_in_maneuver = rm.osc.StoryManeuver()
    cut_in_maneuver.name = "cut_in_maneuver"
    cut_in_maneuver.events = [cut_in_event]

    actor = rm.osc.EntityRef()
    actor.entity_ref = "Target"
    cut_in_group = rm.osc.ManeuverGroup()
    cut_in_group.name = "target_group"
    cut_in_group.actors = [actor]
    cut_in_group.maneuvers = [cut_in_maneuver]

    # --- the traffic-light half ----------------------------------------------
    #
    # ★ THE @phase COMES FROM `rm.osc.phase_names`, NEVER FROM `Phase.name`.
    # `Phase.name` is legally empty and the writer synthesizes names into the
    # OUTPUT only, so a reference built from the model matches nothing in the
    # file — the trap #248 was filed with. esmini accepts the broken form in
    # silence (measured 2026-07-30, #257), which is exactly why this is
    # resolved here and checked by `validate_scenario` rather than by the smoke
    # gate.
    controller = scenario.road_network.traffic_signal_controllers[0]
    phases = rm.osc.phase_names(controller)
    assert phases, "the fixture controller must have phases to reference"

    force = rm.osc.TrafficSignalControllerAction()
    force.traffic_signal_controller_ref = controller.name
    force.phase = phases[0]
    signal_action = rm.osc.TrafficSignalAction()
    signal_action.action = force
    infrastructure = rm.osc.InfrastructureAction()
    infrastructure.traffic_signal = signal_action
    global_action = rm.osc.GlobalAction()
    global_action.infrastructure = infrastructure

    light = rm.osc.Action()
    light.name = "hold_green"
    light.action = global_action

    light_event = rm.osc.Event()
    light_event.name = "light_event"
    light_event.actions = [light]

    light_maneuver = rm.osc.StoryManeuver()
    light_maneuver.name = "light_maneuver"
    light_maneuver.events = [light_event]

    light_group = rm.osc.ManeuverGroup()
    light_group.name = "infrastructure_group"
    # No actors: "allowed for situations where the maneuvers ... are not related
    # to instances of Entity" (§7.3.1), which an infrastructure action is.
    light_group.maneuvers = [light_maneuver]

    act = rm.osc.Act()
    act.name = "act"
    act.maneuver_groups = [cut_in_group, light_group]
    story = rm.osc.Story()
    story.name = "cut_in_story"
    story.acts = [act]

    stack.push(scenario, rm.osc.edit.set_story(scenario, 0, story))

    end = rm.osc.Condition()
    end.name = "end"
    timing = rm.osc.SimulationTimeCondition()
    timing.value = 0.5
    timing.rule = "greaterThan"
    end.simulation_time = timing
    stop_group = rm.osc.ConditionGroup()
    stop_group.conditions = [end]
    stop = rm.osc.Trigger()
    stop.condition_groups = [stop_group]
    stack.push(scenario, rm.osc.edit.set_stop_trigger(scenario, stop))

    full = rm.osc.write_xosc(scenario)
    for _ in range(10):
        while stack.can_undo:
            stack.undo(scenario)
        while stack.can_redo:
            stack.redo(scenario)
    assert rm.osc.write_xosc(scenario) == full, "undo x10 / redo x10 changed the document"

    findings = rm.osc.validate_scenario(scenario)
    assert not findings, [f.message for f in findings]
    return scenario


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--out",
        type=Path,
        default=Path(__file__).resolve().parent.parent / "tests" / "esmini",
        help="directory the pairs are written to (default: tests/esmini)",
    )
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)

    network, junction = build_network()
    xodr = args.out / "signalized.xodr"
    xosc = args.out / "signalized.xosc"

    rm.save_xodr(network, xodr)
    scenario = build_scenario(network, junction, xodr.name)
    rm.osc.save_xosc(scenario, xosc)

    controllers = scenario.road_network.traffic_signal_controllers
    print(f"wrote {xodr}")
    print(f"wrote {xosc}")
    print(
        f"  {len(controllers)} traffic signal controller(s), "
        f"{len(controllers[0].phases)} phases each, "
        f"{sum(len(p.signal_states) for p in controllers[0].phases)} signal states in the first"
    )

    routed_network = build_routed_network()
    routed_xodr = args.out / "routed.xodr"
    routed_xosc = args.out / "routed.xosc"

    rm.save_xodr(routed_network, routed_xodr)
    routed = build_routed_scenario(routed_network, routed_xodr.name)
    rm.osc.save_xosc(routed, routed_xosc)

    print(f"wrote {routed_xodr}")
    print(f"wrote {routed_xosc}")

    cutin_network, cutin_junction = build_cutin_network()
    cutin_xodr = args.out / "cutin.xodr"
    cutin_xosc = args.out / "cutin.xosc"

    rm.save_xodr(cutin_network, cutin_xodr)
    cutin = build_cutin_scenario(cutin_network, cutin_junction, cutin_xodr.name)
    rm.osc.save_xosc(cutin, cutin_xosc)

    print(f"wrote {cutin_xodr}")
    print(f"wrote {cutin_xosc}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
