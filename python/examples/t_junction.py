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

"""Tee a road into the side of another (roadmaker.edit.attach_t_junction).

The hardening-sprint workflow the editor's side-snap uses: attach a road END
to the SIDE of another road. One undoable command splits the target around
the attach station (the removed middle stub becomes the junction area), then
generates a common junction from the three resulting ends with ALL legal
turns — the documented default policy; permission pruning comes later.

Run:  python t_junction.py out.xodr
"""

from __future__ import annotations

import sys

import roadmaker as rm


def main() -> int:
    out_path = sys.argv[1] if len(sys.argv) > 1 else "t_junction_attach.xodr"

    network = rm.RoadNetwork()
    stack = rm.edit.EditStack()

    # A 120 m main road and a side road ending 10 m south of its midpoint.
    stack.push(
        network,
        rm.edit.create_road([(-60.0, 0.0), (60.0, 0.0)], rm.LaneProfile.two_lane_default(), ""),
    )
    stack.push(
        network,
        rm.edit.create_road([(0.0, -50.0), (0.0, -10.0)], rm.LaneProfile.two_lane_default(), ""),
    )
    main_road = network.find_road("1")
    side_end = rm.RoadEnd(network.find_road("2"), rm.ContactPoint.END)

    # Attach at station 60 (the midpoint). gap_m=0 auto-sizes the junction
    # area from the road widths AND the turning geometry — every generated
    # turn gets room for options.generation.min_turn_radius_m (default 6 m);
    # the side road's overhang past that clearance is trimmed into the
    # junction area, like the target's middle stub.
    options = rm.edit.TAttachOptions()
    options.generation.min_turn_radius_m = 6.0
    gap = rm.edit.t_attach_gap(network, side_end, main_road, 60.0, options)
    print(f"junction area spans s = {60.0 - gap:.2f} .. {60.0 + gap:.2f}")
    stack.push(network, rm.edit.attach_t_junction(network, side_end, main_road, 60.0, options))
    print(
        f"teed: {network.road_count} roads, "
        f"{network.junction_count} junction(s)"
    )

    # One undo returns to the exact pre-split network; redo re-tees.
    stack.undo(network)
    assert network.junction_count == 0
    stack.redo(network)
    assert network.junction_count == 1

    findings = rm.validate_network(network)
    errors = [f for f in findings if f.severity == rm.Severity.ERROR]
    print(f"validation: {len(findings)} findings, {len(errors)} errors")

    rm.save_xodr(network, out_path, "t_junction_attach")
    print(f"wrote {out_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
