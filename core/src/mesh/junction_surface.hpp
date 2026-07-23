// Copyright 2026 Robomous
// SPDX-License-Identifier: Apache-2.0

#pragma once

// Internal (non-installed) header: the junction surface is reachable through
// the public build_network_mesh / remesh_junctions API — this pipeline is a
// kernel implementation detail (docs/design/m2/03_junction_blending.md).

#include "roadmaker/geometry/reference_line.hpp"
#include "roadmaker/mesh/mesh.hpp"
#include "roadmaker/road/network.hpp"

#include <vector>

namespace roadmaker {

struct Junction;

/// Builds one junction's watertight, blended 2.5D surface.
///
/// Pipeline (docs/design/m2/03_junction_blending.md): Clipper2 union of the
/// connecting roads' plan-view footprints → constrained Delaunay triangulation
/// with interior Steiner refinement → harmonic (Laplacian) elevation field with
/// Dirichlet boundary taken from the connecting-road border elevations →
/// boundary vertices snapped to the road meshes' exact border vertices so the
/// seam is watertight (§5). Tiny footprints (< 4 m²) fall back to a flat floor
/// at the mean border elevation, still stitched at the boundary.
///
/// Pure and deterministic (fixed iteration order, no address-dependent
/// containers): two calls on the same network produce byte-identical buffers.
/// Returns an empty SubMesh when the junction has no usable footprints — the
/// caller drops empty results.
[[nodiscard]] SubMesh build_junction_surface(const RoadNetwork& network,
                                             const Junction& junction,
                                             const SamplingOptions& sampling = {});

/// Splits a junction floor (from `build_junction_surface`) into its carriageway
/// core and per-corner sidewalk bands (issue #357). Lane-type-aware: a corner's
/// band exists only where a CCW-adjacent arm carries a `LaneType::Sidewalk`
/// outermost lane, and it wraps continuously around the corner curve when BOTH
/// adjacent arms do. The carriageway keeps the floor's `Driving` material and
/// its `surface` (the junction material code); each band is a SEPARATE SubMesh
/// with `material = LaneType::Sidewalk` and (optionally) the authored
/// `JunctionCorner::sidewalk_material` as its `surface` override.
///
/// The two regions share their seam vertices bit-for-bit (both copy the floor's
/// exact positions AND normals), so the split is watertight with no cracks. A
/// junction whose arms carry NO sidewalks is returned VERBATIM as the
/// carriageway (byte-identical to the un-split floor) with no bands — the
/// feature never fires on a rural junction.
struct JunctionFloorSplit {
  SubMesh carriageway;                 ///< Driving core, surface = junction material.
  std::vector<SubMesh> sidewalk_bands; ///< Sidewalk bands, one per sidewalked corner.
};

[[nodiscard]] JunctionFloorSplit split_junction_floor_sidewalks(const RoadNetwork& network,
                                                                const Junction& junction,
                                                                const SubMesh& floor);

/// The junction's authored corner overlays (p4-s2, issue #226): one median nose
/// per contiguous median span of every arm whose corner pair names a
/// `median_material`. (The continuous sidewalk band moved to
/// `split_junction_floor_sidewalks` — issue #357.)
///
/// Overlays only — the floor built by `build_junction_surface` is NEVER cut:
/// carving the wedge out of the union would move the boundary ring and change
/// both the harmonic elevation solve and the `<boundary>` export. Each overlay
/// instead floats `kJunctionDetailLift` above the floor's elevation at that
/// point, which is far below any authored grade and never z-fights.
///
/// Returns empty when nothing is authored, so an unpainted junction meshes and
/// exports exactly as it did before the feature existed.
[[nodiscard]] std::vector<SubMesh> build_junction_corner_details(
    const RoadNetwork& network, const Junction& junction, const SamplingOptions& sampling = {});

} // namespace roadmaker
