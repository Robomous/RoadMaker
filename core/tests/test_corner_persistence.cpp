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

// Persistence of authored junction corner-fillet overrides (p4-s1, issue
// #225): <userData code="rm:corners"> on <junction>, entries ";"-joined,
// fields ":"-joined:
//   "roadA:start|end:roadB:start|end
//    [:r=<num>][:ea=<num>][:eb=<num>][:sw=<name>][:md=<name>]"
// Junction-scope values (p4-s2, issue #226) ride a sibling
// <userData code="rm:junction"> ";"-joined "key=value": "r=<num>;mat=<name>".
// Mirrors the rm:arms contract: stale references are dropped on write, a
// malformed value is all-or-nothing on read (one warning, junction still
// loads).

#include "roadmaker/edit/operations.hpp"
#include "roadmaker/road/authoring.hpp"
#include "roadmaker/xodr/reader.hpp"
#include "roadmaker/xodr/writer.hpp"

#include <gtest/gtest.h>

#include <array>
#include <optional>
#include <string>
#include <vector>

using roadmaker::ContactPoint;
using roadmaker::Junction;
using roadmaker::JunctionCorner;
using roadmaker::JunctionId;
using roadmaker::LaneProfile;
using roadmaker::RoadEnd;
using roadmaker::RoadId;
using roadmaker::RoadNetwork;
using roadmaker::Severity;
using roadmaker::Waypoint;

namespace {

/// A generated three-arm junction over roads "1" (west), "2" (east) and
/// "3" (south); the junction lands with odr_id "1" and a full rm:arms list.
struct TeeFixture {
  RoadNetwork network;
  RoadId west;
  RoadId east;
  RoadId south;
  JunctionId junction;

  TeeFixture() {
    const auto arm = [&](std::vector<Waypoint> waypoints, const char* id) {
      return *roadmaker::author_clothoid_road(
          network, waypoints, LaneProfile::two_lane_default(), "", id);
    };
    west = arm({Waypoint{-40.0, 0.0}, Waypoint{-6.0, 0.0}}, "1");
    east = arm({Waypoint{40.0, 0.0}, Waypoint{6.0, 0.0}}, "2");
    south = arm({Waypoint{0.0, -40.0}, Waypoint{0.0, -6.0}}, "3");
    const std::array<RoadEnd, 3> ends{RoadEnd{.road = west, .contact = ContactPoint::End},
                                      RoadEnd{.road = east, .contact = ContactPoint::End},
                                      RoadEnd{.road = south, .contact = ContactPoint::End}};
    EXPECT_TRUE(roadmaker::edit::create_junction(network, ends)->apply(network).has_value());
    junction = network.find_junction("1");
    EXPECT_TRUE(junction.is_valid());
  }

