#include "roadmaker/edit/operations.hpp"
#include "roadmaker/mesh/mesh_builder.hpp"
#include "roadmaker/road/authoring.hpp"
#include "roadmaker/xodr/reader.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <stdexcept>

using roadmaker::NetworkMesh;
using roadmaker::RoadMesh;

namespace {

/// Throwing loader: GTest reports an uncaught exception as a test failure,
/// which is the correct behavior for a fixture-style helper.
roadmaker::RoadNetwork load_sample(const char* name) {
  auto result = roadmaker::load_xodr(std::filesystem::path(RM_SAMPLES_DIR) / name);
  if (!result) {
    throw std::runtime_error("failed to load sample: " + std::string(name));
  }
  return std::move(result->network);
}

/// Signed area of a triangle in the XY plane (positive = CCW from +Z).
double
triangle_area_xy(const std::vector<double>& p, std::uint32_t a, std::uint32_t b, std::uint32_t c) {
  const double ax = p[a * 3];
  const double ay = p[(a * 3) + 1];
  const double bx = p[b * 3];
  const double by = p[(b * 3) + 1];
  const double cx = p[c * 3];
  const double cy = p[(c * 3) + 1];
  return 0.5 * (((bx - ax) * (cy - ay)) - ((cx - ax) * (by - ay)));
}

double patch_area_xy(const std::vector<double>& positions,
                     const std::vector<std::uint32_t>& indices) {
  double area = 0.0;
  for (std::size_t i = 0; i + 2 < indices.size(); i += 3) {
    area += triangle_area_xy(positions, indices[i], indices[i + 1], indices[i + 2]);
  }
  return area;
}

} // namespace

TEST(Mesh, StraightRoadMeshesWithCorrectExtentsAndAreas) {
  const auto network = load_sample("straight_road.xodr");
  const NetworkMesh mesh = roadmaker::build_network_mesh(network);

  ASSERT_EQ(mesh.roads.size(), 1U);
  const RoadMesh& road = mesh.roads[0];

  // 4 lanes with area (sidewalk, driving x2, shoulder); center lane has none.
  ASSERT_EQ(road.lanes.size(), 4U);

  // Lateral extent: left sidewalk outer edge at +5.5, right shoulder at -5.
  double min_y = 1e9;
  double max_y = -1e9;
  for (std::size_t i = 0; i + 2 < road.positions.size(); i += 3) {
    min_y = std::min(min_y, road.positions[i + 1]);
    max_y = std::max(max_y, road.positions[i + 1]);
  }
  EXPECT_NEAR(max_y, 5.5, 1e-9);
  EXPECT_NEAR(min_y, -5.0, 1e-9);

  // Flat road: all z = 0, all normals point up.
  for (std::size_t i = 2; i < road.positions.size(); i += 3) {
    ASSERT_NEAR(road.positions[i], 0.0, 1e-12);
  }
  for (std::size_t i = 2; i < road.normals.size(); i += 3) {
    ASSERT_NEAR(road.normals[i], 1.0, 1e-12);
  }

  // Per-lane plan-view areas match width x length (and CCW everywhere).
  for (const RoadMesh::LanePatch& patch : road.lanes) {
    const double area = patch_area_xy(road.positions, patch.indices);
    EXPECT_GT(area, 0.0);
    double expected = 0.0;
    switch (patch.odr_lane_id) {
    case 2:
      expected = 2.0 * 100.0;
      break;
    case 1:
    case -1:
      expected = 3.5 * 100.0;
      break;
    case -2:
      expected = 1.5 * 100.0;
      break;
    default:
      FAIL() << "unexpected lane patch " << patch.odr_lane_id;
    }
    EXPECT_NEAR(area, expected, 1e-6);
  }

  // Markings: center broken line + two solid edge lines.
  ASSERT_EQ(road.markings.size(), 3U);
  const auto broken = std::ranges::find_if(road.markings, [](const roadmaker::SubMesh& m) {
    return m.name.find("lane 0") != std::string::npos;
  });
  ASSERT_NE(broken, road.markings.end());
  // 100 m at 3 m dash / 9 m cycle => ~12 dashes, one quad (6 indices) each.
  EXPECT_GE(broken->indices.size(), 11U * 6U);
  const auto solid = std::ranges::find_if(road.markings, [](const roadmaker::SubMesh& m) {
    return m.name.find("lane 1") != std::string::npos;
  });
  ASSERT_NE(solid, road.markings.end());
  // Solid strip area ~ width x length.
  EXPECT_NEAR(patch_area_xy(solid->positions, solid->indices), 0.12 * 100.0, 1e-3);
}

