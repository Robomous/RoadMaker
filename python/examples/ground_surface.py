#!/usr/bin/env python3
"""Auto-form a ground surface from a closed loop of roads (#215, GW-2 step 5).

Four straight roads welded corner-to-corner enclose a square. derive_surfaces
enumerates the bounded faces of the road graph and reconciles the surface
arena so one Surface fills the enclosed area, its bounding_roads tracing the
ring. The call is id-stable and idempotent on unchanged topology.

Run:  python ground_surface.py [out.xodr]
"""

from __future__ import annotations

import sys

import roadmaker as rm


def straight(network: rm.RoadNetwork, odr_id: str, a: tuple, b: tuple) -> rm.RoadId:
    """Author a straight two-lane road from a to b (welds at shared corners)."""
    return rm.author_clothoid_road(
        network, [a, b], rm.LaneProfile.two_lane_rural(), odr_id=odr_id
    )


def main() -> int:
    out_path = sys.argv[1] if len(sys.argv) > 1 else "ground_surface.xodr"

    network = rm.RoadNetwork()

    # A 10 m square: four roads welded corner-to-corner.
    corners = [(0.0, 0.0), (10.0, 0.0), (10.0, 10.0), (0.0, 10.0)]
    for i in range(4):
        straight(network, f"edge{i}", corners[i], corners[(i + 1) % 4])
    print(f"authored {network.road_count} roads forming a closed loop")

    rm.derive_surfaces(network)
    print(f"surfaces after derivation: {network.surface_count}")

    for surface_id in network.surface_ids:
        surface = network.surface(surface_id)
        odr_ids = [network.road(r).odr_id for r in surface.bounding_roads]
        print(f"  {surface!r} bounded by {odr_ids}")

    # Idempotent: a second call with no topology change leaves the count alone.
    rm.derive_surfaces(network)
    assert network.surface_count == 1

    # Give the surface a paved material through the undoable command layer — the
    # kernel behind dropping "Asphalt" from the editor Library onto the surface.
    surface_id = network.surface_ids[0]
    stack = rm.edit.EditStack()
    stack.push(network, rm.edit.set_surface_material(network, surface_id, "asphalt"))
    print(f"material after set:  {network.surface(surface_id).material!r}")
    stack.undo(network)
    print(f"material after undo: {network.surface(surface_id).material!r}")
    stack.redo(network)

    rm.save_xodr(network, out_path)
    print(f"wrote {out_path} (rm:surface userData carries material={network.surface(surface_id).material!r})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
