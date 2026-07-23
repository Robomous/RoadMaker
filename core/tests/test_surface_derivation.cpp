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

// derive_surfaces — planar face-finding that auto-forms one Surface per area
// enclosed by roads (#215, p2-s7). These tests assert the PROPERTY (how many
// surfaces, which roads bound them, id stability, determinism), not a specific
// arena layout.

#include "roadmaker/geometry/poly3.hpp"
#include "roadmaker/road/authoring.hpp"
#include "roadmaker/road/network.hpp"
#include "roadmaker/road/surface.hpp"
#include "roadmaker/road/surface_derivation.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <vector>

using roadmaker::derive_surfaces;
using roadmaker::LaneProfile;
using roadmaker::Poly3;
using roadmaker::RoadId;
using roadmaker::RoadNetwork;
using roadmaker::Surface;
using roadmaker::SurfaceId;
using roadmaker::Waypoint;

namespace {

/// Authors a straight two-lane road from (x0,y0) to (x1,y1).
RoadId
segment(RoadNetwork& network, const char* odr_id, double x0, double y0, double x1, double y1) {
  const std::vector<Waypoint> waypoints{Waypoint{.x = x0, .y = y0}, Waypoint{.x = x1, .y = y1}};
  auto road = roadmaker::author_clothoid_road(
      network, waypoints, LaneProfile::two_lane_default(), "", odr_id);
  EXPECT_TRUE(road.has_value());
  return road.value_or(RoadId{});
}

std::vector<SurfaceId> all_surfaces(const RoadNetwork& network) {
  std::vector<SurfaceId> ids;
  network.for_each_surface([&](SurfaceId id, const Surface&) { ids.push_back(id); });
  return ids;
}

/// bounding_roads of the single surface, as sorted arena indices.
std::vector<std::uint32_t> bounding_indices(const RoadNetwork& network, SurfaceId id) {
  std::vector<std::uint32_t> out;
  const Surface* surface = network.surface(id);
  EXPECT_NE(surface, nullptr);
  if (surface != nullptr) {
    for (const RoadId road : surface->bounding_roads) {
      out.push_back(road.index);
    }
  }
  std::ranges::sort(out);
  return out;
}

/// A unit square as four welded straight roads, corners (0,0)-(10,0)-(10,10)-
/// (0,10). Returns the four road ids in creation order.
std::vector<RoadId> author_square(RoadNetwork& network) {
  return {segment(network, "a", 0.0, 0.0, 10.0, 0.0),
          segment(network, "b", 10.0, 0.0, 10.0, 10.0),
          segment(network, "c", 10.0, 10.0, 0.0, 10.0),
          segment(network, "d", 0.0, 10.0, 0.0, 0.0)};
}

} // namespace

TEST(SurfaceDerivation, EnclosedLoopFormsOneSurface) {
  RoadNetwork network;
  const std::vector<RoadId> roads = author_square(network);

  derive_surfaces(network);

  ASSERT_EQ(network.surface_count(), 1U);
  const SurfaceId id = all_surfaces(network).front();
  const Surface* surface = network.surface(id);
  ASSERT_NE(surface, nullptr);
  ASSERT_EQ(surface->bounding_roads.size(), 4U);

  std::vector<std::uint32_t> got = bounding_indices(network, id);
  std::vector<std::uint32_t> want;
  for (const RoadId road : roads) {
    want.push_back(road.index);
  }
  std::ranges::sort(want);
  EXPECT_EQ(got, want);
}

TEST(SurfaceDerivation, OpenChainFormsNoSurface) {
  RoadNetwork network;
  segment(network, "a", 0.0, 0.0, 10.0, 0.0);
  segment(network, "b", 10.0, 0.0, 20.0, 0.0);
  segment(network, "c", 20.0, 0.0, 30.0, 10.0);

  derive_surfaces(network);

  EXPECT_EQ(network.surface_count(), 0U);
}

