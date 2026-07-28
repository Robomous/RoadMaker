# P7 discovery — Import & Export

*What the code actually looks like against the P7 scope, and the sprint cut
that follows. Written 2026-07-27, before any P7 sprint starts. Roadmap:
[Road to Parity](../README.md) · Acceptance:
[GW-2](../golden_workflows/gw2_simple_scene.md) steps 21–22 · Persistence
layering: [ADR-0008](../../decisions/0008-persistence-layers-asam-first.md).*

## Why this document exists

P7 is the last pillar on the critical path (P5 → P7 → P8) that has not
started, and the read against the code moves work *out* of its first sprint
rather than into it.

The headline finding is that **the hard part of both preview tools is already
built and is simply never called.** `write_xodr` has returned an in-memory
`std::string` since M2; `validate_network` produces the full advisory sweep
with ASAM rule ids. Neither is reachable from the editor without performing
the thing being previewed. p7-s1 is therefore mostly a matter of *surfacing*
kernel output, not producing it — which is why it can afford to also close the
gap named next.

The second finding is a defect the sprint issues do not mention. **The editor
never validates the network you are authoring.** `validate_network` is called
in exactly one place in the entire editor — inside `Document::save`
(`editor/src/document/document.cpp:172`). On load, the diagnostics list is
filled by the *reader* (`document.cpp:97`). So the Diagnostics dock shows
parse findings from the file you opened, and the only way to learn what a
consumer would say about the network you have since built is to save it. That
is what an "OpenDRIVE Export Preview" is *for*, and it makes p7-s1 a bug fix as
much as a feature.

The third finding changes p7-s1's architecture. The mesh exporters are
**path-only**, and their material vocabulary is **private to `core/src/io`**.
A preview that re-derives material names and colours in the editor would be a
second implementation of the export — the exact drift the tool exists to
prevent. The manifest must be computed in the kernel.

## 1. What exists

- **The OpenDRIVE writer is already an in-memory API.**
  `write_xodr` (`core/include/roadmaker/xodr/writer.hpp:84`) returns
  `Expected<std::string>`; `save_xodr` (`:89`) is a thin wrapper that adds an
  `std::ofstream` (`core/src/xodr/writer.cpp:2730-2740`). Output is
  deterministic — no timestamps — which is why ~30 tests already diff it.
- **The validator is complete and cites normative rules.**
  `validate_network` (`writer.hpp:69`) runs ~38 emission sites and returns
  `Diagnostic`s carrying an ASAM checker-rule UID
  (`core/include/roadmaker/xodr/rules.hpp`, ~40 rules; some version-gated, e.g.
  `kJunctionNotOnlyTwo` is cited only against 1.9.0). `WriterOptions`
  (`writer.hpp:42`) exposes the two advisory knobs — `max_grade_warning` and
  `prop_obstruction_clearance`, the latter a whole-network geometry sweep that
  a caller may want to disable.
