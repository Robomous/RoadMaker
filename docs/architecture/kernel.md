# Kernel (core/)

*A tour of the C++20 kernel: the road data model, geometry, OpenDRIVE I/O,
meshing, exporters, and the edit command layer. Layer rules live in the
[architecture overview](overview.md).*

Public headers live under `core/include/roadmaker/`, implementation under
`core/src/`. The module map:

| Module | Headers | What it does |
|---|---|---|
| `road/` | `network.hpp`, `road.hpp`, `lane_section.hpp`, `lane.hpp`, `junction.hpp`, `id.hpp`, `arena.hpp`, `authoring.hpp` | Road-network data model + clothoid authoring API |
| `geometry/` | `reference_line.hpp`, `poly3.hpp` | Plan-view primitives, evaluation, adaptive sampling |
| `xodr/` | `reader.hpp`, `writer.hpp`, `diagnostic.hpp`, `rules.hpp` | OpenDRIVE parsing, validation, serialization |
| `mesh/` | `mesh.hpp`, `mesh_builder.hpp` | Tessellation of the network into render/export meshes |
| `io/` | `gltf_exporter.hpp` | Exporters (glTF today; USD planned) |
| `edit/` | `command.hpp`, `edit_stack.hpp`, `operations.hpp` | Undoable edit commands |
| (root) | `error.hpp`, `tol.hpp`, `version.hpp` | `Expected`/`Error`, named tolerances |

## Road data model

`RoadNetwork` (`road/network.hpp`) owns **all** domain objects. Objects live
in arenas and reference each other exclusively through generational IDs —
never pointers:

- `Id<Tag>` (`road/id.hpp`) is `{index, gen}`. A default-constructed ID is
  invalid; an ID becomes *stale* once its slot is erased. Concrete aliases:
  `RoadId`, `LaneSectionId`, `LaneId`, `JunctionId`.
- Lookups (`network.road(id)`, `lane_section(id)`, `lane(id)`,
  `junction(id)`) return `nullptr` on a stale or invalid ID — staleness is
  always detectable, never undefined behavior.
- Pointers returned by lookups are invalidated by any mutating call. Hold
  IDs across mutations and re-look-up.
- Mutations go through `create_road` / `create_junction` /
  `add_lane_section` / `add_lane` and the cascading `erase_road` /
  `erase_junction`. A separate family of exact-slot `restore_*` /
  `erase_*_exact` methods exists solely for the edit command layer, which
  must resurrect objects under their original IDs on undo.
- Networks are single-threaded (one thread per `RoadNetwork` at a time).

The domain structs themselves are plain data: `Road` holds a `ReferenceLine`,
sorted `LaneSectionId`s, lane offset / elevation / superelevation profiles
(`Poly3` records), and optional predecessor/successor links. `Lane` holds the
signed OpenDRIVE lane ID, type, width polynomials, and road marks. `Junction`
holds `JunctionConnection` entries with per-lane links. Semantics follow the
standard — see [OpenDRIVE conventions](../domain/opendrive.md).

## Geometry

`ReferenceLine` (`geometry/reference_line.hpp`) is a road's plan view: a
contiguous sequence of `GeometryRecord`s, each with a start pose
(`s, x, y, hdg`), a length, and one of four shapes — `LineGeom`, `ArcGeom`,
`SpiralGeom` (clothoid), `ParamPoly3Geom`.

The single evaluation entry point for all consumers (lanes, meshing, editor):

```cpp
PathPoint p = plan_view.evaluate(s);  // {x, y, hdg, curvature}, s clamped
```

