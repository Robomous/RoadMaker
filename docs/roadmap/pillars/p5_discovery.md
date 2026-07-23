# P5 discovery — Terrain & Structures

*What the code actually looks like against the P5 scope, and the sprint
cut that follows. Written 2026-07-23, before any P5 sprint starts.
Roadmap: [Road to Parity](../README.md) · Acceptance:
[GW-2](../golden_workflows/gw2_simple_scene.md) steps 6–8 ·
Terrain scope decision:
[ADR-0006](../../decisions/0006-terrain-scope.md).*

## Why this document exists

P5 is the last pillar on the critical path (P5 → P7 → P8) that has not
started, and the read against the code corrects two premises the sprint
issues inherit. First, ADR-0006 and #232 both lean on "the existing
skirt logic" — **no skirt, terrain, or height-field code exists anywhere
in the tree**; the ground outside the road network is a flat procedural
plane in the renderer. Second, the height field is scheduled *last*
(p5-s4) but is the **foundation** the coupling sprint (p5-s2) needs
first — a constant-zero field is exactly today's flat plate, so building
the field model early costs nothing visually and gives elevation
coupling, brushes, bridge detection, and DEM ingest one shared substrate
instead of three ad-hoc ones. The order inside the pillar changes; the
issue numbers do not.

## 1. What exists

