// Copyright 2026 Robomous
// SPDX-License-Identifier: Apache-2.0

// Persistence of the junction lock flag and of span (virtual) junctions
// (p4-s4, issue #319).
//
// Layer 0 (ADR-0008): a span junction is a spec-valid
//   <junction id=… name=… type="virtual" mainRoad=… orientation="none"
//             sStart=… sEnd=…>
// (ASAM OpenDRIVE 1.9.0 §12.7 Table 69, identical in 1.8.1 §12.7; all four
// attributes are mandatory there and FORBIDDEN elsewhere per
// asam.net:xodr:1.5.0:junctions.common.virtual_junction_attributes), with no
// <connection>, no <boundary> and no elevation grid.
//
// Layer 1: `locked=1` joins the existing <userData code="rm:junction">
// "key=value" list (order r;mat;locked, emitted only when true), and the full
// span list rides a sibling <userData code="rm:spans"> whose value is
// ";"-joined "roadOdrId:s_start:s_end" — spans[0] included, so a well-formed
// value can REPLACE the single Layer-0 span and still round-trip byte for byte.
// Mirrors the rm:arms contract: stale references are dropped on write, a
// malformed value is all-or-nothing on read (one warning, junction still
// loads).

#include "roadmaker/edit/operations.hpp"
#include "roadmaker/road/authoring.hpp"
#include "roadmaker/xodr/reader.hpp"
#include "roadmaker/xodr/writer.hpp"

#include <gtest/gtest.h>

#include <array>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using roadmaker::ContactPoint;
using roadmaker::Junction;
using roadmaker::JunctionId;
using roadmaker::LaneProfile;
using roadmaker::RoadEnd;
using roadmaker::RoadId;
using roadmaker::RoadNetwork;
using roadmaker::Severity;
using roadmaker::SpanArm;
using roadmaker::Waypoint;

namespace {

RoadId straight(RoadNetwork& network, double y, double x_begin, double x_end, const char* odr_id) {
  const std::vector<Waypoint> waypoints{Waypoint{x_begin, y}, Waypoint{x_end, y}};
  return *roadmaker::author_clothoid_road(
      network, waypoints, LaneProfile::two_lane_default(), "", odr_id);
}

/// A generated four-arm cross junction over roads "1".."4"; the junction lands
/// with odr_id "1" and a full rm:arms list.
struct CrossFixture {
  RoadNetwork network;
  JunctionId junction;

  CrossFixture() {
    const auto arm = [&](std::vector<Waypoint> waypoints, const char* id) {
      return *roadmaker::author_clothoid_road(
          network, waypoints, LaneProfile::two_lane_default(), "", id);
    };
    const RoadId west = arm({Waypoint{-40.0, 0.0}, Waypoint{-6.0, 0.0}}, "1");
    const RoadId east = arm({Waypoint{40.0, 0.0}, Waypoint{6.0, 0.0}}, "2");
    const RoadId south = arm({Waypoint{0.0, -40.0}, Waypoint{0.0, -6.0}}, "3");
    const RoadId north = arm({Waypoint{0.0, 40.0}, Waypoint{0.0, 6.0}}, "4");
    const std::array<RoadEnd, 4> ends{RoadEnd{.road = west, .contact = ContactPoint::End},
                                      RoadEnd{.road = east, .contact = ContactPoint::End},
                                      RoadEnd{.road = south, .contact = ContactPoint::End},
                                      RoadEnd{.road = north, .contact = ContactPoint::End}};
    EXPECT_TRUE(roadmaker::edit::create_junction(network, ends)->apply(network).has_value());
    junction = network.find_junction("1");
    EXPECT_TRUE(junction.is_valid());
  }
};

/// A network with one uninterrupted road "1" carrying a mid-road span junction
/// (the crosswalk case: the main road is never cut).
struct SpanFixture {
  RoadNetwork network;
  RoadId road;
  JunctionId junction;

