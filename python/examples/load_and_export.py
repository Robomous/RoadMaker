#!/usr/bin/env python3
# Copyright 2026 Robomous
# SPDX-License-Identifier: Apache-2.0

"""Load an OpenDRIVE file and export its mesh as binary glTF.

Usage:
    python load_and_export.py input.xodr output.glb
"""

import sys

import roadmaker as rm


def main() -> int:
    if len(sys.argv) != 3:
        print(__doc__)
        return 2
    xodr_path, glb_path = sys.argv[1], sys.argv[2]

    network, diagnostics = rm.load_xodr(xodr_path)
    for diagnostic in diagnostics:
        print(diagnostic)
        # Findings that violate a normative ASAM rule carry its UID, and
        # entity ids resolve against the parsed network.
        if diagnostic.rule_id and diagnostic.road:
            road = network.road(diagnostic.road)
            print(f"  -> {diagnostic.rule_id} on road {road.odr_id!r}")
    print(f"loaded {network!r}")

    mesh = rm.build_network_mesh(network)
    rm.export_glb(mesh, glb_path)
    print(f"wrote {glb_path}: {mesh!r}, {mesh.vertex_count} vertices")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
