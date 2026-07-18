// Crosswalk placement helper (p3-s3, issue #222): the pure geometry shared by
// the Crosswalk & Stop Line tool and the Library drag-drop path. Builds a
// 3-arm junction and asserts arm resolution and pair filtering headless.

#include "roadmaker/edit/markings.hpp"
#include "roadmaker/edit/operations.hpp"
#include "roadmaker/road/network.hpp"
#include "roadmaker/road/object.hpp"
#include "roadmaker/road/road.hpp"

#include <gtest/gtest.h>

#include <set>
#include <stdexcept>
#include <vector>

#include "document/crosswalk_placement.hpp"

namespace roadmaker::editor {
namespace {

using roadmaker::ContactPoint;
using roadmaker::RoadEnd;
using roadmaker::RoadId;
using roadmaker::RoadNetwork;
using roadmaker::Waypoint;

/// Three straight two-lane arms meeting near the origin (west, east, south),
/// welded into one junction — the signalized approach layout GW-5 exercises.
RoadNetwork three_arm_junction() {
  RoadNetwork network;
  const auto arm = [&](Waypoint a, Waypoint b) {
    auto command =
        roadmaker::edit::create_road({a, b}, roadmaker::LaneProfile::two_lane_rural(), "");
    if (command == nullptr || !command->apply(network).has_value()) {
      throw std::runtime_error("arm setup failed");
    }
  };
  arm(Waypoint{-40.0, 0.0}, Waypoint{-6.0, 0.0}); // road "1", End at (-6, 0)
  arm(Waypoint{40.0, 0.0}, Waypoint{6.0, 0.0});   // road "2", End at (6, 0)
  arm(Waypoint{0.0, -40.0}, Waypoint{0.0, -6.0}); // road "3", End at (0, -6)
  const std::vector<RoadEnd> ends{{network.find_road("1"), ContactPoint::End},
                                  {network.find_road("2"), ContactPoint::End},
                                  {network.find_road("3"), ContactPoint::End}};
  auto junction = roadmaker::edit::create_junction(network, ends);
  if (junction == nullptr || !junction->apply(network).has_value()) {
    throw std::runtime_error("junction setup failed");
  }
  return network;
}

TEST(CrosswalkPlacement, NearestJunctionArmResolvesTheHoveredApproach) {
  const RoadNetwork network = three_arm_junction();
  // Hover over the west arm road, 10 m out from the junction: road "1" is the
  // approach, its junction end sits at (-6, 0).
  const auto hit = nearest_junction_arm(network, -10.0, 0.0, kCrosswalkSnapThreshold);
  ASSERT_TRUE(hit.has_value());
  EXPECT_EQ(hit->arm_road, network.find_road("1"));
  EXPECT_EQ(hit->junction, network.find_junction("1"));
  EXPECT_NEAR(hit->anchor_x, -6.0, 1e-6);
  EXPECT_NEAR(hit->anchor_y, 0.0, 1e-6);
}

TEST(CrosswalkPlacement, NearestJunctionArmRejectsOpenSpaceBeyondThreshold) {
  const RoadNetwork network = three_arm_junction();
  // 50 m north of the junction — no approach within the snap threshold.
  const auto hit = nearest_junction_arm(network, 0.0, 50.0, kCrosswalkSnapThreshold);
  EXPECT_FALSE(hit.has_value());
}

TEST(CrosswalkPlacement, PairForArmKeepsOnlyThatArmsCrosswalkAndStopLine) {
  const RoadNetwork network = three_arm_junction();
  const RoadId arm = network.find_road("1");
  const auto pair =
      crosswalk_pair_for_arm(network, network.find_junction("1"), arm, edit::CrosswalkParams{});

  // Exactly the crosswalk + its stop line, both owned by the chosen arm.
  int crosswalks = 0;
  int stop_lines = 0;
  std::set<std::string> ids;
  for (const auto& [road, object] : pair) {
    EXPECT_EQ(road, arm) << "an object leaked from another arm";
    ids.insert(object.odr_id);
    if (object.type == ObjectType::Crosswalk) {
      ++crosswalks;
    } else if (object.type_str == "roadMark" && object.subtype == "signalLines") {
      ++stop_lines;
    }
  }
  EXPECT_EQ(crosswalks, 1);
  EXPECT_EQ(stop_lines, 1);
  // Both generators seed ids from the same network, so the helper must have
  // re-numbered the batch: the two objects carry distinct odr ids.
  EXPECT_EQ(ids.size(), pair.size());
}

TEST(CrosswalkPlacement, PairForArmYieldsIdsUniqueAgainstExistingObjects) {
  RoadNetwork network = three_arm_junction();
  const RoadId arm = network.find_road("1");
  // Place the west arm's pair, then compute the east arm's pair against the
  // mutated network: its ids must not collide with the objects already added.
  const auto west =
      crosswalk_pair_for_arm(network, network.find_junction("1"), arm, edit::CrosswalkParams{});
  ASSERT_FALSE(west.empty());
  for (const auto& [road, object] : west) {
    auto command = edit::add_object(network, road, object);
    ASSERT_NE(command, nullptr);
    ASSERT_TRUE(command->apply(network).has_value());
  }
  std::set<std::string> existing;
  network.for_each_object([&](ObjectId, const Object& object) { existing.insert(object.odr_id); });

  const RoadId east = network.find_road("2");
  const auto east_pair =
      crosswalk_pair_for_arm(network, network.find_junction("1"), east, edit::CrosswalkParams{});
  ASSERT_FALSE(east_pair.empty());
  for (const auto& [road, object] : east_pair) {
    EXPECT_FALSE(existing.contains(object.odr_id)) << "id " << object.odr_id << " collides";
  }
}

} // namespace
} // namespace roadmaker::editor