- **The Layer-1 extension registry is machine-readable.**
  `core/include/roadmaker/xodr/rm_codes.hpp` holds all 18 `rm:` userData codes
  with their scope plus `is_registered_rm_code()`, CI-gated against
  ADR-0008 by `core/tests/test_rm_registry.cpp` since `fmt-s2` (#326).
- **Both mesh exporters work and share one material vocabulary.**
  `export_glb` (`core/include/roadmaker/io/gltf_exporter.hpp:33`) and
  `export_usda` (`core/include/roadmaker/io/usd_exporter.hpp:48`).
  `core/src/io/mesh_export_common.hpp` is the single definition of the
  Z-up→Y-up boundary rotation (`to_export_frame`, `:38`), the per-lane-type
  colour palette (`lane_material_color`, `:43`) and the material naming
  (`lane_material_name`, `:86`).
- **A diagnostics surface already exists, model and all.**
  `DiagnosticsModel` (`editor/src/document/diagnostics_model.hpp:29`, columns
  severity/rule/location/message) hosted by `DiagnosticsPanel`
  (`editor/src/panels/diagnostics_panel.hpp:32`), whose double-click resolves a
  finding's location back to an entity through `SelectionModel`. A preview
  needs no new diagnostics UI — only a fresher source.
- **A headless golden-workflow replay exists.** `scripts/gw2_replay.py` drives
  the kernel command layer through the automatable slice of GW-2 and asserts
  each step's outcome, including a byte-identity check across undo×10/redo×10.
- **The editor has a non-modal tool-window precedent** — `help::HelpViewer`
  (`editor/src/help/help_viewer.hpp`), lazily built and held by `QPointer`
  (`editor/src/app/main_window.cpp:2223`). There are **zero `QDialog`
  subclasses** in the editor.

## 2. What does not exist (confirmed by search)

- **Nothing georeferences anything.** No CRS, WKT, proj-string, world origin
  or workspace extents anywhere; the writer *synthesizes* the `<header>` and
  emits no `<geoReference>`. This is #324's whole surface, and it is greenfield.
- **No GIS, lidar or OSM ingest**, and no GDAL/PROJ/PDAL in `cmake/deps.cmake`.
- **No preview of any kind.** `File ▸ Export glTF…` and `Export USD…`
  (`main_window.cpp:2084`, `:2102`) are the same four steps — suggest a name,
  file dialog, call the exporter, toast or warn. **There is no confirmation or
  summary step anywhere in the export path.**
- **No in-memory mesh export.** Both exporters take only a
  `std::filesystem::path`. A preview cannot be built by "exporting and reading
  it back" without writing a file, which is what the acceptance forbids.

## 3. The editor never validates what you are authoring

Stated once, because it is the sprint's real subject and no issue says it:

| When | What fills `Document::diagnostics()` |
|---|---|
| After `load()` | the **reader's** parse findings (`document.cpp:97`) |
| While authoring | *nothing* — the list is whatever the last load or save left |
| After `save()` | `validate_network` on the saved network (`document.cpp:172`) |

So a road built and never saved is never checked, and the dock's contents
describe a document that may no longer exist. The fix is small — extract
`save()`'s validation into a `Document::refresh_diagnostics()` that
republishes and emits `diagnostics_changed()` — and it makes the OpenDRIVE
preview's diagnostics pane a reuse of `DiagnosticsModel` rather than a second
list.

## 4. Two validation paths, and the asymmetry between them

`write_xodr` does **not** call `validate_network`. It calls a different,
hard-failing `validate()` (`core/src/xodr/writer.cpp:313`) that stops at the
**first** finding and collapses it into a single `Error`, discarding the rule
id and every other finding. `validate_network` is the full advisory sweep and
never blocks.

A preview that only reports a refusal therefore shows one message where the
network may have twenty. **Run `validate_network` first and unconditionally,
then attempt `write_xodr`** — so a refused export is explained by the whole
finding list. This ordering is not a preference; reversing it loses data.

## 5. Three silent export omissions

Neither exporter's channel walk is total over `NetworkMesh`
(`core/include/roadmaker/mesh/mesh.hpp:193`), and nothing warns:

- **Ground is never exported.** `mesh.surfaces` (since #215) and
  `mesh.terrain` (since #232) are walked by neither exporter — verified by
  exhaustive grep: the only `mesh.<channel>` sites are
  `gltf_exporter.cpp:67,70,83,88,97` and `usd_exporter.cpp:210,251,282,346,351`.
  A raised road exports without the terrain that follows it; a block's ground
  surface exports as a hole. Tracked as **#390**.
- **USD drops sign-face textures** — stated in the code
  (`usd_exporter.cpp:353-359`); glTF embeds them. Tracked as **#364**.
- **The empty-mesh guard is narrower than "empty".** Both exporters refuse on
  `mesh.roads.empty() && mesh.junction_floors.empty()`
  (`gltf_exporter.cpp:471-476`, `usd_exporter.cpp:191-195`), so a scene made
  only of terrain, surfaces, bridges or props is rejected as *"nothing to
  export"* though it plainly has geometry.

**Why these stayed invisible:** no committed sample exercises them.
`grep -l "rm:surface\|rm:terrain" assets/samples/*.xodr` matches none of the
twelve. Any test that means to pin this behaviour must synthesize its fixture.

**Consequence for the release gate.** GW-2 step 21 asks for a preview of the
exported 3D scene. Until #390 lands, an *honest* preview shows a ground-less
scene. p7-s1 should report the omission by name rather than hide it — but
**#390 should be sequenced before the GW-2 hand-run**, or the gate records a
pass on a preview that is truthfully reporting a defect.

> **Resolved 2026-07-28 (#390).** Both exporters now write `mesh.surfaces` and
> `mesh.terrain`, and the empty-mesh guard became one shared predicate over
> every channel, so a terrain-only or props-only scene exports. GW-2 step 21
> and its pass criteria were amended in the same PR, and **the GW-2 hand-run is
> no longer blocked by this finding.** The remaining declared omission is the
> USD sign face (#364).

## 6. The two exporters disagree about props

Not a defect — a design difference that a preview must respect. glTF builds one
shared mesh per model and caches it so every instance references it
(`prop_mesh_for`, `gltf_exporter.cpp:305`), emitting a node per instance; USD
**bakes world-space geometry for every instance** (`bake_instance`,
`usd_exporter.cpp:299`). For a scene with 200 trees, the glTF
file carries roughly one tree's triangles and the USD file 200. A
format-agnostic "triangle count" would be wrong in one of the two cases, so a
scene manifest is only meaningful **per format**.

Likewise `save_xodr` writes a second file that `write_xodr` knows nothing
about — the terrain `.asc` sidecar (`writer.cpp:2752-2757`). Its name is
emitted into the XML as `<userData code="rm:terrain">`, so a preview can
report it by reading its own output rather than recomputing the name.

## 7. What this implies for p7-s1

- **The scene manifest belongs in the kernel** (`core/src/io/`), beside the
  exporters and including their private `mesh_export_common.hpp`. That header
  should *not* be promoted: the manifest carries material names and colours as
  **data**, so nothing needs to become public API, and `core/tests` already has
  `core/src` on its include path (`core/tests/CMakeLists.txt:93`; precedent
  `test_fill_predicates.cpp`) so a gate can assert against `io_common::`
  directly.
- **The OpenDRIVE summary should be counted out of the emitted XML**, not
  re-walked from the `RoadNetwork`. A summary derived from the output cannot
  drift from the output — no gate required, ever. Re-walking would be a second
  implementation of the writer's emission conditions. Prefer a design that
  cannot drift over a test that detects drift.
- **The USD half needs no `#ifdef`.** The manifest depends on export *policy*,
  not on tinyusdz, so it is computable and correct in a USD-off build. This
  matters: `core/tests/test_usd.cpp` compiles only under `RM_BUILD_USD`
  (`core/tests/CMakeLists.txt:97`), and the `usd-export` CI job runs
  **`ctest -R '^Usd\.'`** with `RM_BUILD_EDITOR=OFF`
  (`.github/workflows/ci.yml:587-602`) — so a gate outside that suite never
  runs there, while a policy gate outside USD entirely runs *everywhere*.
- **Steps 21–22 stop being interactive-only.** `scripts/gw2_replay.py`'s
  docstring currently lists "the export-preview tools" among the aspects out of
  scope for headless replay. A kernel-side, Python-bound manifest deletes that
  clause and turns both steps into replay rows — pre-flight evidence for the
  maintainer's by-hand gate run.

## 8. Sprint cut

The cut and the execution order stand as the
[2026-07 realignment](../updates/2026-07-realignment.md) set them —
**#241 → #324 → #242 → #243 → #244** — with no resequencing implied by this
read. What changes is scope detail, not order:

- **#241 (p7-s1)** — smaller in kernel work than it looks (the writer half is
  already in memory), larger in reach than the issue says (it closes the
  never-validate-while-authoring gap and makes GW-2 21–22 replayable).
- **#324 (p7-s5)** — greenfield; nothing to reconcile. Its `<header>` modelling
  and **#453 (`fmt-f1`)**'s verbatim preservation of unmodeled `<header>`
  children share one header-child dispatch: whichever lands second inherits the
  other's. The OpenDRIVE preview gains a `<header>` read-out once #324 lands.
- **#242/#243/#244** — unchanged by this read; all three depend on #324's world
  frame existing first, which the realignment already records.
- **#390** — recommend scheduling **before the GW-2 hand-run**, per §5.

## 9. What this changes in the tracking

- #241 gets a scope note: the manifest is kernel-side and format-aware; the
  omissions are reported, not repaired; `Document::refresh_diagnostics()` is
  in scope.
- #390 gets the release-gate sequencing note from §5.
- Epic [#256](https://github.com/Robomous/RoadMaker/issues/256) links this
  report.
- No GW-2 step rewrites: steps 21–22 describe the intended product correctly.
  Step 21 gains the #390 caveat when p7-s1 lands.

## Appendix — file:line map

| Concern | Where |
|---|---|
| In-memory OpenDRIVE writer | `core/include/roadmaker/xodr/writer.hpp:84` |
| `save_xodr` wrapper + terrain sidecar | `core/src/xodr/writer.cpp:2730`, `:2751-2757` |
| Full advisory validation | `core/include/roadmaker/xodr/writer.hpp:69` |
| Hard-fail first-defect validation | `core/src/xodr/writer.cpp:313` |
| ASAM rule UIDs | `core/include/roadmaker/xodr/rules.hpp` |
| `rm:` Layer-1 registry + gate | `core/include/roadmaker/xodr/rm_codes.hpp`, `core/tests/test_rm_registry.cpp` |
| Mesh exporters (path-only) | `core/include/roadmaker/io/gltf_exporter.hpp:33`, `usd_exporter.hpp:48` |
| Shared export vocabulary (private) | `core/src/io/mesh_export_common.hpp:38,43,86` |
| Empty-mesh guard | `core/src/io/gltf_exporter.cpp:471`, `core/src/io/usd_exporter.cpp:191` |
| Prop instancing vs baking | `core/src/io/gltf_exporter.cpp:305-330`, `core/src/io/usd_exporter.cpp:299-341` |
| `NetworkMesh` channels | `core/include/roadmaker/mesh/mesh.hpp:193-235` |
| Validation only on save | `editor/src/document/document.cpp:172`; reader findings `:97` |
| Diagnostics model / panel | `editor/src/document/diagnostics_model.hpp:29`, `editor/src/panels/diagnostics_panel.hpp:32` |
| Export actions and dialogs | `editor/src/app/main_window.cpp:2084`, `:2102`, menu `:972-975` |
| Non-modal tool-window precedent | `editor/src/help/help_viewer.hpp`, `editor/src/app/main_window.cpp:2223` |
| Core tests see `core/src` | `core/tests/CMakeLists.txt:93` |
| USD test gating and CI filter | `core/tests/CMakeLists.txt:97`, `.github/workflows/ci.yml:587-602` |
| Headless GW-2 replay | `scripts/gw2_replay.py` |
