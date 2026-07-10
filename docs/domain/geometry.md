# Geometry and meshing

*The clothoid math, continuity guarantees, and tessellation rules behind
RoadMaker's meshes. OpenDRIVE semantics are in [opendrive](opendrive.md); the
module layout is in the [kernel tour](../architecture/kernel.md).*

## Clothoids

A clothoid (OpenDRIVE `spiral`) has curvature that varies linearly with arc
length — the primitive that makes road geometry drivable, because steering
angle changes continuously instead of jumping.

All clothoid math is delegated to
[ebertolazzi/Clothoids](https://github.com/ebertolazzi/Clothoids):

- **Evaluation** uses its Fresnel-based solvers. **Never hand-roll Fresnel
  integrals** — naive series/quadrature implementations lose precision
  exactly where roads live (small curvatures, long arcs).
- **Authoring** uses its G1 Hermite fit: given waypoints, fit a clothoid
  path that interpolates positions and headings with G1 continuity
  (`author_clothoid_road` and the node-editing commands).

Spiral evaluation state is cached per record inside `ReferenceLine` (it is
expensive to rebuild and hot during drag re-meshing); the Clothoids types
never leak into public headers.

## Continuity and tolerances

- Authoring must guarantee **G1 continuity** (position and heading) at every
  geometry-record joint.
- Validators (notably the OpenDRIVE writer's pre-write validation) check
  position/heading gaps at joints against **named tolerances** in
  `core/include/roadmaker/tol.hpp` — never inline magic epsilons
  ([numerics conventions](../standards/cpp-style.md)):

| Constant | Value | Used for |
|---|---|---|
| `tol::kLength` | 1e-6 m | length comparison, station dedup |
| `tol::kAngle` | 1e-9 rad | heading continuity |
| `tol::kChord` | 0.01 m | default chord tolerance for sampling |
| `tol::kCurvatureEpsilon` | 1e-12 1/m | below this, a record is a straight line |
| `tol::kRoundTripPosition` | 1e-4 m | round-trip test equality (position) |
| `tol::kRoundTripHeading` | 1e-6 rad | round-trip test equality (heading) |

All geometry is computed in `double`; conversion to `float` happens only at
render/export boundaries.

## Adaptive sampling

Tessellation station spacing adapts to curvature so flat roads stay cheap and
tight curves stay smooth. For a maximum chord deviation ε between the sampled
polyline and the true curve:

```text
Δs = clamp( sqrt(8·ε / |κ|),  Δs_min,  Δs_max )
```

with ε = `chord_tolerance` (default 1 cm, `tol::kChord`), κ the local
curvature, and clamp bounds `min_step` = 0.05 m / `max_step` = 5 m
(`SamplingOptions` in `geometry/reference_line.hpp`).

On top of the adaptive fill, samples are **mandatory** at:

- every geometry-record boundary,
- every lane-section boundary,
- every width-polynomial knot (and other profile discontinuities), passed in
  as `SamplingOptions::extra_stations`.

The result (`sample_stations()`) is sorted ascending and deduplicated within
`tol::kLength`, and always includes 0 and the road length.

## Watertightness

Within a road, adjacent lanes **share boundary vertices**: each `RoadMesh`
holds one shared vertex grid, and per-lane patches index into it. Lane k's
outer boundary and lane k+1's inner boundary are literally the same
vertices, so the road surface is watertight by construction — no
coincident-vertex welding pass, no T-junction cracks.

## Submeshes and markings

- Meshes are emitted as **per-lane submeshes** (`RoadMesh::LanePatch`),
  tagged with the lane's `LaneId`, signed OpenDRIVE id, and `LaneType` as
  the material class — so exporters and the editor can style driving lanes,
  sidewalks, shoulders, etc. independently, and picking can resolve a
  triangle back to a lane.
- **Lane markings are separate primitives**: thin quad strips slightly above
  the surface, generated from `<roadMark>` records — never baked into
  textures.

## Junction floors

The current junction surface is a plan-view floor:

1. take the footprint polygons of the junction's connecting roads,
2. union them with **Clipper2**,
3. triangulate the union with **CDT** (constrained Delaunay).

Each floor is keyed by `JunctionId` so incremental re-meshing can replace
exactly the affected entry. This is an explicit seam: full 3D junction
surface blending (elevation-aware, curb-aware) is Milestone 2 — see
[junction blending](../design/m2/03_junction_blending.md).

## Incremental re-meshing

Meshing is a pure function of the network. `build_network_mesh()` tessellates
everything; `remesh_roads()` / `remesh_junctions()` update a `NetworkMesh` in
place for only the entities an edit dirtied, guaranteed to match the
from-scratch result. The editor drives this from each command's `DirtySet`
([editing framework](../design/m2/01_editing_framework.md)).

## Round-trip guarantee

Load → save → load must reproduce reference-line geometry within
`tol::kRoundTripPosition` (1e-4 m) and `tol::kRoundTripHeading` (1e-6 rad).
Tests compare sampled poses, not XML text — the writer may re-derive
redundant attributes, but the road a simulator drives must be the same road.