- **The Surface entity, derivation, and fill (p2-s7, #215).** `Surface`
  (`core/include/roadmaker/road/surface.hpp:33`) is a 7th arena entity:
  an ordered ring of `bounding_roads` plus a material name.
  `derive_surfaces` (`core/src/road/surface_derivation.cpp:124`) finds
  enclosed planar faces; `build_surface_mesh`
  (`core/src/mesh/surface_fill.cpp:58`) triangulates the region
  (Clipper2 union + CDT via the shared `fill_backend.hpp`).
- **Surface interiors already follow road elevation.**
  `assign_boundary_elevation_and_solve`
  (`core/src/mesh/fill_backend.hpp:568`) pins boundary z to the nearest
  road border (Dirichlet) and solves a harmonic interior — so *inside*
  an enclosed ring, "terrain follows the road" already holds. The
  editor re-derives and re-meshes surfaces off the same dirty roads as
  the road mesh (`editor/src/document/document.cpp:393-414`), keyed by
  `DirtySet` (`core/include/roadmaker/edit/command.hpp:35`), with an
  incremental `remesh_surfaces` entry point
  (`core/include/roadmaker/mesh/mesh_builder.hpp:77`).
- **Elevation editing is done — beyond what GW-2 step 7 asks.** The
  kernel has `ElevationPoint`, `elevation_profile_points`,
  `set_elevation_profile`, and `set_node_elevation`
  (`core/include/roadmaker/edit/operations.hpp:1059-1088`); the editor
  ships `ProfilePanel` (`editor/src/panels/profile_panel.hpp`) — a z(s)
  editor with grade handles, insert/delete, and the overpass
  ("cross over/under" with clearance) workflow — hosted as a page of the
  2D Editor pane (`editor/src/panels/editor2d_host.hpp`). Step 7's
  missing half is only "terrain follows the road" *outside* enclosed
  surfaces.
- **Junction floors blend graded arms.** The 2.5D junction surface
  (`docs/design/m2/03_junction_blending.md`, `junction_surface.cpp`)
  already handles elevation differences between arms; P5 does not touch
  it.
- **The editor has a reserved seat for P5 tools.** The toolbar registry
  declares a "Terrain & Structures" group (`ToolbarTab::kTerrain`,
  `editor/src/app/shortcut_registry.cpp:42`) that renders nothing until
  the first P5 tool registers; `editor/tests/test_toolbar_registry.cpp`
  asserts exactly that. Adding a 2D-pane editor is one page class plus a
  `register_page()` call (`editor2d_host.hpp:19-26`).
- **Python parity baseline.** `SurfaceId`, `Surface` queries,
  `SurfaceSpan`, and read-only `Road.elevation`/`superelevation` are
  bound (`python/src/bindings.cpp:264,517-518,571`); every new kernel op
  must land in `bindings.cpp` + one example in the same PR (workflow
  rule).
- **The 3D-boolean library is already in the build — unused.** Manifold
  3.5.2 (Apache-2.0) is declared and linked
  (`cmake/deps.cmake:87-97`, `core/CMakeLists.txt:89`) with zero
  production call sites (#268); libigl 2.6.0 is declared and **never
  linked or included** (`cmake/deps.cmake:75-82`). Clipper2 is
  plan-view-only by policy (`CLIPPER2_USINGZ OFF`,
  `cmake/deps.cmake:64`).

## 2. What does not exist (confirmed by search)

- **No terrain, skirt, or height field** — the only matches for
  terrain/skirt/heightmap/DEM in `core/` and `editor/` are the reserved
  toolbar tab and its tests. ADR-0006's "roads cut/conform to the field
  via the existing skirt logic" references M3a code that was never
  built; the "M3a flat skirt" is literally the renderer's procedural
  grass plane at a fixed z (`editor/src/render/gl_renderer.hpp:126-128`,
  `ground_base_z_`) — presentation, not data. **The coupling sprint must
  build the ground representation it couples.**
- **No authored surface boundaries.** `BoundarySource`
  (`surface.hpp:28`) has one enumerator, `Derived`, with a comment
  reserving `Authored` (node-graph) for P5. There are no boundary nodes,
  no tangents, and no surface edit ops beyond `set_surface_material`
  (`operations.hpp:1055`). `rm:surface` persistence is a road-id ring
  only (`core/src/xodr/writer.cpp:2418-2455`).
- **No bridge or structure generation.** OpenDRIVE `<bridge>`/`<tunnel>`
  (§13.11–13.12) are raw-preserved as opaque object extras
  (`core/include/roadmaker/road/road.hpp:108`); nothing detects
  grade-separated spans, and no deck/abutment/pier geometry exists
  anywhere.
- **No raster ingest.** No DEM reader, and no GDAL — that dependency is
  scheduled with P7 (p7-s2, #242).

## 3. The inversion — the height field is the foundation, not the finale

The current cut treats heightmap terrain as the last sprint (p5-s4) and
elevation↔terrain coupling as the second (p5-s2). Read against the code
this is backwards:

- Coupling needs something to couple *to*. Inside enclosed rings the
  harmonic fill already follows the roads; outside them the only ground
  is a flat render plane holding no data. "Terrain follows the road"
  (GW-2 step 7) therefore means deforming a **ground data model** near
  the road — which is exactly the height field ADR-0006 chose.
- A height field with no DEM and no brush edits is a constant-zero
  grid — indistinguishable from today's plate. Building the model +
  sampler first is invisible to users and lets every later consumer
  (conform/cut skirt, brushes, DEM ingest, bridge-span detection,
  P7 GIS import) target one substrate.
- Bridge assignment (p5-s3) is "the road is above the *ground*" — a
  road-z vs terrain-sample comparison. Without the field it would need
  a bespoke proxy that s4 then throws away.

So the field model + sampler moves into p5-s2 as its first work
package, and p5-s4 keeps what actually depends on nothing else: DEM
ingest and brushes. Issue numbers and titles stay; scopes shift.

## 4. Designed once — the ground stack

To avoid designing the ground three times, the shape is fixed here:

- **Model.** One height field per scene in the kernel frame (Z-up,
  meters): a regular grid with origin/spacing/extents and bilinear
  sampling; absent field ⇒ sample = 0 everywhere (today's behavior,
  bit for bit). Extent defaults to network bounds + margin; the flat
  render plane continues beyond it. P7's workspace extents (p7-s5,
  #324) later formalize the bounds in the georeferenced frame.
- **Consumers.** (1) The ground mesh — the renderer's flat plane
  becomes a sampled mesh channel wherever a field exists. (2) The
  conform/cut pass — a skirt band along road edges blends road-edge z
  into the field (cut where the field is above, fill where below),
  ADR-0006's "roads cut/conform". (3) Derived surfaces — their
  *boundary* z stays road-pinned (unchanged); tiny-footprint flat
  floors keep working. (4) Bridge detection (p5-s3) — spans where
  road z clears the field sample by a threshold. (5) Brushes + DEM
  (p5-s4) — raise/lower/smooth as `edit::Command`s over grid cells,
  DEM ingest as a bulk field write.
- **Persistence.** OpenDRIVE has no terrain carrier, and a grid blob
  does not belong in `<userData>` (ADR-0008 Layer 1 is for *sparse*
  enrichment). The field is scene data for the **Layer-2 native
  container** (fmt-s1, #325 — not yet built). Until fmt-s1 lands, a
  sidecar file next to the `.xodr` referenced from a Layer-1
  `rm:terrain` userData element keeps round-trips honest and migrates
  into the container unchanged. Maintainer decision D2.
- **Authored surface boundaries** (p5-s1) are sparse and DO fit
  Layer 1: `rm:surface` grows nodes + tangents alongside the existing
  road-ring attribute; `BoundarySource::Authored` activates the
  reserved enumerator.

## 5. Sprint cut

Order: **#231 ∥ (#232 → #233, #232 → #234)** — the Surface tool is
independent of the ground stack and can run in parallel.

- **p5-s1 #231 — Surface tool: node graph with tangents** *(unchanged
  scope)*. Kernel: `BoundarySource::Authored`, boundary node/tangent
  records, edit ops (move/insert/delete node, tangent drag) as
  commands; editing a **derived** surface detaches it to authored
  (decision D3). Editor: Surface tool in the reserved Terrain group,
  node handles in the viewport; drags = ONE preview session + ONE
  command. GW-2 step 6.
- **p5-s2 #232 — height field + elevation↔terrain coupling**
  *(re-scoped: gains the field foundation)*. Kernel: field model +
  bilinear sampler (absent ⇒ 0), ground mesh channel, conform/cut
  skirt band along road edges. Editor: ground plane draws the sampled
  mesh where a field exists. GW-2 step 7's "terrain follows the road".
- **p5-s3 #233 — Road Construction tool + automatic bridge spans**
  *(after s2)*. Grade-separated span detection against the field
  sampler; deck/abutment/pier/guardrail solids via **Manifold** (the
  identified 3D-boolean consumer #268 was waiting for); span-inflation
  control; `<bridge>` (§13.11) materialized as Layer 0. GW-2 step 8.
- **p5-s4 #234 — DEM import + brushes** *(re-scoped: consumes the s2
  field)*. Raise/lower/smooth brushes as undoable commands; DEM ingest
  per decision D1.

**#268 resolves inside this pillar:** drop libigl now (declared, never
linked, no named consumer — and its GPL `copyleft/` half is a standing
licensing trap); keep Manifold for p5-s3 and make
`core/tests/test_smoke.cpp` say so. Decision D4 confirms; the libigl
drop can ship as an immediate chore commit.

## 6. Maintainer decisions

- **D1 — DEM ingest path.** GDAL arrives with P7 (#242), but p5-s4 is
  scheduled before it. Options: **(a)** a minimal no-dependency ESRI
  ASCII grid (`.asc`) reader in P5, GDAL rasters (GeoTIFF etc.) joining
  in P7 *(recommended — keeps P5 dependency-free and the acceptance
  testable)*; (b) defer DEM ingest to P7 and ship p5-s4 as
  brushes-only.
- **D2 — height-field persistence until fmt-s1.** (a) Sidecar file next
  to the `.xodr`, referenced from Layer-1 `rm:terrain` userData,
  migrating into the Layer-2 container when #325 lands *(recommended)*;
  (b) pull fmt-s1 (#325) ahead, into or before p5-s2.
- **D3 — derived → authored detach semantics.** Editing a derived
  surface's boundary converts it to `Authored`: `derive_surfaces` stops
  re-deriving it when bounding roads change, and an explicit
  "re-derive" action (mirroring the locked-junction precedent, p4-s4)
  returns it to derived. Confirm.
- **D4 — #268.** Drop libigl immediately; keep Manifold for p5-s3.
  Confirm.

## 7. What this changes in the tracking

- Issues #232 and #234 get scope-amendment notes (field foundation into
  s2; s4 consumes it); #233 notes the s2 dependency and the Manifold
  call; #268 notes the P5 resolution. Bodies are amended once D1–D4 are
  decided.
- Epic #254 links this report and records the order
  #231 ∥ #232 → #233/#234.
- No GW-2 amendments: steps 6–8 describe the planned product correctly.

## Appendix — file:line map

| Concern | Where |
|---|---|
| Surface entity / `BoundarySource` | `core/include/roadmaker/road/surface.hpp:28-44` |
| Ring derivation | `core/src/road/surface_derivation.cpp:124` |
| Surface fill + harmonic interior | `core/src/mesh/surface_fill.cpp:58`, `core/src/mesh/fill_backend.hpp:568` |
| Incremental surface re-mesh | `core/include/roadmaker/mesh/mesh_builder.hpp:77`, `editor/src/document/document.cpp:393-414` |
| Elevation ops | `core/include/roadmaker/edit/operations.hpp:1059-1088` |
| Elevation profile editor | `editor/src/panels/profile_panel.hpp`, hosted by `editor/src/panels/editor2d_host.hpp` |
| Flat ground plane (render-only) | `editor/src/render/gl_renderer.hpp:126-128` |
| Reserved Terrain toolbar group | `editor/src/app/shortcut_registry.cpp:42` |
| `rm:surface` round-trip | `core/src/xodr/writer.cpp:2418-2455`, `core/src/xodr/reader.cpp:2494` |
| Raw-preserved `<bridge>`/`<tunnel>` | `core/include/roadmaker/road/road.hpp:108` |
| Manifold / libigl declarations | `cmake/deps.cmake:75-97`, link at `core/CMakeLists.txt:89` |
