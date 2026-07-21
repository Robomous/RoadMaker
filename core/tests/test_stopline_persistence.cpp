// Persistence of junction stop lines (p4-s3, issue #318).
//
// Stop lines are MATERIALIZED, not stored: the writer emits one self-contained
// <object type="roadMark" subtype="signalLines"> per solvable arm — valid
// OpenDRIVE for a foreign reader with no RoadMaker knowledge (ADR-0008 Layer 0)
// — and tags it with <userData code="rm:stopline"> carrying the parametric
// record (Layer 1). The reader absorbs the tagged objects back into
// Junction::stoplines and never adds them to the arena, so a round trip neither
// duplicates the objects nor loses the authoring.
//
// Payload (object scope, attribute form):
//   contact="start|end"  (required — the junction-facing end = the owning arm)
//   distance="<num>"     (omitted when not authored)
//   flipped="true"       (omitted when false)
//   crosswalk="<id>"     (omitted when empty)

#include "roadmaker/edit/operations.hpp"
#include "roadmaker/mesh/junction_stoplines.hpp"
#include "roadmaker/road/authoring.hpp"
#include "roadmaker/road/junction.hpp"
#include "roadmaker/xodr/reader.hpp"
#include "roadmaker/xodr/writer.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using roadmaker::ContactPoint;
using roadmaker::Junction;
using roadmaker::junction_stoplines;
using roadmaker::JunctionId;
using roadmaker::JunctionStopLineInfo;
using roadmaker::kStopLineDefaultDistance;
using roadmaker::kStopLineThickness;
using roadmaker::LaneProfile;
using roadmaker::RoadEnd;
using roadmaker::RoadId;
using roadmaker::RoadNetwork;
using roadmaker::Severity;
using roadmaker::StopLine;
using roadmaker::Waypoint;
using roadmaker::XodrVersion;

namespace {

/// The roomy four-way used across the stop-line suites; the junction lands with
/// odr_id "1" and every arm meets it at its End.
struct CrossFixture {
  RoadNetwork network;
  RoadId west;
  RoadId east;
  RoadId south;
  RoadId north;
  JunctionId junction;

  CrossFixture() {
    const auto arm = [&](std::vector<Waypoint> waypoints, const char* id) {
      return *roadmaker::author_clothoid_road(
          network, waypoints, LaneProfile::two_lane_default(), "", id);
    };
    west = arm({Waypoint{-80.0, 0.0}, Waypoint{-20.0, 0.0}}, "1");
    east = arm({Waypoint{80.0, 0.0}, Waypoint{20.0, 0.0}}, "2");
    south = arm({Waypoint{0.0, -80.0}, Waypoint{0.0, -20.0}}, "3");
    north = arm({Waypoint{0.0, 80.0}, Waypoint{0.0, 20.0}}, "4");
    const std::array<RoadEnd, 4> ends{RoadEnd{.road = west, .contact = ContactPoint::End},
                                      RoadEnd{.road = east, .contact = ContactPoint::End},
                                      RoadEnd{.road = south, .contact = ContactPoint::End},
                                      RoadEnd{.road = north, .contact = ContactPoint::End}};
    EXPECT_TRUE(roadmaker::edit::create_junction(network, ends)->apply(network).has_value());
    junction = network.find_junction("1");
    EXPECT_TRUE(junction.is_valid());
  }

  RoadEnd arm_end(RoadId road) const { return RoadEnd{.road = road, .contact = ContactPoint::End}; }
};

std::string write(const RoadNetwork& network, XodrVersion version = XodrVersion::v1_8_1) {
  auto xml = roadmaker::write_xodr(
      network, "stoplines", roadmaker::WriterOptions{.target_version = version});
  EXPECT_TRUE(xml.has_value());
  return xml.value_or(std::string{});
}

std::size_t count(const std::string& haystack, std::string_view needle) {
  std::size_t n = 0;
  for (std::size_t at = haystack.find(needle); at != std::string::npos;
       at = haystack.find(needle, at + needle.size())) {
    ++n;
  }
  return n;
}

const StopLine* record_for(const RoadNetwork& network, JunctionId junction, const RoadEnd& arm) {
  const Junction* record = network.junction(junction);
  const auto entry = std::ranges::find_if(record->stoplines,
                                          [&](const StopLine& line) { return line.arm == arm; });
  return entry == record->stoplines.end() ? nullptr : &*entry;
}

std::optional<JunctionStopLineInfo>
solved(const RoadNetwork& network, JunctionId junction, const RoadEnd& arm) {
  const std::vector<JunctionStopLineInfo> lines = junction_stoplines(network, junction);
  const auto entry = std::ranges::find_if(
      lines, [&](const JunctionStopLineInfo& info) { return info.arm == arm; });
  if (entry == lines.end()) {
    return std::nullopt;
  }
  return *entry;
}

} // namespace

