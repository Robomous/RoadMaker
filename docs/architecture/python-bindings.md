# Python bindings (python/)

*How the `roadmaker` Python package wraps the kernel: binding design, error
translation, packaging, and the rules for keeping bindings in sync with the
kernel API.*

## What it is

One nanobind module, defined entirely in `python/src/bindings.cpp`, exposed
as `roadmaker` (the compiled extension is `_roadmaker`, re-exported by the
pure-Python package in `python/src/roadmaker/`). It mirrors the public kernel
API and depends only on `core/` — never on the editor, never on Qt
([layer rules](overview.md)).

```python
import roadmaker as rm

network, diagnostics = rm.load_xodr("assets/samples/straight_road.xodr")
mesh = rm.build_network_mesh(network)
rm.export_glb(mesh, "road.glb")
```

## API design rules

- **Pythonic surface.** Properties instead of getters (`network.road_count`),
  natural containers (lists, tuples, pairs), keyword arguments with
  defaults, `__repr__` everywhere, `__bool__`/`__eq__`/`__hash__` on the ID
  types. `load_xodr`/`parse_xodr` return a `(network, diagnostics)` tuple
  rather than a result struct.
- **Exceptions instead of `Expected`.** The kernel returns
  `rm::Expected<T>` ([error contract](kernel.md#error-handling)); the
  binding layer unwraps every result and translates `rm::Error` into Python
  exceptions via a registered exception translator:

  | Kernel `ErrorCode` | Python exception |
  |---|---|
  | `FileNotFound` | `FileNotFoundError` |
  | `IoFailure` | `OSError` |
  | everything else (`MalformedXml`, `InvalidDocument`, `InvalidArgument`) | `ValueError` |

  The error's `context` (path, element location, id) is appended to the
  message. No C++ exception ever crosses the boundary unhandled.
- **Coordinates and units are the kernel's**: right-handed, Z-up, meters,
  radians — except `export_glb`, which writes Y-up like every glTF file
  ([why](overview.md#4-one-coordinate-frame-one-conversion-point)).

## What is bound

- **Data model**: `RoadNetwork`, `Road`, `LaneSection`, `Lane`, `Junction`,
  `JunctionConnection`, plus the ID types (`RoadId`, `LaneSectionId`,
  `LaneId`, `JunctionId`) and enums (`LaneType`, `RoadMarkType`, …).
- **Geometry**: `ReferenceLine`, `PathPoint`, `Poly3`, `Waypoint`,
  `RoadEnd`.
- **OpenDRIVE I/O**: `load_xodr`, `parse_xodr`, `write_xodr`, `save_xodr`,
  and `Diagnostic` (severity, location, message, `rule_id`).
- **Authoring**: `author_clothoid_road` (waypoints → G1 clothoid road),
  `LaneProfile`, `LaneSpec`.
- **Editing** — the `roadmaker.edit` submodule binds the kernel command
  layer ([kernel](kernel.md#edit-command-layer)): `edit.Command`,
  `edit.EditStack`, `edit.DirtySet`, and every command factory
  (`move_waypoint`, `insert_waypoint`, `delete_waypoint`, `create_road`,
  `split_road`, `delete_road`, `create_junction`, `delete_junction`,
  `add_lane`, `remove_lane`, `set_lane_type`, `set_lane_width`,
  `set_road_mark`, `set_node_elevation`, `rename_road`). Create a command
  with a factory, then push it — pushing applies it:

  ```python
  stack = rm.edit.EditStack()
  stack.push(network, rm.edit.set_lane_width(network, lane_id, 4.0))
  stack.undo(network)
  ```

- **Meshing / export**: `MeshOptions` (including `chord_tolerance`),
  `build_network_mesh`, `NetworkMesh`, `export_glb`.

Runnable, tested usage lives in `python/examples/` — `load_and_export.py`,
`author_road.py`, `edit_network.py`.

## Keeping bindings in sync (mandatory)

Every public kernel API change updates `python/src/bindings.cpp` **and** at
least one example in `python/examples/` in the same PR. This is part of the
contribution [workflow](../contributing/workflow.md) and is checked in
review.

## Building and testing

Development install (builds the extension against the in-tree kernel):

```sh
pip install -e python/
pytest python/tests
```

Python tests use pytest idioms — see [testing](../contributing/testing.md).

## Packaging

The wheel is built with **scikit-build-core** (`python/pyproject.toml`):
CMake compiles the kernel and the nanobind module, and the wheel **embeds
the static kernel** — users `pip install roadmaker` with no shared-library
or Qt dependency, and the package stays pure MIT
([licensing](../standards/dependencies.md)). Release wheels are produced by
the release workflow alongside the editor packages
([CI](../contributing/ci.md)).
