# M2 junction surface blending — geometry design

Goal: watertight, visually plausible 3D junction surface where connecting roads
overlap, respecting the elevation of incoming roads. Replaces the M1 plan-view
floors (`core/src/mesh/mesh_builder.cpp`, `kFloorDrop` seam). 2.5D throughout —
Manifold stays reserved for future solid ops (curbs/islands, M3+); no booleans
where a height field suffices.

Everything here is kernel code (`core/src/mesh/junction_surface.{hpp,cpp}`),
pure functions of the network, deterministic (fixed iteration order, no
address-dependent containers) — regeneration determinism is an acceptance test.

## 1. Pipeline

```
(1) footprint      Clipper2 union of connecting-road plan footprints
                   + joint closures across incoming-road ends
(2) boundary       simplify/orient (CCW) → junction boundary polygon (+ holes)
(3) triangulate    CDT (constrained Delaunay, already in tree) with constraint
                   edges = boundary + incoming joint edges; interior refinement
                   to ~sampling step
(4) elevation      harmonic (Laplacian) field over interior vertices,
                   Dirichlet boundary from incoming roads + connecting-road
                   centerline elevations (see §3)
(5) stitch         boundary vertices are EXACT copies of the adjacent road-mesh
                   edge vertices (shared-edge watertight rule)
(6) markings       overlay strips (guidance lines across the junction) as
                   existing marking submeshes, draped on the field
```

**(1) Footprint.** Each connecting road contributes its plan-view outer-border
polygon (sampled at the curvature-adaptive stations — same sampler as meshing,
so edges agree). Each incoming arm contributes a *joint quad*: the strip between
the incoming road's end cross-section and the junction interior, guaranteeing
the union reaches every arm's end even when turn footprints don't overlap near
the curb. Union via `Clipper2::Union` (`FillRule::NonZero`), integer scaling
1e6 (Clipper2's recommended geometric scale for meter data at micrometer
precision).

**(2) Boundary.** The union's outer path(s), oriented counterclockwise. Interior
holes (traffic islands emerging from non-covered pockets) are kept as CDT holes
in M2 (rendered as gaps; island curbs are M3 with Manifold). Collinear cleanup
with `SimplifyPaths` at `tol::kLength` — but **never** simplify vertices lying
on a joint edge (they must match road-mesh vertices exactly, §5).

**(3) Triangulation.** `CDT::Triangulation` with constraint edges: the boundary
polyline and each incoming joint edge's vertex sequence (the road mesh's
end-station cross-section vertices, projected to plan view). Interior Steiner
refinement: insert grid points at the mesh sampling step (default `kChord`-driven,
~1–2 m) so the elevation field has room to bend; CDT's `eraseOuterTrianglesAndHoles`
removes everything outside the boundary.

## 2. Elevation field: harmonic vs natural-neighbor

Two candidates were evaluated on paper (prototype comparison is a Phase 4 task
with a golden-fixture harness, but the doc commits to a default):

| | Laplacian/harmonic on the CDT | Natural-neighbor (Sibson) |
|---|---|---|
| Input | Dirichlet values on boundary vertices | Scattered (s,t,z) samples |
| Solve | Sparse SPD system (cotangent or graph Laplacian), Eigen `SimplicialLDLT` — already a dependency | Voronoi-cell area computations per query point; needs its own robust construction (no in-tree implementation) |
| Behavior | Smooth membrane; no local extrema inside (maximum principle) — exactly the "soap film between road edges" we want; grade mismatches resolve monotonically | Interpolates through interior samples; excellent for measured data, but we have no trustworthy interior samples — only boundary + synthesized centerlines |
| Determinism | Deterministic for fixed vertex order | Deterministic but sensitive to sample placement |
| Failure modes | Over-flat for long skinny junctions (mitigate: include connecting-road centerline elevations as soft interior constraints) | Extrapolation undefined outside sample hull; needs guard rails |
| Cost | One factorization per junction regen; N ≈ few thousand | Per-vertex neighbor search |

