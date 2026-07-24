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

// The realism-defaults registry is a single source of truth:
// docs/domain/realism_defaults.md is the canonical spec, defaults.hpp its one
// code mirror, and the authoring templates / road styles / marking constants
// all derive from the mirror. These tests are what keeps the doc and the code
// in step — the shortcut_registry mechanism (#413): the committed doc's
// rm-defaults tables must be exactly what the registry renders, so a value
// changed in one place without the other fails CI, not review.

#include "roadmaker/road/authoring.hpp"
#include "roadmaker/road/defaults.hpp"
#include "roadmaker/road/lane.hpp"
#include "roadmaker/road/road_style.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace roadmaker {
namespace {

using defaults::RoadClass;

std::string committed_spec_doc() {
  const std::filesystem::path page =
      std::filesystem::path(RM_DOCS_DIR) / "domain" / "realism_defaults.md";
  std::ifstream file(page);
  EXPECT_TRUE(file.is_open()) << "missing " << page.string();
  std::stringstream buffer;
  buffer << file.rdbuf();
  return buffer.str();
}

// The gate: the committed §1.2 table must be what the registry renders. A
// default changed without regenerating the doc (or vice versa) fails HERE
// rather than shipping a doc that lies about the product.
TEST(DefaultsRegistry, DocCrossSectionTableMatchesRegistry) {
  const std::string generated = defaults::cross_section_markdown();
  EXPECT_TRUE(committed_spec_doc().find(generated) != std::string::npos)
      << "docs/domain/realism_defaults.md §1.2 is out of date with the defaults "
         "registry.\nRegenerate the marked table from "
         "defaults::cross_section_markdown():\n\n"
      << generated;
}

TEST(DefaultsRegistry, DocMarkingsTableMatchesRegistry) {
  const std::string generated = defaults::markings_markdown();
  EXPECT_TRUE(committed_spec_doc().find(generated) != std::string::npos)
      << "docs/domain/realism_defaults.md §1.3 is out of date with the defaults "
         "registry.\nRegenerate the marked table from "
         "defaults::markings_markdown():\n\n"
      << generated;
}

// Both renderers must emit their marker comment so the doc's tables stay
// findable (and replaceable) by name.
TEST(DefaultsRegistry, RenderersEmitTheirMarkers) {
  EXPECT_EQ(defaults::cross_section_markdown().rfind("<!-- rm-defaults: cross-section -->", 0), 0U);
  EXPECT_EQ(defaults::markings_markdown().rfind("<!-- rm-defaults: markings -->", 0), 0U);
}

// The four create-road templates are table consumers: every width they author
// must be the registry's value for that road class and lane type.
TEST(DefaultsRegistry, TemplatesDeriveFromTheRegistry) {
  const LaneProfile freeway = LaneProfile::freeway();
  ASSERT_EQ(freeway.right.size(), 4U);
  EXPECT_EQ(freeway.right[0].type, LaneType::Shoulder);
  EXPECT_DOUBLE_EQ(freeway.right[0].width, defaults::kFreewayLeftShoulderWidth);
  EXPECT_DOUBLE_EQ(freeway.right[1].width, defaults::driving_lane_width(RoadClass::Freeway));
  EXPECT_DOUBLE_EQ(freeway.right[2].width, defaults::driving_lane_width(RoadClass::Freeway));
  EXPECT_EQ(freeway.right[3].type, LaneType::Shoulder);
  EXPECT_DOUBLE_EQ(freeway.right[3].width, defaults::kFreewayRightShoulderWidth);

  const LaneProfile arterial = LaneProfile::arterial();
  ASSERT_EQ(arterial.right.size(), 3U);
  EXPECT_DOUBLE_EQ(arterial.right[0].width, defaults::driving_lane_width(RoadClass::Arterial));
  EXPECT_EQ(arterial.right[2].type, LaneType::Sidewalk);
  EXPECT_DOUBLE_EQ(arterial.right[2].width, defaults::kSidewalkWidth);

  const LaneProfile collector = LaneProfile::collector();
  ASSERT_EQ(collector.right.size(), 2U);
  EXPECT_DOUBLE_EQ(collector.right[0].width, defaults::driving_lane_width(RoadClass::Collector));
  EXPECT_EQ(collector.right[1].type, LaneType::Shoulder);
  EXPECT_DOUBLE_EQ(collector.right[1].width, defaults::kShoulderWidth);

  const LaneProfile local = LaneProfile::local_road();
  ASSERT_EQ(local.right.size(), 2U);
  EXPECT_DOUBLE_EQ(local.right[0].width, defaults::driving_lane_width(RoadClass::Local));
  EXPECT_EQ(local.right[1].type, LaneType::Sidewalk);
  EXPECT_DOUBLE_EQ(local.right[1].width, defaults::kSidewalkWidth);
}

// Same for the Library styles — and their marks paint per §1.3: yellow
// separates opposing traffic, white separates same-direction lanes and edges.
TEST(DefaultsRegistry, StylesDeriveFromTheRegistry) {
  const RoadStyle freeway = RoadStyle::freeway();
  ASSERT_EQ(freeway.right.size(), 4U);
  EXPECT_DOUBLE_EQ(freeway.right[0].width.a, defaults::kFreewayLeftShoulderWidth);
  ASSERT_TRUE(freeway.right[0].outer_mark.has_value());
  EXPECT_EQ(freeway.right[0].outer_mark->color, RoadMarkColor::Yellow) << "divided left edge";
  EXPECT_FALSE(freeway.center_mark.has_value());

  const RoadStyle arterial = RoadStyle::arterial();
  ASSERT_TRUE(arterial.center_mark.has_value());
  EXPECT_EQ(arterial.center_mark->type, RoadMarkType::SolidSolid) << "double yellow centerline";
  EXPECT_EQ(arterial.center_mark->color, RoadMarkColor::Yellow);
  EXPECT_DOUBLE_EQ(arterial.right[0].width.a, defaults::driving_lane_width(RoadClass::Arterial));

  const RoadStyle collector = RoadStyle::collector();
  ASSERT_TRUE(collector.center_mark.has_value());
  EXPECT_EQ(collector.center_mark->color, RoadMarkColor::Yellow);
  EXPECT_DOUBLE_EQ(collector.right[0].width.a, defaults::driving_lane_width(RoadClass::Collector));

  const RoadStyle local = RoadStyle::local_road();
  EXPECT_FALSE(local.center_mark.has_value()) << "residential streets carry no painted lines";
  EXPECT_FALSE(local.right[0].outer_mark.has_value());
  EXPECT_DOUBLE_EQ(local.right[0].width.a, defaults::driving_lane_width(RoadClass::Local));
}

// The classless add-lane/taper fallbacks: per-type widths from §1.2, with the
// arterial driving lane as the drivable-sized fallback for unlisted types.
TEST(DefaultsRegistry, PerLaneTypeWidths) {
  EXPECT_DOUBLE_EQ(defaults::lane_width(LaneType::Driving), defaults::kArterialLaneWidth);
  EXPECT_DOUBLE_EQ(defaults::lane_width(LaneType::Shoulder), defaults::kShoulderWidth);
  EXPECT_DOUBLE_EQ(defaults::lane_width(LaneType::Sidewalk), defaults::kSidewalkWidth);
  EXPECT_DOUBLE_EQ(defaults::lane_width(LaneType::Biking), defaults::kBikeLaneWidth);
  EXPECT_DOUBLE_EQ(defaults::lane_width(LaneType::Parking), defaults::kParkingLaneWidth);
  EXPECT_DOUBLE_EQ(defaults::lane_width(LaneType::Median), defaults::kMedianWidth);
  EXPECT_DOUBLE_EQ(defaults::lane_width(LaneType::None), defaults::kArterialLaneWidth);
}

// A fresh RoadMark (and stripe) is a normal line; the constants the mesher and
// the stop-line solver consume are the registry's, not private copies.
TEST(DefaultsRegistry, MarkingConstantsAreTheRegistrys) {
  EXPECT_DOUBLE_EQ(RoadMark{}.width, defaults::kLineWidth);
  EXPECT_DOUBLE_EQ(RoadMarkLine{}.width, defaults::kLineWidth);
}

TEST(DefaultsRegistry, RoadClassNames) {
  EXPECT_STREQ(defaults::road_class_name(RoadClass::Freeway), "freeway");
  EXPECT_STREQ(defaults::road_class_name(RoadClass::Arterial), "arterial");
  EXPECT_STREQ(defaults::road_class_name(RoadClass::Collector), "collector");
  EXPECT_STREQ(defaults::road_class_name(RoadClass::Local), "local");
}

} // namespace
} // namespace roadmaker