  void set_corners(std::vector<JunctionCorner> corners) {
    network.junction(junction)->corners = std::move(corners);
  }
};

/// The value= payload of the junction's rm:corners element, or nullopt when
/// no such element was written.
std::optional<std::string> corners_value(const std::string& xml) {
  const std::string key = "code=\"rm:corners\" value=\"";
  const std::size_t at = xml.find(key);
  if (at == std::string::npos) {
    return std::nullopt;
  }
  const std::size_t begin = at + key.size();
  const std::size_t end = xml.find('"', begin);
  return xml.substr(begin, end - begin);
}

/// Splices a hand-written rm:corners element in after the junction's rm:arms
/// element, the way a file edited by hand (or by an older build) would carry it.
std::string with_corners_element(const std::string& xml, const std::string& value) {
  const std::size_t arms = xml.find("code=\"rm:arms\"");
  EXPECT_NE(arms, std::string::npos);
  const std::size_t close = xml.find("/>", arms);
  EXPECT_NE(close, std::string::npos);
  return xml.substr(0, close + 2) + "<userData code=\"rm:corners\" value=\"" + value + "\" />" +
         xml.substr(close + 2);
}

std::size_t count_warnings_mentioning(const std::vector<roadmaker::Diagnostic>& diagnostics,
                                      std::string_view needle) {
  std::size_t count = 0;
  for (const roadmaker::Diagnostic& diagnostic : diagnostics) {
    if (diagnostic.severity == Severity::Warning &&
        diagnostic.message.find(needle) != std::string::npos) {
      ++count;
    }
  }
  return count;
}

/// Field-for-field corner comparison across two networks (RoadIds are
/// per-network, so the arms are compared by road odr_id).
void expect_same_corners(const RoadNetwork& lhs_network,
                         const Junction& lhs,
                         const RoadNetwork& rhs_network,
                         const Junction& rhs) {
  ASSERT_EQ(lhs.corners.size(), rhs.corners.size());
  for (std::size_t i = 0; i < lhs.corners.size(); ++i) {
    const JunctionCorner& a = lhs.corners[i];
    const JunctionCorner& b = rhs.corners[i];
    EXPECT_EQ(lhs_network.road(a.arm_a.road)->odr_id, rhs_network.road(b.arm_a.road)->odr_id);
    EXPECT_EQ(lhs_network.road(a.arm_b.road)->odr_id, rhs_network.road(b.arm_b.road)->odr_id);
    EXPECT_EQ(a.arm_a.contact, b.arm_a.contact);
    EXPECT_EQ(a.arm_b.contact, b.arm_b.contact);
    EXPECT_EQ(a.radius, b.radius);
    EXPECT_EQ(a.extent_a, b.extent_a);
    EXPECT_EQ(a.extent_b, b.extent_b);
    EXPECT_EQ(a.sidewalk_material, b.sidewalk_material);
    EXPECT_EQ(a.median_material, b.median_material);
  }
}

} // namespace

TEST(CornerPersistence, RadiusOnlyEntryWritesGoldenValue) {
  TeeFixture fixture;
  fixture.set_corners(
      {JunctionCorner{.arm_a = RoadEnd{.road = fixture.west, .contact = ContactPoint::End},
                      .arm_b = RoadEnd{.road = fixture.east, .contact = ContactPoint::End},
                      .radius = 7.5}});

  const auto xml = roadmaker::write_xodr(fixture.network, "corners");
  ASSERT_TRUE(xml.has_value());
  EXPECT_EQ(corners_value(*xml), std::optional<std::string>("1:end:2:end:r=7.5"));
}

TEST(CornerPersistence, AllThreeOptionalsWriteGoldenValueInFixedOrder) {
  TeeFixture fixture;
  fixture.set_corners({JunctionCorner{
      .arm_a = RoadEnd{.road = fixture.east, .contact = ContactPoint::End},
      .arm_b = RoadEnd{.road = fixture.south, .contact = ContactPoint::End},
      .radius = 6.0,
      .extent_a = 3.25,
      .extent_b = 4.125,
  }});

  const auto xml = roadmaker::write_xodr(fixture.network, "corners");
  ASSERT_TRUE(xml.has_value());
  EXPECT_EQ(corners_value(*xml), std::optional<std::string>("2:end:3:end:r=6:ea=3.25:eb=4.125"));
}

TEST(CornerPersistence, MixedOverridesRoundTripAndRewriteByteIdentically) {
  TeeFixture fixture;
  fixture.set_corners({
      JunctionCorner{.arm_a = RoadEnd{.road = fixture.west, .contact = ContactPoint::End},
                     .arm_b = RoadEnd{.road = fixture.east, .contact = ContactPoint::End},
                     .radius = 7.5},
      JunctionCorner{.arm_a = RoadEnd{.road = fixture.east, .contact = ContactPoint::End},
                     .arm_b = RoadEnd{.road = fixture.south, .contact = ContactPoint::End},
                     .extent_a = 3.25,
                     .extent_b = 4.125},
      JunctionCorner{.arm_a = RoadEnd{.road = fixture.south, .contact = ContactPoint::End},
                     .arm_b = RoadEnd{.road = fixture.west, .contact = ContactPoint::End},
                     .radius = 12.75,
                     .extent_a = 1.5,
                     .extent_b = 2.0},
  });

  const auto first = roadmaker::write_xodr(fixture.network, "corners");
  ASSERT_TRUE(first.has_value());

  auto reparsed = roadmaker::parse_xodr(*first, "corners");
  ASSERT_TRUE(reparsed.has_value());
  EXPECT_EQ(roadmaker::count_errors(reparsed->diagnostics), 0U);
  EXPECT_EQ(reparsed->diagnostics.size(), 0U);

  const JunctionId reloaded = reparsed->network.find_junction("1");
  ASSERT_TRUE(reloaded.is_valid());
  expect_same_corners(fixture.network,
                      *fixture.network.junction(fixture.junction),
                      reparsed->network,
                      *reparsed->network.junction(reloaded));

  const auto second = roadmaker::write_xodr(reparsed->network, "corners");
  ASSERT_TRUE(second.has_value());
  EXPECT_EQ(*first, *second);
}