**Decision: harmonic interpolation** (graph Laplacian with cotangent weights on
the CDT, Dirichlet boundary), with connecting-road centerline elevation samples
added as *soft* constraints (quadratic penalty, weight 0.1) to keep long
through-paths from sagging. Natural-neighbor is rejected for M2: it needs a new
robust geometric kernel for no benefit at our data density. The cotangent
assembly is ~40 lines against plain Eigen, so hand-rolling it is preferred to
taking a mesh-processing dependency for one matrix. (As built, that is what
happened; libigl was pinned but never used, and was dropped in #268.)

Boundary Dirichlet values:
- Joint edges: exact z of the road-mesh end cross-section vertices (§5).
- Lane-segment edges (along connecting-road outer borders): z of the connecting
  road's mesh border at the same stations.
- Synthesized closure edges (arms that don't touch): linear blend between the
  adjacent joint corner z values along the edge.

## 3. ASAM alignment — this is also an export feature

OpenDRIVE 1.8.0 introduced first-class junction surface data, and the design
maps onto it 1:1 (all in `12_junctions.md` of the local 1.9.0 reference; all
elements exist identically in 1.8.1):

- **Junction boundary** `<boundary>` with `<segment type="lane">` /
  `<segment type="joint">`, counterclockwise, closed:
  `asam.net:xodr:1.8.0:junctions.boundary.segments_counter_clockwise_order`,
  `…boundary.segments_close_boundry`, `…boundary.segments_for_each_conn_road`,
  `…boundary.only_for_common_junctions`,
  `…boundary.close_gap_with_new_roads` (closure edges that don't follow an
  existing lane require auxiliary boundary roads — M2 writes boundary only when
  no synthesized closure edge remains; else it omits `<boundary>` and keeps the
  surface editor-internal. Auxiliary road generation is M3.)
- **Junction reference line** `<planView>` (one `<geometry>` with one `<line>`):
  `asam.net:xodr:1.8.0:junctions.geometry.only_one_line_element`,
  `…geometry.ref_line_definition` (every junction point reachable
  perpendicular), `…geometry.correct_junction_boundry`. Generator: the line
  through the footprint's oriented-bounding-box major axis, extended to cover
  the boundary.
- **Elevation grid** `<elevationGrid>` (one per junction —
  `asam.net:xodr:1.8.0:junctions.elevation_grid.only_one_elev_grid`), grid
  perpendicular to the junction reference line
  (`…elevation_grid.perpendicular_vectors`), valid for the whole boundary
  (`…elevation_grid.valid_for_entire_boundry`), smooth at entries/exits
  (`…elevation_grid.entry_exit_smoothness`). Export: sample the harmonic field
  on the grid (spacing = max(2 m, footprint/32)); consumers reconstruct via the
  spec's bicubic interpolation (12.11.1, with the c/d-zero fallback rule
  `…elevation_grid.polynome_coefficient_values`) — our sampling step keeps the
  bicubic reconstruction within `rm::tol` of the field at grid Nyquist.
- **Transition zones**: joint segments get `@transitionLength` (default 0 in
  M2 — the field already matches road z exactly at joints; the 12.11.2 cubic
  transition applies when a consumer regenerates from the grid, and our exported
  grid equals road elevation at the joint row, so the transition is a no-op).

**Version handling:** boundary/elevationGrid exist only in ≥1.8.0 — this is why
the version-explicit writer (target 1.8.1, `00` scope) is a Phase 4 dependency.
Writing 1.7 emits geometry meshes only (glTF/USD unaffected) and a structured
warning that junction surface data was dropped (parser-never-silently-drops
rule applies to the writer in spirit). 1.8.1 vs 1.9.0: no normative differences
in these elements; 1.9.0 adds `junctions.connection.smooth_fit` (cited when
writing 1.9.0) — handled in the connection generator (`02` §6), tested for both
targets explicitly.

## 4. Inputs from the network

The field depends on: incoming-road end cross-sections (positions + z + grade
along s at the end station), connecting-road plan footprints and centerline
elevations, lane widths at the ends. Elevation editing (`02` §5) and node moves
mark the junction dirty (`01` §2.4); regeneration re-runs this pipeline —
deterministic, so undo needs no mesh snapshots.

## 5. Watertightness (skirt-stitch)

Rule: **shared edges share exact vertices** (bitwise-equal doubles).
- Joint edges: the junction triangulation does not create its own vertices
  there — it consumes the road mesh's end cross-section vertices verbatim
  (constraint vertices in step 3, z copied in step 4).
- Lane-segment edges: same, from the connecting road's border ring.
- The checker (`expect_watertight`, new test util): every boundary edge of the
  junction submesh appears exactly once in an adjacent road submesh with
  identical endpoints; no T-junctions (vertex-on-edge) anywhere on seams;
  Euler/manifold check per submesh; all triangle normals within 90° of +Z
  (no flips — the surface is a height field by construction).

## 6. Degenerate cases (each with strategy + named fixture)

| Case | Strategy | Fixture (`core/tests/fixtures/junctions/`) |
|---|---|---|
| Near-parallel merge (2 arms at <15°) | Joint quads keep the union connected where turn footprints are slivers; sliver triangles (aspect > 50) collapsed by edge flip before the solve; if the footprint area < 4 m² fall back to flat floor at mean z | `merge_15deg.xodr` |
| >4 arms (5-way, 6-way) | No special casing in the pipeline (union/CDT/solve are arm-count-agnostic); test guards performance (N arms → O(N²) turns) and boundary closure | `five_way.xodr` |
| Steep grade mismatch (arms at ±8% ending at Δz up to 1.5 m) | Harmonic field absorbs it (maximum principle ⇒ no bumps); acceptance: max interior slope ≤ 1.5 × max incoming grade; visual golden | `grade_mismatch.xodr` |
| Tiny footprint (arms nearly touching, area < 4 m²) | Flat floor fallback at area-weighted mean end z, still stitched exactly at joints | `tiny_footprint.xodr` |
| Self-overlapping connecting road (hairpin turn folding over itself in plan) | Union absorbs overlap (NonZero fill); elevation is single-valued (2.5D) — the folded ramp case is out of scope for M2 (validator warns when connecting roads' z differ > 0.3 m where plan views overlap: "junction requires grade separation, not supported") | `hairpin_overlap.xodr` |
| Incoming arm with zero driving lanes (footpath arm) | Arm contributes joint quad only if it has any lane with width > 0; else excluded from footprint but kept in topology | `footpath_arm.xodr` |

Fixtures are hand-authored minimal `.xodr` files; each gets a generation test
(pipeline completes, acceptance invariants hold) and goes into the fuzz corpus
(`core/tests/fuzz/corpus/`) per the standing rule for new xodr features.

## 7. Acceptance criteria (Phase 4 gate)

1. **Watertight**: `expect_watertight` passes on all fixtures + T-junction and
   4-way golden networks (with elevation deltas).
2. **No flipped normals**: all junction-surface normals in the +Z hemisphere.
3. **Elevation continuity**: field z equals road z along every seam vertex
   exactly; along seam edges within `rm::tol` (kLength) at midpoints.
4. **Deterministic**: two regenerations from the same network are byte-identical
   (mesh buffers compared), across platforms (CI matrix runs the same test).
5. **Performance** (informational): 4-way, 2-lane arms regenerates in < 50 ms
   Release on the dev machine.
6. Exported 1.8.1 file with `<boundary>` + `<elevationGrid>` passes our
   validator with rule-id-cited diagnostics only where expected, and loads in
   esmini without junction warnings (manual check, recorded in the PR).
