# Roadmap

*The source of truth for RoadMaker's milestone sequence. The table in the
repository README is a summary of this page.*

RoadMaker converges on a RoadRunner-class editing experience along the
milestones below. The sequence is driven by the
[gap analysis](gap_analysis.md); each milestone from M3a onward is accepted
against a [golden scene](golden_scenes/README.md) — a concrete target scene
whose element checklist replaces "looks like a real editor" with something
verifiable. Scope details live in the [milestone seeds](seeds/m3a.md);
full design docs (like [design/m2](../design/m2/00_overview.md)) are written
when each milestone's own planning task runs.

Two standing rules shape every milestone
([product-parity standard](../standards/product-parity.md)):
parity work lands **kernel-feature-first** (standards data model, then
assets, then render), and scene specs use **only OpenDRIVE/OpenSCENARIO
vocabulary**.

## Milestone sequence

| Milestone | Theme | Contents (summary) | Golden scene |
|---|---|---|---|
| **M1** ✅ | Kernel + viewer | OpenDRIVE I/O, clothoid authoring, meshing, glTF, Python package, read-only Qt editor (v0.1.0 → v0.2.0) | — |
| **M2** ✅ (shipped, v0.3.0) | Editing core | Editing tools, junction 3D surfaces, USD export — delivered per the [M2 design docs](../design/m2/00_overview.md) (reconciled as-built at close-out) | — (per-phase gates, all met) |
| **Hardening** (in flight) | Stability & workflow gaps | Crash-capture infra + interactive soak testing, all known crashes fixed · T-junctions (split road + attach-to-side) · vertical-profile editor + overpass workflow · autosave/crash recovery (pulled forward from M3a) · golden **workflows** join the acceptance process | [GW-1 "First network"](golden_workflows/gw1_first_network.md) + [GW-2 "Recover from crash"](golden_workflows/gw2_recover_from_crash.md) |
| **M3a** | UI revamp & visual completeness | **Opening epic: UI/UX revamp** ([#108](https://github.com/Robomous/RoadMaker/issues/108)) ✅ **complete** (Phases 0–4, v0.5.0) — theme system + dark professional look, labeled toolbar, welcome screen, viewport quality & feedback, **manifest-driven library panel with drag-and-drop creation** (templates, T/X assemblies — pulled forward from M4's Library Browser), CC0 props (trees) end-to-end, discoverability sweep, first-run tour, **golden-look screenshot** joins the release evidence ([`docs/standards/golden-look.png`](../standards/golden-look.png)) · then the standards-completeness track (in flight): kernel `<objects>`/`<signals>` editor exposure, crosswalk/arrow/stop-line road marks, textured viewport mode, terrain per [ADR-0006](../decisions/), sky/lighting pass | [GS-1 "Urban intersection"](golden_scenes/gs1_urban_intersection.md) + golden-look UI capture |
| **M3b** | Real-world import | GIS/lidar import (PDAL/GDAL/PROJ), OSM road-network extraction | [GS-2 "Imported district"](golden_scenes/gs2_imported_district.md) |
| **M4** | Scenario mode | OpenSCENARIO XML kernel (read/write model) · app mode switch (Map ↔ Scenario) · actor placement, lane-anchored routes with offsets, actor attributes panel · **Asset Library Browser** | [GS-3 "Ambulance run"](golden_scenes/gs3_ambulance_run.md) |
| **M5** | Scenario logic | Node-based logic editor for stories/maneuvers/conditions · simulation preview hooks (esmini interop) | GS-3 extended with a logic graph |

**Why a hardening sprint sits between M2 and M3a.** Maintainer dogfooding
of v0.3.0 — sitting down and building a real map — found product gaps that
milestone plans and scene-based acceptance did not catch: T-intersections
(attaching a road end to the *side* of another road) were impossible,
because tool specs were written from the standard's element vocabulary
rather than from user workflows — and GS-1 itself is a 4-arm *endpoint*
junction, so our own acceptance mechanism shared the blind spot; elevation
editing could not express real vertical design; and normal interactive use
crashed. The sprint fixes those before any new milestone features land, and
it adds [golden workflows](golden_workflows/README.md) — path-based
acceptance executed by hand — so this class of gap is measured from now on.
Honesty in the roadmap is a feature: the gap was in our spec process, not
in the backlog.

**Gate NO-GO, 2026-07-13 → extension complete, 2026-07-14.** The first
GW-1/GW-2 gate run on `main` (`de5bd7c`) returned **NO-GO** — six first-hand
findings, one of them a hard crash (right-click lane delete). Per the gate
rules the sprint extended with those findings only, tracked by the
gate-extension epic ([#147](https://github.com/Robomous/RoadMaker/issues/147)).
All six findings are now fixed across PRs #159–#163 (`main` at `3dbfbc5`) and
**#147 is closed**. **Maintainer action item:** re-run the GW-1/GW-2 gate once
on post-extension `main` (one run covers both) to convert the NO-GO to a PASS
and **publish v0.4.0 + v0.5.0**, which stay unpublished until then. Scope and
root causes:
[`docs/design/hardening/gate_extension.md`](../design/hardening/gate_extension.md);
verdict detail: [`golden_workflows/gate-v0.4.0.md`](golden_workflows/gate-v0.4.0.md).

**M2 shipped in v0.3.0.** Its scope, phases, and gates were frozen in the
approved design docs and delivered against them (deviations reconciled
as-built in [design/m2](../design/m2/00_overview.md)); the golden-scene
acceptance process begins with M3a/GS-1. This roadmap positions what comes
after M2. Items the old roadmap parked under "M4+ reach" (web viewer, renderer
upgrade, PyPI wheels matrix, branding/signing) remain valid backlog but are
unscheduled — they attach to milestones opportunistically rather than
defining them. Capabilities deliberately not scheduled at all are recorded
in the gap analysis's
[known exclusions](gap_analysis.md#known-exclusions).

## Why M3 split into M3a and M3b

The old M3 bundled "the world comes in" (GIS/lidar/OSM import) with
everything else. But the biggest *product* gap after M2 is that authored
scenes don't look or validate like real road scenes
([gap 1](gap_analysis.md#gap-1--viewport-visual-completeness)) — and that
gap is mostly kernel work (`<objects>`, `<signals>`, new road-mark types)
plus license-clean assets, not import machinery. Splitting lets visual and
standards completeness land first (M3a), so that when import arrives (M3b),
imported districts render as convincing scenes instead of gray ribbons.

## Library Browser placement

The Asset Library Browser lands in **M4**, but is designed as
**mode-agnostic infrastructure**: one manifest-driven browser that serves
props and materials in map mode and vehicles in scenario mode. M4 is the
right home because the browser's forcing function is actor placement, and
because M4 already carries the mode switch it must integrate with.

**Decided (2026-07-10, recorded in
[#34](https://github.com/Robomous/RoadMaker/issues/34)):** M3a pulls
forward a *minimal read-only* library panel — flat list + drag-to-place,
no search, no categories — whose model the M4 browser then subsumes.
Accepted cost: the runtime manifest format is committed in M3a. The
[M3a seed](seeds/m3a.md) and [M4 seed](seeds/m4.md) both mark this
boundary.

## Tracking

Execution is tracked on the public
[project board](https://github.com/Robomous/RoadMaker/projects) — the
project's task manager. GitHub milestones mirror the table above, each
milestone has an `epic`-labeled issue summarizing its seed, and issues carry
milestone labels (`m2`, `m3a`, …). The board is where day-to-day status
lives; this page is the plan the board follows.

## Acceptance mechanics

- **From the hardening sprint on, every milestone gates on its golden scene
  AND at least one [golden workflow](golden_workflows/README.md)** — a
  scripted sequence of user actions with a time budget and a zero-crash
  requirement, executed by the maintainer by hand at the gate. Golden
  scenes measure the *result*; golden workflows measure the *path*.
- Each milestone's release PR attaches the current render of its golden
  scene from the camera fixed in the spec
  ([process](golden_scenes/README.md)).
- Golden screenshots are regenerated by a manually triggered CI workflow so
  drift between releases is visible.
- A milestone is done when its golden-scene checklist is fully green, its
  standard release gates pass (CI on three platforms, sanitizers), and the
  scene's data round-trips through the relevant standard (OpenDRIVE for
  GS-1/GS-2, plus OpenSCENARIO for GS-3).

## Cross-cutting quality gates

Two gates attach to milestones rather than defining them; each is recorded
in the owning milestone's seed.

- **Scale targets** (owner: **M3b**; measured from M3b on): the editor must
  open and edit a 1,000-road network and import a ~50 km² OSM district.
  The metrics are load time, node-drag latency at scale, and a memory
  ceiling; concrete numbers are *proposed* for maintainer sign-off during
  the M3b planning task. Recorded in the [M3b seed](seeds/m3b.md).
- **Simulator round-trip validation** (owner: **M3a**, then permanent):
  the exported `.xodr` of every golden scene must load in
  [esmini](https://github.com/esmini/esmini) without errors — a headless
  esmini smoke job joins CI in M3a. esmini is lightweight and permissively
  licensed; verify its current license and binary-distribution method at
  implementation time, per the
  [dependency policy](../standards/dependencies.md). CARLA ingestion
  validation stays a manual release-checklist item until it is CI-feasible.
  Recorded in the [M3a seed](seeds/m3a.md) and the
  [golden-scene acceptance mechanics](golden_scenes/README.md).

## Version targets

| Milestone | Release |
|---|---|
| M2 | v0.3.0 |
| Hardening | v0.4.0 |
| M3a | v0.5.0 |
| M3b | v0.6.0 |
| M4 | v0.7.0 |
| M5 | v0.8.0 |

Versions are targets, not promises; a milestone that needs an intermediate
release takes the next patch/minor slot.
