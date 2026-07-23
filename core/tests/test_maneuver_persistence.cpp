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

// Persistence of the junction's authored maneuver overrides (p4-s6, issue
// #227).
//
// Layer 0 (ADR-0008) has nothing to carry: ASAM OpenDRIVE 1.9.0 §12.2 Table 56
// gives <connection> exactly @connectingRoad, @contactPoint, @id and
// @incomingRoad, and §12.4/§12.4.2 describe a connecting road purely by its
// geometry and lane linkage. There is no turn-type, no endpoint slide and no
// control-point carrier anywhere in the standard, so the records ride Layer 1
// alone: a <userData code="rm:maneuver"> whose value is ";"-joined
// "roadOdrId[:lock=1][:turn=left|straight|right|uturn][:so=<num>][:eo=<num>]
//  [:pts=x,y|x,y|…]".
//
// Contract mirrored from rm:floor: fields appear only when they author
// something (so a pre-feature junction re-exports byte-identically), an entry
// that authors nothing is dropped entirely (the AUTHORS-NOTHING ⇒ ERASE rule),
// stale road references are dropped on write, storage order is kept, and a
// malformed entry is all-or-nothing on read. Unknown FIELD keys inside an
// entry are the rm:junction forward-compat case instead: warn and skip.
//
// Nothing is refitted on load — the planView is Layer 0 truth and wins.

#include "roadmaker/edit/operations.hpp"
#include "roadmaker/road/authoring.hpp"
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
using roadmaker::JunctionId;
using roadmaker::kMaxManeuverControlPoints;
using roadmaker::LaneProfile;
using roadmaker::Maneuver;
using roadmaker::RoadEnd;
using roadmaker::RoadId;
using roadmaker::RoadNetwork;
using roadmaker::Severity;
using roadmaker::SpanArm;
using roadmaker::TurnType;
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
  const auto xml = roadmaker::write_xodr(network, "maneuvers");
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

/// Rewrites the rm:maneuver value in place, the way a hand-edited file — or one
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

TEST(ManeuverPersistence, PreSprintFileReExportsByteIdentical) {
  CrossFixture fixture;
  const std::string xml = write(fixture.network);
  EXPECT_EQ(xml.find("rm:maneuver"), std::string::npos)
      << "an unauthored junction must keep its exact pre-p4-s6 bytes";

  auto reparsed = roadmaker::parse_xodr(xml, "maneuvers");
  ASSERT_TRUE(reparsed.has_value());
  EXPECT_TRUE(reparsed->network.junction(reparsed->network.find_junction("1"))->maneuvers.empty());
  EXPECT_EQ(write(reparsed->network), xml);
}

TEST(ManeuverPersistence, RoundTripsEveryFieldIndividually) {
  const std::array<Maneuver, 5> singles{
      Maneuver{.locked = true},
      Maneuver{.turn_type = TurnType::Left},
      Maneuver{.start_offset = 1.25},
      Maneuver{.end_offset = -0.75},
      Maneuver{.control_points = {Waypoint{2.5, -3.0}}},
  };
  const std::array<const char*, 5> expected{
      ":lock=1", ":turn=left", ":so=1.25", ":eo=-0.75", ":pts=2.5,-3"};

  for (std::size_t i = 0; i < singles.size(); ++i) {
    CrossFixture fixture;
    const std::vector<RoadId> turns = fixture.turns();
    ASSERT_FALSE(turns.empty());
    Maneuver record = singles[i];
    record.road = turns[0];
    fixture.network.junction(fixture.junction)->maneuvers = {record};

    const std::string xml = write(fixture.network);
    EXPECT_EQ(user_data_value(xml, "rm:maneuver"),
              std::optional<std::string>(fixture.network.road(turns[0])->odr_id + expected[i]))
        << i;

    auto reparsed = roadmaker::parse_xodr(xml, "maneuvers");
    ASSERT_TRUE(reparsed.has_value()) << i;
    EXPECT_EQ(roadmaker::count_errors(reparsed->diagnostics), 0U) << i;
    const Junction& round = *reparsed->network.junction(reparsed->network.find_junction("1"));
    ASSERT_EQ(round.maneuvers.size(), 1U) << i;
    EXPECT_EQ(round.maneuvers[0].locked, record.locked) << i;
    EXPECT_EQ(round.maneuvers[0].turn_type, record.turn_type) << i;
    EXPECT_EQ(round.maneuvers[0].start_offset, record.start_offset) << i;
    EXPECT_EQ(round.maneuvers[0].end_offset, record.end_offset) << i;
    EXPECT_EQ(round.maneuvers[0].control_points, record.control_points) << i;
    EXPECT_EQ(write(reparsed->network), xml) << i;
  }
}

