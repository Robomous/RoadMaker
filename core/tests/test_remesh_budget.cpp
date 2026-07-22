// Copyright 2026 Robomous
// SPDX-License-Identifier: Apache-2.0

// Informational benchmark for the M2 drag budget (docs/m2/01 §5): on a
// 100-road network, re-meshing ONE dirty road must fit a 16 ms preview
// update on the dev machine (Release). CI only enforces a generous ceiling
// so runner noise never gates; the real number is tracked by eye — it is
// printed on every run.

#include "roadmaker/mesh/mesh_builder.hpp"
#include "roadmaker/road/authoring.hpp"
#include "roadmaker/road/network.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <vector>

using roadmaker::LaneProfile;
using roadmaker::NetworkMesh;
using roadmaker::RoadId;
using roadmaker::RoadNetwork;
using roadmaker::Waypoint;

namespace {

/// 10x10 grid of ~200 m three-waypoint roads (curved, so spiral records and
/// the clothoid evaluator cache are exercised).
RoadNetwork make_grid_network() {
  RoadNetwork network;
  const LaneProfile profile = LaneProfile::two_lane_default();
  for (int row = 0; row < 10; ++row) {
    for (int column = 0; column < 10; ++column) {
      const double x0 = column * 250.0;
      const double y0 = row * 60.0;
      const std::array<Waypoint, 3> waypoints{
          Waypoint{.x = x0, .y = y0},
          Waypoint{.x = x0 + 100.0, .y = y0 + 12.0},
          Waypoint{.x = x0 + 200.0, .y = y0},
      };
      const auto road = roadmaker::author_clothoid_road(network, waypoints, profile);
      if (!road.has_value()) {
        throw std::runtime_error("grid authoring failed: " + road.error().message);
      }
    }
  }
  return network;
}

} // namespace

TEST(RemeshBudget, OneRoadOnAHundredRoadNetwork) {
  const RoadNetwork network = make_grid_network();
  ASSERT_EQ(network.road_count(), 100U);
  NetworkMesh mesh = roadmaker::build_network_mesh(network);
  ASSERT_EQ(mesh.roads.size(), 100U);

  const std::array<RoadId, 1> dirty{mesh.roads[42].road};

  constexpr int kRuns = 100;
  std::vector<double> samples_ms;
  samples_ms.reserve(kRuns);
  for (int run = 0; run < kRuns; ++run) {
    const auto begin = std::chrono::steady_clock::now();
    roadmaker::remesh_roads(network, mesh, dirty);
    const auto end = std::chrono::steady_clock::now();
    samples_ms.push_back(std::chrono::duration<double, std::milli>(end - begin).count());
  }
  std::ranges::nth_element(samples_ms, samples_ms.begin() + (kRuns / 2));
  const double median_ms = samples_ms[kRuns / 2];

  // Informational: the dev-machine Release target is < 16 ms.
  std::printf("[ RemeshBudget ] 1-road remesh on 100-road grid: median %.3f ms (%d runs)\n",
              median_ms,
              kRuns);

  // Generous ceiling only — debug builds and CI runners must never flake.
  EXPECT_LT(median_ms, 160.0);
}
