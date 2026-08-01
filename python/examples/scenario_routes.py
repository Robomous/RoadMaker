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

"""Give an actor a lane-anchored route, and watch what happens to it (p8-s3, #247).

`scenario_actors.py` places an actor. This one gives it a PATH, and then does
the two things GW-6 steps 7 and 8 ask for — which are the two things that
distinguish a lane-anchored route from a polyline that merely looked right when
it was drawn:

  step 7  MOVE a road the route runs along. The route still resolves, because a
          waypoint names a LANE rather than a point in space. Nothing has to be
          recomputed; the route was never in world coordinates to begin with.

  step 8  DELETE a lane the route traverses. The route is REPORTED as
          invalidated — it is not silently dropped, and it is not silently
          re-routed onto a lane nobody chose. Both of those "helpful" behaviours
          destroy what the user authored and give them no way to know.

This file is the headless evidence for those two steps. `python/` links
`roadmaker::core` alone, so anything the editor does that cannot be replayed
here has no evidence at all.

Run:  python python/examples/scenario_routes.py
"""

from __future__ import annotations

import tempfile
from pathlib import Path

import roadmaker as rm


def build_network() -> rm.RoadNetwork:
    """Two straight roads laid end to end and linked, so a lane runs through both."""
    network = rm.RoadNetwork()
    first = rm.author_clothoid_road(
        network, [(0.0, 0.0), (100.0, 0.0)], rm.LaneProfile.two_lane_default(), "", "1"
    )
    command = rm.edit.create_linked_road(
        network,
        [(100.0, 0.0), (200.0, 0.0)],
        rm.LaneProfile.two_lane_default(),
        "second",
        rm.RoadEnd(first, rm.ContactPoint.END),
    )
    stack = rm.edit.EditStack()
    stack.push(network, command)
    return network


def outermost_right_lane(network: rm.RoadNetwork, road_odr_id: str, s: float):
    """The outermost right-hand lane of a road at station `s`.

    Outermost specifically: `rm.edit.remove_lane` refuses anything else, so this
    is the only lane the deletion below can actually remove — and therefore the
    one the route has to run along for step 8 to be about deletion at all.
    """
    road = network.find_road(road_odr_id)
    section = network.lane_section(network.section_at(road, s))
    best = None
    best_id = 0
    for lane_id in section.lanes:
        lane = network.lane(lane_id)
        if lane.odr_id < best_id:
            best_id = lane.odr_id
            best = lane_id
    return best, best_id


def waypoint(road_odr_id: str, lane_odr_id: int, s: float) -> rm.osc.RouteWaypoint:
    position = rm.osc.LanePosition()
    position.road_id = road_odr_id
    position.lane_id = str(lane_odr_id)
    position.s = s
    point = rm.osc.RouteWaypoint()
    point.route_strategy = "shortest"
    point.position = position
    return point


def scenario_with_an_actor() -> rm.osc.Scenario:
    """One car, on a scenario that links a road network.

    ★ BUILT WITH COMMANDS, NOT BY ASSIGNING MEMBERS. `def_rw` on a vector or a
    nested struct hands Python a COPY, so `scenario.entities.scenario_objects
    .append(...)` mutates a temporary and leaves the scenario empty — silently,
    and the failure surfaces much later as "no entity named 'Ego'". The command
    layer is the supported mutator and has no such trap.
    """
    scenario = rm.osc.Scenario()
    stack = rm.osc.edit.ScenarioStack()
    stack.push(scenario, rm.osc.edit.set_logic_file(scenario, "town.xodr"))
    stack.push(
        scenario,
        rm.osc.edit.add_scenario_object(scenario, rm.osc.make_actor(rm.osc.ActorKind.Car, "Ego")),
    )
    return scenario


