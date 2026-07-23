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

// The scene height field (p5-s2, issue #232): the model, the bilinear sampler,
// and the generated flat field. The ABSENT-field case is the load-bearing one —
// it is what keeps every scene without terrain behaving exactly as it did.

#include "roadmaker/road/authoring.hpp"
#include "roadmaker/road/network.hpp"
#include "roadmaker/road/terrain.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <optional>

using roadmaker::field_extent;
using roadmaker::HeightField;
using roadmaker::kMaxFieldSamples;
using roadmaker::LaneProfile;
using roadmaker::make_flat_field;
using roadmaker::network_plan_bounds;
using roadmaker::RoadNetwork;
using roadmaker::sample_height;
using roadmaker::Waypoint;

namespace {

/// A 3x2 grid on a 10 m pitch with origin (0, 0). Row 0 is the LOW-y row.
///   y=10:  10  20  30
///   y=0 :   0   1   2
HeightField ramp_field() {
  HeightField field;
  field.origin_x = 0.0;
  field.origin_y = 0.0;
  field.spacing = 10.0;
  field.cols = 3;
  field.rows = 2;
  field.heights = {0.0, 1.0, 2.0, 10.0, 20.0, 30.0};
  return field;
}

RoadNetwork straight_network() {
  RoadNetwork network;
  const std::vector<Waypoint> line{{0.0, 0.0}, {100.0, 0.0}};
  [[maybe_unused]] const auto road =
      roadmaker::author_clothoid_road(network, line, LaneProfile::two_lane_rural(), "", "r0");
  return network;
}

} // namespace

TEST(HeightField, DefaultConstructedFieldIsAbsent) {
  const HeightField field;
  EXPECT_TRUE(field.empty());
  EXPECT_EQ(field.sample_count(), 0U);
  EXPECT_EQ(field_extent(field), (std::array<double, 4>{0.0, 0.0, 0.0, 0.0}));
}

TEST(HeightField, AnAbsentFieldSamplesZeroEverywhere) {
  // The whole "building the field early is invisible" argument rests on this:
  // with no field, ground height is 0 at every point, which is today's plate.
  const HeightField field;
  EXPECT_DOUBLE_EQ(sample_height(field, 0.0, 0.0), 0.0);
  EXPECT_DOUBLE_EQ(sample_height(field, 1e6, -1e6), 0.0);
  EXPECT_DOUBLE_EQ(sample_height(field, -3.25, 7.75), 0.0);
}

TEST(HeightField, SamplingHitsStoredValuesExactlyAtThePosts) {
  const HeightField field = ramp_field();
  EXPECT_DOUBLE_EQ(sample_height(field, 0.0, 0.0), 0.0);
  EXPECT_DOUBLE_EQ(sample_height(field, 10.0, 0.0), 1.0);
  EXPECT_DOUBLE_EQ(sample_height(field, 20.0, 0.0), 2.0);
  EXPECT_DOUBLE_EQ(sample_height(field, 0.0, 10.0), 10.0);
  EXPECT_DOUBLE_EQ(sample_height(field, 10.0, 10.0), 20.0);
  EXPECT_DOUBLE_EQ(sample_height(field, 20.0, 10.0), 30.0);
}

TEST(HeightField, SamplingInterpolatesBilinearlyBetweenPosts) {
  const HeightField field = ramp_field();
  // Along x on the low row: halfway between 0 and 1.
  EXPECT_DOUBLE_EQ(sample_height(field, 5.0, 0.0), 0.5);
  // Along y on the low column: halfway between 0 and 10.
  EXPECT_DOUBLE_EQ(sample_height(field, 0.0, 5.0), 5.0);
  // Cell centre: the average of its four corners (0, 1, 10, 20).
  EXPECT_DOUBLE_EQ(sample_height(field, 5.0, 5.0), (0.0 + 1.0 + 10.0 + 20.0) / 4.0);
}

TEST(HeightField, RowZeroIsTheLowYEdge) {
  // The .asc sidecar stores rows north-first and this struct stores them
  // south-first, so the direction has to be pinned down somewhere: here.
  const HeightField field = ramp_field();
  EXPECT_LT(sample_height(field, 10.0, 0.0), sample_height(field, 10.0, 10.0));
}

TEST(HeightField, SamplingClampsToTheEdgeOutsideTheExtent) {
  // The field extends FLAT past its border rather than falling to zero — a
  // scene is not suddenly at sea level one metre outside the grid.
  const HeightField field = ramp_field();
  EXPECT_DOUBLE_EQ(sample_height(field, -500.0, 0.0), 0.0);
  EXPECT_DOUBLE_EQ(sample_height(field, 500.0, 10.0), 30.0);
  EXPECT_DOUBLE_EQ(sample_height(field, 20.0, 500.0), 30.0);
  EXPECT_DOUBLE_EQ(sample_height(field, 10.0, -500.0), 1.0);
}

