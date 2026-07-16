// Acceptance tests for the enclosed-area ground surface (#215, p2-s7): a
// derived Surface (the area a ring of roads surrounds) meshes into a watertight
// 2.5D height-field SubMesh whose boundary lies on the ring roads' inner edges
// and whose elevation matches those roads.
//
// Scenarios are built through the real authoring path (author_clothoid_road +
// derive_surfaces) and meshed through the public build_network_mesh, which
// populates NetworkMesh::surfaces from whatever surfaces exist in the arena —
// the same integration a renderer/editor sees. CRITICAL: the junction-surface
// tests must stay green; nothing here touches that pipeline.

#include "roadmaker/mesh/mesh_builder.hpp"
#include "roadmaker/road/authoring.hpp"
#include "roadmaker/road/network.hpp"
#include "roadmaker/road/surface.hpp"
#include "roadmaker/road/surface_derivation.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <utility>
#include <vector>

using roadmaker::BoundarySource;
using roadmaker::derive_surfaces;
using roadmaker::LaneProfile;
using roadmaker::NetworkMesh;
using roadmaker::RoadId;
using roadmaker::RoadMesh;
using roadmaker::RoadNetwork;
using roadmaker::SubMesh;
using roadmaker::Surface;
using roadmaker::SurfaceMesh;
using roadmaker::Waypoint;

namespace {

RoadId
segment(RoadNetwork& network, const char* odr_id, double x0, double y0, double x1, double y1) {
  const std::vector<Waypoint> waypoints{Waypoint{.x = x0, .y = y0}, Waypoint{.x = x1, .y = y1}};
  auto road = roadmaker::author_clothoid_road(
      network, waypoints, LaneProfile::two_lane_default(), "", odr_id);
  EXPECT_TRUE(road.has_value());
  return road.value_or(RoadId{});
}

/// A ~20 m square loop of four welded straight roads (large enough that the
/// enclosed hole is well above the flat-floor threshold).
std::vector<RoadId> author_square(RoadNetwork& network) {
  return {segment(network, "a", 0.0, 0.0, 20.0, 0.0),
          segment(network, "b", 20.0, 0.0, 20.0, 20.0),
          segment(network, "c", 20.0, 20.0, 0.0, 20.0),
          segment(network, "d", 0.0, 20.0, 0.0, 0.0)};
}

/// Boundary vertices of a SubMesh: endpoints of edges used by exactly one
/// triangle (open edges of the height field).
std::vector<bool> boundary_flags(const SubMesh& s) {
  auto key = [](std::uint32_t a, std::uint32_t b) {
    return a < b ? std::pair{a, b} : std::pair{b, a};
  };
  std::map<std::pair<std::uint32_t, std::uint32_t>, int> edge_count;
  for (std::size_t i = 0; i + 2 < s.indices.size(); i += 3) {
    ++edge_count[key(s.indices[i], s.indices[i + 1])];
    ++edge_count[key(s.indices[i + 1], s.indices[i + 2])];
    ++edge_count[key(s.indices[i + 2], s.indices[i])];
  }
  std::vector<bool> on_boundary(s.positions.size() / 3, false);
  for (const auto& [edge, count] : edge_count) {
    if (count == 1) {
      on_boundary[edge.first] = true;
      on_boundary[edge.second] = true;
    }
  }
  return on_boundary;
}

/// Every road-mesh triangle edge as a 3D segment (endpoint pairs). The ring
/// roads' inner edges — the seam the surface stitches to — are a subset of
/// these, so distance-to-nearest is the watertightness metric.
std::vector<std::array<double, 6>> road_edges(const NetworkMesh& mesh) {
  std::vector<std::array<double, 6>> edges;
  const auto add = [&](const std::vector<double>& p, std::uint32_t a, std::uint32_t b) {
    edges.push_back(
        {p[a * 3], p[(a * 3) + 1], p[(a * 3) + 2], p[b * 3], p[(b * 3) + 1], p[(b * 3) + 2]});
  };
  for (const RoadMesh& road : mesh.roads) {
    for (const RoadMesh::LanePatch& patch : road.lanes) {
      for (std::size_t i = 0; i + 2 < patch.indices.size(); i += 3) {
        add(road.positions, patch.indices[i], patch.indices[i + 1]);
        add(road.positions, patch.indices[i + 1], patch.indices[i + 2]);
        add(road.positions, patch.indices[i + 2], patch.indices[i]);
      }
    }
  }
  return edges;
}

double point_segment_distance_3d(const std::array<double, 3>& p, const std::array<double, 6>& seg) {
  const std::array<double, 3> a{seg[0], seg[1], seg[2]};
  const std::array<double, 3> b{seg[3], seg[4], seg[5]};
  const double abx = b[0] - a[0], aby = b[1] - a[1], abz = b[2] - a[2];
  const double len2 = (abx * abx) + (aby * aby) + (abz * abz);
  double t = 0.0;
  if (len2 > 0.0) {
    t = (((p[0] - a[0]) * abx) + ((p[1] - a[1]) * aby) + ((p[2] - a[2]) * abz)) / len2;
    t = std::clamp(t, 0.0, 1.0);
  }
  const double dx = p[0] - (a[0] + (t * abx));
  const double dy = p[1] - (a[1] + (t * aby));
  const double dz = p[2] - (a[2] + (t * abz));
  return std::sqrt((dx * dx) + (dy * dy) + (dz * dz));
}

} // namespace

