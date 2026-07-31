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

"""Place actors on lanes of a road network, undoably (p8-s2, #246).

`scenario_signals.py` builds the traffic-signal half of a scenario from a live
network. This one builds the OTHER half — the actors — and it is what the
editor's Scenario mode pushes for every click, one command at a time.

Three ideas are worth watching for:

  1. ACTORS ANCHOR TO LANES, NOT TO THE WORLD. A `LanePosition` names the road
     and the lane by their OpenDRIVE `@id` STRINGS, so the actor follows its
     road through every later edit. A `WorldPosition` would be a snapshot of
     where the road happened to be when the actor was dropped — move the road
     and the actor stays behind, floating. That difference is GW-6 step 7.

  2. PLACING IS ONE COMMAND. `place_scenario_object` adds the entity AND places
     it in a single undoable step, because placing an actor is a single gesture.
     Two commands would need two Ctrl+Z for one click.

  3. THE UNDO CONTRACT IS UNCHANGED. Apply then undo leaves `write_xosc()`
     byte-identical, exactly as `rm.edit` does against `write_xodr`. The
     fingerprint check at the end is the whole reason this can be a
     golden-workflow replay rather than a demo.

Run:  python python/examples/scenario_actors.py
"""

from __future__ import annotations

import tempfile
from pathlib import Path

import roadmaker as rm


def build_network() -> tuple[rm.RoadNetwork, str]:
    """A single straight road, and the OpenDRIVE @id a scenario references it by."""
    network = rm.RoadNetwork()
    road = rm.author_clothoid_road(
        network,
        [(0.0, 0.0), (200.0, 0.0)],
        rm.LaneProfile.two_lane_default(),
        "",
        "1",
    )
    # The odr_id is the ONLY thing that may cross into the .xosc: a RoadId is a
    # generational arena handle, runtime-only and never valid across a load.
    return network, network.road(road).odr_id


def lane_position(road_odr_id: str, lane_odr_id: str, s: float) -> rm.osc.LanePosition:
    position = rm.osc.LanePosition()
    position.road_id = road_odr_id
    position.lane_id = lane_odr_id
    position.s = s
    position.offset = 0.0  # the lane centre, which is where an actor belongs
    return position


def main() -> None:
    network, road_odr_id = build_network()

    scenario = rm.osc.Scenario()
    stack = rm.osc.edit.ScenarioStack()

    # `Expected<void>` surfaces in Python as an exception, not as a return
    # value — so a failed push RAISES ValueError and a successful one returns
    # None. Checking the return value instead would silently accept every
    # refusal.
    def push(command) -> None:
        stack.push(scenario, command)

    # A <LanePosition> in a scenario that links no road network names a roadId
    # nothing can resolve, and write_xosc REFUSES it
    # (asam.net:xosc:1.0.0:scenario_logic.invalid_elements_if_no_road_network).
    # So the logic file comes first, always.
    push(rm.osc.edit.set_logic_file(scenario, "town.xodr"))

    # Two vehicles and a pedestrian, straight from the catalogue. make_actor
    # fills in <Performance> and <Axles> — required children of <Vehicle> in
    # every revision, and the two a caller assembling one by hand forgets.
    push(
        rm.osc.edit.place_scenario_object(
            scenario,
            rm.osc.make_actor(rm.osc.ActorKind.Car, "Car1"),
            lane_position(road_odr_id, "-1", 20.0),
        )
    )
    push(
        rm.osc.edit.place_scenario_object(
            scenario,
            rm.osc.make_actor(rm.osc.ActorKind.Truck, "Truck1"),
            lane_position(road_odr_id, "-1", 80.0),
        )
    )
    push(
        rm.osc.edit.place_scenario_object(
            scenario,
            rm.osc.make_actor(rm.osc.ActorKind.Pedestrian, "Pedestrian1"),
            lane_position(road_odr_id, "1", 50.0),
        )
    )

    # 50 km/h. Speeds are m/s in the model; mph and km/h are a display concern
    # and never reach the file.
    push(rm.osc.edit.set_entity_init_speed(scenario, "Car1", 13.89))
    push(rm.osc.edit.set_entity_init_speed(scenario, "Truck1", 11.0))

    text = rm.osc.write_xosc(scenario)
    print(f"--- {len(text.splitlines())} lines, {len(scenario.entities.scenario_objects)} actors")
    for line in text.splitlines():
        if "ScenarioObject name=" in line or "LanePosition" in line or "AbsoluteTargetSpeed" in line:
            print(line.strip())

    # --- the catalogue --------------------------------------------------------

    print("\n--- catalogue")
    for archetype in rm.osc.actor_catalog():
        print(
            f"  {archetype.label:<12} {archetype.category:<12} "
            f"{archetype.width:.2f} x {archetype.length:.2f} x {archetype.height:.2f} m"
        )

    # --- the undo contract ----------------------------------------------------

    print("\n--- undo x5 / redo x5 is a fixed point")
    before = rm.osc.write_xosc(scenario)
    for _ in range(5):
        stack.undo(scenario)
    for _ in range(5):
        stack.redo(scenario)
    assert rm.osc.write_xosc(scenario) == before, "undo/redo was not byte-identical"
    print("  ok")

    # Unwinding everything must reach the empty document exactly — a placement
    # that left an entity behind after its undo would show up here and nowhere
    # else.
    while stack.can_undo:
        stack.undo(scenario)
    assert rm.osc.write_xosc(scenario) == rm.osc.write_xosc(rm.osc.Scenario())
    print("  full unwind reaches an empty scenario")
    for _ in range(5):
        stack.redo(scenario)

    # --- refusals -------------------------------------------------------------

    print("\n--- refusals are commands, never None")
    before_refusals = rm.osc.write_xosc(scenario)

    def expect_refused(command, what: str) -> None:
        # A refusal is a Command the caller can push and read the error from —
        # never None, which a caller would have to remember to check for.
        assert command is not None, "a factory returned None instead of a refusing command"
        try:
            stack.push(scenario, command)
        except ValueError as error:
            print(f"  {what}: {error}")
        else:
            raise AssertionError(f"{what} was accepted")
        assert rm.osc.write_xosc(scenario) == before_refusals, "a refusal half-mutated the document"

    expect_refused(
        rm.osc.edit.set_entity_init_pose(scenario, "Car1", lane_position("", "-1", 10.0)),
        "a lane position naming no road is refused at PLACEMENT, not at save",
    )
    expect_refused(
        rm.osc.edit.set_entity_init_speed(scenario, "Car1", -5.0),
        "a negative speed is refused, never clamped to 0",
    )
    expect_refused(
        rm.osc.edit.place_scenario_object(
            scenario,
            rm.osc.make_actor(rm.osc.ActorKind.Car, "Car1"),
            lane_position(road_odr_id, "-1", 5.0),
        ),
        "a duplicate @name is refused — it is the key every entityRef resolves through",
    )

    # --- and it survives a file ----------------------------------------------

    with tempfile.TemporaryDirectory() as tmp:
        path = Path(tmp) / "actors.xosc"
        rm.osc.save_xosc(scenario, path)
        parsed = rm.osc.parse_xosc(path.read_text())
        assert rm.osc.write_xosc(parsed.scenario) == rm.osc.write_xosc(scenario)
        print("\n--- write -> read -> write is byte-identical")


if __name__ == "__main__":
    main()