  SpanFixture() {
    road = straight(network, 0.0, 0.0, 120.0, "1");
    junction = network.create_junction("9", "crosswalk");
    network.junction(junction)->spans.push_back(
        SpanArm{.road = road, .s_start = 50.0, .s_end = 56.5});
    network.junction(junction)->locked = true;
  }
};

std::string write(const RoadNetwork& network) {
  const auto xml = roadmaker::write_xodr(network, "junction_lock");
  EXPECT_TRUE(xml.has_value());
  return xml.has_value() ? *xml : std::string{};
}

/// The value= payload of the junction's userData element with `code`, or
/// nullopt when no such element was written.
std::optional<std::string> user_data_value(const std::string& xml, std::string_view code) {
  const std::string key = "code=\"" + std::string(code) + "\" value=\"";
  const std::size_t at = xml.find(key);
  if (at == std::string::npos) {
    return std::nullopt;
  }
  const std::size_t begin = at + key.size();
  return xml.substr(begin, xml.find('"', begin) - begin);
}

std::size_t count_warnings_mentioning(const std::vector<roadmaker::Diagnostic>& diagnostics,
                                      std::string_view needle) {
  std::size_t total = 0;
  for (const roadmaker::Diagnostic& diagnostic : diagnostics) {
    if (diagnostic.severity == Severity::Warning &&
        diagnostic.message.find(needle) != std::string::npos) {
      ++total;
    }
  }
  return total;
}

/// Rewrites the rm:junction (or rm:spans) value in place, the way a file
/// edited by hand — or written by a newer build — would carry it.
std::string
with_user_data_value(const std::string& xml, std::string_view code, const std::string& value) {
  const std::string key = "code=\"" + std::string(code) + "\" value=\"";
  const std::size_t at = xml.find(key);
  EXPECT_NE(at, std::string::npos);
  const std::size_t begin = at + key.size();
  const std::size_t end = xml.find('"', begin);
  return xml.substr(0, begin) + value + xml.substr(end);
}

} // namespace

// --- the lock flag ---------------------------------------------------------

TEST(JunctionLockPersistence, UnlockedArmJunctionKeepsItsPreFeatureBytes) {
  CrossFixture fixture;
  const std::string xml = write(fixture.network);

  // Neither the flag nor any virtual-junction attribute may appear: rule
  // asam.net:xodr:1.5.0:junctions.common.virtual_junction_attributes forbids
  // @mainRoad/@orientation/@sStart/@sEnd on a non-virtual junction.
  // (`orientation` on its own would also match the stop-line <object>s, so the
  // whole opening tag is asserted.)
  EXPECT_NE(xml.find("<junction id=\"1\">"), std::string::npos) << xml;
  EXPECT_EQ(xml.find("locked"), std::string::npos);
  EXPECT_EQ(xml.find("type=\"virtual\""), std::string::npos);
  EXPECT_EQ(xml.find("mainRoad"), std::string::npos);
  EXPECT_EQ(xml.find("rm:spans"), std::string::npos);

  auto reparsed = roadmaker::parse_xodr(xml, "junction_lock");
  ASSERT_TRUE(reparsed.has_value());
  EXPECT_FALSE(reparsed->network.junction(reparsed->network.find_junction("1"))->locked);
  EXPECT_EQ(write(reparsed->network), xml);
}

TEST(JunctionLockPersistence, LockedArmJunctionSurvivesWriteParseWrite) {
  CrossFixture fixture;
  fixture.network.junction(fixture.junction)->locked = true;
  const std::string xml = write(fixture.network);
  EXPECT_EQ(user_data_value(xml, "rm:junction"), std::optional<std::string>("locked=1"));

  auto reparsed = roadmaker::parse_xodr(xml, "junction_lock");
  ASSERT_TRUE(reparsed.has_value());
  EXPECT_EQ(roadmaker::count_errors(reparsed->diagnostics), 0U);
  const Junction& round = *reparsed->network.junction(reparsed->network.find_junction("1"));
  EXPECT_TRUE(round.locked);
  EXPECT_EQ(round.arms.size(), 4U);
  EXPECT_TRUE(round.spans.empty()); // arms-xor-spans
  EXPECT_EQ(write(reparsed->network), xml) << "write->parse->write must be byte-identical";
}

TEST(JunctionLockPersistence, LockRidesAlongsideTheOtherJunctionScopeKeys) {
  CrossFixture fixture;
  Junction& junction = *fixture.network.junction(fixture.junction);
  junction.default_corner_radius = 4.25;
  junction.material = "concrete";
  junction.locked = true;

  const std::string xml = write(fixture.network);
  // Writer key order is r;mat;locked — the flag is appended, so older keys keep
  // their bytes.
  EXPECT_EQ(user_data_value(xml, "rm:junction"),
            std::optional<std::string>("r=4.25;mat=concrete;locked=1"));

  auto reparsed = roadmaker::parse_xodr(xml, "junction_lock");
  ASSERT_TRUE(reparsed.has_value());
  const Junction& round = *reparsed->network.junction(reparsed->network.find_junction("1"));
  EXPECT_TRUE(round.locked);
  EXPECT_EQ(round.default_corner_radius, std::optional<double>(4.25));
  EXPECT_EQ(round.material, "concrete");
  EXPECT_EQ(write(reparsed->network), xml);
}

