#pragma once

#include "roadmaker/road/id.hpp"
#include "roadmaker/road/lane.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace roadmaker {

/// Triangle mesh with its own buffers. Kernel frame: right-handed, Z-up,
/// meters; positions/normals are xyz triplets; indices are CCW when viewed
/// from +Z (up). double here — conversion to float happens at export/render
/// boundaries only.
struct SubMesh {
  std::vector<double> positions;
  std::vector<double> normals;
  std::vector<std::uint32_t> indices;

  /// Material class for surfaces; markings and junction floors carry their
  /// role in `name` and are materialized separately by exporters.
  LaneType material = LaneType::None;

  std::string name;
};

/// One road's tessellation. Lane patches index into the SHARED grid buffers
/// (positions/normals) so adjacent lanes reuse identical boundary vertices —
/// the road surface is watertight by construction.
struct RoadMesh {
  RoadId road;
  std::string name;

  /// Shared vertex grid for all lane patches (xyz triplets).
  std::vector<double> positions;
  std::vector<double> normals;

  struct LanePatch {
    LaneId lane;
    int odr_lane_id = 0;
    LaneType material = LaneType::None;
    /// Triangles into RoadMesh::positions/normals.
    std::vector<std::uint32_t> indices;
  };

  std::vector<LanePatch> lanes;

  /// Lane markings as thin quad strips slightly above the surface —
  /// separate primitives, never baked into textures (M1 rule).
  std::vector<SubMesh> markings;
};

/// One junction's blended surface, keyed by its id so incremental re-meshing
/// can replace exactly the affected entry.
struct JunctionFloor {
  JunctionId junction;
  SubMesh mesh;
};

/// Whole-network tessellation result.
struct NetworkMesh {
  std::vector<RoadMesh> roads;

  /// Blended 2.5D junction surfaces (Clipper2 union → CDT → harmonic elevation
  /// field, stitched watertight to the road meshes). Built by
  /// junction_surface.cpp — docs/design/m2/03_junction_blending.md.
  std::vector<JunctionFloor> junction_floors;
};

} // namespace roadmaker
