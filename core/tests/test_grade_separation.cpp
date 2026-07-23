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

// Phase 3 (WP3) — the grade-separation query. An overpass is where two roads
// cross in plan view, differ in elevation by >= the clearance, and NO junction
// connects them; that last clause is what separates it from an intersection.

#include "roadmaker/edit/operations.hpp"
#include "roadmaker/road/authoring.hpp"
#include "roadmaker/road/grade_separation.hpp"
#include "roadmaker/road/network.hpp"
#include "roadmaker/road/road.hpp"

#include <gtest/gtest.h>

#include <array>
#include <vector>

namespace roadmaker {
namespace {

using edit::create_junction;
using edit::ElevationPoint;
using edit::set_elevation_profile;

/// A straight road from `a` to `b`, optionally raised to a constant elevation.
RoadId straight_road(RoadNetwork& network, Waypoint a, Waypoint b, double z, const char* id) {
  const std::array<Waypoint, 2> waypoints{a, b};
  const RoadId road =
      author_clothoid_road(network, waypoints, LaneProfile::two_lane_rural(), "", id).value();
  if (z != 0.0) {
    const double length = network.road(road)->plan_view.length();
    std::vector<ElevationPoint> profile{{.s = 0.0, .z = z, .grade = 0.0},
                                        {.s = length, .z = z, .grade = 0.0}};
    EXPECT_TRUE(set_elevation_profile(network, road, profile)->apply(network).has_value());
  }
  return road;
}

TEST(GradeSeparation, TruePositiveOverpassIsDetectedWithTheRightRoadOnTop) {
  RoadNetwork network;
  const RoadId high = straight_road(network, {-50, 0}, {50, 0}, 5.0, "high");
  const RoadId low = straight_road(network, {0, -50}, {0, 50}, 0.0, "low");

  const std::vector<GradeSeparation> seps = find_grade_separations(network);
  ASSERT_EQ(seps.size(), 1U);
  EXPECT_EQ(seps[0].upper, high);
  EXPECT_EQ(seps[0].lower, low);
  EXPECT_NEAR(seps[0].clearance, 5.0, 1e-6);
  EXPECT_NEAR(seps[0].s_upper, 50.0, 1.0); // crosses near its midpoint
  EXPECT_NEAR(seps[0].s_lower, 50.0, 1.0);
}

TEST(GradeSeparation, CrossingUnderTheClearanceThresholdIsNotAnOverpass) {
  RoadNetwork network;
  straight_road(network, {-50, 0}, {50, 0}, 1.0, "a"); // only 1 m above
  straight_road(network, {0, -50}, {0, 50}, 0.0, "b");
  EXPECT_TRUE(find_grade_separations(network).empty());
}

TEST(GradeSeparation, ParallelRoadsThatNeverCrossAreNotOverpasses) {
  RoadNetwork network;
  straight_road(network, {-50, 0}, {50, 0}, 5.0, "a");
  straight_road(network, {-50, 20}, {50, 20}, 0.0, "b"); // 20 m to the side
  EXPECT_TRUE(find_grade_separations(network).empty());
}

TEST(GradeSeparation, ACustomClearanceThresholdIsHonoured) {
  RoadNetwork network;
  straight_road(network, {-50, 0}, {50, 0}, 2.0, "a");
  straight_road(network, {0, -50}, {0, 50}, 0.0, "b");
  EXPECT_TRUE(find_grade_separations(network).empty());       // 2 m < default 3 m
  EXPECT_EQ(find_grade_separations(network, 1.5).size(), 1U); // 2 m >= 1.5 m
}

TEST(GradeSeparation, AJunctionConnectingTheRoadsSuppressesTheCrossing) {
  RoadNetwork network;
  // Two roads meeting end-to-end at the origin at a 4 m vertical gap: a crossing
  // the query reports UNTIL a junction ties them together.
  const RoadId a = straight_road(network, {-40, 0}, {0, 0}, 4.0, "a");
  const RoadId b = straight_road(network, {0, 0}, {0, 40}, 0.0, "b");
  ASSERT_EQ(find_grade_separations(network).size(), 1U);

  auto command = create_junction(
      network, std::array{RoadEnd{a, ContactPoint::End}, RoadEnd{b, ContactPoint::Start}});
  ASSERT_NE(command, nullptr);
  ASSERT_TRUE(command->apply(network).has_value());

  // Now a junction connects them — it is an intersection, not an overpass.
  EXPECT_TRUE(find_grade_separations(network).empty());
}

TEST(GradeSeparation, DeterministicAcrossRepeatedQueries) {
  RoadNetwork network;
  straight_road(network, {-50, 0}, {50, 0}, 5.0, "high");
  straight_road(network, {0, -50}, {0, 50}, 0.0, "low");
  EXPECT_EQ(find_grade_separations(network), find_grade_separations(network));
}

} // namespace
} // namespace roadmaker
