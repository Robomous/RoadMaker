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
robust — rendering is deliberately the thin part.

## Architecture

```mermaid
graph TD
    P["python/ — nanobind bindings<br/><code>import roadmaker</code>"] --> C
    E["editor/ — Qt 6 Widgets viewer/editor<br/><code>roadmaker-editor</code>"] --> C
    C["core/ — C++20 kernel<br/>geometry · road model · OpenDRIVE I/O · meshing · glTF<br/>static or shared (<code>RM_BUILD_SHARED</code>)"]
```

Three layers, one strict rule: `core/` has zero UI, GL, or Python
dependencies; `python/` and `editor/` depend on `core/` and never on each
other. Qt (LGPLv3, dynamically linked) is confined to the editor — the
kernel and Python package stay pure MIT.

## Milestone 1 status (viewer + kernel)

- [x] OpenDRIVE 1.6/1.7 reader (line/arc/spiral/paramPoly3, lanes, elevation, junctions) with structured warnings
- [x] Curvature-adaptive mesh generation with per-lane materials and lane markings
- [x] glTF 2.0 (`.glb`) export
- [x] Clothoid authoring API (waypoints → G1 clothoid path → valid OpenDRIVE out)
- [x] Python package (`pip install`, pythonic API, runnable examples)
- [x] Read-only editor (Qt 6 Widgets): 3D viewport with picking, scene tree, properties, diagnostics panel
- [x] Shared-library kernel + installable CMake package (`find_package(roadmaker)`)
- [x] Self-contained installers per OS on release tags
- [x] CI green on macOS / Linux / Windows with sanitizers, format check, fuzzing

## Download

Prebuilt, self-contained editor packages (Qt included — install nothing else)
are attached to every [GitHub release](https://github.com/robomous/roadmaker/releases):
a DMG for macOS, an NSIS installer + portable ZIP for Windows, and an
AppImage + tarball for Linux. Python wheels are published alongside.

## Quickstart (from source)

```sh
git clone https://github.com/robomous/roadmaker.git
cd roadmaker
python3 scripts/setup_qt.py        # one-time: provisions Qt into ./.qt/
cmake --preset dev-macos           # or dev-linux / dev-windows
cmake --build --preset dev-macos
ctest --preset dev-macos

# open a sample in the editor (plain binary on Linux/Windows,
# .app bundle on macOS)
./build/dev-macos/editor/roadmaker-editor.app/Contents/MacOS/roadmaker-editor \
    assets/samples/straight_road.xodr
```

Building only the kernel? Skip `setup_qt.py` and configure with
`-DRM_BUILD_EDITOR=OFF`. Add `-DRM_BUILD_SHARED=ON` for a shared
`roadmaker_core` with install rules — third parties can then
`find_package(roadmaker)` and link `roadmaker::core`.

### Python

```python
import roadmaker as rm
network, diagnostics = rm.load_xodr("assets/samples/straight_road.xodr")
rm.export_glb(network, "road.glb")
```

## Roadmap

| Milestone | Scope |
|---|---|
| **M1** | Kernel + read-only viewer: OpenDRIVE I/O, clothoid authoring, meshing, glTF, Python |
| **M2** | Interactive editing tools, 3D junction surface blending, OpenUSD export |
| **M3** | GIS/lidar import (PDAL/GDAL), OpenSCENARIO authoring |

## License

MIT © 2026 Robomous. Third-party dependencies are listed with their licenses
in [THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md).

RoadMaker is not affiliated with, endorsed by, or sponsored by The MathWorks,
Inc. RoadRunner is a trademark of The MathWorks, Inc.