TEST(Mesh, CurvedRoadAppliesElevationAndSuperelevation) {
  const auto network = load_sample("curved_road.xodr");
  const NetworkMesh mesh = roadmaker::build_network_mesh(network);
  ASSERT_EQ(mesh.roads.size(), 1U);
  const RoadMesh& road = mesh.roads[0];

  // Elevation: z rises along the road (b = 0.02 at s=0).
  double max_z = -1e9;
  for (std::size_t i = 2; i < road.positions.size(); i += 3) {
    max_z = std::max(max_z, road.positions[i]);
  }
  EXPECT_GT(max_z, 0.5);

  // Superelevation beyond s=30 tilts normals off vertical somewhere.
  bool tilted = false;
  for (std::size_t i = 2; i < road.normals.size(); i += 3) {
    if (road.normals[i] < 1.0 - 1e-6) {
      tilted = true;
      break;
    }
  }
  EXPECT_TRUE(tilted);
}

TEST(Mesh, TJunctionBuildsOneJunctionFloor) {
  const auto network = load_sample("t_junction.xodr");
  const NetworkMesh mesh = roadmaker::build_network_mesh(network);

  EXPECT_EQ(mesh.roads.size(), 5U);
  ASSERT_EQ(mesh.junction_floors.size(), 1U);

  EXPECT_TRUE(mesh.junction_floors[0].junction.is_valid());
  const roadmaker::SubMesh& floor = mesh.junction_floors[0].mesh;
  ASSERT_FALSE(floor.indices.empty());

  // The surface must cover at least the through-connection footprint
  // (20 m x 3.5 m) and, on this flat network, sit at road elevation (z = 0)
  // rather than the old dropped floor.
  EXPECT_GT(patch_area_xy(floor.positions, floor.indices), 20.0 * 3.5 * 0.9);
  for (std::size_t i = 2; i < floor.positions.size(); i += 3) {
    EXPECT_NEAR(floor.positions[i], 0.0, 1e-9);
  }
  // Height field: every normal points into the +Z hemisphere.
  for (std::size_t i = 2; i < floor.normals.size(); i += 3) {
    EXPECT_GT(floor.normals[i], 0.0);
  }
}

TEST(Mesh, OptionsCanDisableMarkingsAndFloors) {
  const auto network = load_sample("t_junction.xodr");
  roadmaker::MeshOptions options;
  options.markings = false;
  options.junction_floors = false;
  const NetworkMesh mesh = roadmaker::build_network_mesh(network, options);
  EXPECT_TRUE(mesh.junction_floors.empty());
  for (const RoadMesh& road : mesh.roads) {
    EXPECT_TRUE(road.markings.empty());
  }
}

TEST(RemeshRoads, TouchesOnlyListedRoadsAndMatchesFullRebuild) {
  auto network = load_sample("t_junction.xodr");
  NetworkMesh mesh = roadmaker::build_network_mesh(network);
  ASSERT_GE(mesh.roads.size(), 2U);

  const roadmaker::RoadId edited = mesh.roads.front().road;
  // Untouched roads must keep their exact heap buffers (pointer identity).
  std::vector<std::pair<roadmaker::RoadId, const double*>> untouched;
  for (const RoadMesh& road : mesh.roads) {
    if (road.road != edited) {
      untouched.emplace_back(road.road, road.positions.data());
    }
  }

  network.road(edited)->elevation = {{.s = 0.0, .a = 2.5}};
  const std::array<roadmaker::RoadId, 1> dirty{edited};
  roadmaker::remesh_roads(network, mesh, dirty);

  // The edited road was rebuilt with the new elevation...
  const auto rebuilt = std::ranges::find(mesh.roads, edited, &RoadMesh::road);
  ASSERT_NE(rebuilt, mesh.roads.end());
  EXPECT_NEAR(rebuilt->positions[2], 2.5, 1e-9);

  // ...every other road kept its buffers, and the result equals a full
  // rebuild entry-for-entry.
  for (const auto& [id, data] : untouched) {
    const auto road = std::ranges::find(mesh.roads, id, &RoadMesh::road);
    ASSERT_NE(road, mesh.roads.end());
    EXPECT_EQ(road->positions.data(), data);
  }
  const NetworkMesh full = roadmaker::build_network_mesh(network);
  ASSERT_EQ(mesh.roads.size(), full.roads.size());
  for (const RoadMesh& road : full.roads) {
    const auto incremental = std::ranges::find(mesh.roads, road.road, &RoadMesh::road);
    ASSERT_NE(incremental, mesh.roads.end());
    EXPECT_EQ(incremental->positions, road.positions);
  }
}

