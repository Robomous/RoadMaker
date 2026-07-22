# Repository tour

*A map of the repository: what each top-level directory contains, the key root files, and where to read next.*

## Layout

```
core/       C++20 kernel — the heart of the project
editor/     Qt 6 Widgets desktop editor
python/     Python bindings (nanobind) + examples + pytest suite
assets/     Sample .xodr files and the asset manifest
cmake/      Shared CMake modules (deps, warnings, sanitizers, Qt pin, packaging)
scripts/    Developer/CI tooling (Qt provisioning, license checks, asset fetching)
tests/      Cross-cutting integration tests (installed-package consumer)
licenses/   Full license texts required for redistribution (LGPL-3.0, GPL-3.0)
docs/       This documentation tree
```

## Directories

- **`core/`** — the kernel: geometry, the road network model, ASAM OpenDRIVE
  I/O, mesh generation, glTF export, and the edit command layer. It has zero
  UI, OpenGL, Qt, or Python dependencies — that rule and the rest of the
  layer discipline are defined in the
  [architecture overview](../architecture/overview.md). Tests live in
  `core/tests/`, including the fuzz targets and corpus under
  `core/tests/fuzz/`. Details: [kernel](../architecture/kernel.md).
- **`editor/`** — the Qt 6 Widgets application (`roadmaker-editor`). Editor
  logic lives in testable model/document classes under `editor/src/`;
  headless tests live in `editor/tests/`. Details:
  [editor](../architecture/editor.md).
- **`python/`** — the `roadmaker` Python package: nanobind bindings in
  `python/src/`, runnable scripts in `python/examples/`, pytest suite in
  `python/tests/`. Details: [Python bindings](../architecture/python-bindings.md).
- **`assets/`** — `samples/` holds the demo `.xodr` files (see
  [Running](running.md)); `manifest.json` records external assets and their
  licenses, enforced by `scripts/check_asset_licenses.py`
  (policy: [assets](../standards/assets.md)).
- **`cmake/`** — `deps.cmake` (all pinned third-party dependencies),
  `QtVersion.cmake` (the single Qt version pin), `warnings.cmake`,
  `sanitizers.cmake`, and the packaging/install machinery.
- **`scripts/`** — `setup_qt.py` (one-time Qt provisioning — see
  [Building](building.md)), `check_asset_licenses.py`, `fetch_assets.py`,
  `fetch_asam_specs.py`.
- **`tests/`** — `consume_installed/` is a minimal external project that
  `find_package(roadmaker)`s the installed kernel; CI builds and runs it to
  prove the CMake package works outside the build tree.
- **`licenses/`** — verbatim LGPL-3.0/GPL-3.0 texts shipped for Qt
  redistribution compliance (Qt is the project's one LGPL dependency — see
  [dependency policy](../standards/dependencies.md)).
- **`docs/`** — this tree: getting started, contributing, standards,
  architecture, domain notes, [M2 design docs](../design/m2/00_overview.md),
  decision records, and the roadmap.

## Key root files

| File | Purpose |
|---|---|
| `CMakeLists.txt` | Top-level build: options (`RM_BUILD_*`), subdirectory wiring |
| `CMakePresets.json` | The `dev-*` / `ci-*` / `release-*` presets — see [Building](building.md) |
| `LICENSE` | Apache-2.0 license for the project itself |
| `THIRD_PARTY_LICENSES.md` | Every dependency with its license — updated in the same commit as any dependency change |
| `ASSETS_LICENSES.md` | Same, for bundled art/data assets |
| `README.md` / `CONTRIBUTING.md` | Front door; both link into this docs tree |

## Generated directories (gitignored)

- **`.qt/`** — the Qt SDK provisioned by `scripts/setup_qt.py`; safe to
  delete and re-provision.
- **`build/`** — one subdirectory per CMake preset (`build/dev-macos/`, …);
  safe to delete at any time.

## Where next

- Build it: [Building](building.md)
- Run it: [Running](running.md)
- Understand it: [Architecture overview](../architecture/overview.md)
- Change it: [Contributing workflow](../contributing/workflow.md)
