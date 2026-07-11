#include "roadmaker/edit/snap.hpp"
#include "roadmaker/road/authoring.hpp"
#include "roadmaker/road/network.hpp"
#include "roadmaker/tol.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <numbers>
#include <stdexcept>
#include <vector>

namespace roadmaker::edit {
namespace {

RoadId make_road(RoadNetwork& network, const std::vector<Waypoint>& waypoints, std::string name) {
  auto id = author_clothoid_road(network, waypoints, LaneProfile::two_lane_default(), name);
  if (!id) {
    throw std::runtime_error(id.error().message);
  }
  return *id;
}

/// Straight road along +X from (0,0) to (100,0): endpoint headings are 0.
RoadNetwork straight_network() {
  RoadNetwork network;
  make_road(network, {{.x = 0.0, .y = 0.0}, {.x = 100.0, .y = 0.0}}, "straight");
  return network;
}

void expect_angle_near(double actual, double expected, double tolerance) {
  EXPECT_NEAR(std::remainder(actual - expected, 2.0 * std::numbers::pi), 0.0, tolerance);
}

TEST(SnapPoint, EndpointWinsOverTangentAndGrid) {
  const RoadNetwork network = straight_network();
  const auto result = snap_point(network,
                                 {.x = 99.5, .y = 0.5},
                                 {.radius = 2.0, .grid = 1.0, .endpoints = true, .tangent = true});
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->kind, SnapKind::RoadEndpoint);
  EXPECT_NEAR(result->position.x, 100.0, tol::kRoundTripPosition);
  EXPECT_NEAR(result->position.y, 0.0, tol::kRoundTripPosition);
  EXPECT_TRUE(result->road.has_value());
  // Endpoint snaps carry the continuation heading so Create Road can lock
  // the chained fit when the click lands ON the end (02 §2).
  ASSERT_TRUE(result->heading.has_value());
  expect_angle_near(*result->heading, 0.0, tol::kAngle);
}

TEST(SnapPoint, StartEndpointCarriesReversedContinuationHeading) {
  const RoadNetwork network = straight_network();
  const auto result =
      snap_point(network, {.x = 0.4, .y = -0.3}, {.radius = 2.0, .endpoints = true});
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->kind, SnapKind::RoadEndpoint);
  ASSERT_TRUE(result->heading.has_value());
  expect_angle_near(*result->heading, std::numbers::pi, tol::kAngle);
}

TEST(SnapPoint, ClosestEndpointWinsWithinKind) {
  RoadNetwork network;
  make_road(network, {{.x = 0.0, .y = 0.0}, {.x = 100.0, .y = 0.0}}, "a");
  const RoadId near_road =
      make_road(network, {{.x = 101.5, .y = 0.0}, {.x = 200.0, .y = 0.0}}, "b");

  const auto result = snap_point(network, {.x = 101.0, .y = 0.0}, {.radius = 2.0});
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->kind, SnapKind::RoadEndpoint);
  EXPECT_NEAR(result->position.x, 101.5, tol::kRoundTripPosition);
  ASSERT_TRUE(result->road.has_value());
  EXPECT_EQ(*result->road, near_road);
}

TEST(SnapPoint, TangentContinuationBeyondEndCarriesHeading) {
  const RoadNetwork network = straight_network();
  const auto result = snap_point(network, {.x = 105.0, .y = 0.5}, {.radius = 2.0});
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->kind, SnapKind::TangentContinuation);
  EXPECT_NEAR(result->position.x, 105.0, tol::kRoundTripPosition);
  EXPECT_NEAR(result->position.y, 0.0, tol::kRoundTripPosition);
  ASSERT_TRUE(result->heading.has_value());
  expect_angle_near(*result->heading, 0.0, tol::kAngle);
  EXPECT_TRUE(result->road.has_value());
}

TEST(SnapPoint, TangentAtRoadStartPointsAwayFromRoad) {
  const RoadNetwork network = straight_network();
  const auto result = snap_point(network, {.x = -5.0, .y = 0.3}, {.radius = 2.0});
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->kind, SnapKind::TangentContinuation);
  EXPECT_NEAR(result->position.x, -5.0, tol::kRoundTripPosition);
  EXPECT_NEAR(result->position.y, 0.0, tol::kRoundTripPosition);
  ASSERT_TRUE(result->heading.has_value());
  expect_angle_near(*result->heading, std::numbers::pi, tol::kAngle);
}

TEST(SnapPoint, HeadingEqualsEvaluateAtEndOnCurvedRoad) {
  RoadNetwork network;
  const RoadId road = make_road(
      network, {{.x = 0.0, .y = 0.0}, {.x = 40.0, .y = 10.0}, {.x = 80.0, .y = 40.0}}, "curved");
  const ReferenceLine& line = network.road(road)->plan_view;
  const PathPoint end = line.evaluate(line.length());

  // 3 m beyond the end along the tangent, 0.2 m off to the side: the
  // projection lands back on the tangent ray.
  const double ux = std::cos(end.hdg);
  const double uy = std::sin(end.hdg);
  const Waypoint cursor{.x = end.x + 3.0 * ux - 0.2 * uy, .y = end.y + 3.0 * uy + 0.2 * ux};

  const auto result = snap_point(network, cursor, {.radius = 2.0});
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->kind, SnapKind::TangentContinuation);
  ASSERT_TRUE(result->heading.has_value());
  expect_angle_near(*result->heading, end.hdg, tol::kAngle);
  EXPECT_NEAR(result->position.x, end.x + 3.0 * ux, tol::kRoundTripPosition);
  EXPECT_NEAR(result->position.y, end.y + 3.0 * uy, tol::kRoundTripPosition);
}