def main() -> None:
    network = build_network()
    _, lane_odr_id = outermost_right_lane(network, "2", 90.0)
    print(f"routing along lane {lane_odr_id}")

    scenario = scenario_with_an_actor()
    stack = rm.osc.edit.ScenarioStack()

    # Place the actor, then give it a route. Two gestures, two commands — the
    # route is its own <PrivateAction> beside the teleport, which is the model
    # the reader would have produced from the same file.
    start = rm.osc.LanePosition()
    start.road_id = "1"
    start.lane_id = str(lane_odr_id)
    start.s = 10.0
    stack.push(scenario, rm.osc.edit.set_entity_init_pose(scenario, "Ego", start))

    route = rm.osc.Route()
    route.name = "EgoRoute"
    route.waypoints = [
        waypoint("1", lane_odr_id, 10.0),
        waypoint("2", lane_odr_id, 90.0),
    ]
    stack.push(scenario, rm.osc.edit.assign_route(scenario, "Ego", route))

    routed = rm.osc.write_xosc(scenario)
    print(f"scenario written: {len(routed)} bytes, "
          f"{len(rm.osc.assigned_routes(scenario))} route(s) assigned")

    # --- the route resolves against the network ------------------------------
    resolved = rm.osc.resolve_route(network, route)
    assert resolved.complete, [f.message for f in resolved.findings]
    print(f"resolved: {len(resolved.legs)} leg(s), "
          f"lanes {[leg.lane_odr_id for leg in resolved.legs]}")
    assert not rm.osc.validate_routes(network, scenario)

    # --- GW-6 step 7: move a road, and the route follows ---------------------
    #
    # Nothing about the route changes. That is the whole point: it names lanes,
    # so there is nothing in it that a translation could invalidate.
    map_stack = rm.edit.EditStack()
    map_stack.push(network, rm.edit.translate_road(network, network.find_road("2"), 0.0, 5.0))
    after_move = rm.osc.resolve_route(network, route)
    assert after_move.complete, "the route stopped resolving because a road moved"
    assert len(after_move.legs) == len(resolved.legs)
    print("step 7: the road moved and the route still resolves")

    # --- GW-6 step 8: delete a traversed lane, and the route is DIAGNOSED ----
    doomed, _ = outermost_right_lane(network, "2", 90.0)
    map_stack.push(network, rm.edit.remove_lane(network, doomed))

    broken = rm.osc.resolve_route(network, route)
    assert not broken.complete, "deleting a traversed lane left the route 'complete'"
    assert broken.findings, "the route broke in silence"
    print("step 8: the lane was deleted and the route is reported, not repaired:")
    for found in broken.findings:
        print(f"  [{found.severity}] {found.location}: {found.message}")

    # ★ THE ROUTE IS STILL IN THE DOCUMENT, unchanged. Nothing dropped it and
    # nothing re-routed it — the scenario says exactly what the user authored,
    # and the diagnostics say why it no longer works.
    still_there = rm.osc.assigned_routes(scenario)
    assert len(still_there) == 1
    assert len(still_there[0].route.waypoints) == 2
    assert rm.osc.write_xosc(scenario) == routed, "the scenario changed itself"
    print("the route is untouched: the network broke, not the document")

    # ...and validate_routes surfaces the same thing at document level, naming
    # the entity rather than only the route.
    findings = rm.osc.validate_routes(network, scenario)
    assert findings
    print(f"validate_routes: {findings[0].location}")

    # --- the undo contract, unchanged ----------------------------------------
    stack.undo(scenario)
    stack.undo(scenario)
    assert rm.osc.write_xosc(scenario) == rm.osc.write_xosc(scenario_with_an_actor()), (
        "undo did not restore the document byte-identically"
    )
    print("undo x2 restored the document byte-identically")

    with tempfile.TemporaryDirectory() as directory:
        path = Path(directory) / "routes.xosc"
        stack.redo(scenario)
        stack.redo(scenario)
        rm.osc.save_xosc(scenario, str(path))
        reloaded = rm.osc.parse_xosc(path.read_text(), str(path))
        assert rm.osc.write_xosc(reloaded.scenario) == routed, "the round trip lost the route"
        print("save -> reload -> write is byte-identical")


if __name__ == "__main__":
    main()
