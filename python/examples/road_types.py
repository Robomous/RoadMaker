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

"""Road <type> and <speed> records — ASAM OpenDRIVE 1.9.0 §10.4, §10.4.1.

A road's `<type>` says what the road is FOR ("motorway", "townLocal", ...) over
an s-range, and its optional `<speed>` child says what may legally be driven
there.  Both are read and written since #454; before that the reader whitelisted
`<type>` in its unknown-child sweep and parsed nothing, so a foreign file's road
types and speed limits vanished on every load->save with no diagnostic at all.

The point worth taking away is `@max`.  Its schema type `t_maxSpeed` is a UNION
of a number and exactly two string literals -- "no limit" and "undefined" -- so
the model keeps the verbatim spelling and derives the number from it.  A build
that stored `@max` as a double would rewrite a German autobahn as `max="0"` on
the first save, and nothing would crash, warn or fail a schema check.

Run:  python python/examples/road_types.py
"""

from __future__ import annotations

import pathlib

import roadmaker as rm

REPO = pathlib.Path(__file__).resolve().parents[2]
SAMPLE = REPO / "assets" / "samples" / "straight_road.xodr"
CORPUS = REPO / "core" / "tests" / "fuzz" / "corpus" / "road_type_speed.xodr"


def describe(speed: rm.RoadSpeed | None) -> str:
    """How a posted limit reads, including the two forms that are not numbers."""
    if speed is None:
        return "no posted limit"
    if speed.max is None:
        # "no limit" / "undefined" -- or something the source file invented.
        return f"{speed.max_str!r} (not a number)"
    unit = speed.unit or "m/s (implied)"
    return f"{speed.max:g} {unit}"


def dump(path: pathlib.Path) -> None:
    network, diagnostics = rm.load_xodr(str(path))
    print(f"\n=== {path.name} ===")

    for road_id in network.road_ids:
        road = network.road(road_id)
        if not road.types:
            print(f"  {road.name!r}: declares no type")
            continue
        for record in road.types:
            standard = "" if rm.is_known_road_type(record.type) else "  <- not an e_roadType literal"
            country = f", country={record.country}" if record.country else ""
            print(
                f"  {road.name!r}: from s={record.s:g} m it is a {record.type!r}{country}"
                f", {describe(record.speed)}{standard}"
            )

    for d in diagnostics:
        print(f"  [{d.severity}] {d.message}")


def main() -> None:
    # A well-formed sample: one arterial with a 50 km/h limit.
    dump(SAMPLE)

    # The corpus seed, which carries every case the reader has a branch for --
    # including the two string literals and a road that declares no type.
    dump(CORPUS)

    # Round-tripping is where a double-typed @max would show itself.
    network, _ = rm.load_xodr(str(CORPUS))
    written = rm.write_xodr(network, "road_type_speed")
    assert 'max="no limit"' in written, "the autobahn lost its limit"
    assert 'max="undefined"' in written
    assert 'max="0"' not in written, "a literal was rewritten as a number"
    print("\nRound trip preserved both t_maxSpeed literals.")

    # Which spellings the standard actually defines. e_roadType has thirteen
    # literals and the list is IDENTICAL in OpenDRIVE 1.8.1 and 1.9.0, so
    # nothing here is revision-conditional.
    print("\nSpelling check:")
    for spelling in ("motorway", "townArterial", "motorWay", "autobahnPlus"):
        known = "yes" if rm.is_known_road_type(spelling) else "no  (preserved verbatim anyway)"
        print(f"  {spelling:<14} -> {known}")

    # docs/domain/realism_defaults.md §1.7 binds each road class to one of
    # those literals. There is deliberately no companion default SPEED: a limit
    # is a fact about a road, not about its class, and inventing one would be a
    # guess where the honest answer is silence.
    print("\nRoad class -> @type (realism_defaults.md §1.7):")
    for road_class in (
        rm.RoadClass.FREEWAY,
        rm.RoadClass.ARTERIAL,
        rm.RoadClass.COLLECTOR,
        rm.RoadClass.LOCAL,
    ):
        print(f"  {str(road_class):<24} -> {rm.road_type_name(road_class)}")

    # Authoring applies that binding (#454). A road built from one of the four
    # templates carries its class's <type>; one built from a hand-assembled
    # profile carries none, because a bespoke cross section is not entitled to
    # claim it is a motorway.
    network = rm.RoadNetwork()
    rm.author_clothoid_road(
        network, [(0.0, 0.0), (200.0, 0.0)], rm.LaneProfile.freeway(), "Autobahn", "1"
    )
    bespoke = rm.LaneProfile()
    bespoke.right = [rm.LaneSpec()]
    assert bespoke.road_class is None
    rm.author_clothoid_road(network, [(0.0, 40.0), (200.0, 40.0)], bespoke, "Bespoke", "2")

    written = rm.write_xodr(network, "authored")
    assert 'type="motorway"' in written, "the freeway did not declare itself one"
    assert written.count("<type") == 1, "the hand-built road claimed a class it never had"
    print("\nAuthored a freeway and a bespoke road: only the freeway declares a <type>.")

    # Restyling rewrites the class but keeps what the road itself knew. A speed
    # limit survives a restyle for the same reason no class supplies one.
    road_id = next(r for r in network.road_ids if network.road(r).odr_id == "1")
    stack = rm.edit.EditStack()
    stack.push(network, rm.edit.apply_road_style(network, road_id, rm.RoadStyle.local_road()))
    assert network.road(road_id).types[0].type == "townLocal"
    print("Restyled it as a local street: @type followed the class.")

    stack.undo(network)
    assert network.road(road_id).types[0].type == "motorway"
    print("Undid the restyle: @type came back with it.")


if __name__ == "__main__":
    main()
