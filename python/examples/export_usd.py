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

"""Load an OpenDRIVE file and export its mesh as OpenUSD ASCII (.usda).

Usage:
    python export_usd.py input.xodr output.usda

USDA (ASCII) is the only USD flavor RoadMaker writes in M2; .usdc/.usdz crate
output is unsupported (see docs/design/m2/04_usd_export.md). Every consumer
(usdview, Omniverse/Isaac Sim, Blender) reads .usda.

`export_usda` is only present when roadmaker was built with RM_BUILD_USD=ON.
The published wheels ship USD-off, so this example feature-detects it.
"""

import sys

import roadmaker as rm


def main() -> int:
    if len(sys.argv) != 3:
        print(__doc__)
        return 2
    if not hasattr(rm, "export_usda"):
        print(
            "This roadmaker build has no USD exporter. Rebuild the kernel with "
            "-DRM_BUILD_USD=ON (the published wheels ship USD-off)."
        )
        return 1

    xodr_path, usda_path = sys.argv[1], sys.argv[2]

    network, diagnostics = rm.load_xodr(xodr_path)
    for diagnostic in diagnostics:
        print(diagnostic)
    print(f"loaded {network!r}")

    mesh = rm.build_network_mesh(network)
    rm.export_usda(mesh, usda_path)
    print(f"wrote {usda_path}: {mesh!r}, {mesh.vertex_count} vertices")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