TEST(CornerPersistence, StaleRoadReferenceIsNotWritten) {
  TeeFixture fixture;
  // A road outside the junction, deleted after the corner names it — the
  // dangling-reference case the writer must drop (the junction itself stays
  // intact so this test isolates the corner rule).
  const auto authored = roadmaker::author_clothoid_road(
      fixture.network,
      std::vector<Waypoint>{Waypoint{100.0, 100.0}, Waypoint{140.0, 100.0}},
      LaneProfile::two_lane_default(),
      "",
      "99");
  ASSERT_TRUE(authored.has_value());
  const RoadId dangling = *authored;
  fixture.set_corners(
      {JunctionCorner{.arm_a = RoadEnd{.road = fixture.west, .contact = ContactPoint::End},
                      .arm_b = RoadEnd{.road = dangling, .contact = ContactPoint::End},
                      .radius = 7.5}});
  ASSERT_TRUE(fixture.network.erase_road(dangling));

  const auto xml = roadmaker::write_xodr(fixture.network, "corners");
  ASSERT_TRUE(xml.has_value());
  EXPECT_EQ(corners_value(*xml), std::nullopt);
  EXPECT_EQ(xml->find("rm:corners"), std::string::npos);
}

TEST(CornerPersistence, NoCornersOrEmptyOverrideEmitsNoElement) {
  TeeFixture without;
  const auto plain = roadmaker::write_xodr(without.network, "corners");
  ASSERT_TRUE(plain.has_value());
  EXPECT_EQ(plain->find("rm:corners"), std::string::npos);
  EXPECT_NE(plain->find("rm:arms"), std::string::npos);

  TeeFixture empty_override;
  empty_override.set_corners({JunctionCorner{
      .arm_a = RoadEnd{.road = empty_override.west, .contact = ContactPoint::End},
      .arm_b = RoadEnd{.road = empty_override.east, .contact = ContactPoint::End},
  }});
  const auto xml = roadmaker::write_xodr(empty_override.network, "corners");
  ASSERT_TRUE(xml.has_value());
  EXPECT_EQ(xml->find("rm:corners"), std::string::npos);

  // A junction with rm:arms but no rm:corners still round-trips byte-identically.
  auto reparsed = roadmaker::parse_xodr(*plain, "corners");
  ASSERT_TRUE(reparsed.has_value());
  EXPECT_EQ(reparsed->diagnostics.size(), 0U);
  EXPECT_TRUE(reparsed->network.junction(reparsed->network.find_junction("1"))->corners.empty());
  const auto again = roadmaker::write_xodr(reparsed->network, "corners");
  ASSERT_TRUE(again.has_value());
  EXPECT_EQ(*plain, *again);
}

class CornerPersistenceMalformed : public testing::TestWithParam<const char*> {};

