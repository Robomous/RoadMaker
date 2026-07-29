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

"""Import an OpenStreetMap extract as an editable road network (p7-s4, #244).

The import is three phases, and they are three calls because that is the shape
of the design rather than a convenience:

  1. `rm.osm.load`  -- a pure parse.  Bytes in, OSM primitives out, in lon/lat.
  2. `rm.osm.plan`  -- a pure conversion.  Reads the graph, writes a plan, and
     touches no network at all, so **every compromise is readable BEFORE
     anything changes**.
  3. `rm.osm.import_plan` -- ONE undoable command for the whole district.

The purity of step 2 is forced rather than stylistic.  A composite command
unwinds its whole prefix when a stage fails, which is right for a single
intersection and exactly wrong for a district: one un-fittable way must be
dropped with a warning, not abort sixteen hundred good roads.  So everything
that can fail happens before any command exists.

Run:  python python/examples/osm_import.py
"""

from __future__ import annotations

import pathlib

import roadmaker as rm

REPO = pathlib.Path(__file__).resolve().parents[2]
FIXTURES = REPO / "core" / "tests" / "data" / "osm"


def show_diagnostics(diagnostics, limit: int = 12) -> None:
    for d in diagnostics[:limit]:
        where = f" [{d.location}]" if d.location else ""
        print(f"    [{d.severity}]{where} {d.message}")
    if len(diagnostics) > limit:
        print(f"    ... and {len(diagnostics) - limit} more")


def main() -> None:
    # --- 1. the scene must already know where on the earth it is -----------
    # The importer NEVER sets this itself.  Silently georeferencing a scene
    # gives it a projection the user never chose, so the refusal below is
    # gis::crs_transform's own -- the same words the GIS and lidar importers
    # use, because it is literally the same call.
    network = rm.RoadNetwork()
    try:
        graph, _ = rm.osm.load(str(FIXTURES / "topology.osm"))
        rm.osm.plan(graph, network)
    except (ValueError, RuntimeError) as error:
        print(f"1. Without a world origin, the import refuses:\n   {error}\n")

    geo = rm.GeoReference()
    geo.projection = rm.tmerc_projection(52.3702, 4.8952)  # Amsterdam
    network.set_georeference(geo)

    # --- 2. read the extract ----------------------------------------------
    graph, read_notes = rm.osm.load(str(FIXTURES / "district.osm"))
    print(f"2. Read {len(graph.ways)} road way(s) of {graph.source_way_count} in the file")
    print(f"   CRS {graph.crs}, bounds {graph.bounds}")
    if graph.relation_count:
        print(f"   {graph.relation_count} relation(s) counted and not used, "
              f"{graph.turn_restriction_count} of them turn restrictions")
    show_diagnostics(read_notes, limit=4)

    # --- 3. plan, and read the compromises BEFORE committing --------------
    plan, plan_notes = rm.osm.plan(graph, network)
    print(f"\n3. Planned {len(plan.roads)} road(s) over {plan.area_km2:.2f} km²; "
          f"{plan.dropped_ways} way(s) dropped")

    # The contract cuts both ways: a compromised road is named with its
    # MEASURED numbers, and a clean one produces nothing at all.
    compromised = [r for r in plan.roads if not r.compromise.lossless]
    print(f"   {len(compromised)} road(s) were compromised, "
          f"{len(plan.roads) - len(compromised)} imported losslessly")
    for road in compromised[:3]:
        c = road.compromise
        print(f"     {road.odr_id}: {c.source_nodes} nodes -> {c.kept_nodes}, "
              f"{c.merged_nodes} merged, deviation {c.max_deviation_m:.3f} m "
              f"(tolerance {c.tolerance_used_m:.2f} m)")
    show_diagnostics(plan_notes, limit=6)

    # --- 4. one command for the whole district ----------------------------
    topology, _ = rm.osm.load(str(FIXTURES / "topology.osm"))
    plan, _ = rm.osm.plan(topology, network)

    stack = rm.edit.EditStack()
    stack.push(network, rm.osm.import_plan(network, plan))
    roads = len(network.road_ids)
    print(f"\n4. Imported {len(plan.roads)} planned road(s); the network now holds {roads} "
          f"(the extra ones are the junctions' connecting roads)")
    assert stack.size == 1, "a district must be ONE undo unit"

    # And it undoes as one, exactly.
    before = rm.write_xodr(network, "after")
    stack.undo(network)
    print(f"   Undo restored the scene to {len(network.road_ids)} road(s) in one step")
    assert len(network.road_ids) == 0
    assert before != rm.write_xodr(network, "after")

    # --- 5. re-importing the same extract is a no-op that says so ---------
    stack.redo(network)
    again, _ = rm.osm.plan(topology, network)
    print(f"\n5. Re-planning the same extract: {len(again.roads)} new road(s), "
          f"{again.skipped_existing} already present")

    # --- 6. what this build will not read ---------------------------------
    print("\n6. Format scope (ADR-0012):")
    for name in ("district.osm", "district.osm.pbf"):
        print(f"   {name:<20} openable: {rm.osm.is_osm(name)}")
    try:
        rm.osm.load(str(FIXTURES / "refused.osm.pbf"))
    except (ValueError, RuntimeError) as error:
        # The refusal names ZLIB, not protobuf -- the actual obstacle -- and
        # offers the one command that fixes it.
        print(f"   {error}")


if __name__ == "__main__":
    main()
