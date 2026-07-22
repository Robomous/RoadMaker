// Copyright 2026 Robomous
// SPDX-License-Identifier: Apache-2.0

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

TEST(CrosswalkPlacement, ForArmKeepsOnlyThatArmsCrosswalk) {
  const RoadNetwork network = three_arm_junction();
  const RoadId arm = network.find_road("1");
  const auto placed =
      crosswalk_for_arm(network, network.find_junction("1"), arm, edit::CrosswalkParams{});

  // Exactly the crosswalk, owned by the chosen arm. The companion stop line is
  // no longer an object at all (p4-s3, #318): the arm already has a derived one,
  // and the caller links it with edit::set_stopline_distance in the same macro.
  ASSERT_TRUE(placed.has_value());
  EXPECT_EQ(placed->first, arm) << "an object leaked from another arm";
  EXPECT_EQ(placed->second.type, ObjectType::Crosswalk);
}

TEST(CrosswalkPlacement, ForArmYieldsAnIdUniqueAgainstExistingObjects) {
  RoadNetwork network = three_arm_junction();
  const RoadId arm = network.find_road("1");
  // Place the west arm's crosswalk, then compute the east arm's against the
  // mutated network: its id must not collide with the object already added.
  const auto west =
      crosswalk_for_arm(network, network.find_junction("1"), arm, edit::CrosswalkParams{});
  ASSERT_TRUE(west.has_value());
  auto command = edit::add_object(network, west->first, west->second);
  ASSERT_NE(command, nullptr);
  ASSERT_TRUE(command->apply(network).has_value());

  std::set<std::string> existing;
  network.for_each_object([&](ObjectId, const Object& object) { existing.insert(object.odr_id); });

  const RoadId east = network.find_road("2");
  const auto east_placed =
      crosswalk_for_arm(network, network.find_junction("1"), east, edit::CrosswalkParams{});
  ASSERT_TRUE(east_placed.has_value());
  EXPECT_FALSE(existing.contains(east_placed->second.odr_id))
      << "id " << east_placed->second.odr_id << " collides";
}

TEST(CrosswalkPlacement, NearestJunctionArmReportsWhichEndTouchesTheJunction) {
  const RoadNetwork network = three_arm_junction();
  // The west arm runs -40 -> -6, so it meets the junction at its End; the tool
  // needs that contact to name the arm's stop line.
  const auto hit = nearest_junction_arm(network, -20.0, 0.0, kCrosswalkSnapThreshold);
  ASSERT_TRUE(hit.has_value());
  EXPECT_EQ(hit->arm_road, network.find_road("1"));
  EXPECT_EQ(hit->contact, ContactPoint::End);
}

} // namespace
} // namespace roadmaker::editor
