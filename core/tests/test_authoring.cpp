#include "roadmaker/geometry/reference_line.hpp"
#include "roadmaker/road/authoring.hpp"
#include "roadmaker/tol.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <numbers>
#include <vector>

using roadmaker::EndpointHeadings;
using roadmaker::LaneProfile;
using roadmaker::LaneSpec;
using roadmaker::LaneType;
using roadmaker::ReferenceLine;
using roadmaker::Waypoint;

namespace {

void expect_lane(const LaneSpec& lane, LaneType type, double width, bool outer_marking) {
  EXPECT_EQ(lane.type, type);
  EXPECT_DOUBLE_EQ(lane.width, width);
  EXPECT_EQ(lane.outer_marking, outer_marking);
}

void expect_angle_near(double actual, double expected, double tolerance) {
  EXPECT_NEAR(std::remainder(actual - expected, 2.0 * std::numbers::pi), 0.0, tolerance);
}

const std::vector<Waypoint> kBend = {
    Waypoint{.x = 0.0, .y = 0.0}, Waypoint{.x = 60.0, .y = 10.0}, Waypoint{.x = 120.0, .y = 0.0}};

// Template contents are product behavior (02_editing_tools.md §2): a changed
// lane count, type, width, or marking must fail a test, not slip through.

TEST(LaneProfileTemplates, TwoLaneRuralContents) {
  const LaneProfile profile = LaneProfile::two_lane_rural();
  ASSERT_EQ(profile.left.size(), 1U);
  ASSERT_EQ(profile.right.size(), 2U);
  expect_lane(profile.left[0], LaneType::Driving, 3.5, true);
  expect_lane(profile.right[0], LaneType::Driving, 3.5, true);
  expect_lane(profile.right[1], LaneType::Shoulder, 1.0, false);
  EXPECT_TRUE(profile.center_marking);
}

TEST(LaneProfileTemplates, UrbanSidewalkContents) {
  const LaneProfile profile = LaneProfile::urban_sidewalk();
  ASSERT_EQ(profile.left.size(), 2U);
  ASSERT_EQ(profile.right.size(), 2U);
  for (const auto& side : {profile.left, profile.right}) {
    expect_lane(side[0], LaneType::Driving, 3.5, true);
    expect_lane(side[1], LaneType::Sidewalk, 2.0, false);
  }
  EXPECT_TRUE(profile.center_marking);
}

TEST(LaneProfileTemplates, HighwayContents) {
  const LaneProfile profile = LaneProfile::highway();
  ASSERT_EQ(profile.left.size(), 3U);
  ASSERT_EQ(profile.right.size(), 3U);
  for (const auto& side : {profile.left, profile.right}) {
    expect_lane(side[0], LaneType::Driving, 3.75, false);
    expect_lane(side[1], LaneType::Driving, 3.75, true);
    expect_lane(side[2], LaneType::Shoulder, 2.5, false);
  }
  EXPECT_FALSE(profile.center_marking);
}

TEST(LaneProfileTemplates, TwoLaneDefaultIsTheRuralTemplate) {
  const LaneProfile alias = LaneProfile::two_lane_default();
  const LaneProfile rural = LaneProfile::two_lane_rural();
  ASSERT_EQ(alias.left.size(), rural.left.size());
  ASSERT_EQ(alias.right.size(), rural.right.size());
  for (std::size_t i = 0; i < rural.left.size(); ++i) {
    expect_lane(
        alias.left[i], rural.left[i].type, rural.left[i].width, rural.left[i].outer_marking);
  }
  for (std::size_t i = 0; i < rural.right.size(); ++i) {
    expect_lane(
        alias.right[i], rural.right[i].type, rural.right[i].width, rural.right[i].outer_marking);
  }
  EXPECT_EQ(alias.center_marking, rural.center_marking);
}

// fit_clothoid_path with EndpointHeadings — the Create Road tangent-snap fit.

TEST(FitClothoidPath, UnlockedEndpointHeadingsMatchThePointOnlyFit) {
  const auto plain = roadmaker::fit_clothoid_path(kBend);
  const auto locked = roadmaker::fit_clothoid_path(kBend, EndpointHeadings{});
  ASSERT_TRUE(plain.has_value());
  ASSERT_TRUE(locked.has_value());
  ASSERT_EQ(plain->records().size(), locked->records().size());
  EXPECT_NEAR(plain->length(), locked->length(), roadmaker::tol::kLength);
  const auto plain_end = plain->evaluate(plain->length());
  const auto locked_end = locked->evaluate(locked->length());
  EXPECT_NEAR(plain_end.x, locked_end.x, roadmaker::tol::kRoundTripPosition);
  EXPECT_NEAR(plain_end.y, locked_end.y, roadmaker::tol::kRoundTripPosition);
}

TEST(FitClothoidPath, LockedStartHeadingIsInterpolatedExactly) {
  const double heading = 0.4;
  const auto line = roadmaker::fit_clothoid_path(kBend, {.start = heading});
  ASSERT_TRUE(line.has_value());
  expect_angle_near(line->evaluate(0.0).hdg, heading, roadmaker::tol::kAngle);
  // The fit still passes through every waypoint.
  const auto end = line->evaluate(line->length());
  EXPECT_NEAR(line->evaluate(0.0).x, kBend.front().x, roadmaker::tol::kRoundTripPosition);
  EXPECT_NEAR(end.x, kBend.back().x, roadmaker::tol::kRoundTripPosition);
  EXPECT_NEAR(end.y, kBend.back().y, roadmaker::tol::kRoundTripPosition);
}

TEST(FitClothoidPath, LockedEndHeadingIsInterpolatedExactly) {
  const double heading = -0.3;
  const auto line = roadmaker::fit_clothoid_path(kBend, {.end = heading});
  ASSERT_TRUE(line.has_value());
  expect_angle_near(line->evaluate(line->length()).hdg, heading, roadmaker::tol::kAngle);
}

TEST(FitClothoidPath, LockedBothEndsInterpolateBothExactly) {
  const auto line = roadmaker::fit_clothoid_path(kBend, {.start = 0.5, .end = -0.5});
  ASSERT_TRUE(line.has_value());
  expect_angle_near(line->evaluate(0.0).hdg, 0.5, roadmaker::tol::kAngle);
  expect_angle_near(line->evaluate(line->length()).hdg, -0.5, roadmaker::tol::kAngle);
}

TEST(FitClothoidPath, LockedFitRejectsDegenerateInputLikeThePointOnlyFit) {
  const std::vector<Waypoint> coincident = {Waypoint{.x = 1.0, .y = 1.0},
                                            Waypoint{.x = 1.0, .y = 1.0}};
  EXPECT_FALSE(
      roadmaker::fit_clothoid_path(coincident, EndpointHeadings{.start = 0.0}).has_value());
  const std::vector<Waypoint> single = {Waypoint{.x = 0.0, .y = 0.0}};
  EXPECT_FALSE(roadmaker::fit_clothoid_path(single, EndpointHeadings{.start = 0.0}).has_value());
}

} // namespace