TEST(ManeuverPersistence, RoundTripsEveryTurnTypeSpelling) {
  for (const TurnType turn :
       {TurnType::Left, TurnType::Straight, TurnType::Right, TurnType::UTurn}) {
    CrossFixture fixture;
    const std::vector<RoadId> turns = fixture.turns();
    ASSERT_FALSE(turns.empty());
    fixture.network.junction(fixture.junction)->maneuvers = {
        Maneuver{.road = turns[0], .turn_type = turn}};

    const std::string xml = write(fixture.network);
    auto reparsed = roadmaker::parse_xodr(xml, "maneuvers");
    ASSERT_TRUE(reparsed.has_value());
    const Junction& round = *reparsed->network.junction(reparsed->network.find_junction("1"));
    ASSERT_EQ(round.maneuvers.size(), 1U);
    EXPECT_EQ(round.maneuvers[0].turn_type, std::optional<TurnType>(turn));
    EXPECT_EQ(write(reparsed->network), xml);
  }
}

TEST(ManeuverPersistence, RoundTripsAllFieldsTogetherAndStaysByteStable) {
  CrossFixture fixture;
  const std::vector<RoadId> turns = fixture.turns();
  ASSERT_GE(turns.size(), 3U);
  Junction& junction = *fixture.network.junction(fixture.junction);
  junction.maneuvers = {
      Maneuver{.road = turns[0],
               .locked = true,
               .turn_type = TurnType::UTurn,
               .start_offset = 0.5,
               .end_offset = -1.5,
               .control_points = {Waypoint{1.0, 2.0}, Waypoint{-3.25, 4.5}, Waypoint{0.0, 0.0}}},
      Maneuver{.road = turns[1], .turn_type = TurnType::Right},
      Maneuver{.road = turns[2], .locked = true, .control_points = {Waypoint{7.5, 8.25}}},
  };

  const std::string xml = write(fixture.network);
  const std::string road0 = fixture.network.road(turns[0])->odr_id;
  const std::string road1 = fixture.network.road(turns[1])->odr_id;
  const std::string road2 = fixture.network.road(turns[2])->odr_id;
  EXPECT_EQ(user_data_value(xml, "rm:maneuver"),
            std::optional<std::string>(road0 +
                                       ":lock=1:turn=uturn:so=0.5:eo=-1.5:pts=1,2|-3.25,4.5|0,0;" +
                                       road1 + ":turn=right;" + road2 + ":lock=1:pts=7.5,8.25"));

  auto reparsed = roadmaker::parse_xodr(xml, "maneuvers");
  ASSERT_TRUE(reparsed.has_value());
  EXPECT_EQ(roadmaker::count_errors(reparsed->diagnostics), 0U);
  const Junction& round = *reparsed->network.junction(reparsed->network.find_junction("1"));
  ASSERT_EQ(round.maneuvers.size(), 3U);
  EXPECT_TRUE(round.maneuvers[0].locked);
  EXPECT_EQ(round.maneuvers[0].turn_type, std::optional<TurnType>(TurnType::UTurn));
  EXPECT_EQ(round.maneuvers[0].start_offset, std::optional<double>(0.5));
  EXPECT_EQ(round.maneuvers[0].end_offset, std::optional<double>(-1.5));
  EXPECT_EQ(round.maneuvers[0].control_points,
            (std::vector<Waypoint>{Waypoint{1.0, 2.0}, Waypoint{-3.25, 4.5}, Waypoint{0.0, 0.0}}));
  EXPECT_FALSE(round.maneuvers[1].locked);
  EXPECT_EQ(round.maneuvers[1].turn_type, std::optional<TurnType>(TurnType::Right));
  EXPECT_TRUE(round.maneuvers[1].control_points.empty());
  EXPECT_TRUE(round.maneuvers[2].locked);
  EXPECT_FALSE(round.maneuvers[2].turn_type.has_value());
  EXPECT_EQ(round.maneuvers[2].control_points, (std::vector<Waypoint>{Waypoint{7.5, 8.25}}));

  // save→reload→save is byte-stable, and stays so on a second lap.
  const std::string again = write(reparsed->network);
  EXPECT_EQ(again, xml);
  auto twice = roadmaker::parse_xodr(again, "maneuvers");
  ASSERT_TRUE(twice.has_value());
  EXPECT_EQ(write(twice->network), xml);
}