TEST(StopLinePersistence, DerivedDefaultsExportOnePerArm) {
  CrossFixture fixture;
  const std::string xml = write(fixture.network);

  EXPECT_EQ(count(xml, "subtype=\"signalLines\""), 4U) << "one materialized line per arm";
  EXPECT_EQ(count(xml, "code=\"rm:stopline\""), 4U);
  // Fully derived: the tag carries its identity and nothing else.
  EXPECT_EQ(count(xml, "contact=\"end\""), 4U);
  EXPECT_EQ(count(xml, "distance="), 0U);
  EXPECT_EQ(count(xml, "flipped="), 0U);
  EXPECT_EQ(count(xml, "crosswalk="), 0U);
  // Deterministic, namespaced ids.
  for (const char* id : {"sl_1_1_e", "sl_1_2_e", "sl_1_3_e", "sl_1_4_e"}) {
    EXPECT_EQ(count(xml, std::string("id=\"") + id + "\""), 1U) << "missing stop line " << id;
  }
}

TEST(StopLinePersistence, TheExportedObjectIsAPlainValidStopLine) {
  CrossFixture fixture;
  const std::string xml = write(fixture.network);

  // Layer 0: a foreign reader that ignores userData still gets a placed,
  // dimensioned road-mark object. @length is the along-road thickness and
  // @width the across-lanes span (inverted vs the crosswalk — see the header).
  EXPECT_EQ(count(xml, "type=\"roadMark\" subtype=\"signalLines\""), 4U);
  EXPECT_EQ(count(xml, "length=\"0.3\""), 4U);
  const std::optional<JunctionStopLineInfo> info =
      solved(fixture.network, fixture.junction, fixture.arm_end(fixture.west));
  ASSERT_TRUE(info.has_value());
  EXPECT_GT(info->span, 2.0);
}

TEST(StopLinePersistence, DerivedObjectsAreAbsorbedNotDuplicated) {
  CrossFixture fixture;
  const std::string xml = write(fixture.network);

  auto reparsed = roadmaker::parse_xodr(xml, "stoplines");
  ASSERT_TRUE(reparsed.has_value());
  EXPECT_TRUE(reparsed->diagnostics.empty()) << "a clean round trip must be silent";

  // Absorbed: the objects are NOT in the arena...
  std::size_t arena_objects = 0;
  reparsed->network.for_each_object(
      [&](roadmaker::ObjectId, const roadmaker::Object&) { ++arena_objects; });
  EXPECT_EQ(arena_objects, 0U);
  // ...and a pure default stores no record either — it is simply re-derived.
  const JunctionId junction = reparsed->network.find_junction("1");
  ASSERT_TRUE(junction.is_valid());
  EXPECT_TRUE(reparsed->network.junction(junction)->stoplines.empty());
  EXPECT_EQ(junction_stoplines(reparsed->network, junction).size(), 4U);

  // Writing again reproduces the file byte for byte — no drift, no doubling.
  EXPECT_EQ(write(reparsed->network), xml);
}