TEST(SnapPoint, NoTangentOverTheRoadBody) {
  const RoadNetwork network = straight_network();
  const auto result =
      snap_point(network, {.x = 50.0, .y = 0.5}, {.radius = 2.0, .endpoints = false});
  EXPECT_FALSE(result.has_value());
}

TEST(SnapPoint, RadiusBoundsTheCapture) {
  RoadNetwork network;
  const RoadId road =
      make_road(network, {{.x = 0.0, .y = 0.0}, {.x = 100.0, .y = 0.0}}, "straight");
  const ReferenceLine& line = network.road(road)->plan_view;
  const PathPoint end = line.evaluate(line.length());
  const SnapOptions endpoints_only{.radius = 2.0, .endpoints = true, .tangent = false};

  // Just inside and just outside the radius, measured from the road's
  // actual evaluated endpoint (margins far above double rounding noise).
  const auto inside = snap_point(network, {.x = end.x + 2.0 - 1e-6, .y = end.y}, endpoints_only);
  ASSERT_TRUE(inside.has_value());
  EXPECT_EQ(inside->kind, SnapKind::RoadEndpoint);

  const auto outside = snap_point(network, {.x = end.x + 2.0 + 1e-3, .y = end.y}, endpoints_only);
  EXPECT_FALSE(outside.has_value());
}

TEST(SnapPoint, GridSnapsWhenNothingElseInRange) {
  const RoadNetwork network = straight_network();
  const auto result = snap_point(network, {.x = 50.3, .y = 20.4}, {.radius = 2.0, .grid = 1.0});
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->kind, SnapKind::Grid);
  EXPECT_NEAR(result->position.x, 50.0, tol::kRoundTripPosition);
  EXPECT_NEAR(result->position.y, 20.0, tol::kRoundTripPosition);
  EXPECT_FALSE(result->road.has_value());
  EXPECT_FALSE(result->heading.has_value());
}

TEST(SnapPoint, GridRespectsRadius) {
  const RoadNetwork network;
  const auto result = snap_point(network, {.x = 5.0, .y = 5.0}, {.radius = 2.0, .grid = 10.0});
  EXPECT_FALSE(result.has_value());
}

TEST(SnapPoint, DisabledKindsAreSkipped) {
  const RoadNetwork network = straight_network();

  const auto nothing = snap_point(
      network, {.x = 99.9, .y = 0.1}, {.radius = 2.0, .endpoints = false, .tangent = false});
  EXPECT_FALSE(nothing.has_value());

  const auto tangent_only =
      snap_point(network, {.x = 101.0, .y = 0.2}, {.radius = 2.0, .endpoints = false});
  ASSERT_TRUE(tangent_only.has_value());
  EXPECT_EQ(tangent_only->kind, SnapKind::TangentContinuation);
}

TEST(SnapPoint, ExcludedRoadContributesNoCandidates) {
  RoadNetwork network;
  const RoadId dragged =
      make_road(network, {{.x = 0.0, .y = 0.0}, {.x = 100.0, .y = 0.0}}, "dragged");
  const RoadId other =
      make_road(network, {{.x = 101.0, .y = 5.0}, {.x = 200.0, .y = 5.0}}, "other");

  // On the dragged road's own endpoint: without the exclusion it snaps to
  // itself at distance 0 and masks the neighbour.
  const Waypoint cursor{.x = 100.0, .y = 0.0};
  const auto self = snap_point(network, cursor, {.radius = 6.0});
  ASSERT_TRUE(self.has_value());
  ASSERT_TRUE(self->road.has_value());
  EXPECT_EQ(*self->road, dragged);

  const auto excluded = snap_point(network, cursor, {.radius = 6.0, .exclude_road = dragged});
  ASSERT_TRUE(excluded.has_value());
  EXPECT_EQ(excluded->kind, SnapKind::RoadEndpoint);
  ASSERT_TRUE(excluded->road.has_value());
  EXPECT_EQ(*excluded->road, other);
  EXPECT_NEAR(excluded->position.x, 101.0, tol::kRoundTripPosition);
  EXPECT_NEAR(excluded->position.y, 5.0, tol::kRoundTripPosition);

  // Excluding the only road in range yields no snap at all.
  const auto none =
      snap_point(network, {.x = 150.0, .y = 5.2}, {.radius = 2.0, .exclude_road = other});
  EXPECT_FALSE(none.has_value());
}

TEST(SnapPoint, EmptyNetworkWithoutGridReturnsNothing) {
  const RoadNetwork network;
  const auto result = snap_point(network, {.x = 0.0, .y = 0.0}, {.radius = 2.0});
  EXPECT_FALSE(result.has_value());
}

} // namespace
} // namespace roadmaker::edit