// --- span (virtual) junctions ----------------------------------------------

TEST(JunctionLockPersistence, SingleRoadSpanJunctionWritesAValidVirtualJunction) {
  SpanFixture fixture;
  const std::string xml = write(fixture.network);

  // Layer 0: all four mandatory attributes of §12.7 Table 69.
  EXPECT_NE(xml.find("<junction id=\"9\" name=\"crosswalk\" type=\"virtual\" mainRoad=\"1\" "
                     "orientation=\"none\" sStart=\"50\" sEnd=\"56.5\">"),
            std::string::npos)
      << xml;
  // …and none of the common-junction machinery.
  EXPECT_EQ(xml.find("<connection"), std::string::npos);
  EXPECT_EQ(xml.find("<boundary"), std::string::npos);
  EXPECT_EQ(xml.find("elevationGrid"), std::string::npos);
  EXPECT_EQ(xml.find("rm:arms"), std::string::npos);
  // Layer 1: the full list, spans[0] included.
  EXPECT_EQ(user_data_value(xml, "rm:spans"), std::optional<std::string>("1:50:56.5"));
  EXPECT_EQ(user_data_value(xml, "rm:junction"), std::optional<std::string>("locked=1"));

  auto reparsed = roadmaker::parse_xodr(xml, "junction_lock");
  ASSERT_TRUE(reparsed.has_value());
  EXPECT_EQ(roadmaker::count_errors(reparsed->diagnostics), 0U);
  EXPECT_TRUE(reparsed->diagnostics.empty())
      << "never write what you cannot read back; first: " << reparsed->diagnostics.front().message;

  const Junction& round = *reparsed->network.junction(reparsed->network.find_junction("9"));
  ASSERT_EQ(round.spans.size(), 1U);
  EXPECT_EQ(reparsed->network.road(round.spans[0].road)->odr_id, "1");
  EXPECT_DOUBLE_EQ(round.spans[0].s_start, 50.0);
  EXPECT_DOUBLE_EQ(round.spans[0].s_end, 56.5);
  EXPECT_TRUE(round.locked) << "a span junction is locked by definition";
  EXPECT_TRUE(round.arms.empty());
  EXPECT_TRUE(round.connections.empty());
  EXPECT_EQ(write(reparsed->network), xml) << "write->parse->write must be byte-identical";
}

TEST(JunctionLockPersistence, ParallelRoadSpanJunctionRoundTripsEverySpan) {
  RoadNetwork network;
  const RoadId north = straight(network, 6.0, 0.0, 120.0, "1");
  const RoadId south = straight(network, -6.0, 0.0, 120.0, "2");
  const JunctionId junction = network.create_junction("9", "crosswalk");
  network.junction(junction)->spans = {SpanArm{.road = north, .s_start = 40.0, .s_end = 44.0},
                                       SpanArm{.road = south, .s_start = 41.5, .s_end = 45.5}};
  network.junction(junction)->locked = true;

  const std::string xml = write(network);
  // Only spans[0] fits Layer 0; the whole list rides rm:spans.
  EXPECT_NE(xml.find("mainRoad=\"1\""), std::string::npos);
  EXPECT_NE(xml.find("sStart=\"40\" sEnd=\"44\""), std::string::npos);
  EXPECT_EQ(user_data_value(xml, "rm:spans"), std::optional<std::string>("1:40:44;2:41.5:45.5"));

  auto reparsed = roadmaker::parse_xodr(xml, "junction_lock");
  ASSERT_TRUE(reparsed.has_value());
  EXPECT_TRUE(reparsed->diagnostics.empty());
  const Junction& round = *reparsed->network.junction(reparsed->network.find_junction("9"));
  ASSERT_EQ(round.spans.size(), 2U);
  EXPECT_EQ(reparsed->network.road(round.spans[1].road)->odr_id, "2");
  EXPECT_DOUBLE_EQ(round.spans[1].s_start, 41.5);
  EXPECT_DOUBLE_EQ(round.spans[1].s_end, 45.5);
  EXPECT_EQ(write(reparsed->network), xml);
}

