# Changelog

All notable changes to RoadMaker are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [0.3.0] - Unreleased

Milestone 2 (M2): interactive editing core, 3D junction blending, and OpenUSD
export. This is the first release in which OpenDRIVE networks can be authored
and edited in the editor, not just viewed.

### Added
- **Interactive editing tools** (8, issues #9–#17): Select/Move (multi-pick,
  rubber band, node drag), Edit Nodes (insert/delete/drag), Delete with
  referential integrity, Create Road (lane-profile templates + snapping UX),
  Lane Profile editor (add/remove lanes, type, width, road marks), editable
  Properties panel (manual binding), Elevation editor (node z, cubic-profile
  re-fit), and Create Junction (topology, connecting roads, lane links).
- **Undo/redo architecture** — kernel command layer (`edit::Command`,
  `EditStack`, restore-in-place generational arenas); every mutation is a
  command whose apply→revert leaves `write_xodr()` byte-identical, and a
  failed apply leaves the network untouched. The editor drives a single
  `QUndoStack`; drags collapse to one command on release.
- **Incremental re-mesh** — only dirty roads are re-meshed on edit, with a
  `mesh_changed` payload so the viewport updates without a full rebuild
  (issue #4).
- **Snapping queries** — grid, endpoint, and tangent-continuation snaps in the
  kernel, surfaced through the Create Road UX (issue #5).
- **Junction 3D blended surfaces** — watertight 2.5D harmonic-elevation
  junction surface (footprint → CDT → harmonic field → stitch), replacing the
  M1 flat plan-view floors (issue #18).
- **Junction `<planView>` + `<elevationGrid>` export** (OpenDRIVE ≥1.8) — the
  blended junction surface is written as an ASAM reference line and sampled
  elevation grid (issue #19). `<boundary>` export is deferred to M3 (issue #60).
- **OpenUSD (`.usda`) export** — ASCII OpenUSD exporter behind `RM_BUILD_USD`,
  backed by tinyusdz v0.9.1, validated in CI via `pxr.UsdUtils.ComplianceChecker`
  (issue #20).
- **Lucide icon set** — toolbar/action icons fetched from Lucide plus custom
  glyphs, tinted through `Icons::get()` (issue #7).

### Changed
- Kernel shared-library `SOVERSION`/`VERSION` bumped to `0.3`.

## [0.2.0] - 2026-07-10

### Changed
- **Editor migrated to Qt 6 Widgets** (LGPLv3, dynamic linking only),
  replacing the GLFW + Dear ImGui viewer. Editor logic moved into testable
  `document/` and model classes with headless (offscreen) tests.
- **Kernel buildable as a shared library** (`RM_BUILD_SHARED`) with install
  rules and a `find_package(roadmaker)` package; wheels continue to embed the
  static kernel.

### Added
- Platform installers and packaging for the Qt editor.

## [0.1.0] - 2026-07-10

First public release (Milestone 1) — geometric and standards correctness.

### Added
- **ASAM OpenDRIVE 1.6/1.7 reader** — line/arc/spiral/paramPoly3 plan views,
  lane sections/widths/offsets, elevation & superelevation, road marks, and
  junctions with resolved links. Nothing is silently dropped: skipped or
  coerced input becomes a structured diagnostic.
- **OpenDRIVE 1.7 writer** with pre-write validation (geometry continuity,
  lane-link consistency); deterministic output.
- **Clothoid authoring API** — fit a G1 clothoid path through waypoints
  (ebertolazzi/Clothoids), apply a lane profile, emit valid OpenDRIVE;
  round-trips hold at 1e-4 m / 1e-6 rad.
- **Mesh pipeline** — curvature-adaptive sampling, watertight per-road lane
  surfaces, per-lane-type materials, lane markings as separate primitives,
  plan-view junction floors (Clipper2 + CDT).
- **glTF 2.0 (`.glb`) export** — Y-up, meters, valid accessors.
- **Python bindings** (`pip install`) — full kernel coverage with pythonic
  errors and reprs, plus runnable examples.
- **Read-only editor** — OpenDRIVE viewer with 3D viewport, scene tree,
  inspector, and diagnostics log.

[0.3.0]: https://github.com/Robomous/RoadMaker/compare/v0.2.0...HEAD
[0.2.0]: https://github.com/Robomous/RoadMaker/compare/v0.1.0...v0.2.0
[0.1.0]: https://github.com/Robomous/RoadMaker/releases/tag/v0.1.0
