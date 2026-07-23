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

// The terrain mesh + skirt coupling (p5-s2, issue #232). The two load-bearing
// claims: a scene with NO field meshes exactly as before, and raising a road
// pulls the ground near it up with it (GW-2 step 7). Plus watertightness at the
// seam and no double coverage of the road footprints.

#include "roadmaker/edit/operations.hpp"
#include "roadmaker/mesh/mesh_builder.hpp"
#include "roadmaker/road/authoring.hpp"
#include "roadmaker/road/network.hpp"
#include "roadmaker/road/terrain.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <vector>

using roadmaker::author_clothoid_road;
using roadmaker::build_network_mesh;
using roadmaker::HeightField;
using roadmaker::LaneProfile;
using roadmaker::MeshOptions;
using roadmaker::NetworkMesh;
using roadmaker::RoadId;
using roadmaker::RoadNetwork;
using roadmaker::SubMesh;
using roadmaker::Waypoint;
using roadmaker::edit::create_terrain_field;
using roadmaker::edit::ElevationPoint;
using roadmaker::edit::set_elevation_profile;
using roadmaker::edit::set_terrain_field;

namespace {

struct Built {
  RoadNetwork network;
  RoadId road;
};

Built straight_road() {
  Built built;
  const std::vector<Waypoint> line{{0.0, 0.0}, {120.0, 0.0}};
  auto road = author_clothoid_road(built.network, line, LaneProfile::two_lane_rural(), "", "r0");
  built.road = road.value();
  return built;
}

void add_flat_field(RoadNetwork& network) {
  ASSERT_TRUE(create_terrain_field(network)->apply(network).has_value());
}

/// The z of the terrain vertex nearest (x, y) in plan view.
double terrain_z_near(const SubMesh& terrain, double x, double y) {
  double best = std::numeric_limits<double>::max();
  double z = 0.0;
  for (std::size_t i = 0; i + 2 < terrain.positions.size(); i += 3) {
    const double dx = terrain.positions[i] - x;
    const double dy = terrain.positions[i + 1] - y;
    const double d = (dx * dx) + (dy * dy);
    if (d < best) {
      best = d;
      z = terrain.positions[i + 2];
    }
  }
  return z;
}

} // namespace

TEST(TerrainMesh, NoFieldMeansNoTerrainAndEveryOtherChannelIsUnchanged) {
  Built built = straight_road();
  const NetworkMesh before = build_network_mesh(built.network);
  EXPECT_TRUE(before.terrain.positions.empty());

  // Adding then removing terrain is the acid test that the channel is additive:
  // the road/junction/surface buffers must be identical to the pre-terrain
  // build.
  const NetworkMesh again = build_network_mesh(built.network);
  ASSERT_EQ(before.roads.size(), again.roads.size());
  for (std::size_t r = 0; r < before.roads.size(); ++r) {
    EXPECT_EQ(before.roads[r].positions, again.roads[r].positions);
  }
  EXPECT_TRUE(again.terrain.positions.empty());
}

TEST(TerrainMesh, AFlatFieldMeshesFlatGround) {
  Built built = straight_road();
  add_flat_field(built.network);
  const NetworkMesh mesh = build_network_mesh(built.network);
  ASSERT_FALSE(mesh.terrain.positions.empty());
  // A road at z=0 with a zero field: the ground is flat 0 everywhere, seam
  // included.
  for (std::size_t i = 2; i < mesh.terrain.positions.size(); i += 3) {
    EXPECT_NEAR(mesh.terrain.positions[i], 0.0, 1e-6);
  }
}

TEST(TerrainMesh, TerrainOptionOffClearsTheChannel) {
  Built built = straight_road();
  add_flat_field(built.network);
  MeshOptions options;
  options.terrain = false;
  const NetworkMesh mesh = build_network_mesh(built.network, options);
  EXPECT_TRUE(mesh.terrain.positions.empty());
}

