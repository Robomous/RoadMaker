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
EGO_START = (-40.0, -1.75, 0.0)


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

    car = rm.osc.Vehicle()
    car.name = "car"
    box = rm.osc.BoundingBox()
    box.center_x, box.center_z = 1.4, 0.75
    box.width, box.length, box.height = 2.0, 5.0, 1.5
    car.bounding_box = box

    ego = rm.osc.ScenarioObject()
    ego.name = "Ego"
    ego.entity_object = car
    stack.push(scenario, rm.osc.edit.add_scenario_object(scenario, ego))

    position = rm.osc.WorldPosition()
    position.x, position.y, position.z = EGO_START
    position.h = 0.0
    stack.push(scenario, rm.osc.edit.set_entity_init_position(scenario, "Ego", position))

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


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--out",
        type=Path,
        default=Path(__file__).resolve().parent.parent / "tests" / "esmini",
        help="directory the pair is written to (default: tests/esmini)",
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
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
