<img src="https://cdn.robomous.ai/public-images/robomous-banner.svg" alt="Robomous.ai" width=300 />

-----

# RoadMaker

[![CI](https://github.com/robomous/roadmaker/actions/workflows/ci.yml/badge.svg)](https://github.com/robomous/roadmaker/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)

**RoadMaker** is an open source (MIT) road-network authoring toolkit for
autonomous-driving simulation, developed by [Robomous](https://robomous.ai) —
an alternative to MathWorks RoadRunner. It authors clothoid-based road
geometry with full lane semantics, reads and writes
[ASAM OpenDRIVE](https://www.asam.net/standards/detail/opendrive/), and
generates simulation-ready 3D meshes.

The core value is **geometric and standards correctness**: exported OpenDRIVE
validates, junctions carry coherent lane logic, and meshes are watertight and
robust. A C++20 kernel does the work; a Qt 6 Widgets editor and a Python
package sit on top ([architecture](docs/architecture/overview.md)).

![The RoadMaker editor: a T-junction lined with tree props, the Library
catalogue open, in the graphite-amber theme](docs/standards/golden-look.png)

Drag a road assembly, an intersection, or a prop straight from the **Library**
onto the scene — every drop is one undoable edit:

![Placing trees along a junction by dragging them from the Library](docs/user-guide/img/workflow.gif)

Prebuilt editor packages (DMG / NSIS installer / AppImage) and Python wheels
are attached to every
[GitHub release](https://github.com/robomous/roadmaker/releases).

## Quickstart (from source)

```sh
git clone https://github.com/robomous/roadmaker.git
cd roadmaker
python3 scripts/setup_qt.py        # one-time: provisions Qt into ./.qt/
cmake --preset dev-macos           # or dev-linux / dev-windows
cmake --build --preset dev-macos
ctest --preset dev-macos
./build/dev-macos/editor/roadmaker-editor.app/Contents/MacOS/roadmaker-editor \
    assets/samples/straight_road.xodr
```

Details, kernel-only builds, and troubleshooting:
[building](docs/getting-started/building.md) ·
[running](docs/getting-started/running.md) ·
[Python quickstart](docs/getting-started/running.md#python-package)

## Documentation

New to authoring? The [user guide](docs/user-guide/index.md) walks through the
editor tool by tool.

Everything lives under [`docs/`](docs/README.md): getting started,
the [user guide](docs/user-guide/index.md),
[contributing](docs/contributing/workflow.md),
[standards](docs/standards/cpp-style.md),
[architecture](docs/architecture/overview.md),
[domain conventions](docs/domain/opendrive.md), design docs, and decision
records.

## Export formats

RoadMaker tessellates a network into simulation-ready 3D meshes and writes:

- **glTF 2.0** (`.glb`) — binary, self-contained, always available.
- **OpenUSD** (`.usda`) — ASCII only, in editor/optional builds configured with
  `-DRM_BUILD_USD=ON` (prebuilt release packages include it; Python wheels ship
  it off). `.usdc`/`.usdz` **crate** output is intentionally unsupported in M2 —
  every USD consumer (usdview, Omniverse/Isaac Sim, Blender) reads `.usda`. See
  [docs/design/m2/04_usd_export.md](docs/design/m2/04_usd_export.md).

## Roadmap

Summary — the source of truth is
[docs/roadmap/roadmap.md](docs/roadmap/roadmap.md); day-to-day status lives
on the public [project board](https://github.com/Robomous/RoadMaker/projects):

| Milestone | Theme | Golden scene |
|---|---|---|
| **M1** ✅ | Kernel + read-only viewer (OpenDRIVE I/O, clothoids, meshing, glTF, Python) | — |
| **M2** ✅ | Editing core: tools, junction 3D surfaces, USD export (v0.2.0 → v0.3.0) | — |
| **M3a** | Visual & standards completeness: `<objects>`, `<signals>`, full markings, textured viewport, props, terrain | GS-1 urban intersection |
| **M3b** | Real-world import: GIS/lidar (PDAL/GDAL), OSM extraction | GS-2 imported district |
| **M4** | Scenario mode: OpenSCENARIO kernel, Map ↔ Scenario modes, actors & routes, Asset Library Browser | GS-3 ambulance run |
| **M5** | Scenario logic: node-based logic editor, simulation preview (esmini) | GS-3 + logic graph |

## License

MIT © 2026 Robomous. Third-party dependencies are listed with their licenses
in [THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md); bundled assets in
[ASSETS_LICENSES.md](ASSETS_LICENSES.md).

RoadMaker is not affiliated with, endorsed by, or sponsored by The MathWorks,
Inc. RoadRunner is a trademark of The MathWorks, Inc.