TEST(ManeuverPersistence, RecordsThatAuthorNothingAreNotWritten) {
  CrossFixture fixture;
  const std::vector<RoadId> turns = fixture.turns();
  ASSERT_GE(turns.size(), 2U);
  // A defaulted record is indistinguishable from no record at all — that is
  // what makes override-twice byte-identical to never having touched the
  // maneuver (AUTHORS-NOTHING ⇒ ERASE).
  fixture.network.junction(fixture.junction)->maneuvers = {Maneuver{.road = turns[0]},
                                                           Maneuver{.road = turns[1]}};
  EXPECT_EQ(write(fixture.network).find("rm:maneuver"), std::string::npos);
}

TEST(ManeuverPersistence, StaleRoadRecordIsNotWritten) {
  CrossFixture fixture;
  const std::vector<RoadId> turns = fixture.turns();
  ASSERT_GE(turns.size(), 2U);
  fixture.network.junction(fixture.junction)->maneuvers = {
      Maneuver{.road = turns[0], .turn_type = TurnType::Left},
      Maneuver{.road = turns[1], .locked = true}};
  const std::string kept = fixture.network.road(turns[0])->odr_id;
  fixture.network.erase_road(turns[1]);

  const std::string xml = write(fixture.network);
  EXPECT_EQ(user_data_value(xml, "rm:maneuver"), std::optional<std::string>(kept + ":turn=left"));

  auto reparsed = roadmaker::parse_xodr(xml, "maneuvers");
  ASSERT_TRUE(reparsed.has_value());
  EXPECT_EQ(count_warnings_mentioning(reparsed->diagnostics, "rm:maneuver"), 0U);
}

TEST(ManeuverPersistence, SpanJunctionNeverCarriesManeuverRecords) {
  RoadNetwork network;
  const RoadId road = straight(network, 0.0, 0.0, 120.0, "1");
  const JunctionId junction = network.create_junction("9", "crosswalk");
  network.junction(junction)->spans.push_back(
      SpanArm{.road = road, .s_start = 50.0, .s_end = 56.5});
  network.junction(junction)->locked = true;
  // A span junction has no connections; a record that somehow reached it is
  // inert.
  network.junction(junction)->maneuvers.push_back(
      Maneuver{.road = road, .locked = true, .turn_type = TurnType::Straight});

  const std::string xml = write(network);
  EXPECT_EQ(xml.find("rm:maneuver"), std::string::npos);

  auto reparsed = roadmaker::parse_xodr(xml, "maneuvers");
  ASSERT_TRUE(reparsed.has_value());
  EXPECT_TRUE(reparsed->network.junction(reparsed->network.find_junction("9"))->maneuvers.empty());
  EXPECT_EQ(write(reparsed->network), xml);
}

TEST(ManeuverPersistence, MalformedValueWarnsAndDropsTheWholeRecordSet) {
  CrossFixture fixture;
  const std::vector<RoadId> turns = fixture.turns();
  ASSERT_GE(turns.size(), 2U);
  fixture.network.junction(fixture.junction)->maneuvers = {
      Maneuver{.road = turns[0], .locked = true},
      Maneuver{.road = turns[1], .turn_type = TurnType::Straight}};
  const std::string xml = write(fixture.network);
  const std::string good = *user_data_value(xml, "rm:maneuver");
  const std::string first = fixture.network.road(turns[0])->odr_id;

  // A point list one past kMaxManeuverControlPoints — longer than any writer
  // would emit, so the whole value goes.
  std::string long_points = first + ":pts=";
  for (std::size_t i = 0; i <= kMaxManeuverControlPoints; ++i) {
    if (i != 0) {
      long_points += '|';
    }
    long_points += std::to_string(i) + ",0";
  }

  // All-or-nothing on the entry grammar, exactly like rm:floor: an
  // unresolvable road, the same road twice, a bad flag value, an unknown turn
  // spelling, a repeated known field, a non-numeric or malformed point, an
  // empty point list, an over-long point list, and an entry authoring nothing.
  for (const std::string& bad : {std::string("no_such_road:lock=1"),
                                 first + ":lock=1;" + first + ":turn=left",
                                 first + ":lock=0",
                                 first + ":turn=diagonal",
                                 first + ":turn=left:turn=right",
                                 first + ":so=1:so=2",
                                 first + ":so=abc",
                                 first + ":pts=1,2|3",
                                 first + ":pts=1,two",
                                 first + ":pts=",
                                 long_points,
                                 first}) {
    auto reparsed =
        roadmaker::parse_xodr(with_user_data_value(xml, "rm:maneuver", bad), "maneuvers");
    ASSERT_TRUE(reparsed.has_value()) << bad;
    EXPECT_EQ(count_warnings_mentioning(reparsed->diagnostics, "malformed rm:maneuver userData"),
              1U)
        << bad;
    EXPECT_TRUE(reparsed->network.junction(reparsed->network.find_junction("1"))->maneuvers.empty())
        << bad;
  }

  // The known-good value is the control.
  auto control = roadmaker::parse_xodr(with_user_data_value(xml, "rm:maneuver", good), "maneuvers");
  ASSERT_TRUE(control.has_value());
  EXPECT_EQ(count_warnings_mentioning(control->diagnostics, "malformed rm:maneuver userData"), 0U);
}

