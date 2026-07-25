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

// Auto-orientation (p6-s14, #416): the pure facing rule shared by placement and
// the "auto" action. The four side x travel-direction combinations are the
// issue's acceptance criterion, so they are spelled out one test each rather
// than table-driven — a failure names the case.

#include "roadmaker/edit/operations.hpp"
#include "roadmaker/road/authoring.hpp"
#include "roadmaker/road/defaults.hpp"
#include "roadmaker/road/junction.hpp"
#include "roadmaker/road/lane.hpp"
#include "roadmaker/road/lane_section.hpp"
#include "roadmaker/road/network.hpp"
#include "roadmaker/road/road.hpp"
#include "roadmaker/road/signal_facing.hpp"
#include "roadmaker/xodr/reader.hpp"
#include "roadmaker/xodr/writer.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <filesystem>
#include <numbers>
#include <string>
#include <utility>
#include <vector>

namespace roadmaker {
namespace {

/// A straight two-lane street along +X: one driving lane each side of the
/// reference line, so a single road already exercises both sides.
RoadId author_street(RoadNetwork& network) {
  const std::vector<Waypoint> waypoints{Waypoint{.x = 0.0, .y = 0.0},
                                        Waypoint{.x = 120.0, .y = 0.0}};
  auto road = author_clothoid_road(network, waypoints, LaneProfile::two_lane_default(), "", "1");
  if (!road.has_value()) {
    throw std::runtime_error("author_street: " + road.error().message);
  }
  return *road;
}

/// Flips every driving lane's @direction, turning the road's travel around
/// without touching its geometry. This is how the two "reversed" combinations
/// are reached: the lane grouping still says right-is-+s, the attribute
/// overrides it.
std::size_t reverse_every_driving_lane(RoadNetwork& network, RoadId road_id) {
  const Road* road = network.road(road_id);
  if (road == nullptr) {
    return 0;
  }
  std::vector<LaneId> driving;
  for (const LaneSectionId section_id : road->sections) {
    const LaneSection* section = network.lane_section(section_id);
    if (section == nullptr) {
      continue;
    }
    for (const LaneId lane_id : section->lanes) {
      const Lane* lane = network.lane(lane_id);
      if (lane != nullptr && lane->type == LaneType::Driving && lane->odr_id != 0) {
        driving.push_back(lane_id);
      }
    }
  }
  std::size_t reversed = 0;
  for (const LaneId lane_id : driving) {
    auto command = edit::set_lane_direction(network, lane_id, LaneDirection::Reversed);
    if (command != nullptr && command->apply(network).has_value()) {
      ++reversed;
    }
  }
  return reversed;
}

/// Did a real driving lane decide this facing, or did the side-only fallback?
/// The four acceptance cases assert this, because a lane-lookup bug that
/// silently degrades to the fallback answers two of them CORRECTLY by
/// coincidence — which is exactly how it hid the first time.
bool governed_by_a_lane(const RoadNetwork& network, RoadId road, double t) {
  const Expected<SignalApproach> approach = signal_approach(network, road, 60.0, t);
  return approach.has_value() && approach->has_driving_lane;
}

constexpr double kToe = defaults::kSignToeOut;

// --- the four acceptance cases ----------------------------------------------
//
// A sign faces the traffic it governs, canted AWAY from the roadway. On this
// road (+X, so road heading 0) that means a right-side sign under standard
// travel ends up looking back down -s, and a left-side one looking up +s.

TEST(SignalFacing, RightSideStandardTravelFacesOncomingPlusSTraffic) {
  RoadNetwork network;
  const RoadId road = author_street(network);

  EXPECT_TRUE(governed_by_a_lane(network, road, -5.0))
      << "this case is only meaningful if a driving lane actually decided it";
  const Expected<SignalFacing> facing = auto_signal_facing(network, road, 60.0, -5.0);
  ASSERT_TRUE(facing.has_value()) << facing.error().message;
  EXPECT_EQ(facing->orientation, ObjectOrientation::Plus)
      << "the right-hand lane runs toward +s, so the sign applies to '+' traffic";
  EXPECT_DOUBLE_EQ(facing->h_offset, kToe);
}

TEST(SignalFacing, LeftSideStandardTravelFacesOncomingMinusSTraffic) {
  RoadNetwork network;
  const RoadId road = author_street(network);

  EXPECT_TRUE(governed_by_a_lane(network, road, 5.0));
  const Expected<SignalFacing> facing = auto_signal_facing(network, road, 60.0, 5.0);
  ASSERT_TRUE(facing.has_value()) << facing.error().message;
  EXPECT_EQ(facing->orientation, ObjectOrientation::Minus)
      << "left-of-reference lanes run against +s in right-hand traffic";
  EXPECT_DOUBLE_EQ(facing->h_offset, kToe);
}

TEST(SignalFacing, RightSideReversedTravelFlipsBothOrientationAndToeOut) {
  RoadNetwork network;
  const RoadId road = author_street(network);
  ASSERT_GT(reverse_every_driving_lane(network, road), 0U);

  const Expected<SignalFacing> facing = auto_signal_facing(network, road, 60.0, -5.0);
  ASSERT_TRUE(facing.has_value()) << facing.error().message;
  EXPECT_EQ(facing->orientation, ObjectOrientation::Minus);
  EXPECT_DOUBLE_EQ(facing->h_offset, -kToe)
      << "the cant must still lean away from the roadway, so it changes sign with the traffic";
}

TEST(SignalFacing, LeftSideReversedTravelFlipsBothOrientationAndToeOut) {
  RoadNetwork network;
  const RoadId road = author_street(network);
  ASSERT_GT(reverse_every_driving_lane(network, road), 0U);

  const Expected<SignalFacing> facing = auto_signal_facing(network, road, 60.0, 5.0);
  ASSERT_TRUE(facing.has_value()) << facing.error().message;
  EXPECT_EQ(facing->orientation, ObjectOrientation::Plus);
  EXPECT_DOUBLE_EQ(facing->h_offset, -kToe);
}

// --- what the cant actually does in world terms ------------------------------

TEST(SignalFacing, TheCantLeansAwayFromTheRoadwayOnBothSides) {
  RoadNetwork network;
  const RoadId road = author_street(network);

  // World face heading, mirroring the mesher: the orientation's datum plus the
  // offset. On this +X road, +t (left) lies at +pi/2 and -t (right) at -pi/2.
  const auto face_heading = [&](double t) {
    const Expected<SignalFacing> facing = auto_signal_facing(network, road, 60.0, t);
    EXPECT_TRUE(facing.has_value());
    const double datum = facing->orientation == ObjectOrientation::Plus ? std::numbers::pi : 0.0;
    return datum + facing->h_offset;
  };

  // Right-side sign: faces back down -s (pi), canted toward -t, i.e. PAST pi
  // and on its way around to -pi/2.
  EXPECT_GT(face_heading(-5.0), std::numbers::pi);
  EXPECT_LT(face_heading(-5.0), std::numbers::pi + std::numbers::pi / 2.0);

  // Left-side sign: faces up +s (0), canted toward +t.
  EXPECT_GT(face_heading(5.0), 0.0);
  EXPECT_LT(face_heading(5.0), std::numbers::pi / 2.0);
}

TEST(SignalFacing, TheToeOutIsTheRegistrysAndNothingElse) {
  RoadNetwork network;
  const RoadId road = author_street(network);

  const Expected<SignalFacing> facing = auto_signal_facing(network, road, 60.0, -5.0);
  ASSERT_TRUE(facing.has_value());
  EXPECT_DOUBLE_EQ(std::abs(facing->h_offset), defaults::kSignToeOut)
      << "the cant must come from the realism-defaults registry, never from a local literal";
}

// --- edges ------------------------------------------------------------------

TEST(SignalFacing, AStationOnTheReferenceLineBorrowsTheGoverningLanesSide) {
  RoadNetwork network;
  const RoadId road = author_street(network);

  // t == 0 has no side of its own; the nearest driving lane supplies one, so
  // the result is still a well-formed facing rather than a coin flip.
  const Expected<SignalFacing> facing = auto_signal_facing(network, road, 60.0, 0.0);
  ASSERT_TRUE(facing.has_value()) << facing.error().message;
  EXPECT_DOUBLE_EQ(std::abs(facing->h_offset), kToe);
}

TEST(SignalFacing, ASignOnTheOuterShoulderIsGovernedByTheLaneItStandsBeside) {
  RoadNetwork network;
  const RoadId road = author_street(network);

  // Well outside the carriageway on the right. The nearest driving lane is the
  // right-hand one, not the left one across the centre line.
  const Expected<SignalFacing> facing = auto_signal_facing(network, road, 60.0, -12.0);
  ASSERT_TRUE(facing.has_value()) << facing.error().message;
  EXPECT_EQ(facing->orientation, ObjectOrientation::Plus);
  EXPECT_DOUBLE_EQ(facing->h_offset, kToe);
}

TEST(SignalFacing, ACrossSectionWithNoDrivingLaneStillYieldsAFacing) {
  RoadNetwork network;
  const RoadId road = author_street(network);
  // Retype every driving lane: nothing is left to govern the sign.
  const Road* live = network.road(road);
  ASSERT_NE(live, nullptr);
  for (const LaneSectionId section_id : live->sections) {
    const LaneSection* section = network.lane_section(section_id);
    ASSERT_NE(section, nullptr);
    for (const LaneId lane_id : section->lanes) {
      const Lane* lane = network.lane(lane_id);
      if (lane == nullptr || lane->type != LaneType::Driving) {
        continue;
      }
      auto command = edit::set_lane_type(network, lane_id, LaneType::Sidewalk);
      ASSERT_NE(command, nullptr);
      ASSERT_TRUE(command->apply(network).has_value());
    }
  }

  // Not an error: the side convention alone still says a right-side sign
  // governs +s traffic, which is what a reader expects to see.
  const Expected<SignalFacing> facing = auto_signal_facing(network, road, 60.0, -5.0);
  ASSERT_TRUE(facing.has_value()) << facing.error().message;
  EXPECT_EQ(facing->orientation, ObjectOrientation::Plus);
  EXPECT_DOUBLE_EQ(facing->h_offset, kToe);

  const Expected<SignalApproach> approach = signal_approach(network, road, 60.0, -5.0);
  ASSERT_TRUE(approach.has_value());
  EXPECT_FALSE(approach->has_driving_lane) << "the fallback must be visible to callers";
}

// --- the junction-interior rule ----------------------------------------------

TEST(SignalFacing, AConnectingRoadIsOrientedFromItsApproachNotItsInteriorLanes) {
  // A roomy four-way. Every arm runs from 80 m out to 20 m short of the centre.
  RoadNetwork network;
  const auto arm = [&](double x0, double y0, double x1, double y1, const char* id) {
    const std::vector<Waypoint> waypoints{Waypoint{.x = x0, .y = y0}, Waypoint{.x = x1, .y = y1}};
    auto road = author_clothoid_road(network, waypoints, LaneProfile::two_lane_default(), "", id);
    if (!road.has_value()) {
      throw std::runtime_error("arm: " + road.error().message);
    }
    return RoadEnd{.road = *road, .contact = ContactPoint::End};
  };
  const std::vector<RoadEnd> ends{arm(-80.0, 0.0, -20.0, 0.0, "1"),
                                  arm(80.0, 0.0, 20.0, 0.0, "2"),
                                  arm(0.0, -80.0, 0.0, -20.0, "3"),
                                  arm(0.0, 80.0, 0.0, 20.0, "4")};
  auto junction = edit::create_junction(network, ends);
  ASSERT_NE(junction, nullptr);
  ASSERT_TRUE(junction->apply(network).has_value());

  // Every connecting road must resolve from the connection that feeds it. A
  // connector's own left/right grouping is an artifact of how it was planned,
  // so reading its lanes would aim a sign off the junction interior.
  std::size_t connectors = 0;
  network.for_each_road([&](RoadId road_id, const Road& road) {
    if (!road.junction.is_valid()) {
      return;
    }
    ++connectors;
    const double mid = road.plan_view.length() / 2.0;
    const Expected<SignalApproach> approach = signal_approach(network, road_id, mid, -3.0);
    ASSERT_TRUE(approach.has_value()) << approach.error().message;
    EXPECT_TRUE(approach->from_junction_approach)
        << "a connecting road must take its direction from its approach";

    // And the direction must be the one traffic actually travels: entering at
    // the connection's contact point and running away from it.
    const Junction* owner = network.junction(road.junction);
    ASSERT_NE(owner, nullptr);
    for (const JunctionConnection& connection : owner->connections) {
      if (connection.connecting_road != road_id) {
        continue;
      }
      const TravelDirection expected = connection.contact_point == ContactPoint::Start
                                           ? TravelDirection::Forward
                                           : TravelDirection::Backward;
      EXPECT_EQ(approach->travel, expected);
    }
  });
  EXPECT_GT(connectors, 0U) << "the fixture must actually produce connecting roads";
}

TEST(SignalFacing, AnApproachRoadStillReadsItsOwnLanes) {
  // The complement of the rule above: a road that merely LEADS to a junction is
  // ordinary carriageway, and its own lanes are the right thing to read.
  RoadNetwork network;
  const RoadId road = author_street(network);

  const Expected<SignalApproach> approach = signal_approach(network, road, 60.0, -5.0);
  ASSERT_TRUE(approach.has_value());
  EXPECT_FALSE(approach->from_junction_approach);
}

TEST(SignalFacing, RejectsAStaleRoadAndAnOffRoadStation) {
  RoadNetwork network;
  const RoadId road = author_street(network);

  EXPECT_FALSE(auto_signal_facing(network, RoadId{}, 10.0, -5.0).has_value());
  EXPECT_FALSE(auto_signal_facing(network, road, -50.0, -5.0).has_value());
  EXPECT_FALSE(auto_signal_facing(network, road, 10'000.0, -5.0).has_value());
}

// --- fuzz corpus -------------------------------------------------------------
//
// Regenerate with:
//   roadmaker_core_tests --gtest_also_run_disabled_tests \
//                        --gtest_filter='SignalFacing.DISABLED_WriteCorpusSeed'
TEST(SignalFacing, DISABLED_WriteCorpusSeed) {
  namespace fs = std::filesystem;
  RoadNetwork network;
  const RoadId road = author_street(network);

  // Both orientations, both signs of hOffset, plus one signal that carries a
  // heading and no orientation at all — the three shapes the reader has to
  // survive.
  const auto place = [&](const char* odr_id, double t) {
    Signal sign;
    sign.odr_id = odr_id;
    sign.type = "R1-1";
    sign.subtype = "-1";
    sign.country = "US";
    sign.dynamic = false;
    sign.s = 20.0 * (std::stod(odr_id));
    sign.t = t;
    const SignalId id = network.add_signal(road, sign);
    auto command = edit::auto_orient_signal(network, id);
    ASSERT_NE(command, nullptr);
    ASSERT_TRUE(command->apply(network).has_value());
  };
  place("1", -5.0); // -> orientation "+", positive hOffset
  place("2", 5.0);  // -> orientation "-", positive hOffset

  Signal freehand;
  freehand.odr_id = "3";
  freehand.type = "R1-2";
  freehand.subtype = "-1";
  freehand.country = "US";
  freehand.dynamic = false;
  freehand.s = 90.0;
  freehand.t = -5.0;
  freehand.orientation = ObjectOrientation::None;
  freehand.h_offset = -0.75; // a hand-set heading: the override case
  network.add_signal(road, freehand);

  ASSERT_TRUE(roadmaker::save_xodr(
                  network, fs::path(RM_FUZZ_CORPUS_DIR) / "signal_facing.xodr", "signal_facing")
                  .has_value());
}

TEST(SignalFacing, TheFuzzCorpusSeedParsesAndReExports) {
  namespace fs = std::filesystem;
  auto seed = roadmaker::load_xodr(fs::path(RM_FUZZ_CORPUS_DIR) / "signal_facing.xodr");
  ASSERT_TRUE(seed.has_value()) << seed.error().message;
  ASSERT_EQ(seed->network.signal_count(), 3U);

  // Every facing survives the round trip exactly — including the hand-set
  // heading, which is the override rule's persistence half.
  const auto written = roadmaker::write_xodr(seed->network, "signal_facing");
  ASSERT_TRUE(written.has_value());
  const auto again = roadmaker::parse_xodr(*written, "signal_facing");
  ASSERT_TRUE(again.has_value());

  std::vector<std::pair<ObjectOrientation, double>> facings;
  again->network.for_each_signal(
      [&](SignalId, const Signal& sig) { facings.emplace_back(sig.orientation, sig.h_offset); });
  ASSERT_EQ(facings.size(), 3U);
  EXPECT_EQ(facings[0].first, ObjectOrientation::Plus);
  EXPECT_DOUBLE_EQ(facings[0].second, defaults::kSignToeOut);
  EXPECT_EQ(facings[1].first, ObjectOrientation::Minus);
  EXPECT_DOUBLE_EQ(facings[1].second, defaults::kSignToeOut);
  EXPECT_EQ(facings[2].first, ObjectOrientation::None);
  EXPECT_DOUBLE_EQ(facings[2].second, -0.75);
}

} // namespace
} // namespace roadmaker