TEST(HeightField, AMalformedFieldSamplesZeroRatherThanReadingOutOfBounds) {
  HeightField field = ramp_field();
  field.heights.pop_back(); // size no longer rows*cols
  EXPECT_DOUBLE_EQ(sample_height(field, 5.0, 5.0), 0.0);

  HeightField zero_spacing = ramp_field();
  zero_spacing.spacing = 0.0;
  EXPECT_DOUBLE_EQ(sample_height(zero_spacing, 5.0, 5.0), 0.0);
}

TEST(HeightField, ExtentSpansThePosts) {
  const HeightField field = ramp_field();
  EXPECT_EQ(field_extent(field), (std::array<double, 4>{0.0, 0.0, 20.0, 10.0}));
}

TEST(HeightField, PlanBoundsAreNulloptWithoutGeometry) {
  const RoadNetwork empty;
  EXPECT_FALSE(network_plan_bounds(empty).has_value());
}

TEST(HeightField, PlanBoundsEncloseTheRoadSurfaceNotJustItsCenterline) {
  const RoadNetwork network = straight_network();
  const auto bounds = network_plan_bounds(network);
  ASSERT_TRUE(bounds.has_value());
  // The road runs along y = 0, so a centerline-only box would be zero-height.
  EXPECT_LT((*bounds)[1], -1.0);
  EXPECT_GT((*bounds)[3], 1.0);
  EXPECT_LE((*bounds)[0], 0.0);
  EXPECT_GE((*bounds)[2], 100.0);
}

TEST(HeightField, MakeFlatFieldCoversTheNetworkPlusTheMargin) {
  const RoadNetwork network = straight_network();
  const HeightField field = make_flat_field(network, 10.0, 50.0);
  ASSERT_FALSE(field.empty());

  const auto bounds = network_plan_bounds(network);
  ASSERT_TRUE(bounds.has_value());
  const std::array<double, 4> extent = field_extent(field);
  EXPECT_LE(extent[0], (*bounds)[0] - 50.0);
  EXPECT_LE(extent[1], (*bounds)[1] - 50.0);
  EXPECT_GE(extent[2], (*bounds)[2] + 50.0);
  EXPECT_GE(extent[3], (*bounds)[3] + 50.0);

  EXPECT_EQ(field.heights.size(), field.cols * field.rows);
  for (const double z : field.heights) {
    EXPECT_DOUBLE_EQ(z, 0.0);
  }
  // A brand-new field is flat, so it samples exactly like the absent one; that
  // is what makes "create terrain" a visually neutral act until it is edited.
  EXPECT_DOUBLE_EQ(sample_height(field, 50.0, 0.0), 0.0);
}

TEST(HeightField, MakeFlatFieldIsDeterministicAndOriginSnapped) {
  const RoadNetwork network = straight_network();
  const HeightField a = make_flat_field(network);
  const HeightField b = make_flat_field(network);
  EXPECT_EQ(a, b);
  // Origin snapped OUT to a whole multiple of the spacing.
  EXPECT_DOUBLE_EQ(std::fmod(a.origin_x, a.spacing), 0.0);
  EXPECT_DOUBLE_EQ(std::fmod(a.origin_y, a.spacing), 0.0);
}

TEST(HeightField, MakeFlatFieldReturnsAnAbsentFieldForAnEmptyNetwork) {
  const RoadNetwork empty;
  EXPECT_TRUE(make_flat_field(empty).empty());
}

TEST(HeightField, MakeFlatFieldCoarsensRatherThanCroppingAtTheSampleCap) {
  RoadNetwork network;
  // 200 km of road at the 10 m default would want 20001 posts per axis.
  const std::vector<Waypoint> line{{0.0, 0.0}, {200000.0, 0.0}};
  [[maybe_unused]] const auto road =
      roadmaker::author_clothoid_road(network, line, LaneProfile::two_lane_rural(), "", "long");

  const HeightField field = make_flat_field(network, 10.0, 50.0);
  ASSERT_FALSE(field.empty());
  EXPECT_LE(field.cols, kMaxFieldSamples);
  EXPECT_LE(field.rows, kMaxFieldSamples);
  EXPECT_GT(field.spacing, 10.0); // coarsened, not cropped

  const auto bounds = network_plan_bounds(network);
  ASSERT_TRUE(bounds.has_value());
  const std::array<double, 4> extent = field_extent(field);
  EXPECT_GE(extent[2], (*bounds)[2] + 50.0);
}