Clothoid math (Fresnel evaluation, G1 Hermite fitting for the authoring API
in `road/authoring.hpp`) is delegated to the
[ebertolazzi/Clothoids](https://github.com/ebertolazzi/Clothoids) library —
never hand-rolled. `sample_stations()` implements curvature-adaptive station
sampling. The math and the sampling formula are specified in
[geometry & meshing](../domain/geometry.md).

## OpenDRIVE I/O

`load_xodr(path)` / `parse_xodr(text)` (`xodr/reader.hpp`) parse OpenDRIVE
1.6/1.7 and return an `Expected<XodrParseResult>` — the network **plus** a
`std::vector<Diagnostic>`. A parse fails outright only on structural problems
(unreadable file, malformed XML, missing `<OpenDRIVE>` root). Everything the
parser skipped, coerced, or guessed at becomes a `Diagnostic`
(`xodr/diagnostic.hpp`) with severity, an XPath-ish location, and — where a
normative checker rule exists — an ASAM rule UID in `rule_id`
(constants in `xodr/rules.hpp`, e.g.
`asam.net:xodr:1.4.0:ids.id_unique_in_class`). Input is never silently
dropped. The citation convention is documented in
[working with the ASAM references](../domain/references.md).

`write_xodr(network)` / `save_xodr(network, path)` (`xodr/writer.hpp`)
serialize OpenDRIVE 1.7 XML and **validate before writing**: monotonic
stations, G1 geometry continuity, lane-link consistency. A network that would
produce invalid OpenDRIVE is refused with an `Error` rather than written.
Output is deterministic (no timestamps).

Round-trip tests hold load→save→load geometry equal within the named
tolerances `tol::kRoundTripPosition` (1e-4 m) and `tol::kRoundTripHeading`
(1e-6 rad) from `tol.hpp`.

## Meshing

`build_network_mesh(network, options)` (`mesh/mesh_builder.hpp`) tessellates
every road and junction into a `NetworkMesh` (`mesh/mesh.hpp`):

- Stations come from curvature-adaptive sampling
  ([formula](../domain/geometry.md#adaptive-sampling)) plus mandatory samples
  at record, lane-section, and width-knot boundaries.
- Each `RoadMesh` holds **one shared vertex grid**; per-lane `LanePatch`
  entries (tagged with `LaneId` and `LaneType` material) index into it, so
  adjacent lanes reuse identical boundary vertices — the road surface is
  watertight by construction.
- Lane markings are separate thin quad-strip submeshes, never baked into
  textures.
- Junction floors are plan-view polygons: a Clipper2 union of the
  connecting-road footprints, triangulated with CDT. Full 3D junction
  surface blending is Milestone 2 — the seam is explicit in the code and
  designed in [junction blending](../design/m2/03_junction_blending.md).

`remesh_roads()` / `remesh_junctions()` re-tessellate only the listed
entities, updating a `NetworkMesh` in place — the incremental entry point the
editor uses after edits, guaranteed to produce the same result as a full
`build_network_mesh`.

## Exporters

- **glTF** — `export_glb(mesh, path)` (`io/gltf_exporter.hpp`) writes binary
  glTF 2.0. This is the **single place** in the codebase where the kernel's
  Z-up frame is converted to glTF's Y-up: `(x, y, z) → (x, z, −y)`, units
  stay meters, winding stays CCW.
- **OpenUSD** — planned behind the `RM_BUILD_USD` CMake option (currently a
  stub; the exporter lands in Milestone 2 per
  [the USD export design](../design/m2/04_usd_export.md) and
  [decision 0005](../decisions/0005-tinyusdz-usda.md): tinyusdz, USDA only).

## Edit command layer

`edit::Command` (`edit/command.hpp`) is one undoable kernel mutation with a
strict contract: `apply(network)` then `revert(network)` restores a state
whose `write_xodr()` output is byte-identical to the pre-apply output; a
failed `apply` leaves the network unchanged. Commands capture value
snapshots — never arena pointers, never UI state — and report a `DirtySet`
(roads/junctions to re-mesh, topology flag) after applying.

Commands are created through factories in `edit/operations.hpp`
(`move_waypoint`, `create_road`, `split_road`, `add_lane`, `set_lane_width`,
…) and pushed onto either:

- `edit::EditStack` (`edit/edit_stack.hpp`) — the headless undo/redo stack
  used by Python and tests, or
- the editor's `QUndoStack` via a bridge — a document is never driven by
  both ([editor architecture](editor.md#undo-bridge)).

Design rationale: [editing framework](../design/m2/01_editing_framework.md).

## Error handling

No exceptions cross the public kernel API. Fallible operations return
`rm::Expected<T>` (`error.hpp`, a pinned `tl::expected`) carrying an `Error`
with a code, message, and context. Named tolerances live in `tol.hpp` — never
inline a magic epsilon. The full contract is in
[cpp-style](../standards/cpp-style.md); how errors surface in Python is in
[Python bindings](python-bindings.md).