TEST(ManeuverPersistence, ExactlyKMaxControlPointsStillLoads) {
  CrossFixture fixture;
  const std::vector<RoadId> turns = fixture.turns();
  ASSERT_FALSE(turns.empty());
  Maneuver record{.road = turns[0]};
  for (std::size_t i = 0; i < kMaxManeuverControlPoints; ++i) {
    record.control_points.push_back(Waypoint{static_cast<double>(i), 0.0});
  }
  fixture.network.junction(fixture.junction)->maneuvers = {record};

  const std::string xml = write(fixture.network);
  auto reparsed = roadmaker::parse_xodr(xml, "maneuvers");
  ASSERT_TRUE(reparsed.has_value());
  const Junction& round = *reparsed->network.junction(reparsed->network.find_junction("1"));
  ASSERT_EQ(round.maneuvers.size(), 1U);
  EXPECT_EQ(round.maneuvers[0].control_points.size(), kMaxManeuverControlPoints);
  EXPECT_EQ(write(reparsed->network), xml);
}

TEST(ManeuverPersistence, OverLongPointListIsTruncatedOnWrite) {
  // The command layer holds authors to the bound, so a longer list can only
  // come from a hand-built network. The writer must never emit a value its own
  // reader would drop whole.
  CrossFixture fixture;
  const std::vector<RoadId> turns = fixture.turns();
  ASSERT_FALSE(turns.empty());
  Maneuver record{.road = turns[0]};
  for (std::size_t i = 0; i < kMaxManeuverControlPoints + 7; ++i) {
    record.control_points.push_back(Waypoint{static_cast<double>(i), 0.0});
  }
  fixture.network.junction(fixture.junction)->maneuvers = {record};

  const std::string xml = write(fixture.network);
  auto reparsed = roadmaker::parse_xodr(xml, "maneuvers");
  ASSERT_TRUE(reparsed.has_value());
  EXPECT_EQ(count_warnings_mentioning(reparsed->diagnostics, "rm:maneuver"), 0U);
  const Junction& round = *reparsed->network.junction(reparsed->network.find_junction("1"));
  ASSERT_EQ(round.maneuvers.size(), 1U);
  EXPECT_EQ(round.maneuvers[0].control_points.size(), kMaxManeuverControlPoints);
}

TEST(ManeuverPersistence, UnknownFieldKeyWarnsButKeepsTheEntry) {
  CrossFixture fixture;
  const std::vector<RoadId> turns = fixture.turns();
  ASSERT_FALSE(turns.empty());
  fixture.network.junction(fixture.junction)->maneuvers = {
      Maneuver{.road = turns[0], .turn_type = TurnType::Left}};
  const std::string xml = write(fixture.network);
  const std::string first = fixture.network.road(turns[0])->odr_id;

  // Forward compatibility (the rm:junction rule): a field a newer RoadMaker
  // added is reported and skipped, and the fields this build understands still
  // load.
  auto reparsed = roadmaker::parse_xodr(
      with_user_data_value(xml, "rm:maneuver", first + ":turn=left:speed=13.9"), "maneuvers");
  ASSERT_TRUE(reparsed.has_value());
  EXPECT_EQ(count_warnings_mentioning(reparsed->diagnostics, "'speed=13.9' is not understood"), 1U);
  EXPECT_EQ(count_warnings_mentioning(reparsed->diagnostics, "malformed rm:maneuver userData"), 0U);
  const Junction& round = *reparsed->network.junction(reparsed->network.find_junction("1"));
  ASSERT_EQ(round.maneuvers.size(), 1U);
  EXPECT_EQ(round.maneuvers[0].turn_type, std::optional<TurnType>(TurnType::Left));
}

