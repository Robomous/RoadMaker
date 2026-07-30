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

"""Place a composite prop assembly, move it as one unit, and define your own.

An assembly (p6-s9, #323) is several prop models pinned together by fixed
relative transforms — the bundled `signal_mast` is a pole, a horizontal arm and
two signal heads. It places as one unit, moves as one unit, and deletes as one
unit, while each part stays an ordinary OpenDRIVE ``<object>`` that any reader
can consume; the grouping travels in a ``rm:assembly`` userData record.

Not to be confused with ``rm.edit.assembly``, which builds T/X road junctions.

Usage:
    python prop_assembly.py output.xodr
"""

import pathlib
import sys

import roadmaker as rm


def main() -> int:
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    xodr_path = sys.argv[1]

    network = rm.RoadNetwork()
    road_id = rm.author_clothoid_road(
        network,
        [(0.0, 0.0), (200.0, 0.0)],
        rm.LaneProfile.urban_sidewalk(),
        name="Assembly Demo Street",
    )
    stack = rm.edit.EditStack()

    # --- the bundled assembly ------------------------------------------------
    mast = rm.props.assembly("signal_mast")
    print(f"{mast.label!r} has {len(mast.parts)} parts:")
    for index, part in enumerate(mast.parts):
        print(
            f"  {index}: {part.model:12s} du={part.du:5.2f} dv={part.dv:5.2f} "
            f"dz={part.dz:5.2f} dyaw={part.dyaw:5.2f}"
        )

    # One command, one undo entry, however many parts.
    stack.push(network, rm.edit.place_assembly(network, road_id, 60.0, -6.0, 0.0, "signal_mast"))
    print(f"placed: {network.object_count} objects")

    # Any part is a handle on the whole unit.
    parts = network.assembly_parts(network.objects_of(road_id)[0])
    anchor = network.object(parts[0])
    print(f"anchor at s={anchor.s:.1f} t={anchor.t:.1f}, instance {anchor.assembly.instance!r}")

    # --- moving it moves every part -----------------------------------------
    stack.push(network, rm.edit.move_assembly(network, parts[2], 140.0, -6.0))
    moved = network.object(parts[0])
    print(f"after move_assembly, the anchor is at s={moved.s:.1f} (grabbed part 2, not part 0)")

    # A single part cannot be dragged out of formation: its pose is derived from
    # the anchor, so the command refuses rather than let the record diverge.
    refused = rm.edit.move_object(network, parts[2], 10.0, -6.0)
    try:
        stack.push(network, refused)
        print("!! move_object should have refused an assembly part")
        return 1
    except ValueError as error:
        print(f"move_object on a part refused, as designed: {error}")

    # Breaking the part out is the explicit way to do it.
    stack.push(network, rm.edit.detach_assembly_part(network, parts[2]))
    stack.push(network, rm.edit.move_object(network, parts[2], 10.0, -6.0))
    print(f"detached part now stands alone at s={network.object(parts[2]).s:.1f}")

    # --- your own definition ------------------------------------------------
    # A project registers its assemblies exactly the way it registers imported
    # prop models: one call, replacing any previous set.
    def oak(offset: float) -> rm.props.AssemblyPart:
        part = rm.props.AssemblyPart()
        part.model = "tree_oak"
        part.dv = offset
        part.scale = 0.8
        return part

    grove = rm.props.PropAssembly()
    grove.id = "my_grove"
    grove.label = "Three oaks"
    # ASSIGN the list, do not append to `grove.parts` in place: reading a bound
    # std::vector member hands back a fresh Python list, so `.append()` would
    # mutate a copy and leave the assembly empty.
    grove.parts = [oak(-6.0), oak(0.0), oak(6.0)]
    rm.props.register_project_assemblies([grove])
    stack.push(network, rm.edit.place_assembly(network, road_id, 100.0, 10.0, 0.0, "my_grove"))
    print(f"my_grove placed: {network.object_count} objects total")

    # --- undo/redo, then write ----------------------------------------------
    stack.undo(network)
    print(f"undo removed the grove in one step: {network.object_count} objects")
    stack.redo(network)

    # An assembly's parts interpenetrate on purpose — the arm bolts into the
    # pole — so the obstruction checker exempts one placement from itself (R6).
    diagnostics = rm.validate_network(network)
    print(f"validate_network: {len(diagnostics)} findings")
    for diagnostic in diagnostics:
        print(f"  {diagnostic.severity}: {diagnostic.message}")
    pathlib.Path(xodr_path).write_text(rm.write_xodr(network, "prop_assembly"))
    print(f"wrote {xodr_path}")

    # The overlay is process-wide state; drop it when the "project" closes.
    rm.props.clear_project_assemblies()
    return 0


if __name__ == "__main__":
    sys.exit(main())
