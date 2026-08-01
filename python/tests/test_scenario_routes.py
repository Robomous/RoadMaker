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

"""Lane-anchored routes from Python (p8-s3, #247).

★ THIS FILE IS GW-6 STEP 8'S HEADLESS EVIDENCE, and that is why it exists here
rather than only in C++. The maintainer ruling on #257 makes a golden workflow's
proof a REPLAY, and `python/CMakeLists.txt` links `roadmaker::core` alone — so a
behaviour that cannot be driven from Python has no evidence at all.

The two steps being proven:

  step 7  move a road the route runs along, and the route still resolves;
  step 8  delete a lane the route traverses, and the route is REPORTED as
          invalidated — never silently dropped, never silently re-routed.
"""

from __future__ import annotations

import pathlib
import subprocess
import sys

import pytest

import roadmaker as rm


def two_linked_roads() -> rm.RoadNetwork:
    """Two straight roads end to end and LINKED, so a lane runs through both."""
    network = rm.RoadNetwork()
    first = rm.author_clothoid_road(
        network, [(0.0, 0.0), (100.0, 0.0)], rm.LaneProfile.two_lane_default(), "", "1"
    )
    stack = rm.edit.EditStack()
    stack.push(
        network,
        rm.edit.create_linked_road(
            network,
            [(100.0, 0.0), (200.0, 0.0)],
            rm.LaneProfile.two_lane_default(),
            "second",
            rm.RoadEnd(first, rm.ContactPoint.END),
        ),
    )
    return network


def outermost_right_lane(network: rm.RoadNetwork, road_odr_id: str, s: float):
    """The outermost right-hand lane — the only one `remove_lane` will delete."""
    road = network.find_road(road_odr_id)
    section = network.lane_section(network.section_at(road, s))
    best, best_id = None, 0
    for lane_id in section.lanes:
        lane = network.lane(lane_id)
        if lane.odr_id < best_id:
            best, best_id = lane_id, lane.odr_id
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


def routed_scenario(lane_odr_id: int) -> tuple[rm.osc.Scenario, rm.osc.Route]:
    """A car with a two-waypoint route, built entirely through commands.

    ★ Commands, NOT member assignment. `def_rw` on a vector hands Python a COPY,
    so `scenario.entities.scenario_objects.append(...)` mutates a temporary and
    leaves the scenario empty — silently, surfacing much later as "no entity
    named 'Ego'". The command layer has no such trap.
    """
    scenario = rm.osc.Scenario()
    stack = rm.osc.edit.ScenarioStack()
    stack.push(scenario, rm.osc.edit.set_logic_file(scenario, "town.xodr"))
    stack.push(
        scenario,
        rm.osc.edit.add_scenario_object(scenario, rm.osc.make_actor(rm.osc.ActorKind.Car, "Ego")),
    )
    route = rm.osc.Route()
    route.name = "EgoRoute"
    route.waypoints = [waypoint("1", lane_odr_id, 10.0), waypoint("2", lane_odr_id, 90.0)]
    stack.push(scenario, rm.osc.edit.assign_route(scenario, "Ego", route))
    return scenario, route


@pytest.fixture
def scene():
    network = two_linked_roads()
    _, lane_odr_id = outermost_right_lane(network, "2", 90.0)
    scenario, route = routed_scenario(lane_odr_id)
    return network, scenario, route, lane_odr_id


def test_a_route_across_linked_roads_resolves(scene):
    network, scenario, route, lane_odr_id = scene
    resolved = rm.osc.resolve_route(network, route)
    assert resolved.complete, [f.message for f in resolved.findings]
    assert resolved.findings == []
    # The path went THROUGH the link rather than jumping between the endpoints.
    assert len(resolved.legs) >= 2
    assert [leg.lane_odr_id for leg in resolved.legs] == [lane_odr_id] * len(resolved.legs)
    assert resolved.legs[0].s_start == pytest.approx(10.0)
    assert resolved.legs[-1].s_end == pytest.approx(90.0)
    assert rm.osc.validate_routes(network, scenario) == []


def test_moving_a_road_leaves_the_route_resolvable(scene):
    """GW-6 step 7 — the step a world-positioned polyline would fail."""
    network, scenario, route, _ = scene
    before = rm.osc.resolve_route(network, route)
    assert before.complete

    stack = rm.edit.EditStack()
    stack.push(network, rm.edit.translate_road(network, network.find_road("2"), 0.0, 5.0))

    after = rm.osc.resolve_route(network, route)
    assert after.complete, "the route stopped resolving because a road moved"
    assert len(after.legs) == len(before.legs)
    assert rm.osc.validate_routes(network, scenario) == []


