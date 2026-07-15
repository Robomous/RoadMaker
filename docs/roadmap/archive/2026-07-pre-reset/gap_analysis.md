# Gap analysis — RoadMaker vs a commercial-grade editing experience

*The five product gaps between the current plans and a commercial-grade
road/scenario editor, each mapped to what exists today, what is missing per
layer, and the milestone that owns it.*

The original roadmap (M1 viewer → M2 editing → M3 import/scenario) produces
a *correct* editor but never schedules what makes it *comparable as a
product*. Comparing against the editing experience of commercial tools
(functional capabilities only — see the
[product-parity rules](../../../standards/product-parity.md)) surfaces five gaps,
in increasing order of size. The [roadmap](roadmap.md) assigns each an
owning milestone; the [golden scenes](golden_scenes/README.md) make them
measurable.

## Gap 1 — viewport visual completeness

A road scene reads as "real" when it has textured asphalt, crosswalks, lane
arrows, stop lines, signs and lights, vegetation, terrain, and plausible
lighting. Today's viewport renders flat per-lane materials on an infinite
grid.

This is only partly a rendering problem. Crosswalks, arrows, and stop lines
are **kernel features first** (see the
[decomposition rule](../../../standards/product-parity.md#the-decomposition-rule-kernel-first-then-assets-then-render)):
OpenDRIVE models them as `<objects>`, `<signals>`, and road-mark types that
RoadMaker does not parse, represent, or write yet.

| Layer | Exists today | Missing |
|---|---|---|
| Kernel | Lane meshes, line road marks (solid/broken), per-lane materials | `<objects>` (crosswalks, poles, props), `<signals>` (lights, signs), crosswalk/arrow/stop-line road-mark types, parser+writer+validation for all of them |
| Editor/render | Sober flat-shaded mode; textured mode scoped as "optional" in the [M2 assets design](../../../design/m2/05_assets.md) | Textured mode as the default (sober mode kept as a toggle), terrain skirt + procedural ground, sky/lighting pass, prop rendering (instancing) |
| Assets | Icon pipeline, manifest + license gate | Asphalt/concrete/grass textures, vegetation/pole/sign prop set (CC0), sign-face graphics |

**Owner: M3a.** Golden scene GS-1.

## Gap 2 — asset library browser

A runtime, user-facing browsable library — folders, thumbnails, search,
drag-to-place — of vehicles, props, materials, and markings. This is a
*system*, absent from all plans, and distinct from the build-time asset
pipeline (which provisions editor chrome like icons and textures).

| Layer | Exists today | Missing |
|---|---|---|
| Kernel | — | Placed-object model (prop instances as OpenDRIVE `<object>` entries), asset references in the document |
| Editor | Panels framework, drag/drop infrastructure in Qt | The browser panel (folders/thumbnails/search, `QAbstractItemModel`-based), drag-to-place interaction, thumbnail generation/cache |
| Assets | `assets/manifest.json` + license gate | Runtime library manifest format (categories, preview metadata), the library content itself |

**Owner: M4** (designed mode-agnostic; see the
[open option](roadmap.md#library-browser-placement) about a minimal
read-only version in M3a). Golden scene GS-3 exercises it.

## Gap 3 — scenario editing mode

Placing actors, drawing lane-anchored routes with lateral offsets, editing
actor attributes. The old M3 mentioned OpenSCENARIO *kernel* work; the
editing UX was unspecified.

| Layer | Exists today | Missing |
|---|---|---|
| Kernel | Road network + s/t coordinates (route anchoring substrate); OpenSCENARIO XML 1.4.0 reference text fetched | `core/xosc/` data model, reader/writer with diagnostics, entities/routes/story basics |
| Editor | Selection/tools framework (M2) | Actor placement tool, route drawing (lane-anchored polyline with offset), actor attributes panel, scenario document coupling to the map document |
| Assets | — | Vehicle/pedestrian models (CC0) for actors |

**Owner: M4.** Golden scene GS-3.

## Gap 4 — logic editor

A node-based visual editor for OpenSCENARIO stories/maneuvers — states,
transitions, conditions. A sub-product in its own right; mentioned nowhere
in the old plans.

| Layer | Exists today | Missing |
|---|---|---|
| Kernel | — | Storyboard/maneuver/condition model (M4's OpenSCENARIO work provides the data model; logic-editor-specific queries on top) |
| Editor | — | The node canvas (technology decision pending — see the [M5 seed](seeds/m5.md)), node/port/edge model, validation feedback, sync with the scenario document |
| Assets | — | Node iconography (within the existing icon policy) |

**Owner: M5.** Golden scene GS-3 extended with a logic graph.

## Gap 5 — application modes

Map editing and scenario editing are different activities with different
tools, panels, and selection semantics. Commercial editors make this an
explicit app-level mode switch. RoadMaker has no architectural decision on
modes yet — and retrofitting one after scenario tools exist would be
expensive.

| Layer | Exists today | Missing |
|---|---|---|
| Editor | Single implicit "map" mode; `Document`/`SelectionModel`/`ToolManager` designed for it | Mode concept (Map ↔ Scenario): which panels/tools/selection types each mode exposes, what state survives a switch, where the switch lives in the UI |

**Owner: M4** decides and implements; the
[M4 seed](seeds/m4.md) presents the options without deciding today.

## Gap 6 — material depth and built structures

Benchmarking against commercial-editor footage (2026-07-13) found ~40% of the visual
target unscheduled, clustering into three scheduled gaps now owned by the
**Materials & Structures (v0.7.0)** milestone (prerequisite for GS-2's approval):

| Cluster | Exists today | Missing | Owner |
|---|---|---|---|
| Material system | M3a textured mode: fixed asphalt/concrete textures, per-lane surface classification | PBR-lite (albedo + normal + roughness), an assignable **material library** with variants (new/worn asphalt), drag-onto-surface / properties assignment | Materials & Structures / GS-2 |
| Built structures | Roads carry elevation (overpasses via profile) | Auto-generated **bridge structures** — deck with thickness, abutments/piers, guardrails (Manifold solids), offered on grade separation | Materials & Structures / GS-4 |
| City content density | CC0 trees end-to-end | **Buildings, streetlights, gantry signs** as Library props (CC0), for district-scale scenes | Materials & Structures / GS-2 |

**Owner: Materials & Structures (v0.7.0).** Seed:
[seeds/materials-structures.md](seeds/materials-structures.md); golden scene:
[GS-4](golden_scenes/gs4_rural_overpass.md). Interaction polish landed alongside
GS-1 — **placement drops land under the cursor, and a Move tool + transform
gizmo** make elements movable (epic [#178](https://github.com/Robomous/RoadMaker/issues/178)).

## Reading the gaps

Two patterns repeat across all five:

1. **Kernel-first decomposition** — every visual/UX gap starts as missing
   OpenDRIVE/OpenSCENARIO data-model support. That ordering is now a
   [standing rule](../../../standards/product-parity.md).
2. **Acceptance by scene, not by feature list** — features landed piecemeal
   don't compose into a convincing product on their own. Each milestone
   therefore targets a [golden scene](golden_scenes/README.md) whose
   checklist ties kernel features, assets, and rendering together.

## Known exclusions

Honest scope is part of open-source credibility: the commercial-editor
capabilities below are deliberately **not scheduled** on any milestone.
**Backlog** means the exclusion is pragmatic and revisitable if demand
appears; **permanent exclusion** means a hard constraint (licensing) rules
it out.

| Capability | Status | Rationale |
|---|---|---|
| OpenCRG road-surface detail | Backlog | High-fidelity surface simulation is niche; no golden scene needs it yet. |
| FBX export | **Permanent exclusion** | Requires the proprietary Autodesk FBX SDK, which the [dependency policy](../../../standards/dependencies.md) forbids; glTF and USD are the interchange formats instead. |
| Procedural traffic / swarm generation | Backlog | Simulation-runtime territory — esmini/CARLA consume RoadMaker's exported scenes and do this better. |
| Heightmap terrain under the network (DEM import + raise/lower/smooth brush) | **Scheduled — M3b (v0.8.0)** | Ground the network sits *in*, not just a skirt: a single height field per scene via DEM import (rides M3b's GDAL work) plus a basic brush; roads conform via the skirt/cut logic. Accepted in [ADR-0006](../../../decisions/0006-terrain-scope.md) (Option B). |
| Terrain sculpting beyond a single height field | Backlog | Multi-layer materials, overhangs, and LOD (ADR-0006 Option C) are a different product surface; the M3b height field covers the dogfooding expectation. |
| Sensor-simulation asset packs | Backlog | Sensor-material metadata serves specific simulators; revisit when a consuming integration demands it. |
