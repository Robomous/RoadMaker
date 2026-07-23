/*
 * Copyright 2026 Robomous
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "roadmaker/edit/operations.hpp"
#include "roadmaker/mesh/mesh_builder.hpp"
#include "roadmaker/road/authoring.hpp"
#include "roadmaker/xodr/reader.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <numbers>
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

namespace {
/// A straight 100 m road carrying one crosswalk object with `data` at s=50,
/// spanning 6 m across (u) x 3 m deep (v). Returns the built network mesh.
roadmaker::NetworkMesh mesh_with_crosswalk(const roadmaker::CrosswalkData& data) {
  roadmaker::RoadNetwork network;
  const std::vector<roadmaker::Waypoint> waypoints{roadmaker::Waypoint{0.0, 0.0},
                                                   roadmaker::Waypoint{100.0, 0.0}};
  auto road = roadmaker::author_clothoid_road(
      network, waypoints, roadmaker::LaneProfile::two_lane_default(), "", "1");
  if (!road.has_value()) {
    throw std::runtime_error("author road");
  }
  roadmaker::Object cw;
  cw.odr_id = "cw";
  cw.type = roadmaker::ObjectType::Crosswalk;
  cw.s = 50.0;
  cw.t = 0.0;
  cw.hdg = std::numbers::pi / 2.0;
  cw.length = 6.0; // crossing span along u
  cw.width = 3.0;  // walking depth along v
  cw.crosswalk = data;
  if (!roadmaker::edit::add_object(network, *road, cw)->apply(network).has_value()) {
    throw std::runtime_error("add crosswalk");
  }
  return roadmaker::build_network_mesh(network);
}

const roadmaker::SubMesh* find_crosswalk(const roadmaker::NetworkMesh& mesh) {
  for (const auto& road : mesh.roads) {
    for (const auto& marking : road.markings) {
      if (marking.name.find("crosswalk") != std::string::npos) {
        return &marking;
      }
    }
  }
  return nullptr;
}
} // namespace

TEST(Mesh, ParametricCrosswalkBarCountFollowsDashAndGap) {
  // 6 m span, 0.5 dash + 0.5 gap => cycle 1.0 => 6 bars, one quad (6 idx) each.
  const auto mesh = mesh_with_crosswalk(roadmaker::CrosswalkData{
      .dash_length = 0.5, .dash_gap = 0.5, .material = "material.paint_white"});
  const roadmaker::SubMesh* cw = find_crosswalk(mesh);
  ASSERT_NE(cw, nullptr);
  EXPECT_EQ(cw->indices.size() / 6U, 6U);
  EXPECT_EQ(cw->surface, "material.paint_white"); // material code carried for tint
}

TEST(Mesh, ParametricCrosswalkSolidIsOneQuadPlusBorders) {
  // dash 0 => one solid quad; border_width>0 => two border quads. 3 quads total.
  const auto mesh =
      mesh_with_crosswalk(roadmaker::CrosswalkData{.border_width = 0.2, .dash_length = 0.0});
  const roadmaker::SubMesh* cw = find_crosswalk(mesh);
  ASSERT_NE(cw, nullptr);
  EXPECT_EQ(cw->indices.size() / 6U, 3U); // 1 solid + 2 borders
}

TEST(Mesh, ForeignCrosswalkWithoutDataFallsBackToZebra) {
  roadmaker::RoadNetwork network;
  const std::vector<roadmaker::Waypoint> waypoints{roadmaker::Waypoint{0.0, 0.0},
                                                   roadmaker::Waypoint{100.0, 0.0}};
  auto road = roadmaker::author_clothoid_road(
      network, waypoints, roadmaker::LaneProfile::two_lane_default(), "", "1");
  ASSERT_TRUE(road.has_value());
  roadmaker::Object cw;
  cw.odr_id = "cw";
  cw.type = roadmaker::ObjectType::Crosswalk;
  cw.s = 50.0;
  cw.hdg = std::numbers::pi / 2.0;
  cw.length = 6.0;
  cw.width = 3.0; // no CrosswalkData → fallback zebra
  ASSERT_TRUE(roadmaker::edit::add_object(network, *road, cw)->apply(network).has_value());
  const auto mesh = roadmaker::build_network_mesh(network);
  const roadmaker::SubMesh* found = find_crosswalk(mesh);
  ASSERT_NE(found, nullptr);
  EXPECT_FALSE(found->indices.empty()); // still meshes as bars
  EXPECT_TRUE(found->surface.empty());  // no material code without CrosswalkData
}

TEST(Mesh, RoadGridCarriesContinuousPlanarUVs) {
  const auto network = load_sample("straight_road.xodr");
  const NetworkMesh mesh = roadmaker::build_network_mesh(network);
  ASSERT_EQ(mesh.roads.size(), 1U);
  const RoadMesh& road = mesh.roads[0];

  // One uv pair per shared-grid vertex, so every lane patch inherits UVs.
  ASSERT_FALSE(road.uvs.empty());
  ASSERT_EQ(road.uvs.size() * 3U, road.positions.size() * 2U);

  // Planar mapping is u = s (along the road), v = t (across it). straight_road
  // is flat and unrotated (heading 0, z = 0), so t coincides with world y —
  // v must equal each vertex's world y exactly. u must span the full 0..100 m
  // length from a 0 datum. Together this proves the s/t planar mapping and its
  // continuity across lane boundaries (the shared grid has one uv per vertex).
  double min_u = 1e9;
  double max_u = -1e9;
  const std::size_t vertex_count = road.positions.size() / 3;
  for (std::size_t i = 0; i < vertex_count; ++i) {
    const double u = road.uvs[i * 2];
    const double v = road.uvs[(i * 2) + 1];
    EXPECT_NEAR(v, road.positions[(i * 3) + 1], 1e-9); // v = t = world y here
    min_u = std::min(min_u, u);
    max_u = std::max(max_u, u);
  }
  EXPECT_NEAR(min_u, 0.0, 1e-9);
  EXPECT_NEAR(max_u, 100.0, 1e-9);
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

  // 3 arm meshes; the 2 connecting roads emit no lane surface of their own —
  // the junction floor IS the junction surface (issue #103, no coplanar
  // double-draw).
  EXPECT_EQ(mesh.roads.size(), 3U);
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

// WP3 (#237): a lane whose material varies along s splits into one patch per
// <material> record, each carrying its surface code, with a shared boundary
// row at the record start; a record-less lane stays one patch, empty surface.
TEST(Mesh, LaneMaterialSplitsPatchesBySurfaceCode) {
  roadmaker::RoadNetwork network;
  const std::array<roadmaker::Waypoint, 2> waypoints{roadmaker::Waypoint{.x = 0.0, .y = 0.0},
                                                     roadmaker::Waypoint{.x = 60.0, .y = 0.0}};
  const auto road_id = roadmaker::author_clothoid_road(
      network, waypoints, roadmaker::LaneProfile::two_lane_default(), "", "1");
  ASSERT_TRUE(road_id.has_value());

  const roadmaker::LaneSectionId section = network.road(*road_id)->sections[0];
  roadmaker::LaneId inner_right;
  for (const roadmaker::LaneId lane_id : network.lane_section(section)->lanes) {
    if (network.lane(lane_id)->odr_id == -1) {
      inner_right = lane_id;
    }
  }
  ASSERT_TRUE(inner_right.is_valid());

  // New asphalt for [0, 30), worn for [30, end).
  auto command = roadmaker::edit::set_lane_material(
      network,
      inner_right,
      {roadmaker::LaneMaterial{.s_offset = 0.0, .friction = 0.9, .surface = "rm:asphalt"},
       roadmaker::LaneMaterial{.s_offset = 30.0, .friction = 0.7, .surface = "rm:asphalt_worn"}});
  ASSERT_TRUE(command->apply(network).has_value());

  const NetworkMesh mesh = roadmaker::build_network_mesh(network);
  const auto road = std::ranges::find(mesh.roads, *road_id, &RoadMesh::road);
  ASSERT_NE(road, mesh.roads.end());

  std::vector<const RoadMesh::LanePatch*> right_patches;
  for (const RoadMesh::LanePatch& patch : road->lanes) {
    if (patch.odr_lane_id == -1) {
      right_patches.push_back(&patch);
    }
  }
  ASSERT_EQ(right_patches.size(), 2U);
  // Ordered by the station rows, so the new-asphalt patch comes first.
  const auto x_span = [&](const RoadMesh::LanePatch& patch) {
    double min_x = 1e9;
    double max_x = -1e9;
    for (const std::uint32_t index : patch.indices) {
      min_x = std::min(min_x, road->positions[index * 3]);
      max_x = std::max(max_x, road->positions[index * 3]);
    }
    return std::pair{min_x, max_x};
  };
  const auto [first_min, first_max] = x_span(*right_patches[0]);
  const auto [second_min, second_max] = x_span(*right_patches[1]);
  EXPECT_EQ(right_patches[0]->surface, "rm:asphalt");
  EXPECT_EQ(right_patches[1]->surface, "rm:asphalt_worn");
  // Shared boundary row at s=30: first patch ends where the second begins.
  EXPECT_NEAR(first_max, 30.0, 1e-6);
  EXPECT_NEAR(second_min, 30.0, 1e-6);
  EXPECT_NEAR(first_min, 0.0, 1e-6);
  EXPECT_NEAR(second_max, 60.0, 1e-6);

  // The other (record-less) driving lane stays one patch with an empty code.
  const auto inner_left = std::ranges::find(road->lanes, 1, &RoadMesh::LanePatch::odr_lane_id);
  ASSERT_NE(inner_left, road->lanes.end());
  EXPECT_TRUE(inner_left->surface.empty());
}