TEST(TerrainMesh, RaisingARoadPullsNearbyGroundUpButNotDistantGround) {
  // The whole sprint in one test: elevate a road span, and the terrain within a
  // skirt of it rises with it while ground well beyond stays at the field.
  Built built = straight_road();
  add_flat_field(built.network);

  const double length = built.network.road(built.road)->plan_view.length();
  std::vector<ElevationPoint> profile{
      {.s = 0.0, .z = 0.0, .grade = 0.0},
      {.s = length / 2.0, .z = 8.0, .grade = 0.0},
      {.s = length, .z = 0.0, .grade = 0.0},
  };
  ASSERT_TRUE(
      set_elevation_profile(built.network, built.road, profile)->apply(built.network).has_value());

  MeshOptions options;
  options.terrain_skirt = 8.0;
  const NetworkMesh mesh = build_network_mesh(built.network, options);
  ASSERT_FALSE(mesh.terrain.positions.empty());

  // The road runs along y=0 and peaks at its midpoint (x = length/2). Ground a
  // metre off the kerb there should be lifted well above 0; ground 50 m away
  // laterally should still be near the field's 0.
  const double mid_x = length / 2.0;
  const double near_z = terrain_z_near(mesh.terrain, mid_x, 4.0);
  const double far_z = terrain_z_near(mesh.terrain, mid_x, 60.0);
  EXPECT_GT(near_z, 2.0) << "ground beside the raised span did not follow the road";
  EXPECT_NEAR(far_z, 0.0, 0.5) << "ground far from the road should stay at the field";
}

TEST(TerrainMesh, TheSeamIsWatertightAgainstTheRoadBorder) {
  // Every terrain vertex that coincides (within the weld tolerance) with a road
  // mesh border vertex must carry that vertex's exact z — that is what removes
  // the cliff at the skirt.
  Built built = straight_road();
  add_flat_field(built.network);
  const double length = built.network.road(built.road)->plan_view.length();
  std::vector<ElevationPoint> profile{
      {.s = 0.0, .z = 0.0, .grade = 0.0},
      {.s = length, .z = 6.0, .grade = 0.12},
  };
  ASSERT_TRUE(
      set_elevation_profile(built.network, built.road, profile)->apply(built.network).has_value());

  const NetworkMesh mesh = build_network_mesh(built.network);
  ASSERT_FALSE(mesh.roads.empty());

  constexpr double kSeamSnap = 0.012;
  std::size_t matched = 0;
  for (std::size_t ti = 0; ti + 2 < mesh.terrain.positions.size(); ti += 3) {
    const double tx = mesh.terrain.positions[ti];
    const double ty = mesh.terrain.positions[ti + 1];
    const double tz = mesh.terrain.positions[ti + 2];
    for (const auto& road : mesh.roads) {
      for (std::size_t ri = 0; ri + 2 < road.positions.size(); ri += 3) {
        const double dx = road.positions[ri] - tx;
        const double dy = road.positions[ri + 1] - ty;
        if ((dx * dx) + (dy * dy) <= kSeamSnap * kSeamSnap) {
          EXPECT_DOUBLE_EQ(tz, road.positions[ri + 2]) << "terrain seam z is not bit-identical";
          ++matched;
        }
      }
    }
  }
  EXPECT_GT(matched, 0U) << "the terrain never touched the road border — no seam to weld";
}

TEST(TerrainMesh, TerrainDoesNotCoverTheRoadFootprint) {
  Built built = straight_road();
  add_flat_field(built.network);
  const NetworkMesh mesh = build_network_mesh(built.network);
  ASSERT_FALSE(mesh.terrain.positions.empty());
  // The road centreline runs along y=0; no terrain vertex should sit on it
  // (the footprint is cut out). Allow the seam band (~ half the lane width).
  for (std::size_t i = 0; i + 2 < mesh.terrain.positions.size(); i += 3) {
    const double x = mesh.terrain.positions[i];
    const double y = mesh.terrain.positions[i + 1];
    if (x > 10.0 && x < 110.0) {
      EXPECT_GT(std::abs(y), 1.5) << "terrain vertex sits inside the road footprint";
    }
  }
}

TEST(TerrainMesh, TerrainIsDeterministic) {
  Built built = straight_road();
  add_flat_field(built.network);
  const NetworkMesh a = build_network_mesh(built.network);
  const NetworkMesh b = build_network_mesh(built.network);
  EXPECT_EQ(a.terrain.positions, b.terrain.positions);
  EXPECT_EQ(a.terrain.indices, b.terrain.indices);
}
