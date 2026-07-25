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

#include "roadmaker/assets/prop_library.hpp"
#include "roadmaker/assets/sign_catalog.hpp"
#include "roadmaker/road/authoring.hpp"
#include "roadmaker/road/defaults.hpp"
#include "roadmaker/road/lane.hpp"
#include "roadmaker/road/network.hpp"
#include "roadmaker/road/road_style.hpp"
#include "roadmaker/road/signal_facing.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <numbers>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

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

TEST(DefaultsRegistry, DocSignsTableMatchesRegistry) {
  const std::string generated = defaults::signs_markdown();
  EXPECT_TRUE(committed_spec_doc().find(generated) != std::string::npos)
      << "docs/domain/realism_defaults.md §1.4 is out of date with the defaults "
         "registry.\nRegenerate the marked table from "
         "defaults::signs_markdown():\n\n"
      << generated;
}

TEST(DefaultsRegistry, DocSignMountingTableMatchesRegistry) {
  const std::string generated = defaults::sign_mounting_markdown();
  EXPECT_TRUE(committed_spec_doc().find(generated) != std::string::npos)
      << "docs/domain/realism_defaults.md §1.4 mounting table is out of date with "
         "the defaults registry.\nRegenerate the marked table from "
         "defaults::sign_mounting_markdown():\n\n"
      << generated;
}

TEST(DefaultsRegistry, DocSignalsLightingTableMatchesRegistry) {
  const std::string generated = defaults::signals_lighting_markdown();
  EXPECT_TRUE(committed_spec_doc().find(generated) != std::string::npos)
      << "docs/domain/realism_defaults.md §1.5 is out of date with the defaults "
         "registry.\nRegenerate the marked table from "
         "defaults::signals_lighting_markdown():\n\n"
      << generated;
}

TEST(DefaultsRegistry, DocTreesBuildingsTableMatchesRegistry) {
  const std::string generated = defaults::trees_buildings_markdown();
  EXPECT_TRUE(committed_spec_doc().find(generated) != std::string::npos)
      << "docs/domain/realism_defaults.md §1.6 is out of date with the defaults "
         "registry.\nRegenerate the marked table from "
         "defaults::trees_buildings_markdown():\n\n"
      << generated;
}

TEST(DefaultsRegistry, DocOrientationTableMatchesRegistry) {
  const std::string generated = defaults::orientation_markdown();
  EXPECT_TRUE(committed_spec_doc().find(generated) != std::string::npos)
      << "docs/domain/realism_defaults.md's auto-orientation table is out of date with the "
         "defaults registry.\nRegenerate the marked table from "
         "defaults::orientation_markdown():\n\n"
      << generated;
}