TEST(JunctionLockPersistence, StaleSpanRoadIsWarnedAboutAndNotWritten) {
  SpanFixture fixture;
  fixture.network.junction(fixture.junction)
      ->spans.push_back(SpanArm{.road = RoadId{}, .s_start = 1.0, .s_end = 2.0});

  const auto findings = roadmaker::validate_network(fixture.network, {});
  EXPECT_EQ(count_warnings_mentioning(findings, "span junction references a road"), 1U);

  // All-or-nothing: nothing virtual is written, and the junction still loads.
  const std::string xml = write(fixture.network);
  EXPECT_EQ(xml.find("type=\"virtual\""), std::string::npos);
  EXPECT_EQ(xml.find("rm:spans"), std::string::npos);
  auto reparsed = roadmaker::parse_xodr(xml, "junction_lock");
  ASSERT_TRUE(reparsed.has_value());
  EXPECT_TRUE(reparsed->network.junction(reparsed->network.find_junction("9"))->spans.empty());
}

// --- degradation -----------------------------------------------------------

TEST(JunctionLockPersistence, UnknownRmJunctionKeyWarnsAndTheJunctionStillLoads) {
  CrossFixture fixture;
  fixture.network.junction(fixture.junction)->locked = true;
  const std::string xml =
      with_user_data_value(write(fixture.network), "rm:junction", "locked=1;flux=3");

  auto reparsed = roadmaker::parse_xodr(xml, "junction_lock");
  ASSERT_TRUE(reparsed.has_value());
  EXPECT_EQ(count_warnings_mentioning(reparsed->diagnostics, "'flux=3' is not understood"), 1U);
  const Junction& round = *reparsed->network.junction(reparsed->network.find_junction("1"));
  EXPECT_TRUE(round.locked) << "an unknown key is skipped, the known ones still apply";
  EXPECT_EQ(round.arms.size(), 4U);
}

TEST(JunctionLockPersistence, BogusLockedValueWarnsAndDropsTheWholeValue) {
  CrossFixture fixture;
  fixture.network.junction(fixture.junction)->locked = true;
  const std::string xml =
      with_user_data_value(write(fixture.network), "rm:junction", "locked=bogus");

  auto reparsed = roadmaker::parse_xodr(xml, "junction_lock");
  ASSERT_TRUE(reparsed.has_value());
  EXPECT_EQ(count_warnings_mentioning(reparsed->diagnostics, "malformed rm:junction"), 1U);
  EXPECT_FALSE(reparsed->network.junction(reparsed->network.find_junction("1"))->locked);
}

TEST(JunctionLockPersistence, MalformedSpansKeepsTheLayerZeroSpan) {
  SpanFixture fixture;
  const std::string good = write(fixture.network);

  for (const std::string& value : {std::string("1:50"),
                                   std::string("1:50:56.5;77:1:2"),
                                   std::string("1:56.5:50"),
                                   std::string("1:x:2")}) {
    const std::string xml = with_user_data_value(good, "rm:spans", value);
    auto reparsed = roadmaker::parse_xodr(xml, "junction_lock");
    ASSERT_TRUE(reparsed.has_value()) << value;
    EXPECT_EQ(count_warnings_mentioning(reparsed->diagnostics, "malformed rm:spans"), 1U) << value;

    const Junction& round = *reparsed->network.junction(reparsed->network.find_junction("9"));
    ASSERT_EQ(round.spans.size(), 1U) << value;
    EXPECT_EQ(reparsed->network.road(round.spans[0].road)->odr_id, "1");
    EXPECT_DOUBLE_EQ(round.spans[0].s_start, 50.0);
    EXPECT_DOUBLE_EQ(round.spans[0].s_end, 56.5);
    EXPECT_TRUE(round.locked);
  }
}

