// Copyright 2026 Robomous
// SPDX-License-Identifier: Apache-2.0

// Persistence of the junction floor's per-connecting-road surface spans
// (p4-s5, issue #320).
//
// Layer 0 (ADR-0008) has nothing to carry: ASAM OpenDRIVE 1.9.0 §12.10 gives
// <junction> only <boundary> and <elevationGrid>, both of them derived OUTPUT
// geometry with no say in how the pavement is triangulated. The records
// therefore ride Layer 1 alone: a <userData code="rm:floor"> whose value is
// ";"-joined "roadOdrId[:inc=0][:sort=<int>]".
//
// The code is rm:floor and not rm:surface — that one already belongs to the
// P2 ground surfaces (a root-level element) and its bytes must stay stable.
//
// Contract mirrored from rm:corners: fields appear only when they author
// something (so a pre-feature junction re-exports byte-identically), stale road
// references are dropped on write, storage order is kept, and a malformed entry
// is all-or-nothing on read. Unknown FIELD keys inside an entry are the
// rm:junction forward-compat case instead: warn and skip the field.

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
using roadmaker::SurfaceSpan;
using roadmaker::Waypoint;

namespace {

RoadId straight(RoadNetwork& network, double y, double x_begin, double x_end, const char* odr_id) {
  const std::vector<Waypoint> waypoints{Waypoint{x_begin, y}, Waypoint{x_end, y}};
  return *roadmaker::author_clothoid_road(
      network, waypoints, LaneProfile::two_lane_default(), "", odr_id);
}

/// A generated four-arm cross junction over roads "1".."4"; the junction lands
/// with odr_id "1", a full rm:arms list and twelve connecting roads.
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

  /// The connecting roads of the junction, in connection order, de-duplicated.
  [[nodiscard]] std::vector<RoadId> turns() const {
    std::vector<RoadId> out;
    for (const roadmaker::JunctionConnection& connection :
         network.junction(junction)->connections) {
      if (std::ranges::find(out, connection.connecting_road) == out.end()) {
        out.push_back(connection.connecting_road);
      }
    }
    return out;
  }
};