// Every renderer must emit its marker comment so the doc's tables stay
// findable (and replaceable) by name.
TEST(DefaultsRegistry, RenderersEmitTheirMarkers) {
  EXPECT_EQ(defaults::cross_section_markdown().rfind("<!-- rm-defaults: cross-section -->", 0), 0U);
  EXPECT_EQ(defaults::markings_markdown().rfind("<!-- rm-defaults: markings -->", 0), 0U);
  EXPECT_EQ(defaults::signs_markdown().rfind("<!-- rm-defaults: signs -->", 0), 0U);
  EXPECT_EQ(defaults::sign_mounting_markdown().rfind("<!-- rm-defaults: sign-mounting -->", 0), 0U);
  EXPECT_EQ(
      defaults::signals_lighting_markdown().rfind("<!-- rm-defaults: signals-lighting -->", 0), 0U);
  EXPECT_EQ(defaults::trees_buildings_markdown().rfind("<!-- rm-defaults: trees-buildings -->", 0),
            0U);
  EXPECT_EQ(defaults::orientation_markdown().rfind("<!-- rm-defaults: orientation -->", 0), 0U);
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

// Auto-orientation's cant is a registry value, not a literal copied into the
// facing rule (#416). Asserted through the rule itself rather than by reading
// the constant back, so a hand-rolled toe-out in signal_facing.cpp fails here.
TEST(DefaultsRegistry, AutoOrientationToeOutIsTheRegistrys) {
  RoadNetwork network;
  const std::vector<Waypoint> waypoints{Waypoint{.x = 0.0, .y = 0.0},
                                        Waypoint{.x = 120.0, .y = 0.0}};
  auto road = author_clothoid_road(network, waypoints, LaneProfile::two_lane_default(), "", "1");
  ASSERT_TRUE(road.has_value());
  const Expected<SignalFacing> facing = auto_signal_facing(network, *road, 60.0, -5.0);
  ASSERT_TRUE(facing.has_value());
  EXPECT_DOUBLE_EQ(std::abs(facing->h_offset), defaults::kSignToeOut);
}

// The registry's angles are constructed from their degree measure, never from
// the doc's rounded radian display: 0.052 rad would be 2.979 degrees, a
// different angle from the 3 degrees the spec names.
TEST(DefaultsRegistry, AngleConstantsAreExactDegreeMeasures) {
  EXPECT_DOUBLE_EQ(defaults::kSignToeOut * 180.0 / std::numbers::pi, 3.0);
  EXPECT_DOUBLE_EQ(defaults::kPropRotationSnap * 180.0 / std::numbers::pi, 15.0);
}

// --- Shipped props vs §1.5/§1.6 (#415) ------------------------------------
//
// scripts/gen_prop_meshes.py authors every prop at its true world size and is
// stdlib-only, so it cannot include defaults.hpp. These tests are the join:
// retune a mesh away from the spec (or move a constant without regenerating)
// and CI fails here. The editor's half of the gate — that the shipped manifest
// no longer scales the plants — lives in test_library_model.cpp, because core
// cannot parse the Qt-JSON manifest.

const props::PropModel& shipped(std::string_view id) {
  const props::PropModel* model = props::model(id);
  EXPECT_NE(model, nullptr) << "no bundled prop model \"" << id << '"';
  static const props::PropModel k_missing{};
  return model != nullptr ? *model : k_missing;
}

TEST(DefaultsRegistry, StreetlightsMountAtTheRegistryHeight) {
  EXPECT_DOUBLE_EQ(shipped("streetlight_single").height, defaults::kStreetlightMountingHeight);
  EXPECT_DOUBLE_EQ(shipped("streetlight_double").height, defaults::kStreetlightMountingHeight);
}

// The oak IS §1.6's default street tree; the other plants only have to stay in
// their band — a pine is not a shade tree and a shrub is not a tree at all.
TEST(DefaultsRegistry, StreetTreeIsTheRegistrys) {
  const props::PropModel& oak = shipped("tree_oak");
  EXPECT_DOUBLE_EQ(oak.height, defaults::kStreetTreeHeight);
  EXPECT_DOUBLE_EQ(oak.radius, defaults::kStreetTreeCanopyDiameter / 2.0);

  for (const char* id : {"tree_pine", "tree_birch", "tree_poplar"}) {
    const props::PropModel& tree = shipped(id);
    EXPECT_GE(tree.height, defaults::kOrnamentalTreeMinHeight) << id;
    EXPECT_LE(tree.height, defaults::kMatureTreeMaxHeight) << id;
  }

  EXPECT_LT(shipped("shrub").height, defaults::kOrnamentalTreeMinHeight)
      << "a shrub must read as smaller than the smallest ornamental tree";
}

TEST(DefaultsRegistry, BuildingsFollowThePerFloorRule) {
  // §1.6's rule is whole floors plus a parapet. fmod lands just under the
  // divisor as often as just over it (3.7 has no exact binary form), so the
  // distance to the *nearest* multiple is what the rule means.
  const auto off_floor_grid = [](double height) {
    const double rem = std::fmod(height - defaults::kParapetHeight, defaults::kFloorHeight);
    return std::min(rem, defaults::kFloorHeight - rem);
  };
  EXPECT_NEAR(off_floor_grid(shipped("building_mid").height), 0.0, 1e-9);
  EXPECT_NEAR(off_floor_grid(shipped("building_tower").height), 0.0, 1e-9);

  // The low building is a two-storey house: in band, and big enough in plan
  // that its picking radius covers the footprint sanity rectangle.
  const props::PropModel& low = shipped("building_low");
  EXPECT_GE(low.height, defaults::kHouse2StoryMinHeight);
  EXPECT_LE(low.height, defaults::kHouse2StoryMaxHeight);
  EXPECT_GE(
      low.radius,
      std::hypot(defaults::kHouseFootprintLength / 2.0, defaults::kHouseFootprintWidth / 2.0));
}

// --- The US sign pack vs §1.4 (#414) --------------------------------------
//
// roadmaker::signs::catalog() is the product's answer to "what is a stop
// sign?" — one table feeding identity authoring, mesh selection, the junction
// signalize templates and the Library manifest. Its face extents must be the
// §1.4 constants and nothing else, so a designation cannot quietly acquire a
// size the spec never granted it.

const signs::SignDef& pack(std::string_view key) {
  const signs::SignDef* def = signs::find_by_key(key);
  EXPECT_NE(def, nullptr) << "no catalogue entry \"" << key << '"';
  static const signs::SignDef k_missing{};
  return def != nullptr ? *def : k_missing;
}

TEST(DefaultsRegistry, PackFaceSizesComeFromTheRegistry) {
  EXPECT_DOUBLE_EQ(pack("us.r1_1").face_width, defaults::kSignStopFace);
  EXPECT_DOUBLE_EQ(pack("us.r1_1").face_height, defaults::kSignStopFace);
  EXPECT_DOUBLE_EQ(pack("us.r1_2").face_width, defaults::kSignYieldFace);
  EXPECT_DOUBLE_EQ(pack("us.r2_1").face_width, defaults::kSignSpeedLimitWidth);
  EXPECT_DOUBLE_EQ(pack("us.r2_1").face_height, defaults::kSignSpeedLimitHeight);
  EXPECT_DOUBLE_EQ(pack("us.r5_1").face_width, defaults::kSignDoNotEnterFace);
  EXPECT_DOUBLE_EQ(pack("us.r6_1_right").face_width, defaults::kSignOneWayWidth);
  EXPECT_DOUBLE_EQ(pack("us.r6_1_right").face_height, defaults::kSignOneWayHeight);
  EXPECT_DOUBLE_EQ(pack("us.r6_1_left").face_width, defaults::kSignOneWayWidth);
  EXPECT_DOUBLE_EQ(pack("us.r3_1").face_width, defaults::kSignTurnRestrictionFace);
  EXPECT_DOUBLE_EQ(pack("us.r3_2").face_width, defaults::kSignTurnRestrictionFace);
  EXPECT_DOUBLE_EQ(pack("us.r4_7").face_width, defaults::kSignKeepRightWidth);
  EXPECT_DOUBLE_EQ(pack("us.r4_7").face_height, defaults::kSignKeepRightHeight);
  EXPECT_DOUBLE_EQ(pack("us.w1_2").face_width, defaults::kSignWarningFace);
  EXPECT_DOUBLE_EQ(pack("us.w3_1").face_width, defaults::kSignWarningFace);
  EXPECT_DOUBLE_EQ(pack("us.w11_2").face_width, defaults::kSignWarningFace);
  EXPECT_DOUBLE_EQ(pack("us.s1_1").face_width, defaults::kSignSchoolFace);
  EXPECT_DOUBLE_EQ(pack("us.d3_1").face_height, defaults::kSignStreetNameHeight);
  // §1.4: a street-name blade's length follows its legend, so it declares none.
  EXPECT_DOUBLE_EQ(pack("us.d3_1").face_width, 0.0);
}

// §14.1 (Table 122): @unit is the unit OF @value — the two travel together.
// The catalogue is where a placement gets both, so enforce the pairing here
// rather than discovering a unit-less speed limit in an exported file.
TEST(DefaultsRegistry, PackValuesAlwaysCarryTheirUnit) {
  for (const signs::SignDef& def : signs::catalog()) {
    EXPECT_EQ(def.default_value.has_value(), !def.unit.empty()) << def.key;
    if (def.default_value.has_value()) {
      // e_unitSpeed literal; §1.4 puts mph on the face regardless of the
      // editor's display-unit toggle, so the authored value is already mph.
      EXPECT_EQ(def.unit, "mph") << def.key;
    }
  }
}

// Every static entry is a plated sign that a face can be baked onto, and every
// entry names a distinct tag. The traffic-light head is the one shapeless
// entry: it is an ASAM catalogue signal, not a US sign.
TEST(DefaultsRegistry, PackEntriesAreWellFormed) {
  std::vector<std::string_view> keys;
  for (const signs::SignDef& def : signs::catalog()) {
    keys.push_back(def.key);
    EXPECT_FALSE(def.type.empty()) << def.key;
    EXPECT_FALSE(def.subtype.empty()) << def.key;
    EXPECT_FALSE(def.country.empty()) << def.key;
    EXPECT_FALSE(def.model_id.empty()) << def.key;
    EXPECT_FALSE(def.label.empty()) << def.key;
    if (def.dynamic) {
      EXPECT_EQ(def.shape, signs::FaceShape::None) << def.key;
    } else {
      EXPECT_NE(def.shape, signs::FaceShape::None) << def.key;
      EXPECT_GT(def.face_height, 0.0) << def.key;
    }
  }
  std::sort(keys.begin(), keys.end());
  EXPECT_EQ(std::adjacent_find(keys.begin(), keys.end()), keys.end()) << "duplicate catalogue key";
}

// The mesh half of the §1.4 gate. scripts/gen_prop_meshes.py builds every pack
// sign to the spec's face size and mounting height, and is stdlib-only so it
// cannot include defaults.hpp — this is what holds it to the registry, exactly
// as the §1.5/§1.6 props are held (#415).
TEST(DefaultsRegistry, PackSignsAreBuiltToTheRegistrysFaceAndMounting) {
  // prop_meshes.gen.cpp is emitted at four decimals, so that — not the double
  // epsilon — is the resolution these comparisons can have.
  constexpr double kGenerated = 1e-4;
  for (const signs::SignDef& def : signs::catalog()) {
    if (def.dynamic) {
      continue; // a traffic-light head is not a plated sign
    }
    const props::PropModel& mesh = shipped(def.model_id);
    // §1.4 mounts a sign by its face's BOTTOM edge, so the model's bounding
    // height is the mounting height plus the face height. A blade whose length
    // follows its legend still has a spec'd height.
    EXPECT_NEAR(mesh.height, defaults::kSignMountUrban + def.face_height, kGenerated)
        << def.key << ": face bottom edge must sit at the §1.4 mounting height";
    ASSERT_TRUE(mesh.face_plate.has_value()) << def.key;
    // The plate's centre follows from the same rule.
    EXPECT_NEAR(mesh.face_plate->z, defaults::kSignMountUrban + def.face_height / 2.0, kGenerated)
        << def.key;
    // The texture area is inscribed in the field, so it can never exceed the
    // face the spec grants the sign.
    if (def.face_width > 0.0) {
      EXPECT_LE(mesh.face_plate->half_w, def.face_width / 2.0 + kGenerated) << def.key;
      EXPECT_NEAR(mesh.radius, std::hypot(def.face_width / 2.0, def.face_height / 2.0), kGenerated)
          << def.key;
    }
    EXPECT_LE(mesh.face_plate->half_h, def.face_height / 2.0 + kGenerated) << def.key;
    // Artwork the catalogue names must be artwork the mesh actually shows.
    EXPECT_EQ(mesh.face_plate->symbol, def.symbol) << def.key;
  }
}

// The post is spec'd, so it is gated too — a sign on a fence post reads wrong
// at a glance even when its face is right.
TEST(DefaultsRegistry, PackSignsStandOnTheRegistrysPost) {
  const props::PropModel& stop = shipped("sign_us_r1_1");
  const props::PropPart* post = nullptr;
  for (const props::PropPart& part : stop.parts) {
    if (part.name == "post") {
      post = &part;
    }
  }
  ASSERT_NE(post, nullptr) << "a pack sign is post-mounted";
  double max_radius = 0.0;
  for (std::size_t i = 0; i + 2 < post->positions.size(); i += 3) {
    max_radius = std::max(max_radius, std::hypot(post->positions[i], post->positions[i + 1]));
  }
  EXPECT_NEAR(max_radius, defaults::kSignPostDiameter / 2.0, 1e-4);
}

TEST(DefaultsRegistry, RoadClassNames) {
  EXPECT_STREQ(defaults::road_class_name(RoadClass::Freeway), "freeway");
  EXPECT_STREQ(defaults::road_class_name(RoadClass::Arterial), "arterial");
  EXPECT_STREQ(defaults::road_class_name(RoadClass::Collector), "collector");
  EXPECT_STREQ(defaults::road_class_name(RoadClass::Local), "local");
}

} // namespace
} // namespace roadmaker