TEST(StopLinePersistence, AuthoredDistanceRoundTrips) {
  CrossFixture fixture;
  const RoadEnd arm = fixture.arm_end(fixture.west);
  ASSERT_TRUE(roadmaker::edit::set_stopline_distance(fixture.network, fixture.junction, arm, 2.5)
                  ->apply(fixture.network)
                  .has_value());
  const std::string xml = write(fixture.network);
  EXPECT_EQ(count(xml, "distance=\"2.5\""), 1U);

  auto reparsed = roadmaker::parse_xodr(xml, "stoplines");
  ASSERT_TRUE(reparsed.has_value());
  EXPECT_TRUE(reparsed->diagnostics.empty());
  const JunctionId junction = reparsed->network.find_junction("1");
  const RoadEnd reloaded{.road = reparsed->network.find_road("1"), .contact = ContactPoint::End};

  const StopLine* record = record_for(reparsed->network, junction, reloaded);
  ASSERT_NE(record, nullptr);
  ASSERT_TRUE(record->distance.has_value());
  EXPECT_DOUBLE_EQ(*record->distance, 2.5);
  EXPECT_DOUBLE_EQ(solved(reparsed->network, junction, reloaded)->distance, 2.5);
  EXPECT_EQ(write(reparsed->network), xml);
}

TEST(StopLinePersistence, FlipRoundTrips) {
  CrossFixture fixture;
  const RoadEnd arm = fixture.arm_end(fixture.west);
  ASSERT_TRUE(roadmaker::edit::flip_stopline(fixture.network, fixture.junction, arm)
                  ->apply(fixture.network)
                  .has_value());
  const std::string xml = write(fixture.network);
  EXPECT_EQ(count(xml, "flipped=\"true\""), 1U);
  EXPECT_EQ(count(xml, "distance="), 0U) << "flipping alone authors no setback";

  auto reparsed = roadmaker::parse_xodr(xml, "stoplines");
  ASSERT_TRUE(reparsed.has_value());
  EXPECT_TRUE(reparsed->diagnostics.empty());
  const JunctionId junction = reparsed->network.find_junction("1");
  const RoadEnd reloaded{.road = reparsed->network.find_road("1"), .contact = ContactPoint::End};

  const StopLine* record = record_for(reparsed->network, junction, reloaded);
  ASSERT_NE(record, nullptr);
  EXPECT_TRUE(record->flipped);
  EXPECT_FALSE(record->distance.has_value());
  EXPECT_TRUE(solved(reparsed->network, junction, reloaded)->flipped);
  EXPECT_EQ(write(reparsed->network), xml);
}

TEST(StopLinePersistence, CrosswalkLinkRoundTrips) {
  CrossFixture fixture;
  const RoadEnd arm = fixture.arm_end(fixture.west);
  ASSERT_TRUE(roadmaker::edit::set_stopline_distance(
                  fixture.network, fixture.junction, arm, 3.0, std::string("7"))
                  ->apply(fixture.network)
                  .has_value());
  const std::string xml = write(fixture.network);
  EXPECT_EQ(count(xml, "crosswalk=\"7\""), 1U);

  auto reparsed = roadmaker::parse_xodr(xml, "stoplines");
  ASSERT_TRUE(reparsed.has_value());
  const JunctionId junction = reparsed->network.find_junction("1");
  const RoadEnd reloaded{.road = reparsed->network.find_road("1"), .contact = ContactPoint::End};
  EXPECT_EQ(record_for(reparsed->network, junction, reloaded)->crosswalk_odr_id, "7");
  EXPECT_EQ(write(reparsed->network), xml);
}

TEST(StopLinePersistence, EveryArmCanBeAuthoredIndependently) {
  CrossFixture fixture;
  ASSERT_TRUE(roadmaker::edit::set_stopline_distance(
                  fixture.network, fixture.junction, fixture.arm_end(fixture.west), 2.0)
                  ->apply(fixture.network)
                  .has_value());
  ASSERT_TRUE(roadmaker::edit::flip_stopline(
                  fixture.network, fixture.junction, fixture.arm_end(fixture.east))
                  ->apply(fixture.network)
                  .has_value());
  const std::string xml = write(fixture.network);

  auto reparsed = roadmaker::parse_xodr(xml, "stoplines");
  ASSERT_TRUE(reparsed.has_value());
  const JunctionId junction = reparsed->network.find_junction("1");
  EXPECT_EQ(reparsed->network.junction(junction)->stoplines.size(), 2U);
  EXPECT_EQ(junction_stoplines(reparsed->network, junction).size(), 4U)
      << "the two untouched arms still derive their defaults";
  EXPECT_EQ(write(reparsed->network), xml);
}