TEST_P(CornerPersistenceMalformed, WarnsOnceAndDropsAllCornersButKeepsTheJunction) {
  TeeFixture fixture;
  const auto base = roadmaker::write_xodr(fixture.network, "corners");
  ASSERT_TRUE(base.has_value());
  const std::string xml = with_corners_element(*base, GetParam());

  auto reparsed = roadmaker::parse_xodr(xml, "corners");
  ASSERT_TRUE(reparsed.has_value());
  EXPECT_EQ(roadmaker::count_errors(reparsed->diagnostics), 0U);
  EXPECT_EQ(count_warnings_mentioning(reparsed->diagnostics, "rm:corners"), 1U);
  EXPECT_EQ(reparsed->diagnostics.size(), 1U);

  const JunctionId reloaded = reparsed->network.find_junction("1");
  ASSERT_TRUE(reloaded.is_valid());
  const Junction& junction = *reparsed->network.junction(reloaded);
  EXPECT_TRUE(junction.corners.empty());
  // The rest of the junction — including rm:arms — still loads.
  EXPECT_EQ(junction.arms.size(), 3U);
  EXPECT_FALSE(junction.connections.empty());
}

INSTANTIATE_TEST_SUITE_P(Values,
                         CornerPersistenceMalformed,
                         testing::Values("1:middle:2:end:r=7.5", // bad contact word
                                         "1:end:99:end:r=7.5",   // road does not exist
                                         "1:end:2:end:r=abc",    // unparseable number
                                         "1:end:2:end",          // no optional authored
                                         "1:end:2",              // missing fields
                                         "1:end:2:end:x=3",      // unknown optional key
                                         "1:end:2:end:r=1:r=2",  // duplicate key
                                         "" /* empty value */));

TEST(CornerPersistence, ValidEntryAfterMalformedOneDropsTheWholeValue) {
  TeeFixture fixture;
  const auto base = roadmaker::write_xodr(fixture.network, "corners");
  ASSERT_TRUE(base.has_value());
  const std::string xml = with_corners_element(*base, "1:end:2:end:r=7.5;2:end:3:nope:r=6");

  auto reparsed = roadmaker::parse_xodr(xml, "corners");
  ASSERT_TRUE(reparsed.has_value());
  EXPECT_EQ(count_warnings_mentioning(reparsed->diagnostics, "rm:corners"), 1U);
  EXPECT_TRUE(reparsed->network.junction(reparsed->network.find_junction("1"))->corners.empty());
}

// --- p4-s2 (#226): materials in rm:corners, junction scope in rm:junction ----

namespace {

/// The value= payload of the junction's rm:junction element, or nullopt.
std::optional<std::string> junction_value(const std::string& xml) {
  const std::string key = "code=\"rm:junction\" value=\"";
  const std::size_t at = xml.find(key);
  if (at == std::string::npos) {
    return std::nullopt;
  }
  const std::size_t begin = at + key.size();
  return xml.substr(begin, xml.find('"', begin) - begin);
}

/// Splices a hand-written rm:junction element in after the junction's rm:arms
/// element (the same trick with_corners_element uses).
std::string with_junction_element(const std::string& xml, const std::string& value) {
  const std::size_t arms = xml.find("code=\"rm:arms\"");
  EXPECT_NE(arms, std::string::npos);
  const std::size_t close = xml.find("/>", arms);
  EXPECT_NE(close, std::string::npos);
  return xml.substr(0, close + 2) + "<userData code=\"rm:junction\" value=\"" + value + "\" />" +
         xml.substr(close + 2);
}

} // namespace

TEST(CornerPersistence, CornersUserDataRoundTripsMaterials) {
  TeeFixture fixture;
  fixture.set_corners(
      {JunctionCorner{.arm_a = RoadEnd{.road = fixture.west, .contact = ContactPoint::End},
                      .arm_b = RoadEnd{.road = fixture.east, .contact = ContactPoint::End},
                      .radius = 8.0,
                      .sidewalk_material = "concrete",
                      .median_material = "paint_white"}});

  const auto xml = roadmaker::write_xodr(fixture.network, "corners");
  ASSERT_TRUE(xml.has_value());
  EXPECT_EQ(corners_value(*xml),
            std::optional<std::string>("1:end:2:end:r=8:sw=concrete:md=paint_white"));

  auto reparsed = roadmaker::parse_xodr(*xml, "corners");
  ASSERT_TRUE(reparsed.has_value());
  const Junction& loaded = *reparsed->network.junction(reparsed->network.find_junction("1"));
  ASSERT_EQ(loaded.corners.size(), 1U);
  EXPECT_EQ(loaded.corners[0].sidewalk_material, std::optional<std::string>("concrete"));
  EXPECT_EQ(loaded.corners[0].median_material, std::optional<std::string>("paint_white"));

  const auto rewritten = roadmaker::write_xodr(reparsed->network, "corners");
  ASSERT_TRUE(rewritten.has_value());
  EXPECT_EQ(*rewritten, *xml);
}

