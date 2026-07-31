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
specification explicitly leaves open (§10.10). Both are unproven until esmini
loads the file.

THE DECOMPOSITION HERE IS DELIBERATELY A SCRIPT, NOT KERNEL CODE. Turning one
junction timeline into one `TrafficSignalController` per OpenDRIVE `<controller>`
is p8-s1 PR-D's `edit::` factory; doing it once here to produce a fixture is not
the same commitment. When PR-D lands, this script re-points at that factory and
the fixture must not change — which is itself a test of the factory.

The tracked output is pinned by XoscFixture.TheTrackedScenarioIsExactlyWhatThe
WriterEmitsToday (core/tests/test_xosc_reader.cpp), so regenerating is a
deliberate act with a red test in front of it, never a silent drift.

Run:  python scripts/gen_xosc_fixtures.py [--out tests/esmini]
"""

from __future__ import annotations

import argparse
from pathlib import Path

import roadmaker as rm

# The SignalState -> @state spelling, and the one genuinely open choice here.
#
# ★ MEASURED, 2026-07-30, AND THE MEASUREMENT IS NOT WHAT WAS EXPECTED: esmini
# v3.5.0 does not validate this token AT ALL. Running the fixture with
# state="wibble", with a trafficSignalId naming no <signal>, with a
# trafficSignalControllerRef naming no controller, and with a nonexistent phase
# name each produced a byte-identical log and exit 0 — including when a
# TrafficSignalControllerAction in a story genuinely executed against the
# controller (the log shows "ev complete after 1 execution", so the probe was
# not merely failing to run).
#
# So the engine CI pins has NO OPINION, and the choice cannot be settled by
# asking it. It rests instead on RoadMaker's own semantics — SignalState is a
# colour enum and these are its colours, which is also what esmini's own example
# scenarios use. The specification leaves the token open (§10.10: a whole-box
# signal may carry a composite "on;off;off", a per-bulb one "on"/"off"), so
# there is no normative answer to look up either. The table stays one dictionary
# wide precisely so reversing the choice costs one edit, and it is recorded as
# UNVALIDATED rather than proven.
STATE_TOKEN = {
    rm.SignalState.RED: "red",
    rm.SignalState.YELLOW: "yellow",
    rm.SignalState.GREEN: "green",
    rm.SignalState.OFF: "off",
}

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


def signals_by_controller(network: rm.RoadNetwork) -> dict[str, list[str]]:
    """OpenDRIVE controller @id -> the signal @ids its <control> children name."""
    return {
        network.controller(cid).odr_id: [
            control.signal_odr_id for control in network.controller(cid).controls
        ]
        for cid in network.controller_ids
    }


def decompose(network: rm.RoadNetwork, junction: rm.JunctionId) -> list:
    """One junction timeline -> one TrafficSignalController per OpenDRIVE <controller>.

    Read the PLAN, never Junction.phases: RoadMaker stores phase states sparsely
    and Red by omission, and junction_phases() is what Red-fills them into the
    dense list OpenSCENARIO requires. A phase stored as "no state" would
    otherwise export as a signal that is never red.
    """
    plan = rm.junction_phases(network, junction)
    assert plan.phases, "the junction has no cycle to export — signalize it first"

    controls = signals_by_controller(network)
    # Sorted by @id at the point of construction, which is the ordering contract
    # osc/scenario.hpp states for RoadNetworkRef: for_each_controller walks arena
    # SLOTS, and a freed slot is reused, so arena order is not creation order.
    controllers = []
    for controller_odr_id in sorted(controls):
        owned = set(controls[controller_odr_id])
        out = rm.osc.TrafficSignalController()
        out.name = controller_odr_id  # the OpenDRIVE @id, per §10.10

        phases = []
        for info in plan.phases:
            phase = rm.osc.Phase()
            phase.name = info.name
            phase.duration = info.duration
            states = []
            for entry in info.signal_states:
                signal = network.signal(entry.signal)
                if signal.odr_id not in owned:
                    continue
                state = rm.osc.TrafficSignalState()
                state.traffic_signal_id = signal.odr_id
                state.state = STATE_TOKEN[entry.state]
                states.append(state)
            # Assign whole lists: def_rw on a vector hands back a COPY, so
            # phase.signal_states.append(...) is silently lost.
            phase.signal_states = sorted(states, key=lambda s: s.traffic_signal_id)
            phases.append(phase)
        out.phases = phases
        controllers.append(out)
    return controllers


def build_scenario(network: rm.RoadNetwork, junction: rm.JunctionId, xodr_name: str) -> rm.osc.Scenario:
    scenario = rm.osc.Scenario()

    header = scenario.header
    header.description = "RoadMaker signalized-junction fixture"
    scenario.header = header

    logic = rm.osc.FileRef()
    logic.filepath = xodr_name  # relative: esmini resolves it against the .xosc
    road_network = scenario.road_network
    road_network.logic_file = logic
    road_network.traffic_signal_controllers = decompose(network, junction)
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

    teleport = rm.osc.TeleportAction()
    position = rm.osc.WorldPosition()
    position.x, position.y, position.z = EGO_START
    position.h = 0.0
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
    timing = rm.osc.SimulationTimeCondition()
    timing.value = 0.5
    timing.rule = "greaterThan"
    end.simulation_time = timing
    group = rm.osc.ConditionGroup()
    group.conditions = [end]
    stop = storyboard.stop_trigger
    stop.condition_groups = [group]
    storyboard.stop_trigger = stop
    scenario.storyboard = storyboard

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
