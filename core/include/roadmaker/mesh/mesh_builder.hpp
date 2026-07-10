#pragma once

#include "roadmaker/export.hpp"
#include "roadmaker/geometry/reference_line.hpp"
#include "roadmaker/mesh/mesh.hpp"
#include "roadmaker/road/network.hpp"

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

} // namespace roadmaker
