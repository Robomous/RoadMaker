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

"""Generate standalone intersections (roadmaker.edit.assembly).

The parametric-assembly workflow the editor's drag-and-drop library uses:
one undoable command lays down the stub roads of a standalone junction and
generates its connecting roads. `t_intersection` builds a 3-way tee (a
through road plus a perpendicular stem); `x_intersection` a 4-way crossing.
Each is ONE command — undo removes the whole assembly, redo restores it.

Run:  python x_intersection.py [out.xodr]
"""

from __future__ import annotations

import sys

import roadmaker as rm

assembly = rm.edit.assembly


def main() -> int:
    out_path = sys.argv[1] if len(sys.argv) > 1 else "x_intersection.xodr"

    network = rm.RoadNetwork()
    stack = rm.edit.EditStack()

    # A 4-way crossing centered at the origin, oriented along +x. Defaults:
    # 30 m arms, two-lane-rural profile, auto-sized junction gap.
    pose = assembly.Pose(0.0, 0.0, 0.0)
    stack.push(network, assembly.x_intersection(network, pose))
    print(f"X-intersection: {network.road_count} roads, {network.junction_count} junction(s)")

    # Tune the assembly: longer arms, a wider junction, a highway cross section.
    params = assembly.IntersectionParams()
    params.arm_length_m = 45.0
    params.gap_m = 12.0
    params.profile = rm.LaneProfile.highway()
    stack.push(network, assembly.t_intersection(network, assembly.Pose(200.0, 0.0, 0.0), params))
    print(f"+ T-intersection: {network.road_count} roads, {network.junction_count} junction(s)")

    # One undo removes exactly the last assembly; redo restores it.
    stack.undo(network)
    assert network.junction_count == 1
    stack.redo(network)
    assert network.junction_count == 2

    findings = rm.validate_network(network)
    errors = [f for f in findings if f.severity == rm.Severity.ERROR]
    print(f"validation: {len(findings)} findings, {len(errors)} errors")
    assert not errors, "generated intersections must be valid OpenDRIVE"

    rm.save_xodr(network, out_path)
    print(f"wrote {out_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
