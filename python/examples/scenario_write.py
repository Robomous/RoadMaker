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

"""Write an OpenSCENARIO scenario beside a road network (p8-s1, issue #245).

A `.xosc` is a SECOND Layer-0 file, not a sidecar: it sits beside its `.xodr`
at the top level of the project, stem-matched, and stays standalone-openable in
any tool that reads OpenSCENARIO.

This example authors a small signalized network, exports it, and writes a
scenario that references it — one vehicle teleported onto the road and a
traffic-signal controller whose phases carry the junction's timing.

    python python/examples/scenario_write.py

Note on list members: nanobind's `def_rw` on a std::vector hands back a COPY,
so `scenario.entities.scenario_objects.append(x)` mutates a temporary and is
silently lost. Build a Python list and assign it back wholesale, as below.
"""

from __future__ import annotations

import tempfile
from pathlib import Path

import roadmaker as rm


def build_scenario(logic_file: str) -> rm.osc.Scenario:
    """A minimal scenario: one ego vehicle and one signal controller."""
    scenario = rm.osc.Scenario()
    scenario.header.description = "RoadMaker scenario example"

    road_network = scenario.road_network
    logic = rm.osc.FileRef()
    logic.filepath = logic_file
    road_network.logic_file = logic

    # One TrafficSignalController per OpenDRIVE <controller>, named by that
    # controller's @id — the specification references it by exactly that id,
    # never by a human-readable label.
    controller = rm.osc.TrafficSignalController()
    controller.name = "1"

    phases = []
    for name, duration, semantics, states in (
        ("go", 20.0, rm.osc.PhaseSemantics.Go, [("1", "green")]),
        ("clear", 3.0, rm.osc.PhaseSemantics.AttentionStop, [("1", "yellow")]),
        ("stop", 23.0, rm.osc.PhaseSemantics.Stop, [("1", "red")]),
    ):
        phase = rm.osc.Phase()
        phase.name = name
        phase.duration = duration
        phase.semantics = semantics
        signal_states = []
        for signal_id, state in states:
            signal_state = rm.osc.TrafficSignalState()
            signal_state.traffic_signal_id = signal_id
            signal_state.state = state
            signal_states.append(signal_state)
        # Assigned wholesale — appending to phase.signal_states would be lost.
        phase.signal_states = signal_states
        phases.append(phase)
    controller.phases = phases
    road_network.traffic_signal_controllers = [controller]
    scenario.road_network = road_network

    car = rm.osc.Vehicle()
    car.name = "car"
    box = rm.osc.BoundingBox()
    box.center_x, box.center_z = 1.4, 0.75
    box.width, box.length, box.height = 2.0, 5.0, 1.5
    car.bounding_box = box

    ego = rm.osc.ScenarioObject()
    ego.name = "Ego"
    ego.entity_object = car
    entities = scenario.entities
    entities.scenario_objects = [ego]
    scenario.entities = entities

    position = rm.osc.WorldPosition()
    position.x, position.y, position.h = 10.0, 0.0, 0.0
    teleport = rm.osc.TeleportAction()
    teleport.position = position
    action = rm.osc.PrivateAction()
    action.teleport = teleport

    ego_init = rm.osc.Private()
    ego_init.entity_ref = "Ego"
    ego_init.actions = [action]

    storyboard = scenario.storyboard
    init = storyboard.init
    actions = init.actions
    actions.privates = [ego_init]
    init.actions = actions
    storyboard.init = init

    end = rm.osc.Condition()
    end.name = "end"
    simulation_time = rm.osc.SimulationTimeCondition()
    simulation_time.value = 10.0
    end.simulation_time = simulation_time
    group = rm.osc.ConditionGroup()
    group.conditions = [end]
    stop_trigger = rm.osc.Trigger()
    stop_trigger.condition_groups = [group]
    storyboard.stop_trigger = stop_trigger
    scenario.storyboard = storyboard

    return scenario


def main() -> None:
    network = rm.RoadNetwork()
    rm.author_clothoid_road(
        network,
        [(0.0, 0.0), (120.0, 0.0)],
        rm.LaneProfile.two_lane_rural(),
        name="Main Street",
    )

    out = Path(tempfile.mkdtemp(prefix="rm_scenario_"))
    scene = out / "town.xodr"
    rm.save_xodr(network, scene)

    # Stem-matched, beside the .xodr, at the top level of the project.
    scenario = build_scenario(scene.name)

    findings = rm.osc.validate_scenario(scenario)
    print(f"validate_scenario: {len(findings)} finding(s)")
    for finding in findings:
        rule = finding.rule_id or "(no normative rule)"
        print(f"  {finding.location}: {finding.message} [{rule}]")

    text = rm.osc.write_xosc(scenario)
    path = scene.with_suffix(".xosc")
    rm.osc.save_xosc(scenario, path)
    print(f"wrote {path} ({len(text)} bytes)")

    # Deterministic: the same scenario always writes the same bytes, which is
    # what lets a scenario take part in the undo/redo fingerprinting the
    # golden-workflow replays use.
    assert rm.osc.write_xosc(scenario) == text, "write_xosc is not deterministic"

    # 1.2 is the default because validation means 'esmini accepts the file'.
    # 1.4 adds exactly one thing: Phase/@semantics.
    at_1_4 = rm.osc.write_xosc(scenario, rm.osc.OscVersion.V1_4)
    assert 'revMinor="2"' in text and "semantics=" not in text
    assert 'revMinor="4"' in at_1_4 and 'semantics="go"' in at_1_4
    print("revision targeting: 1.2 omits @semantics, 1.4 emits it")

    # A refusal is an exception, not a silently wrong file. An empty
    # trafficSignalId would produce a scenario that looks right and references
    # nothing.
    broken = build_scenario(scene.name)
    controller = broken.road_network.traffic_signal_controllers[0]
    phase = controller.phases[0]
    states = phase.signal_states
    states[0].traffic_signal_id = ""
    phase.signal_states = states
    controller.phases = [phase] + list(controller.phases)[1:]
    road_network = broken.road_network
    road_network.traffic_signal_controllers = [controller]
    broken.road_network = road_network
    try:
        rm.osc.write_xosc(broken)
        raise AssertionError("expected a refusal for an empty trafficSignalId")
    except ValueError as exc:
        print(f"refused as expected: {exc}")

    print(text[: text.index("<Entities")])


if __name__ == "__main__":
    main()
