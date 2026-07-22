# Copyright 2026 Robomous
# SPDX-License-Identifier: Apache-2.0

"""Tee/cross an intersection onto an existing road (rm.edit.assembly).

`tee_onto_road` / `cross_onto_road` drop a T (3-way) or X (4-way) intersection
ONTO an existing road at a station: the stem(s) are aligned to the road tangent
and the target is split + joined into the junction, in one undoable command —
the kernel side of dragging a Library assembly onto a road (gate finding 1).

Run:  python tee_onto_road.py out.xodr
"""

from __future__ import annotations

import sys

import roadmaker as rm


def main() -> int:
    out_path = sys.argv[1] if len(sys.argv) > 1 else "tee_onto_road.xodr"

    network = rm.RoadNetwork()
    stack = rm.edit.EditStack()

    # A straight two-lane road to tee onto.
    stack.push(
        network,
        rm.edit.create_road([(0.0, 0.0), (200.0, 0.0)], rm.LaneProfile.two_lane_rural(), "A"),
    )
    target = network.find_road("1")

    # Tee a perpendicular stem into the road at s = 100 m — aligned + attached.
    stack.push(network, rm.edit.assembly.tee_onto_road(network, target, 100.0))
    print(f"teed: {network.junction_count} junction, {network.road_count} roads")
    assert network.junction_count == 1

    # Undo is byte-identical — the junction and stem disappear.
    stack.undo(network)
    assert network.junction_count == 0

    # Re-tee and save.
    stack.push(network, rm.edit.assembly.tee_onto_road(network, target, 100.0))
    rm.save_xodr(network, out_path, "tee_onto_road")
    print(f"wrote {out_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