TEST(StopLinePersistence, TargetVersion181And190SerializeIdentically) {
  // §13.7: the stop line carries no <outline> and no <markings>, so nothing in
  // the element differs between the two writer targets. Only the header revMinor
  // may differ.
  CrossFixture fixture;
  ASSERT_TRUE(roadmaker::edit::set_stopline_distance(
                  fixture.network, fixture.junction, fixture.arm_end(fixture.west), 2.5)
                  ->apply(fixture.network)
                  .has_value());

  const std::string v181 = write(fixture.network, XodrVersion::v1_8_1);
  const std::string v190 = write(fixture.network, XodrVersion::v1_9_0);
  const auto objects_of = [](const std::string& xml) {
    std::vector<std::string> lines;
    for (std::size_t at = xml.find("<object "); at != std::string::npos;
         at = xml.find("<object ", at + 1)) {
      lines.push_back(xml.substr(at, xml.find('>', at) - at));
    }
    return lines;
  };
  EXPECT_EQ(objects_of(v181), objects_of(v190));
  EXPECT_EQ(count(v181, "code=\"rm:stopline\""), count(v190, "code=\"rm:stopline\""));
}

// --- degradation -------------------------------------------------------------

TEST(StopLinePersistence, MalformedStoplineDataWarnsAndKeepsTheObjectLive) {
  CrossFixture fixture;
  std::string xml = write(fixture.network);
  // Corrupt one record's contact: the parametric layer is unreadable, so the
  // line degrades to Layer 0 — a plain object that still renders — rather than
  // vanishing.
  const std::size_t at = xml.find("contact=\"end\"");
  ASSERT_NE(at, std::string::npos);
  xml.replace(at, std::string("contact=\"end\"").size(), "contact=\"middl\"");

  auto reparsed = roadmaker::parse_xodr(xml, "stoplines");
  ASSERT_TRUE(reparsed.has_value());
  EXPECT_FALSE(reparsed->diagnostics.empty());
  EXPECT_EQ(reparsed->diagnostics.front().severity, Severity::Warning);

  std::size_t arena_objects = 0;
  reparsed->network.for_each_object([&](roadmaker::ObjectId, const roadmaker::Object& object) {
    ++arena_objects;
    EXPECT_EQ(object.subtype, "signalLines");
  });
  EXPECT_EQ(arena_objects, 1U) << "the unreadable line stays a live object";
}

TEST(StopLinePersistence, MalformedDistanceIsRejectedWholesale) {
  CrossFixture fixture;
  const RoadEnd arm = fixture.arm_end(fixture.west);
  ASSERT_TRUE(roadmaker::edit::set_stopline_distance(fixture.network, fixture.junction, arm, 2.5)
                  ->apply(fixture.network)
                  .has_value());
  std::string xml = write(fixture.network);
  const std::size_t at = xml.find("distance=\"2.5\"");
  ASSERT_NE(at, std::string::npos);
  xml.replace(at, std::string("distance=\"2.5\"").size(), "distance=\"abc\"");

  auto reparsed = roadmaker::parse_xodr(xml, "stoplines");
  ASSERT_TRUE(reparsed.has_value());
  EXPECT_FALSE(reparsed->diagnostics.empty());
  const JunctionId junction = reparsed->network.find_junction("1");
  EXPECT_TRUE(reparsed->network.junction(junction)->stoplines.empty())
      << "an unreadable field drops the whole record, never guesses";
  std::size_t arena_objects = 0;
  reparsed->network.for_each_object(
      [&](roadmaker::ObjectId, const roadmaker::Object&) { ++arena_objects; });
  EXPECT_EQ(arena_objects, 1U);
}

