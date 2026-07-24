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

// Phase 4 (WP4) — the Manifold solid generator, exercised through the public
// remesh_bridges() entry point (the internal build_bridge_solid / manifold_bridge
// helpers are not exported from the shared kernel, and the house rule is that
// tests use the public API only). The deck sweeps along the road's own frame, so
// curved/superelevated/widening decks are tapers not discontinuities;
// guardrails/piers/abutments union in; the result is a single watertight solid;
// and a span too short to build is skipped, not slivered.

#include "roadmaker/edit/operations.hpp"
#include "roadmaker/mesh/mesh.hpp"
#include "roadmaker/mesh/mesh_builder.hpp"
#include "roadmaker/road/authoring.hpp"
#include "roadmaker/road/bridge.hpp"
#include "roadmaker/road/network.hpp"
#include "roadmaker/road/road.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <map>
#include <utility>
#include <vector>

namespace roadmaker {
namespace {

using edit::ElevationPoint;
using edit::set_elevation_profile;

RoadId road_between(RoadNetwork& network, std::vector<Waypoint> waypoints, const char* id) {
  return author_clothoid_road(network, waypoints, LaneProfile::two_lane_rural(), "", id).value();
}

/// Add a `<bridge>` record directly to a road (the command layer has its own
/// test; here we drive the generator on the record).
void add_bridge(RoadNetwork& network, RoadId road_id, double s, double length) {
  Bridge bridge;
  bridge.odr_id = "b";
  bridge.s = s;
  bridge.length = length;
  network.road(road_id)->bridges.push_back(bridge);
}

void raise_road(RoadNetwork& network, RoadId road_id, double z) {
  const double length = network.road(road_id)->plan_view.length();
  std::vector<ElevationPoint> profile{{.s = 0.0, .z = z, .grade = 0.0},
                                      {.s = length, .z = z, .grade = 0.0}};
  ASSERT_TRUE(set_elevation_profile(network, road_id, profile)->apply(network).has_value());
}

/// The first bridge solid the public generator produces for `network` (empty
/// SubMesh when none was built — e.g. a refused short span).
SubMesh bridge_solid(const RoadNetwork& network, const BridgeParams& params = {}) {
  NetworkMesh mesh;
  MeshOptions options;
  options.bridges = params;
  remesh_bridges(network, mesh, options);
  return mesh.bridges.empty() ? SubMesh{} : mesh.bridges.front().mesh;
}

/// A closed 2-manifold: welding the faceted vertices by quantized position, every
/// undirected edge is shared by exactly two triangles. The real watertightness
/// assertion on the emitted geometry.
bool is_watertight(const SubMesh& mesh) {
  if (mesh.indices.empty()) {
    return false;
  }
  const auto key = [&](std::uint32_t v) {
    const auto q = [&](double x) { return static_cast<std::int64_t>(std::llround(x * 1e5)); };
    return std::array<std::int64_t, 3>{q(mesh.positions[(v * 3) + 0]),
                                       q(mesh.positions[(v * 3) + 1]),
                                       q(mesh.positions[(v * 3) + 2])};
  };
  std::map<std::array<std::int64_t, 3>, int> ids;
  std::vector<int> canonical(mesh.positions.size() / 3);
  for (std::size_t v = 0; v < canonical.size(); ++v) {
    const auto [it, inserted] =
        ids.emplace(key(static_cast<std::uint32_t>(v)), static_cast<int>(ids.size()));
    canonical[v] = it->second;
  }
  std::map<std::pair<int, int>, int> edges;
  for (std::size_t t = 0; t + 2 < mesh.indices.size(); t += 3) {
    const int a = canonical[mesh.indices[t + 0]];
    const int b = canonical[mesh.indices[t + 1]];
    const int c = canonical[mesh.indices[t + 2]];
    for (const auto& [u, w] : {std::pair{a, b}, std::pair{b, c}, std::pair{c, a}}) {
      edges[{std::min(u, w), std::max(u, w)}]++;
    }
  }
  for (const auto& [edge, count] : edges) {
    if (count != 2) {
      return false;
    }
  }
  return true;
}

TEST(BridgeSolids, StraightSpanIsAFacetedWatertightSolidWithPlanarUVs) {
  RoadNetwork network;
  const RoadId road = road_between(network, {{0, 0}, {120, 0}}, "r");
  add_bridge(network, road, 40.0, 24.0);
  const SubMesh solid = bridge_solid(network);
  ASSERT_FALSE(solid.indices.empty());
  EXPECT_EQ(solid.positions.size() % 9, 0U); // faceted: 3 unique verts per triangle
  const std::size_t verts = solid.positions.size() / 3;
  EXPECT_EQ(solid.normals.size(), solid.positions.size());
  EXPECT_EQ(solid.uvs.size(), verts * 2); // planar UV per vertex
  for (std::size_t v = 0; v < verts; ++v) {
    const double nx = solid.normals[(v * 3) + 0];
    const double ny = solid.normals[(v * 3) + 1];
    const double nz = solid.normals[(v * 3) + 2];
    EXPECT_NEAR(std::sqrt((nx * nx) + (ny * ny) + (nz * nz)), 1.0, 1e-9);
  }
  EXPECT_TRUE(is_watertight(solid));
}

TEST(BridgeSolids, CurvedSpanStillWatertight) {
  RoadNetwork network;
  const RoadId road = road_between(network, {{0, 0}, {60, 30}, {120, 0}}, "arc");
  add_bridge(network, road, 30.0, 40.0);
  EXPECT_TRUE(is_watertight(bridge_solid(network)));
}

TEST(BridgeSolids, ShortSpanProducesNoSolid) {
  RoadNetwork network;
  const RoadId road = road_between(network, {{0, 0}, {120, 0}}, "r");
  add_bridge(network, road, 50.0, 1.0); // 1 m < 2 * 0.8 m deck depth
  EXPECT_TRUE(bridge_solid(network).indices.empty());
}

TEST(BridgeSolids, ALongerSpanGrowsGeometryFromThePierRule) {
  // The span-length pier rule is observable: a span past the pier-free length
  // gains piers, so its solid carries more geometry than a short pier-free span.
  RoadNetwork shortn;
  const RoadId sr = road_between(shortn, {{0, 0}, {200, 0}}, "s");
  add_bridge(shortn, sr, 10.0, 24.0); // < 30 m free span: no pier
  RoadNetwork longn;
  const RoadId lr = road_between(longn, {{0, 0}, {200, 0}}, "l");
  add_bridge(longn, lr, 10.0, 90.0); // > 30 m: piers appear
  EXPECT_GT(bridge_solid(longn).positions.size(), bridge_solid(shortn).positions.size());
}

TEST(BridgeSolids, ViaductOverNoFieldStillBuilds) {
  // "Bridge over nothing": no height field, so pier footings sample ground z=0.
  RoadNetwork network;
  const RoadId road = road_between(network, {{0, 0}, {120, 0}}, "r");
  raise_road(network, road, 8.0);
  add_bridge(network, road, 40.0, 40.0);
  EXPECT_TRUE(is_watertight(bridge_solid(network)));
}

TEST(BridgeSolids, DeckRidesWithTheRoadElevation) {
  RoadNetwork network;
  const RoadId road = road_between(network, {{0, 0}, {120, 0}}, "r");
  raise_road(network, road, 6.0);
  add_bridge(network, road, 40.0, 24.0);
  const SubMesh solid = bridge_solid(network);
  ASSERT_FALSE(solid.indices.empty());
  double max_z = -1e9;
  for (std::size_t v = 0; v < solid.positions.size(); v += 3) {
    max_z = std::max(max_z, solid.positions[v + 2]);
  }
  // The deck top sits at the road surface (6 m), lifted by the guardrail (1 m).
  EXPECT_NEAR(max_z, 7.0, 0.5);
}

TEST(BridgeSolids, DeterministicSolid) {
  RoadNetwork network;
  const RoadId road = road_between(network, {{0, 0}, {120, 0}}, "r");
  add_bridge(network, road, 40.0, 24.0);
  const SubMesh a = bridge_solid(network);
  const SubMesh b = bridge_solid(network);
  EXPECT_EQ(a.positions, b.positions);
  EXPECT_EQ(a.indices, b.indices);
}

} // namespace
} // namespace roadmaker