std::string write(const RoadNetwork& network) {
  const auto xml = roadmaker::write_xodr(network, "surface_spans");
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

/// Rewrites the rm:floor value in place, the way a hand-edited file — or one
/// written by a newer build — would carry it.
std::string
with_user_data_value(const std::string& xml, std::string_view code, const std::string& value) {
  const std::string key = "code=\"" + std::string(code) + "\" value=\"";
  const std::size_t at = xml.find(key);
  EXPECT_NE(at, std::string::npos);
  const std::size_t begin = at + key.size();
  const std::size_t end = xml.find('"', begin);
  return xml.substr(0, begin) + value + xml.substr(end);
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

} // namespace

TEST(SurfaceSpanPersistence, PreSprintFileReExportsByteIdentical) {
  CrossFixture fixture;
  const std::string xml = write(fixture.network);
  EXPECT_EQ(xml.find("rm:floor"), std::string::npos)
      << "an unauthored junction must keep its exact pre-p4-s5 bytes";

  auto reparsed = roadmaker::parse_xodr(xml, "surface_spans");
  ASSERT_TRUE(reparsed.has_value());
  EXPECT_TRUE(
      reparsed->network.junction(reparsed->network.find_junction("1"))->surface_spans.empty());
  EXPECT_EQ(write(reparsed->network), xml);
}

TEST(SurfaceSpanPersistence, RoundTripsAuthoredRecords) {
  CrossFixture fixture;
  const std::vector<RoadId> turns = fixture.turns();
  ASSERT_GE(turns.size(), 3U);
  Junction& junction = *fixture.network.junction(fixture.junction);
  junction.surface_spans = {SurfaceSpan{.road = turns[0], .included = false},
                            SurfaceSpan{.road = turns[1], .sort_index = 2},
                            SurfaceSpan{.road = turns[2], .included = false, .sort_index = -3}};

  const std::string xml = write(fixture.network);
  const std::string* road0 = &fixture.network.road(turns[0])->odr_id;
  const std::string* road1 = &fixture.network.road(turns[1])->odr_id;
  const std::string* road2 = &fixture.network.road(turns[2])->odr_id;
  EXPECT_EQ(user_data_value(xml, "rm:floor"),
            std::optional<std::string>(*road0 + ":inc=0;" + *road1 + ":sort=2;" + *road2 +
                                       ":inc=0:sort=-3"));

  auto reparsed = roadmaker::parse_xodr(xml, "surface_spans");
  ASSERT_TRUE(reparsed.has_value());
  EXPECT_EQ(roadmaker::count_errors(reparsed->diagnostics), 0U);
  const Junction& round = *reparsed->network.junction(reparsed->network.find_junction("1"));
  ASSERT_EQ(round.surface_spans.size(), 3U);
  EXPECT_FALSE(round.surface_spans[0].included);
  EXPECT_EQ(round.surface_spans[0].sort_index, 0);
  EXPECT_TRUE(round.surface_spans[1].included);
  EXPECT_EQ(round.surface_spans[1].sort_index, 2);
  EXPECT_FALSE(round.surface_spans[2].included);
  EXPECT_EQ(round.surface_spans[2].sort_index, -3);
  EXPECT_EQ(write(reparsed->network), xml) << "write->parse->write must be byte-identical";
}

TEST(SurfaceSpanPersistence, RecordsThatAuthorNothingAreNotWritten) {
  CrossFixture fixture;
  const std::vector<RoadId> turns = fixture.turns();
  ASSERT_GE(turns.size(), 2U);
  Junction& junction = *fixture.network.junction(fixture.junction);
  // A defaulted record is indistinguishable from no record at all — that is
  // what makes toggle-twice byte-identical to never having touched the span.
  junction.surface_spans = {SurfaceSpan{.road = turns[0]}, SurfaceSpan{.road = turns[1]}};
  EXPECT_EQ(write(fixture.network).find("rm:floor"), std::string::npos);
}

TEST(SurfaceSpanPersistence, StaleRoadRecordIsNotWritten) {
  CrossFixture fixture;
  const std::vector<RoadId> turns = fixture.turns();
  ASSERT_GE(turns.size(), 2U);
  Junction& junction = *fixture.network.junction(fixture.junction);
  junction.surface_spans = {SurfaceSpan{.road = turns[0], .sort_index = 5},
                            SurfaceSpan{.road = turns[1], .included = false}};
  const std::string kept = fixture.network.road(turns[0])->odr_id;
  fixture.network.erase_road(turns[1]);

  const std::string xml = write(fixture.network);
  EXPECT_EQ(user_data_value(xml, "rm:floor"), std::optional<std::string>(kept + ":sort=5"));

  // Every surviving record still resolves, so the file reloads clean.
  auto reparsed = roadmaker::parse_xodr(xml, "surface_spans");
  ASSERT_TRUE(reparsed.has_value());
  EXPECT_EQ(count_warnings_mentioning(reparsed->diagnostics, "rm:floor"), 0U);
}

TEST(SurfaceSpanPersistence, SpanJunctionNeverCarriesFloorRecords) {
  RoadNetwork network;
  const RoadId road = straight(network, 0.0, 0.0, 120.0, "1");
  const JunctionId junction = network.create_junction("9", "crosswalk");
  network.junction(junction)->spans.push_back(
      SpanArm{.road = road, .s_start = 50.0, .s_end = 56.5});
  network.junction(junction)->locked = true;
  // A span junction has no floor; a record that somehow reached it is inert.
  network.junction(junction)->surface_spans.push_back(
      SurfaceSpan{.road = road, .included = false, .sort_index = 7});

  const std::string xml = write(network);
  EXPECT_EQ(xml.find("rm:floor"), std::string::npos);

  auto reparsed = roadmaker::parse_xodr(xml, "surface_spans");
  ASSERT_TRUE(reparsed.has_value());
  EXPECT_TRUE(
      reparsed->network.junction(reparsed->network.find_junction("9"))->surface_spans.empty());
  EXPECT_EQ(write(reparsed->network), xml);
}

TEST(SurfaceSpanPersistence, MalformedValueWarnsAndDropsTheWholeRecordSet) {
  CrossFixture fixture;
  const std::vector<RoadId> turns = fixture.turns();
  ASSERT_GE(turns.size(), 2U);
  Junction& junction = *fixture.network.junction(fixture.junction);
  junction.surface_spans = {SurfaceSpan{.road = turns[0], .included = false},
                            SurfaceSpan{.road = turns[1], .sort_index = 2}};
  const std::string xml = write(fixture.network);
  const std::string good = *user_data_value(xml, "rm:floor");
  const std::string first = fixture.network.road(turns[0])->odr_id;

  // All-or-nothing on the entry grammar, exactly like rm:corners: an
  // unresolvable road, the same road twice, a bad flag value, a bad or
  // out-of-range sort index, and an entry that authors nothing.
  for (const std::string& bad : {std::string("no_such_road:inc=0"),
                                 first + ":inc=0;" + first + ":sort=2",
                                 first + ":inc=1",
                                 first + ":sort=2.5",
                                 first + ":sort=99999",
                                 first + ":sort=0",
                                 first}) {
    auto reparsed = roadmaker::parse_xodr(with_user_data_value(xml, "rm:floor", bad), "spans");
    ASSERT_TRUE(reparsed.has_value()) << bad;
    EXPECT_EQ(count_warnings_mentioning(reparsed->diagnostics, "malformed rm:floor userData"), 1U)
        << bad;
    EXPECT_TRUE(
        reparsed->network.junction(reparsed->network.find_junction("1"))->surface_spans.empty())
        << bad;
  }

  // The known-good value is the control.
  auto control = roadmaker::parse_xodr(with_user_data_value(xml, "rm:floor", good), "spans");
  ASSERT_TRUE(control.has_value());
  EXPECT_EQ(count_warnings_mentioning(control->diagnostics, "malformed rm:floor userData"), 0U);
}

TEST(SurfaceSpanPersistence, UnknownFieldKeyWarnsButKeepsTheEntry) {
  CrossFixture fixture;
  const std::vector<RoadId> turns = fixture.turns();
  ASSERT_FALSE(turns.empty());
  Junction& junction = *fixture.network.junction(fixture.junction);
  junction.surface_spans = {SurfaceSpan{.road = turns[0], .sort_index = 4}};
  const std::string xml = write(fixture.network);
  const std::string first = fixture.network.road(turns[0])->odr_id;

  // Forward compatibility (the rm:junction rule): a field a newer RoadMaker
  // added is reported and skipped, and the fields this build understands still
  // load.
  auto reparsed = roadmaker::parse_xodr(
      with_user_data_value(xml, "rm:floor", first + ":sort=4:blend=0.5"), "spans");
  ASSERT_TRUE(reparsed.has_value());
  EXPECT_EQ(count_warnings_mentioning(reparsed->diagnostics, "'blend=0.5' is not understood"), 1U);
  const Junction& round = *reparsed->network.junction(reparsed->network.find_junction("1"));
  ASSERT_EQ(round.surface_spans.size(), 1U);
  EXPECT_EQ(round.surface_spans[0].sort_index, 4);
}

// --- fuzz corpus -----------------------------------------------------------

// DISABLED by default so normal runs never write into the source tree.
// Regenerate the committed p4-s5 seed with
// --gtest_also_run_disabled_tests --gtest_filter='*SurfaceSpan*WriteCorpusSeed'.
// The four malformed companions (bad_floor_unknown_road.xodr,
// bad_floor_dup_entry.xodr, bad_floor_bad_sort.xodr, bad_floor_bad_inc.xodr)
// are hand-derived from it and are NOT regenerated.
TEST(SurfaceSpanPersistence, DISABLED_WriteCorpusSeed) {
  namespace fs = std::filesystem;
  const fs::path corpus = fs::path(RM_FUZZ_CORPUS_DIR);

  CrossFixture fixture;
  const std::vector<RoadId> turns = fixture.turns();
  ASSERT_GE(turns.size(), 2U);
  fixture.network.junction(fixture.junction)->surface_spans = {
      SurfaceSpan{.road = turns[0], .included = false},
      SurfaceSpan{.road = turns[1], .sort_index = 2}};
  ASSERT_TRUE(
      roadmaker::save_xodr(fixture.network, corpus / "junction_floor_spans.xodr", "junction_floor")
          .has_value());
}

TEST(SurfaceSpanPersistence, TheFuzzCorpusSeedsParseAndReExport) {
  namespace fs = std::filesystem;
  const fs::path corpus = fs::path(RM_FUZZ_CORPUS_DIR);

  auto seed = roadmaker::load_xodr(corpus / "junction_floor_spans.xodr");
  ASSERT_TRUE(seed.has_value()) << "junction_floor_spans.xodr must parse";
  EXPECT_TRUE(seed->diagnostics.empty());
  const Junction& junction = *seed->network.junction(seed->network.find_junction("1"));
  ASSERT_EQ(junction.surface_spans.size(), 2U);
  EXPECT_FALSE(junction.surface_spans[0].included);
  EXPECT_EQ(junction.surface_spans[1].sort_index, 2);
  EXPECT_FALSE(write(seed->network).empty());

  // The hand-derived malformed companions degrade with a warning; none fails
  // the load and none crashes.
  for (const char* name : {"bad_floor_unknown_road.xodr",
                           "bad_floor_dup_entry.xodr",
                           "bad_floor_bad_sort.xodr",
                           "bad_floor_bad_inc.xodr"}) {
    auto bad = roadmaker::load_xodr(corpus / name);
    ASSERT_TRUE(bad.has_value()) << name;
    EXPECT_FALSE(bad->diagnostics.empty()) << name;
    EXPECT_FALSE(write(bad->network).empty()) << name;
  }
}
