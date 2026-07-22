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

/// The junction's authored corner overlays (p4-s2, issue #226): one sidewalk
/// wedge per corner whose JunctionCorner entry names a `sidewalk_material`, and
/// one median nose per contiguous median span of every arm whose corner pair
/// names a `median_material`.
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
