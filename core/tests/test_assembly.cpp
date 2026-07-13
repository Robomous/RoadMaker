#include "roadmaker/edit/assembly.hpp"
#include "roadmaker/road/authoring.hpp"
#include "roadmaker/road/network.hpp"
#include "roadmaker/xodr/diagnostic.hpp"
#include "roadmaker/xodr/writer.hpp"

#include <gtest/gtest.h>

#include <memory>
#include <string>

#include "support/network_compare.hpp"

namespace {

using roadmaker::count_errors;
using roadmaker::LaneProfile;
using roadmaker::RoadNetwork;
using roadmaker::validate_network;
using roadmaker::edit::Command;
using roadmaker::edit::assembly::IntersectionParams;
using roadmaker::edit::assembly::Pose;
using roadmaker::edit::assembly::t_intersection;
using roadmaker::edit::assembly::x_intersection;
using roadmaker::test::expect_network_matches;
using roadmaker::test::snapshot_xodr;

/// apply→revert byte-identical, re-apply reproduces, final revert pristine —
/// the M2 command oracle (see test_edit_operations.cpp).
void expect_round_trip(RoadNetwork& network, Command& command) {
  const std::string before = snapshot_xodr(network);
  ASSERT_TRUE(command.apply(network).has_value());
  const std::string after = snapshot_xodr(network);
  EXPECT_NE(before, after);
  ASSERT_TRUE(command.revert(network).has_value());
  expect_network_matches(network, before);
  ASSERT_TRUE(command.apply(network).has_value());
  expect_network_matches(network, after);
  ASSERT_TRUE(command.revert(network).has_value());
  expect_network_matches(network, before);
}

TEST(Assembly, TIntersectionIsValidWithOneJunctionAndThreeArms) {
  RoadNetwork network;
  auto command = t_intersection(network, Pose{.x = 0.0, .y = 0.0, .heading = 0.0});
  ASSERT_TRUE(command->apply(network).has_value());
  EXPECT_EQ(count_errors(validate_network(network)), 0U);
  EXPECT_EQ(network.junction_count(), 1U);
  EXPECT_GE(network.road_count(), 3U); // 3 arms + generated connecting roads
  // Undo removes exactly what it created.
  ASSERT_TRUE(command->revert(network).has_value());
  EXPECT_EQ(network.road_count(), 0U);
  EXPECT_EQ(network.junction_count(), 0U);
}

TEST(Assembly, TIntersectionRoundTripsByteIdentical) {
  RoadNetwork network;
  auto command = t_intersection(network, Pose{.heading = 0.3});
  expect_round_trip(network, *command);
}

TEST(Assembly, XIntersectionIsValidWithOneJunctionAndFourArms) {
  RoadNetwork network;
  auto command = x_intersection(network, Pose{});
  ASSERT_TRUE(command->apply(network).has_value());
  EXPECT_EQ(count_errors(validate_network(network)), 0U);
  EXPECT_EQ(network.junction_count(), 1U);
  EXPECT_GE(network.road_count(), 4U);
}

TEST(Assembly, XIntersectionRoundTripsByteIdentical) {
  RoadNetwork network;
  auto command = x_intersection(network, Pose{.x = 20.0, .y = -5.0, .heading = 1.1});
  expect_round_trip(network, *command);
}

TEST(Assembly, HonorsAnExplicitGapAndArmLength) {
  RoadNetwork network;
  IntersectionParams params;
  params.gap_m = 10.0;
  params.arm_length_m = 25.0;
  auto command = x_intersection(network, Pose{}, params);
  ASSERT_TRUE(command->apply(network).has_value());
  EXPECT_EQ(count_errors(validate_network(network)), 0U);
}

TEST(Assembly, RejectsNonPositiveArmLength) {
  RoadNetwork network;
  IntersectionParams params;
  params.arm_length_m = 0.0;
  auto command = t_intersection(network, Pose{}, params);
  EXPECT_FALSE(command->apply(network).has_value());
  EXPECT_EQ(network.road_count(), 0U); // failed apply leaves the network untouched
}

TEST(Assembly, RejectsEmptyLaneProfile) {
  RoadNetwork network;
  IntersectionParams params;
  params.profile = LaneProfile{}; // no lanes either side
  auto command = x_intersection(network, Pose{}, params);
  EXPECT_FALSE(command->apply(network).has_value());
  EXPECT_EQ(network.road_count(), 0U);
}

} // namespace
