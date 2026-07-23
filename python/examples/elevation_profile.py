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

"""Edit a road's vertical profile (roadmaker.edit.set_elevation_profile).

The hardening-sprint profile editor's kernel API: explicit elevation nodes
(station, z, optional locked grade) fitted as a C1 piecewise cubic. A hill
with level ends, checked by the validator's advisory grade warning.

Run:  python elevation_profile.py out.xodr
"""

from __future__ import annotations

import sys

import roadmaker as rm


def main() -> int:
    out_path = sys.argv[1] if len(sys.argv) > 1 else "elevation_profile.xodr"

    network = rm.RoadNetwork()
    stack = rm.edit.EditStack()
    stack.push(
        network,
        rm.edit.create_road([(0.0, 0.0), (200.0, 0.0)], rm.LaneProfile.two_lane_default(), ""),
    )
    road = network.find_road("1")

    # A 6 m hill: level ends (locked 0 % grade), crest at midspan.
    stack.push(
        network,
        rm.edit.set_elevation_profile(
            network,
            road,
            [
                rm.edit.ElevationPoint(0.0, 0.0, 0.0),
                rm.edit.ElevationPoint(100.0, 6.0, 0.0),
                rm.edit.ElevationPoint(200.0, 0.0, 0.0),
            ],
        ),
    )
    points = rm.edit.elevation_profile_points(network, road)
    print("profile nodes:", [(round(p.s, 1), round(p.z, 2)) for p in points])

    warnings = [f for f in rm.validate_network(network) if f.severity == rm.Severity.WARNING]
    for finding in warnings:
        print("advisory:", finding.message)

    rm.save_xodr(network, out_path, "elevation_profile")
    print(f"wrote {out_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