TEST(StopLinePersistence, UnknownAttributeWarnsButKeepsTheRecord) {
  CrossFixture fixture;
  const RoadEnd arm = fixture.arm_end(fixture.west);
  ASSERT_TRUE(roadmaker::edit::set_stopline_distance(fixture.network, fixture.junction, arm, 2.5)
                  ->apply(fixture.network)
                  .has_value());
  std::string xml = write(fixture.network);
  const std::size_t at = xml.find("code=\"rm:stopline\"");
  ASSERT_NE(at, std::string::npos);
  xml.insert(at, "futureField=\"9\" ");

  auto reparsed = roadmaker::parse_xodr(xml, "stoplines");
  ASSERT_TRUE(reparsed.has_value());
  EXPECT_FALSE(reparsed->diagnostics.empty()) << "the unknown field is reported";
  const JunctionId junction = reparsed->network.find_junction("1");
  const RoadEnd reloaded{.road = reparsed->network.find_road("1"), .contact = ContactPoint::End};
  const StopLine* record = record_for(reparsed->network, junction, reloaded);
  ASSERT_NE(record, nullptr) << "a newer field must not delete the stop line";
  EXPECT_DOUBLE_EQ(*record->distance, 2.5);
}

TEST(StopLinePersistence, StoplineOnARoadEndWithNoJunctionStaysAPlainObject) {
  // A hand-edited or foreign file can tag an object on a road end no junction
  // claims. There is nowhere to put the record, so the object is restored live
  // with its userData preserved verbatim rather than dropped.
  RoadNetwork network;
  const RoadId lonely = *roadmaker::author_clothoid_road(
      network,
      std::vector<Waypoint>{Waypoint{0.0, 0.0}, Waypoint{60.0, 0.0}},
      LaneProfile::two_lane_default(),
      "",
      "1");
  ASSERT_TRUE(lonely.is_valid());
  roadmaker::Object marker;
  marker.road = lonely;
  marker.odr_id = "sl_x";
  marker.type_str = "roadMark";
  marker.subtype = "signalLines";
  marker.s = 55.0;
  marker.t = -1.75;
  marker.length = kStopLineThickness;
  marker.width = 3.5;
  marker.preserved.children.push_back("<userData code=\"rm:stopline\" contact=\"end\"/>");
  ASSERT_TRUE(network.add_object(lonely, std::move(marker)).is_valid());

  const std::string xml = write(network);
  auto reparsed = roadmaker::parse_xodr(xml, "stoplines");
  ASSERT_TRUE(reparsed.has_value());
  EXPECT_FALSE(reparsed->diagnostics.empty()) << "the orphan tag is reported";

  std::size_t arena_objects = 0;
  reparsed->network.for_each_object([&](roadmaker::ObjectId, const roadmaker::Object& object) {
    ++arena_objects;
    EXPECT_EQ(object.odr_id, "sl_x");
  });
  EXPECT_EQ(arena_objects, 1U);
  // The userData survived, so a later edit that does attach a junction can
  // still read the record.
  EXPECT_NE(write(reparsed->network).find("rm:stopline"), std::string::npos);
}

TEST(StopLinePersistence, ALegacyPlainSignalLinesObjectSurvivesAndSuppressesTheDefault) {
  CrossFixture fixture;
  roadmaker::Object legacy;
  legacy.road = fixture.west;
  legacy.odr_id = "900";
  legacy.type_str = "roadMark";
  legacy.subtype = "signalLines";
  legacy.s = 55.0;
  legacy.t = -1.75;
  legacy.length = kStopLineThickness;
  legacy.width = 3.5;
  ASSERT_TRUE(fixture.network.add_object(fixture.west, std::move(legacy)).is_valid());

  const std::string xml = write(fixture.network);
  // Three derived lines, plus the legacy object which is NOT tagged.
  EXPECT_EQ(count(xml, "code=\"rm:stopline\""), 3U);
  EXPECT_EQ(count(xml, "subtype=\"signalLines\""), 4U);

  auto reparsed = roadmaker::parse_xodr(xml, "stoplines");
  ASSERT_TRUE(reparsed.has_value());
  std::size_t arena_objects = 0;
  reparsed->network.for_each_object(
      [&](roadmaker::ObjectId, const roadmaker::Object&) { ++arena_objects; });
  EXPECT_EQ(arena_objects, 1U) << "the legacy object round-trips verbatim";
  EXPECT_EQ(junction_stoplines(reparsed->network, reparsed->network.find_junction("1")).size(), 3U);
  EXPECT_EQ(write(reparsed->network), xml);
}

