#pragma once

// Internal (non-installed) header: the junction surface is reachable through
// the public build_network_mesh / remesh_junctions API — this pipeline is a
// kernel implementation detail (docs/design/m2/03_junction_blending.md).

#include "roadmaker/geometry/reference_line.hpp"
#include "roadmaker/mesh/mesh.hpp"
#include "roadmaker/road/network.hpp"

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

} // namespace roadmaker