TEST(CornerPersistence, CornersUserDataMaterialOnlyEntryPersists) {
  TeeFixture fixture;
  fixture.set_corners(
      {JunctionCorner{.arm_a = RoadEnd{.road = fixture.east, .contact = ContactPoint::End},
                      .arm_b = RoadEnd{.road = fixture.south, .contact = ContactPoint::End},
                      .sidewalk_material = "concrete"}});

  const auto xml = roadmaker::write_xodr(fixture.network, "corners");
  ASSERT_TRUE(xml.has_value());
  EXPECT_EQ(corners_value(*xml), std::optional<std::string>("2:end:3:end:sw=concrete"));

  auto reparsed = roadmaker::parse_xodr(*xml, "corners");
  ASSERT_TRUE(reparsed.has_value());
  const Junction& loaded = *reparsed->network.junction(reparsed->network.find_junction("1"));
  ASSERT_EQ(loaded.corners.size(), 1U);
  EXPECT_FALSE(loaded.corners[0].radius.has_value());
  EXPECT_EQ(loaded.corners[0].sidewalk_material, std::optional<std::string>("concrete"));
}

class CornerMaterialPersistenceMalformed : public testing::TestWithParam<const char*> {};

TEST_P(CornerMaterialPersistenceMalformed, MalformedCornerMaterialDropsWholeValue) {
  TeeFixture fixture;
  const auto base = roadmaker::write_xodr(fixture.network, "corners");
  ASSERT_TRUE(base.has_value());
  const std::string xml = with_corners_element(*base, GetParam());

  auto reparsed = roadmaker::parse_xodr(xml, "corners");
  ASSERT_TRUE(reparsed.has_value());
  EXPECT_EQ(count_warnings_mentioning(reparsed->diagnostics, "rm:corners"), 1U);
  EXPECT_TRUE(reparsed->network.junction(reparsed->network.find_junction("1"))->corners.empty());
}

INSTANTIATE_TEST_SUITE_P(
    Values,
    CornerMaterialPersistenceMalformed,
    testing::Values("1:end:2:end:sw=",                           // empty material name
                    "1:end:2:end:sw=a:sw=b",                     // duplicate key
                    "1:end:2:end:sw=a b",                        // whitespace in the token
                    "1:end:2:end:r=1:ea=2:eb=3:sw=a:md=b:x=1")); // ten fields (cap is nine)

TEST(CornerPersistence, JunctionUserDataRoundTrips) {
  TeeFixture fixture;
  Junction& junction = *fixture.network.junction(fixture.junction);
  junction.default_corner_radius = 12.0;
  junction.material = "concrete";

  const auto xml = roadmaker::write_xodr(fixture.network, "corners");
  ASSERT_TRUE(xml.has_value());
  EXPECT_EQ(junction_value(*xml), std::optional<std::string>("r=12;mat=concrete"));

  auto reparsed = roadmaker::parse_xodr(*xml, "corners");
  ASSERT_TRUE(reparsed.has_value());
  const Junction& loaded = *reparsed->network.junction(reparsed->network.find_junction("1"));
  EXPECT_EQ(loaded.default_corner_radius, std::optional<double>(12.0));
  EXPECT_EQ(loaded.material, "concrete");

  const auto rewritten = roadmaker::write_xodr(reparsed->network, "corners");
  ASSERT_TRUE(rewritten.has_value());
  EXPECT_EQ(*rewritten, *xml);
}