TEST(SurfaceDerivation, TwoAdjacentLoopsFormTwoSurfaces) {
  RoadNetwork network;
  // Two unit squares sharing the middle edge (10,0)-(10,10).
  segment(network, "a_bot", 0.0, 0.0, 10.0, 0.0);
  segment(network, "a_left", 0.0, 0.0, 0.0, 10.0);
  segment(network, "a_top", 0.0, 10.0, 10.0, 10.0);
  segment(network, "mid", 10.0, 0.0, 10.0, 10.0); // shared edge
  segment(network, "b_bot", 10.0, 0.0, 20.0, 0.0);
  segment(network, "b_right", 20.0, 0.0, 20.0, 10.0);
  segment(network, "b_top", 10.0, 10.0, 20.0, 10.0);

  derive_surfaces(network);

  EXPECT_EQ(network.surface_count(), 2U);
}

TEST(SurfaceDerivation, RederiveIsIdempotent) {
  RoadNetwork network;
  author_square(network);

  derive_surfaces(network);
  ASSERT_EQ(network.surface_count(), 1U);
  const std::vector<SurfaceId> first = all_surfaces(network);

  derive_surfaces(network);
  EXPECT_EQ(network.surface_count(), 1U);
  const std::vector<SurfaceId> second = all_surfaces(network);

  EXPECT_EQ(first, second); // same ids: nothing was erased or created
}

TEST(SurfaceDerivation, SurvivingLoopKeepsSurfaceId) {
  RoadNetwork network;
  const std::vector<RoadId> roads = author_square(network);

  derive_surfaces(network);
  ASSERT_EQ(network.surface_count(), 1U);
  const SurfaceId original = all_surfaces(network).front();

  // Mutate one bounding road's geometry WITHOUT changing topology or the xy
  // endpoints the face-finder welds on: a vertical elevation offset.
  roadmaker::Road* road = network.road(roads.front());
  ASSERT_NE(road, nullptr);
  road->elevation.push_back(Poly3{.s = 0.0, .a = 2.0});

  derive_surfaces(network);

  EXPECT_EQ(network.surface_count(), 1U);
  const SurfaceId after = all_surfaces(network).front();
  EXPECT_EQ(original, after);
  EXPECT_NE(network.surface(original), nullptr);
}

TEST(SurfaceDerivation, OpeningLoopErasesSurface) {
  RoadNetwork network;
  const std::vector<RoadId> roads = author_square(network);

  derive_surfaces(network);
  ASSERT_EQ(network.surface_count(), 1U);

  ASSERT_TRUE(network.erase_road(roads.front()));
  derive_surfaces(network);

  EXPECT_EQ(network.surface_count(), 0U);
}

TEST(SurfaceDerivation, DerivationIsDeterministic) {
  // Same loop, same road creation order, but every road authored in the
  // OPPOSITE direction. Canonicalization is rotation- and direction-invariant,
  // so the bounding_roads sequence must come out identical.
  RoadNetwork a;
  segment(a, "a", 0.0, 0.0, 10.0, 0.0);
  segment(a, "b", 10.0, 0.0, 10.0, 10.0);
  segment(a, "c", 10.0, 10.0, 0.0, 10.0);
  segment(a, "d", 0.0, 10.0, 0.0, 0.0);
  derive_surfaces(a);
  ASSERT_EQ(a.surface_count(), 1U);

  RoadNetwork b;
  segment(b, "a", 10.0, 0.0, 0.0, 0.0);   // reversed
  segment(b, "b", 10.0, 10.0, 10.0, 0.0); // reversed
  segment(b, "c", 0.0, 10.0, 10.0, 10.0); // reversed
  segment(b, "d", 0.0, 0.0, 0.0, 10.0);   // reversed
  derive_surfaces(b);
  ASSERT_EQ(b.surface_count(), 1U);

  const Surface* sa = a.surface(all_surfaces(a).front());
  const Surface* sb = b.surface(all_surfaces(b).front());
  ASSERT_NE(sa, nullptr);
  ASSERT_NE(sb, nullptr);

  std::vector<std::uint32_t> ka;
  for (const RoadId road : sa->bounding_roads) {
    ka.push_back(road.index);
  }
  std::vector<std::uint32_t> kb;
  for (const RoadId road : sb->bounding_roads) {
    kb.push_back(road.index);
  }
  EXPECT_EQ(ka, kb); // identical canonical ring, order included
}