TEST(ManeuverPersistence, LoadDoesNotRefitTheConnectingRoadGeometry) {
  // Layer 0 wins: the planView already in the file is the truth, and control
  // points are authoring provenance only. Loading a file whose control points
  // sit nowhere near the road must not move a single geometry record.
  CrossFixture fixture;
  const std::vector<RoadId> turns = fixture.turns();
  ASSERT_FALSE(turns.empty());
  fixture.network.junction(fixture.junction)->maneuvers = {
      Maneuver{.road = turns[0], .control_points = {Waypoint{500.0, -500.0}}}};

  const std::string xml = write(fixture.network);
  auto reparsed = roadmaker::parse_xodr(xml, "maneuvers");
  ASSERT_TRUE(reparsed.has_value());
  const roadmaker::Road& before = *fixture.network.road(turns[0]);
  const roadmaker::Road& after =
      *reparsed->network.road(reparsed->network.find_road(before.odr_id));
  EXPECT_DOUBLE_EQ(after.length, before.length);
  ASSERT_EQ(after.plan_view.records().size(), before.plan_view.records().size());
  EXPECT_EQ(write(reparsed->network), xml);
}

// --- fuzz corpus -----------------------------------------------------------

// DISABLED by default so normal runs never write into the source tree.
// Regenerate the committed p4-s6 seed with
// --gtest_also_run_disabled_tests --gtest_filter='*Maneuver*WriteCorpusSeed'.
// The malformed companions (bad_maneuver_turn.xodr, bad_maneuver_points.xodr,
// bad_maneuver_dup_entry.xodr) are hand-derived from it and are NOT
// regenerated.
TEST(ManeuverPersistence, DISABLED_WriteCorpusSeed) {
  namespace fs = std::filesystem;
  const fs::path corpus = fs::path(RM_FUZZ_CORPUS_DIR);

  CrossFixture fixture;
  const std::vector<RoadId> turns = fixture.turns();
  ASSERT_GE(turns.size(), 2U);
  fixture.network.junction(fixture.junction)->maneuvers = {
      Maneuver{.road = turns[0],
               .locked = true,
               .turn_type = TurnType::UTurn,
               .start_offset = 0.5,
               .end_offset = -1.5,
               .control_points = {Waypoint{1.0, 2.0}, Waypoint{-3.25, 4.5}}},
      Maneuver{.road = turns[1], .turn_type = TurnType::Right}};
  ASSERT_TRUE(roadmaker::save_xodr(
                  fixture.network, corpus / "junction_maneuvers.xodr", "junction_maneuvers")
                  .has_value());
}

TEST(ManeuverPersistence, TheFuzzCorpusSeedsParseAndReExport) {
  namespace fs = std::filesystem;
  const fs::path corpus = fs::path(RM_FUZZ_CORPUS_DIR);

  auto seed = roadmaker::load_xodr(corpus / "junction_maneuvers.xodr");
  ASSERT_TRUE(seed.has_value()) << "junction_maneuvers.xodr must parse";
  EXPECT_TRUE(seed->diagnostics.empty());
  const Junction& junction = *seed->network.junction(seed->network.find_junction("1"));
  ASSERT_EQ(junction.maneuvers.size(), 2U);
  EXPECT_TRUE(junction.maneuvers[0].locked);
  EXPECT_EQ(junction.maneuvers[0].turn_type, std::optional<TurnType>(TurnType::UTurn));
  EXPECT_EQ(junction.maneuvers[0].start_offset, std::optional<double>(0.5));
  EXPECT_EQ(junction.maneuvers[0].end_offset, std::optional<double>(-1.5));
  EXPECT_EQ(junction.maneuvers[0].control_points.size(), 2U);
  EXPECT_EQ(junction.maneuvers[1].turn_type, std::optional<TurnType>(TurnType::Right));
  EXPECT_FALSE(write(seed->network).empty());

  // The hand-derived malformed companions degrade with a warning; none fails
  // the load and none crashes.
  for (const char* name :
       {"bad_maneuver_turn.xodr", "bad_maneuver_points.xodr", "bad_maneuver_dup_entry.xodr"}) {
    auto bad = roadmaker::load_xodr(corpus / name);
    ASSERT_TRUE(bad.has_value()) << name;
    EXPECT_FALSE(bad->diagnostics.empty()) << name;
    EXPECT_FALSE(write(bad->network).empty()) << name;
  }
}