def test_deleting_a_traversed_lane_is_reported_not_repaired(scene):
    """GW-6 step 8, and the three things it actually asserts."""
    network, scenario, route, _ = scene
    before_bytes = rm.osc.write_xosc(scenario)

    doomed, _ = outermost_right_lane(network, "2", 90.0)
    stack = rm.edit.EditStack()
    stack.push(network, rm.edit.remove_lane(network, doomed))

    resolved = rm.osc.resolve_route(network, route)

    # 1. It is reported, and the finding names the waypoint.
    assert not resolved.complete
    assert resolved.findings, "the route broke in silence"
    assert "Waypoint[1]" in resolved.findings[0].location
    assert resolved.findings[0].severity == rm.Severity.ERROR

    # 2. It is NOT silently deleted — the document still says what was authored.
    assigned = rm.osc.assigned_routes(scenario)
    assert len(assigned) == 1
    assert len(assigned[0].route.waypoints) == 2

    # 3. It is NOT silently re-routed — the scenario's bytes are unchanged. The
    #    network broke; the document did not.
    assert rm.osc.write_xosc(scenario) == before_bytes


def test_validate_routes_names_the_entity(scene):
    _, scenario, _, _ = scene
    findings = rm.osc.validate_routes(rm.RoadNetwork(), scenario)
    assert findings
    # The entity is what a user recognises; the route name alone leaves them
    # hunting for whose route it was.
    assert "Entity[Ego]" in findings[0].location
    assert "EgoRoute" in findings[0].location


def test_an_undo_restores_the_document_byte_identically(scene):
    network, _, route, lane_odr_id = scene
    scenario = rm.osc.Scenario()
    stack = rm.osc.edit.ScenarioStack()
    stack.push(scenario, rm.osc.edit.set_logic_file(scenario, "town.xodr"))
    stack.push(
        scenario,
        rm.osc.edit.add_scenario_object(scenario, rm.osc.make_actor(rm.osc.ActorKind.Car, "Ego")),
    )
    before = rm.osc.write_xosc(scenario)

    stack.push(scenario, rm.osc.edit.assign_route(scenario, "Ego", route))
    routed = rm.osc.write_xosc(scenario)
    assert routed != before

    stack.undo(scenario)
    assert rm.osc.write_xosc(scenario) == before
    stack.redo(scenario)
    assert rm.osc.write_xosc(scenario) == routed


def test_removing_a_waypoint_from_a_two_waypoint_route_is_refused(scene):
    """A deletion that made the document unsavable would be worse than none."""
    _, scenario, _, _ = scene
    before = rm.osc.write_xosc(scenario)
    stack = rm.osc.edit.ScenarioStack()

    with pytest.raises(ValueError, match="at least two"):
        stack.push(scenario, rm.osc.edit.remove_route_waypoint(scenario, "Ego", 0))

    assert rm.osc.write_xosc(scenario) == before
    assert stack.size == 0


def test_a_route_survives_a_save_reload_write_byte_identically(scene, tmp_path):
    _, scenario, _, _ = scene
    written = rm.osc.write_xosc(scenario)
    path = tmp_path / "routes.xosc"
    rm.osc.save_xosc(scenario, str(path))

    reloaded = rm.osc.parse_xosc(path.read_text(), str(path))
    assert rm.osc.write_xosc(reloaded.scenario) == written
    assert len(rm.osc.assigned_routes(reloaded.scenario)) == 1


def test_the_example_script_runs_clean():
    """python/examples/scenario_routes.py is this sprint's hand script.

    An example nobody runs is wrong in ways review does not catch — this one was
    wrong three times before it ran (an enum spelling, a free function that is a
    method, and the `def_rw`-returns-a-copy trap).
    """
    example = pathlib.Path(__file__).resolve().parents[1] / "examples" / "scenario_routes.py"
    result = subprocess.run(
        [sys.executable, str(example)], capture_output=True, text=True, check=False
    )
    assert result.returncode == 0, result.stdout + result.stderr
    assert "step 7: the road moved and the route still resolves" in result.stdout
    assert "step 8: the lane was deleted and the route is reported, not repaired" in result.stdout
    assert "the route is untouched: the network broke, not the document" in result.stdout
