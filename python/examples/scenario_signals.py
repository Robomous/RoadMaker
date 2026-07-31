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

"""Author a scenario ON TOP OF a signalized junction, undoably (p8-s1, #245).

`scenario_write.py` builds a `.xosc` by filling in the model; `scenario_roundtrip.py`
reads one back. This one is the third thing a scenario has to be able to do, and
the one GW-6 is written to accept: build the scenario FROM A ROAD NETWORK, through
undoable kernel commands, so the whole thing can be replayed headlessly.

Two ideas are worth watching for:

  1. THE DECOMPOSITION. RoadMaker stores a signal cycle per JUNCTION, across N
     controllers. OpenSCENARIO has one `TrafficSignalController` per OpenDRIVE
     `<controller>`, each with its own phase list. `sync_traffic_signals` is the
     translation, and it reads the RED-FILLED plan — so a signal the editor shows
     as red is red in the file too, rather than absent (ADR-0014 §8).

  2. THE UNDO CONTRACT. `rm.osc.edit` mirrors `rm.edit`: apply then undo leaves
     `write_xosc()` byte-identical. The fingerprint loop at the end is the same
     check both golden-workflow replays make against `write_xodr`.

Run:  python python/examples/scenario_signals.py
"""

from __future__ import annotations

import tempfile
from pathlib import Path

import roadmaker as rm


def signalized_network() -> tuple[rm.RoadNetwork, rm.JunctionId]:
    """A four-arm crossing with a two-phase signal group."""
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
    return network, junction


def main() -> int:
    network, junction = signalized_network()

    # --- 1. what the decomposition sees, before any scenario exists ----------
    #
    # A pure read: no scenario is involved yet. This is what the editor's
    # Scenario mode will show, and what p8-s4's traffic-light condition editor
    # offers as the phase list.
    decomposed = rm.osc.decompose_junction_signals(network, junction)
    print(f"the junction decomposes into {len(decomposed.controllers)} controller(s):")
    for controller in decomposed.controllers:
        # @name is the OpenDRIVE controller @id, not a label — §10.10. Emitting
        # a friendly name would produce a file that references nothing.
        print(f"  TrafficSignalController name={controller.name!r}")
        for phase in controller.phases:
            states = ", ".join(
                f"{s.traffic_signal_id}={s.state}" for s in phase.signal_states
            )
            print(f"    phase {phase.name!r} for {phase.duration}s: {states}")
    for finding in decomposed.findings:
        print(f"  note: {finding.message}")

    # --- 2. author the scenario, one undoable command at a time --------------
    scenario = rm.osc.Scenario()
    stack = rm.osc.edit.ScenarioStack()

    stack.push(scenario, rm.osc.edit.set_logic_file(scenario, "crossing.xodr"))
    stack.push(scenario, rm.osc.edit.sync_traffic_signals(scenario, network, junction))

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

    start = rm.osc.WorldPosition()
    start.x, start.y = -40.0, -1.75
    start.h = 0.0
    stack.push(scenario, rm.osc.edit.set_entity_init_position(scenario, "Ego", start))

    print(f"\n{stack.size} commands on the stack, can_undo={stack.can_undo}")

    # --- 3. a refusal is a Command, not None ---------------------------------
    #
    # Every factory returns something pushable. A caller never has to check for
    # None, and the reason for the refusal arrives as an exception from push().
    duplicate = rm.osc.edit.add_scenario_object(scenario, ego)
    assert duplicate is not None, "a factory must never return None"
    try:
        stack.push(scenario, duplicate)
        raise AssertionError("the duplicate name should have been refused")
    except ValueError as refusal:
        print(f"refused, as it should be: {refusal}")
    assert stack.size == 4, "a refused command must not be recorded"

    # --- 4. the byte-identity contract ---------------------------------------
    full = rm.osc.write_xosc(scenario)
    for _ in range(10):
        while stack.can_undo:
            stack.undo(scenario)
        while stack.can_redo:
            stack.redo(scenario)
    assert rm.osc.write_xosc(scenario) == full, "undo x10 / redo x10 changed the document"
    print("undo x10 / redo x10: the document is byte-identical")

    # Undoing to the bottom really does empty it — the fixed point above is not
    # just "nothing ever happened".
    empty = rm.osc.Scenario()
    while stack.can_undo:
        stack.undo(scenario)
    assert rm.osc.write_xosc(scenario) == rm.osc.write_xosc(empty)
    while stack.can_redo:
        stack.redo(scenario)
    print("undone to the bottom: back to an empty scenario, and redone again")

    # --- 5. write it out ------------------------------------------------------
    findings = rm.osc.validate_scenario(scenario)
    assert not findings, [f.message for f in findings]

    with tempfile.TemporaryDirectory() as tmp:
        out = Path(tmp)
        rm.save_xodr(network, out / "crossing.xodr")
        rm.osc.save_xosc(scenario, out / "crossing.xosc")
        written = (out / "crossing.xosc").read_text(encoding="utf-8")
        print(f"\nwrote {len(written)} bytes of OpenSCENARIO 1.2 beside crossing.xodr")

        # And it reads back as the same document — the fixed point p8-s1 claims.
        reparsed = rm.osc.load_xosc(out / "crossing.xosc")
        assert rm.osc.write_xosc(reparsed.scenario) == written
        print("re-read and re-written: byte-identical")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
