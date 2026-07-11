# Gap analysis — RoadMaker vs a RoadRunner-class editing experience

*The five product gaps between the current plans and a commercial-grade
road/scenario editor, each mapped to what exists today, what is missing per
layer, and the milestone that owns it.*

The original roadmap (M1 viewer → M2 editing → M3 import/scenario) produces
a *correct* editor but never schedules what makes it *comparable as a
product*. Comparing against the editing experience of commercial tools
(functional capabilities only — see the
[product-parity rules](../standards/product-parity.md)) surfaces five gaps,
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
[decomposition rule](../standards/product-parity.md#the-decomposition-rule-kernel-first-then-assets-then-render)):
OpenDRIVE models them as `<objects>`, `<signals>`, and road-mark types that
RoadMaker does not parse, represent, or write yet.

| Layer | Exists today | Missing |
|---|---|---|
| Kernel | Lane meshes, line road marks (solid/broken), per-lane materials | `<objects>` (crosswalks, poles, props), `<signals>` (lights, signs), crosswalk/arrow/stop-line road-mark types, parser+writer+validation for all of them |
| Editor/render | Sober flat-shaded mode; textured mode scoped as "optional" in the [M2 assets design](../design/m2/05_assets.md) | Textured mode as the default (sober mode kept as a toggle), terrain skirt + procedural ground, sky/lighting pass, prop rendering (instancing) |
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

## Reading the gaps

Two patterns repeat across all five:

1. **Kernel-first decomposition** — every visual/UX gap starts as missing
   OpenDRIVE/OpenSCENARIO data-model support. That ordering is now a
   [standing rule](../standards/product-parity.md).
2. **Acceptance by scene, not by feature list** — features landed piecemeal
   don't compose into a convincing product on their own. Each milestone
   therefore targets a [golden scene](golden_scenes/README.md) whose
   checklist ties kernel features, assets, and rendering together.

## Known exclusions

Honest scope is part of open-source credibility: the RoadRunner
capabilities below are deliberately **not scheduled** on any milestone.
**Backlog** means the exclusion is pragmatic and revisitable if demand
appears; **permanent exclusion** means a hard constraint (licensing) rules
it out.

| Capability | Status | Rationale |
|---|---|---|
| OpenCRG road-surface detail | Backlog | High-fidelity surface simulation is niche; no golden scene needs it yet. |
| FBX export | **Permanent exclusion** | Requires the proprietary Autodesk FBX SDK, which the [dependency policy](../standards/dependencies.md) forbids; glTF and USD are the interchange formats instead. |
| Procedural traffic / swarm generation | Backlog | Simulation-runtime territory — esmini/CARLA consume RoadMaker's exported scenes and do this better. |
| Terrain sculpting beyond the road corridor | Backlog | M3a's terrain skirt covers the corridor; general terrain modeling is a different product surface. |
| Sensor-simulation asset packs | Backlog | Sensor-material metadata serves specific simulators; revisit when a consuming integration demands it. |