TEST(CornerPersistence, JunctionUserDataUnknownKeyIgnoredWithWarning) {
  TeeFixture fixture;
  const auto base = roadmaker::write_xodr(fixture.network, "corners");
  ASSERT_TRUE(base.has_value());
  // Forward compatibility: a field a future RoadMaker adds must not cost the
  // reader the fields it DOES understand.
  const std::string xml = with_junction_element(*base, "r=9;future=42;mat=concrete");

  auto reparsed = roadmaker::parse_xodr(xml, "corners");
  ASSERT_TRUE(reparsed.has_value());
  EXPECT_EQ(count_warnings_mentioning(reparsed->diagnostics, "future=42"), 1U);
  const Junction& loaded = *reparsed->network.junction(reparsed->network.find_junction("1"));
  EXPECT_EQ(loaded.default_corner_radius, std::optional<double>(9.0));
  EXPECT_EQ(loaded.material, "concrete");
}

class JunctionUserDataMalformed : public testing::TestWithParam<const char*> {};

TEST_P(JunctionUserDataMalformed, JunctionUserDataMalformedValueDroppedWithWarning) {
  TeeFixture fixture;
  const auto base = roadmaker::write_xodr(fixture.network, "corners");
  ASSERT_TRUE(base.has_value());
  const std::string xml = with_junction_element(*base, GetParam());

  auto reparsed = roadmaker::parse_xodr(xml, "corners");
  ASSERT_TRUE(reparsed.has_value());
  EXPECT_EQ(count_warnings_mentioning(reparsed->diagnostics, "malformed rm:junction"), 1U);
  const Junction& loaded = *reparsed->network.junction(reparsed->network.find_junction("1"));
  EXPECT_FALSE(loaded.default_corner_radius.has_value());
  EXPECT_TRUE(loaded.material.empty());
  EXPECT_FALSE(loaded.arms.empty()); // the junction itself still loads
}

INSTANTIATE_TEST_SUITE_P(Values,
                         JunctionUserDataMalformed,
                         testing::Values("r=abc",       // unparseable number
                                         "r=1;r=2",     // duplicate known key
                                         "mat=",        // empty material name
                                         "mat=a;mat=b", // duplicate known key
                                         "mat=a:b"));   // reserved character

TEST(CornerPersistence, WriterOmitsEmptyJunctionUserData) {
  TeeFixture fixture;
  const auto xml = roadmaker::write_xodr(fixture.network, "corners");
  ASSERT_TRUE(xml.has_value());
  EXPECT_EQ(junction_value(*xml), std::nullopt);
  EXPECT_EQ(xml->find("rm:junction"), std::string::npos);
}

TEST(CornerPersistence, SaveLoadSaveByteIdenticalWithMaterials) {
  TeeFixture fixture;
  Junction& junction = *fixture.network.junction(fixture.junction);
  junction.default_corner_radius = 5.5;
  junction.material = "asphalt_worn";
  fixture.set_corners(
      {JunctionCorner{.arm_a = RoadEnd{.road = fixture.west, .contact = ContactPoint::End},
                      .arm_b = RoadEnd{.road = fixture.east, .contact = ContactPoint::End},
                      .extent_a = 3.0,
                      .extent_b = 4.0,
                      .sidewalk_material = "concrete"},
       JunctionCorner{.arm_a = RoadEnd{.road = fixture.east, .contact = ContactPoint::End},
                      .arm_b = RoadEnd{.road = fixture.south, .contact = ContactPoint::End},
                      .median_material = "paint_white"}});

  const auto first = roadmaker::write_xodr(fixture.network, "corners");
  ASSERT_TRUE(first.has_value());
  auto reparsed = roadmaker::parse_xodr(*first, "corners");
  ASSERT_TRUE(reparsed.has_value());
  const auto second = roadmaker::write_xodr(reparsed->network, "corners");
  ASSERT_TRUE(second.has_value());
  EXPECT_EQ(*second, *first);

  const Junction& loaded = *reparsed->network.junction(reparsed->network.find_junction("1"));
  expect_same_corners(
      fixture.network, *fixture.network.junction(fixture.junction), reparsed->network, loaded);
  EXPECT_EQ(loaded.default_corner_radius, std::optional<double>(5.5));
  EXPECT_EQ(loaded.material, "asphalt_worn");
}