/// The issue #14 criterion: a lane width edit shows up in the tessellation at
/// a station, driven through the command + incremental re-mesh path the
/// editor panel uses.
TEST(RemeshRoads, ReflectsALaneWidthEditAtStation) {
  roadmaker::RoadNetwork network;
  const std::array<roadmaker::Waypoint, 2> waypoints{roadmaker::Waypoint{.x = 0.0, .y = 0.0},
                                                     roadmaker::Waypoint{.x = 60.0, .y = 0.0}};
  const auto road_id = roadmaker::author_clothoid_road(
      network, waypoints, roadmaker::LaneProfile::two_lane_default(), "", "1");
  ASSERT_TRUE(road_id.has_value());
  NetworkMesh mesh = roadmaker::build_network_mesh(network);

  // Widen the inner right driving lane (-1) from 3.5 m to 5.0 m.
  const roadmaker::LaneSectionId section = network.road(*road_id)->sections[0];
  roadmaker::LaneId inner_right;
  for (const roadmaker::LaneId lane_id : network.lane_section(section)->lanes) {
    if (network.lane(lane_id)->odr_id == -1) {
      inner_right = lane_id;
    }
  }
  auto command = roadmaker::edit::set_lane_width(network, inner_right, 5.0);
  ASSERT_TRUE(command->apply(network).has_value());
  const std::array<roadmaker::RoadId, 1> dirty{*road_id};
  roadmaker::remesh_roads(network, mesh, dirty);

  // At station s=30 the lane's outer boundary now sits at t=-5 (was -3.5).
  const auto road = std::ranges::find(mesh.roads, *road_id, &RoadMesh::road);
  ASSERT_NE(road, mesh.roads.end());
  const auto patch = std::ranges::find(road->lanes, -1, &RoadMesh::LanePatch::odr_lane_id);
  ASSERT_NE(patch, road->lanes.end());
  double min_y_at_station = 0.0;
  bool sampled = false;
  for (const std::uint32_t index : patch->indices) {
    const double x = road->positions[index * 3];
    const double y = road->positions[(index * 3) + 1];
    if (std::abs(x - 30.0) < 2.0) {
      min_y_at_station = std::min(min_y_at_station, y);
      sampled = true;
    }
  }
  ASSERT_TRUE(sampled);
  EXPECT_NEAR(min_y_at_station, -5.0, 1e-9);
}

TEST(RemeshRoads, AppendsNewRoadsAndRemovesErasedOnes) {
  auto network = load_sample("t_junction.xodr");
  NetworkMesh mesh = roadmaker::build_network_mesh(network);
  const std::size_t original_count = mesh.roads.size();

  // A road erased from the network disappears from the mesh.
  const roadmaker::RoadId doomed = mesh.roads.back().road;
  ASSERT_TRUE(network.erase_road(doomed));
  const std::array<roadmaker::RoadId, 1> erased{doomed};
  roadmaker::remesh_roads(network, mesh, erased);
  EXPECT_EQ(mesh.roads.size(), original_count - 1);
  EXPECT_EQ(std::ranges::find(mesh.roads, doomed, &RoadMesh::road), mesh.roads.end());

  // A stale id in the dirty set is a no-op, not a crash.
  roadmaker::remesh_roads(network, mesh, erased);
  EXPECT_EQ(mesh.roads.size(), original_count - 1);
}

TEST(RemeshJunctions, RegeneratesAndRemovesFloors) {
  auto network = load_sample("t_junction.xodr");
  NetworkMesh mesh = roadmaker::build_network_mesh(network);
  ASSERT_EQ(mesh.junction_floors.size(), 1U);
  const roadmaker::JunctionId junction = mesh.junction_floors[0].junction;
  const double old_z = mesh.junction_floors[0].mesh.positions[2];

  // Raising every connecting road's elevation by a constant shifts the whole
  // regenerated surface by that constant (the harmonic field is linear in the
  // Dirichlet boundary values).
  network.for_each_road([&](roadmaker::RoadId, roadmaker::Road& road) {
    if (road.junction == junction) {
      road.elevation = {{.s = 0.0, .a = 3.0}};
    }
  });
  const std::array<roadmaker::JunctionId, 1> dirty{junction};
  roadmaker::remesh_junctions(network, mesh, dirty);
  ASSERT_EQ(mesh.junction_floors.size(), 1U);
  EXPECT_NEAR(mesh.junction_floors[0].mesh.positions[2], old_z + 3.0, 1e-9);

  // An erased junction loses its floor.
  ASSERT_TRUE(network.erase_junction(junction));
  roadmaker::remesh_junctions(network, mesh, dirty);
  EXPECT_TRUE(mesh.junction_floors.empty());
}

TEST(Mesh, DegenerateNetworksProduceEmptyMeshes) {
  const roadmaker::RoadNetwork network;
  const NetworkMesh mesh = roadmaker::build_network_mesh(network);
  EXPECT_TRUE(mesh.roads.empty());
  EXPECT_TRUE(mesh.junction_floors.empty());
}
