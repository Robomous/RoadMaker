#pragma once

#include "roadmaker/export.hpp"
#include "roadmaker/geometry/reference_line.hpp"
#include "roadmaker/mesh/mesh.hpp"
#include "roadmaker/road/network.hpp"

#include <span>

namespace roadmaker {

struct MeshOptions {
  SamplingOptions sampling;

  /// Emit lane-marking strips.
  bool markings = true;

  /// Emit plan-view junction floors.
  bool junction_floors = true;
};

/// Tessellates every road (and junction floor) of the network.
/// Degenerate inputs (roads without geometry or lanes) are skipped — the
/// xodr parser already diagnosed them.
[[nodiscard]] RM_API NetworkMesh build_network_mesh(const RoadNetwork& network,
                                                    const MeshOptions& options = {});

/// Re-tessellates ONLY the listed roads, updating `mesh` in place: existing
/// entries are replaced, new roads appended, erased or degenerate roads
/// removed. Untouched roads keep their vertex buffers untouched (the editor
/// re-uploads only what changed). Meshing stays a pure function of the
/// network — this is the incremental entry point for the same result
/// build_network_mesh produces from scratch (docs/m2/01 §5).
RM_API void remesh_roads(const RoadNetwork& network,
                         NetworkMesh& mesh,
                         std::span<const RoadId> roads,
                         const MeshOptions& options = {});

/// Re-tessellates ONLY the marking layer (lane road marks + object markings:
/// crosswalks, stop lines, lane arrows) of the listed roads, in place — the
/// road surface grid and lane patches are left untouched, so the editor
/// re-uploads only what an object edit changed. This is the mesh consumer of
/// edit::DirtySet::objects (docs/design/m3a/01 §2.4). A road not yet present
/// in `mesh` is built in full; markings off clears them.
RM_API void remesh_objects(const RoadNetwork& network,
                           NetworkMesh& mesh,
                           std::span<const RoadId> roads,
                           const MeshOptions& options = {});

/// Same contract for junction floors, keyed by JunctionId. With
/// options.junction_floors off, listed junctions simply lose their floors.
RM_API void remesh_junctions(const RoadNetwork& network,
                             NetworkMesh& mesh,
                             std::span<const JunctionId> junctions,
                             const MeshOptions& options = {});

} // namespace roadmaker
