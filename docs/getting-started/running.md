# Running RoadMaker

*How to launch the editor you just built, open the sample OpenDRIVE files, and get started with the Python package.*

Everything below assumes you have completed a build — see
[Building](building.md). Prefer not to build at all? Self-contained editor
packages (DMG, NSIS installer + ZIP, AppImage + tarball) and Python wheels
are attached to every [GitHub release](https://github.com/robomous/roadmaker/releases).

## The editor

The editor binary is `roadmaker-editor`, built under `build/<preset>/editor/`.
It accepts an `.xodr` file as a command-line argument.

macOS (the editor is an `.app` bundle):

```sh
./build/dev-macos/editor/roadmaker-editor.app/Contents/MacOS/roadmaker-editor \
    assets/samples/straight_road.xodr
```

Linux:

```sh
./build/dev-linux/editor/roadmaker-editor assets/samples/straight_road.xodr
```

Windows:

```sh
.\build\dev-windows\editor\roadmaker-editor.exe assets\samples\straight_road.xodr
```

### Smoke check

A quick way to verify the binary runs at all (this is also what CI runs
against the packaged installers):

```sh
./build/dev-linux/editor/roadmaker-editor --version
```

### Sample files

`assets/samples/` ships three small OpenDRIVE networks:

| File | Contents |
|---|---|
| `straight_road.xodr` | A single straight road — the simplest possible network |
| `curved_road.xodr` | A road with curved (arc/clothoid) geometry |
| `t_junction.xodr` | Three roads meeting in a junction with connecting lanes |

Open them via the command line as above or through the editor's file-open
dialog. What you should see: a 3D viewport with the road mesh (click to pick
roads and lanes), a scene tree, a properties panel, and a diagnostics panel
listing any parser warnings.

## Python package

Install the bindings in editable mode (this builds the kernel into the
package — no separate CMake step needed):

```sh
pip install -e python/
```

Quickstart — load an OpenDRIVE file and export a binary glTF mesh:

```python
import roadmaker as rm

network, diagnostics = rm.load_xodr("assets/samples/straight_road.xodr")
rm.export_glb(network, "road.glb")
```

Run the test suite to confirm the install:

```sh
pytest python/tests
```

### Runnable examples

`python/examples/` contains complete, documented scripts:

| Example | What it shows |
|---|---|
| `load_and_export.py` | Load an `.xodr`, export its mesh as `.glb` |
| `author_road.py` | Author a clothoid road through waypoints, write OpenDRIVE + glTF |
| `edit_network.py` | Undoable editing with the kernel command layer (`roadmaker.edit`) |

```sh
python python/examples/load_and_export.py assets/samples/t_junction.xodr t_junction.glb
```

## Where next

- [Repository tour](repository-tour.md) — what lives where
- [Architecture overview](../architecture/overview.md) — the three layers and their rules
- [Contributing workflow](../contributing/workflow.md) — branches, commits, PRs