// DISABLED by default so normal runs never write into the source tree.
// Regenerate the committed rm:stopline fuzz-corpus seed with
// --gtest_also_run_disabled_tests --gtest_filter='*DISABLED_WriteCorpusSeed'.
// The malformed companion (bad_stopline_malformed.xodr) is hand-derived from
// this file's output and is NOT regenerated.
TEST(StopLinePersistence, DISABLED_WriteCorpusSeed) {
  namespace fs = std::filesystem;
  CrossFixture fixture;
  // Exercise every attribute of the grammar: one arm authors a setback, a flip
  // and a crosswalk link; the other three stay pure defaults.
  ASSERT_TRUE(
      roadmaker::edit::set_stopline_distance(
          fixture.network, fixture.junction, fixture.arm_end(fixture.east), 2.5, std::string("7"))
          ->apply(fixture.network)
          .has_value());
  ASSERT_TRUE(roadmaker::edit::flip_stopline(
                  fixture.network, fixture.junction, fixture.arm_end(fixture.east))
                  ->apply(fixture.network)
                  .has_value());
  ASSERT_TRUE(roadmaker::save_xodr(fixture.network,
                                   fs::path(RM_FUZZ_CORPUS_DIR) / "objects_stopline.xodr",
                                   "objects_stopline")
                  .has_value());
}

TEST(StopLinePersistence, TheFuzzCorpusSeedsParseAndReExport) {
  // The two rm:stopline seeds the fuzzer starts from must be real inputs: the
  // well-formed one absorbs its records, the malformed one degrades with
  // warnings. Neither may crash, and both must survive a re-export.
  namespace fs = std::filesystem;
  const fs::path corpus = fs::path(RM_FUZZ_CORPUS_DIR);

  auto good = roadmaker::load_xodr(corpus / "objects_stopline.xodr");
  ASSERT_TRUE(good.has_value()) << "objects_stopline.xodr must parse";
  const JunctionId junction = good->network.find_junction("1");
  ASSERT_TRUE(junction.is_valid());
  // Road 1's line is a pure default (absorbed silently); road 2's authors
  // distance + flip + crosswalk, so exactly one record survives.
  ASSERT_EQ(good->network.junction(junction)->stoplines.size(), 1U);
  const StopLine& record = good->network.junction(junction)->stoplines.front();
  ASSERT_TRUE(record.distance.has_value());
  EXPECT_DOUBLE_EQ(*record.distance, 2.5);
  EXPECT_TRUE(record.flipped);
  EXPECT_EQ(record.crosswalk_odr_id, "7");
  EXPECT_FALSE(write(good->network).empty());

  auto bad = roadmaker::load_xodr(corpus / "bad_stopline_malformed.xodr");
  ASSERT_TRUE(bad.has_value()) << "a malformed record degrades, it does not fail the load";
  EXPECT_FALSE(bad->diagnostics.empty());
  // Every unreadable tag left its object live rather than dropping the line.
  std::size_t arena_objects = 0;
  bad->network.for_each_object(
      [&](roadmaker::ObjectId, const roadmaker::Object&) { ++arena_objects; });
  EXPECT_GT(arena_objects, 0U);
  EXPECT_FALSE(write(bad->network).empty());
}

TEST(StopLinePersistence, IdCollisionWithAnExistingObjectIsResolved) {
  CrossFixture fixture;
  // An authored object already owning the id the generator would mint.
  roadmaker::Object squatter;
  squatter.road = fixture.east;
  squatter.odr_id = "sl_1_1_e";
  squatter.type_str = "pole";
  squatter.s = 5.0;
  squatter.t = 3.0;
  ASSERT_TRUE(fixture.network.add_object(fixture.east, std::move(squatter)).is_valid());

  const std::string xml = write(fixture.network);
  EXPECT_EQ(count(xml, "id=\"sl_1_1_e\""), 1U) << "the squatter keeps its id";
  EXPECT_EQ(count(xml, "id=\"sl_1_1_e_1\""), 1U) << "the stop line takes a suffixed one";
  EXPECT_EQ(count(xml, "code=\"rm:stopline\""), 4U);

  auto reparsed = roadmaker::parse_xodr(xml, "stoplines");
  ASSERT_TRUE(reparsed.has_value());
  EXPECT_EQ(write(reparsed->network), xml);
}