TEST(SurfaceMesh, SurfaceMeshIsNonEmptyForEnclosedLoop) {
  RoadNetwork network;
  author_square(network);
  derive_surfaces(network);
  ASSERT_EQ(network.surface_count(), 1U);

  const NetworkMesh mesh = roadmaker::build_network_mesh(network);
  ASSERT_EQ(mesh.surfaces.size(), 1U);
  const SubMesh& s = mesh.surfaces.front().mesh;
  EXPECT_FALSE(s.positions.empty());
  EXPECT_FALSE(s.indices.empty());
  EXPECT_GT(s.indices.size() / 3, 0U);
  EXPECT_EQ(s.positions.size(), s.normals.size());
}

TEST(SurfaceMesh, SurfaceMeshBoundaryIsWatertightToRoadBorder) {
  RoadNetwork network;
  author_square(network);
  derive_surfaces(network);

  const NetworkMesh mesh = roadmaker::build_network_mesh(network);
  ASSERT_EQ(mesh.surfaces.size(), 1U);
  const SubMesh& s = mesh.surfaces.front().mesh;
  ASSERT_FALSE(s.indices.empty());

  const std::vector<std::array<double, 6>> edges = road_edges(mesh);
  ASSERT_FALSE(edges.empty());

  // Every boundary-edge vertex of the surface lies on a road inner edge: its
  // distance to the nearest road-mesh edge is ~0 (seam is watertight, and z
  // continuity is captured because the metric is 3D).
  const std::vector<bool> on_boundary = boundary_flags(s);
  std::size_t checked = 0;
  for (std::size_t i = 0; i < on_boundary.size(); ++i) {
    if (!on_boundary[i]) {
      continue;
    }
    const std::array<double, 3> p{
        s.positions[i * 3], s.positions[(i * 3) + 1], s.positions[(i * 3) + 2]};
    double best = std::numeric_limits<double>::max();
    for (const std::array<double, 6>& seg : edges) {
      best = std::min(best, point_segment_distance_3d(p, seg));
    }
    EXPECT_LT(best, 1e-3) << "surface boundary vertex " << i << " off the road border";
    ++checked;
  }
  EXPECT_GT(checked, 0U);
}

TEST(SurfaceMesh, SurfaceMeshElevationMatchesBoundaryRoads) {
  RoadNetwork network;
  const std::vector<RoadId> roads = author_square(network);
  derive_surfaces(network);
  ASSERT_EQ(network.surface_count(), 1U);

  // Raise every bounding road onto a constant plane z = C. A harmonic field
  // with a constant Dirichlet boundary is constant everywhere.
  constexpr double kPlane = 5.0;
  for (const RoadId id : roads) {
    roadmaker::Road* road = network.road(id);
    ASSERT_NE(road, nullptr);
    road->elevation = {{.s = 0.0, .a = kPlane}};
  }

  const NetworkMesh mesh = roadmaker::build_network_mesh(network);
  ASSERT_EQ(mesh.surfaces.size(), 1U);
  const SubMesh& s = mesh.surfaces.front().mesh;
  ASSERT_FALSE(s.positions.empty());
  for (std::size_t i = 2; i < s.positions.size(); i += 3) {
    EXPECT_NEAR(s.positions[i], kPlane, 1e-6) << "surface vertex " << (i / 3) << " off the plane";
  }
}

TEST(SurfaceMesh, EmptyForDegenerateLoop) {
  RoadNetwork network;
  // Three collinear roads laid end-to-end: their footprints union into one long
  // strip with no interior hole, so the ring encloses no real area.
  const RoadId a = segment(network, "a", 0.0, 0.0, 10.0, 0.0);
  const RoadId b = segment(network, "b", 10.0, 0.0, 20.0, 0.0);
  const RoadId c = segment(network, "c", 20.0, 0.0, 30.0, 0.0);
  network.create_surface(Surface{.source = BoundarySource::Derived, .bounding_roads = {a, b, c}});
  ASSERT_EQ(network.surface_count(), 1U);

  const NetworkMesh mesh = roadmaker::build_network_mesh(network);
  // The degenerate surface produces an empty SubMesh, which the builder drops.
  EXPECT_TRUE(mesh.surfaces.empty());
}

TEST(SurfaceMesh, BuildNetworkMeshPopulatesSurfaceChannel) {
  RoadNetwork network;
  author_square(network);
  derive_surfaces(network);
  ASSERT_EQ(network.surface_count(), 1U);

  const NetworkMesh mesh = roadmaker::build_network_mesh(network);
  ASSERT_EQ(mesh.surfaces.size(), 1U);
  EXPECT_TRUE(network.surface(mesh.surfaces.front().surface) != nullptr);
  EXPECT_FALSE(mesh.surfaces.front().mesh.indices.empty());
}