TEST(JunctionLockPersistence, UnresolvableMainRoadWarnsAndLoadsAsAPlainJunction) {
  SpanFixture fixture;
  std::string xml = write(fixture.network);
  const std::size_t at = xml.find("mainRoad=\"1\"");
  ASSERT_NE(at, std::string::npos);
  xml.replace(at, std::string("mainRoad=\"1\"").size(), "mainRoad=\"7\"");
  // Drop rm:spans too, otherwise Layer 1 would repair what Layer 0 lost.
  const std::size_t spans = xml.find("<userData code=\"rm:spans\"");
  ASSERT_NE(spans, std::string::npos);
  xml.erase(spans, xml.find("/>", spans) + 2 - spans);

  auto reparsed = roadmaker::parse_xodr(xml, "junction_lock");
  ASSERT_TRUE(reparsed.has_value());
  EXPECT_EQ(count_warnings_mentioning(reparsed->diagnostics, "unknown mainRoad '7'"), 1U);
  const Junction& round = *reparsed->network.junction(reparsed->network.find_junction("9"));
  EXPECT_TRUE(round.spans.empty());
  EXPECT_TRUE(round.locked) << "the rm:junction locked=1 still applied";
}

// --- fuzz corpus -----------------------------------------------------------

// DISABLED by default so normal runs never write into the source tree.
// Regenerate the committed p4-s4 fuzz-corpus seeds with
// --gtest_also_run_disabled_tests --gtest_filter='*JunctionLock*WriteCorpusSeed'.
// The three malformed companions (bad_locked_value.xodr, bad_spans_inverted.xodr,
// bad_spans_unknown_road.xodr) are hand-derived from these files and are NOT
// regenerated.
TEST(JunctionLockPersistence, DISABLED_WriteCorpusSeed) {
  namespace fs = std::filesystem;
  const fs::path corpus = fs::path(RM_FUZZ_CORPUS_DIR);

  CrossFixture locked;
  locked.network.junction(locked.junction)->locked = true;
  ASSERT_TRUE(
      roadmaker::save_xodr(locked.network, corpus / "locked_junction.xodr", "locked_junction")
          .has_value());

  SpanFixture single;
  ASSERT_TRUE(roadmaker::save_xodr(
                  single.network, corpus / "span_junction_single.xodr", "span_junction_single")
                  .has_value());

  RoadNetwork parallel;
  const RoadId north = straight(parallel, 6.0, 0.0, 120.0, "1");
  const RoadId south = straight(parallel, -6.0, 0.0, 120.0, "2");
  const JunctionId junction = parallel.create_junction("9", "crosswalk");
  parallel.junction(junction)->spans = {SpanArm{.road = north, .s_start = 40.0, .s_end = 44.0},
                                        SpanArm{.road = south, .s_start = 41.5, .s_end = 45.5}};
  parallel.junction(junction)->locked = true;
  ASSERT_TRUE(roadmaker::save_xodr(
                  parallel, corpus / "span_junction_parallel.xodr", "span_junction_parallel")
                  .has_value());
}

TEST(JunctionLockPersistence, TheFuzzCorpusSeedsParseAndReExport) {
  namespace fs = std::filesystem;
  const fs::path corpus = fs::path(RM_FUZZ_CORPUS_DIR);

  auto locked = roadmaker::load_xodr(corpus / "locked_junction.xodr");
  ASSERT_TRUE(locked.has_value()) << "locked_junction.xodr must parse";
  EXPECT_TRUE(locked->diagnostics.empty());
  EXPECT_TRUE(locked->network.junction(locked->network.find_junction("1"))->locked);

  auto single = roadmaker::load_xodr(corpus / "span_junction_single.xodr");
  ASSERT_TRUE(single.has_value());
  EXPECT_TRUE(single->diagnostics.empty());
  EXPECT_EQ(single->network.junction(single->network.find_junction("9"))->spans.size(), 1U);

  auto parallel = roadmaker::load_xodr(corpus / "span_junction_parallel.xodr");
  ASSERT_TRUE(parallel.has_value());
  EXPECT_TRUE(parallel->diagnostics.empty());
  EXPECT_EQ(parallel->network.junction(parallel->network.find_junction("9"))->spans.size(), 2U);

  // The hand-derived malformed companions degrade with a warning; none fails
  // the load and none crashes.
  for (const char* name :
       {"bad_locked_value.xodr", "bad_spans_inverted.xodr", "bad_spans_unknown_road.xodr"}) {
    auto bad = roadmaker::load_xodr(corpus / name);
    ASSERT_TRUE(bad.has_value()) << name;
    EXPECT_FALSE(bad->diagnostics.empty()) << name;
    EXPECT_FALSE(write(bad->network).empty()) << name;
  }
}
