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

"""One scenario, both OpenSCENARIO standards (p8-s6, issue #327).

★ THE POINT OF THIS SCRIPT IS THE DIFFERENCE BETWEEN THE TWO FILES, not either
one on its own. The same internal model emits:

* an **OpenSCENARIO XML 1.2** `.xosc` — complete: actors, placements, routes,
  the storyboard, the traffic-signal controllers;
* an **OpenSCENARIO DSL v2.2.0** `.osc` — a documented **concrete-scenario
  subset**, which is deliberately much smaller.

Everything the 2.x subset cannot express is REPORTED rather than silently
omitted, and this script prints those reports. That is the acceptance: a reader
can see exactly what the second file does not carry, without diffing it against
the first.

Export-only: there is no `.osc` reader. The round trip is via the internal model
and the project container, never via re-importing 2.x.

Run:  python python/examples/scenario_osc2_export.py [--out <dir>]
"""

from __future__ import annotations

import argparse
from pathlib import Path

import roadmaker as rm

START_SPEED = 13.89  # 50 km/h in m/s — the kernel frame's unit, and the DSL's `mps`


def build_scene() -> tuple[rm.RoadNetwork, rm.osc.Scenario]:
    """A signalized crossing, two actors, a route and a storyboard."""
    network = rm.RoadNetwork()
    net_stack = rm.edit.EditStack()

    params = rm.edit.assembly.IntersectionParams()
    params.gap_m = 24.0
    params.arm_length_m = 80.0
    params.profile = rm.LaneProfile.arterial()
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

    header = scenario.header
    header.description = "RoadMaker dual-export example"
    scenario.header = header

    stack.push(scenario, rm.osc.edit.set_logic_file(scenario, "crossing.xodr"))
    stack.push(scenario, rm.osc.edit.sync_traffic_signals(scenario, network, junction))

    approach = network.road(network.road_ids[0]).odr_id

    def place(name: str, lane: str, s: float) -> None:
        position = rm.osc.LanePosition()
        position.road_id = approach
        position.lane_id = lane
        position.s = s
        position.offset = 0.0
        stack.push(
            scenario,
            rm.osc.edit.place_scenario_object(
                scenario, rm.osc.make_actor(rm.osc.ActorKind.Car, name), position
            ),
        )
        stack.push(scenario, rm.osc.edit.set_entity_init_speed(scenario, name, START_SPEED))

    place("Ego", "-1", 20.0)
    place("Target", "-2", 10.0)

    # A storyboard: the half the 2.x subset does not carry.
    change = rm.osc.LaneChangeAction()
    dynamics = change.dynamics
    dynamics.dynamics_shape = "linear"
    dynamics.dynamics_dimension = "time"
    dynamics.value = 2.0
    change.dynamics = dynamics
    target_lane = rm.osc.RelativeTargetLane()
    target_lane.entity_ref = "Target"
    target_lane.value = 1
    change.target = target_lane
    lateral = rm.osc.LateralAction()
    lateral.lane_change = change
    private = rm.osc.PrivateAction()
    private.lateral = lateral
    action = rm.osc.Action()
    action.name = "cut_in"
    action.action = private

    event = rm.osc.Event()
    event.name = "cut_in_event"
    event.actions = [action]
    maneuver = rm.osc.StoryManeuver()
    maneuver.name = "cut_in_maneuver"
    maneuver.events = [event]
    actor = rm.osc.EntityRef()
    actor.entity_ref = "Target"
    group = rm.osc.ManeuverGroup()
    group.name = "target_group"
    group.actors = [actor]
    group.maneuvers = [maneuver]
    act = rm.osc.Act()
    act.name = "act"
    act.maneuver_groups = [group]
    story = rm.osc.Story()
    story.name = "cut_in_story"
    story.acts = [act]
    stack.push(scenario, rm.osc.edit.set_story(scenario, 0, story))

    return network, scenario


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", type=Path, default=Path("."), help="output directory")
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)

    network, scenario = build_scene()

    xodr = args.out / "crossing.xodr"
    rm.save_xodr(network, xodr)

    # --- 1.x, the complete file -------------------------------------------
    findings = rm.osc.validate_scenario(scenario)
    cross = rm.osc.validate_scenario_against_network(scenario, network)
    assert not findings, [f.message for f in findings]
    assert not cross, [f.message for f in cross]

    xosc = args.out / "crossing.xosc"
    rm.osc.save_xosc(scenario, xosc)

    # --- 2.x, the documented subset ---------------------------------------
    osc2 = args.out / "crossing.osc"
    rm.osc.save_osc2(scenario, osc2, "cut_in")

    print(f"wrote {xodr}")
    print(f"wrote {xosc}  (OpenSCENARIO XML 1.2 — complete)")
    print(f"wrote {osc2}  (OpenSCENARIO DSL {rm.osc.OSC2_VERSION} — concrete subset)")

    print("\nthe 2.x subset emits:")
    for row in rm.osc.osc2_supported():
        print(f"  {row.construct:36s} <- {row.source}")

    # ★ THE ACCEPTANCE: what the second file does not carry, said out loud.
    dropped = rm.osc.validate_osc2_subset(scenario)
    print(f"\nnot carried into the 2.x file ({len(dropped)} report(s)):")
    for finding in dropped:
        print(f"  {finding.location}")
        print(f"    {finding.message}")
    if not dropped:
        print("  (nothing — this scenario fits the subset entirely)")

    print("\n--- crossing.osc ---")
    print(osc2.read_text(), end="")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
