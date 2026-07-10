#!/usr/bin/env python3
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
    print(f"loaded {network!r}")

    mesh = rm.build_network_mesh(network)
    rm.export_glb(mesh, glb_path)
    print(f"wrote {glb_path}: {mesh!r}, {mesh.vertex_count} vertices")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
