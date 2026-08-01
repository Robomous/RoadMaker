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

// Road <type> and its <speed> child (ASAM OpenDRIVE 1.9.0 §10.4, §10.4.1) —
// #454, landed with p7-s4 because OSM's `maxspeed` and `highway=*` are the two
// richest tags in an extract and neither had anywhere to go.
//
// THE TRAP THIS FILE EXISTS FOR. @max is `t_maxSpeed`, which is a UNION of a
// number and exactly two string literals, "no limit" and "undefined". Parsing
// it into a double and writing the double back turns a German autobahn's
// max="no limit" into max="0" on the first save — a silent corruption of the
// same class as #476's enum-spelling defect, and one that no crash, no
// validator and no schema check would ever surface.
//
// AND THE SECOND TRAP, which is why the fixtures matter more than the
// assertions. Before this sprint NO committed .xodr carried a <type> element
// at all, and the reader WHITELISTED `type` in its unknown-child sweep while
// parsing nothing — so the drop was silent by construction and every test
// below would have passed forever on a build that read none of it. That is the
// #390 / #324 vacuity trap for the third time; see CommittedFixtures at the
// bottom, which is the part that makes the rest load-bearing.

#include "roadmaker/edit/operations.hpp"
#include "roadmaker/road/authoring.hpp"
#include "roadmaker/road/defaults.hpp"
#include "roadmaker/road/road_style.hpp"
#include "roadmaker/road/road_type.hpp"
#include "roadmaker/xodr/reader.hpp"
#include "roadmaker/xodr/rules.hpp"
#include "roadmaker/xodr/writer.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using roadmaker::Road;
using roadmaker::RoadTypeRecord;
using roadmaker::Severity;

namespace {

std::filesystem::path corpus(std::string_view name) {
  return std::filesystem::path(RM_FUZZ_CORPUS_DIR) / name;
}

bool has_rule(const std::vector<roadmaker::Diagnostic>& diagnostics, std::string_view rule) {
  return std::ranges::any_of(diagnostics, [rule](const auto& d) { return d.rule_id == rule; });
}

bool mentions(const std::vector<roadmaker::Diagnostic>& diagnostics, std::string_view text) {
  return std::ranges::any_of(
      diagnostics, [text](const auto& d) { return d.message.find(text) != std::string::npos; });
}

/// The road named `name`, or nullptr. The corpus seed uses names rather than
/// ids so a test reads as the case it is exercising.
const Road* road_named(const roadmaker::RoadNetwork& network, std::string_view name) {
  const Road* found = nullptr;
  network.for_each_road([&](roadmaker::RoadId, const Road& road) {
    if (road.name == name) {
      found = &road;
    }
  });
  return found;
}

/// Reads the seed, writes it back, and returns the emitted document. The
/// round trip is the whole point: a reader that parses correctly and a writer
/// that emits nothing pass every field assertion individually.
std::string reemit(const roadmaker::RoadNetwork& network, std::string_view name) {
  const auto written = roadmaker::write_xodr(network, std::string(name));
  EXPECT_TRUE(written.has_value());
  return written.value_or(std::string{});
}

} // namespace

// --- the union type ---------------------------------------------------------

TEST(RoadType, NoLimitSurvivesARoundTripAsAStringNotAZero) {
  const auto parsed = roadmaker::load_xodr(corpus("road_type_speed.xodr"));
  ASSERT_TRUE(parsed.has_value());

  const Road* autobahn = road_named(parsed->network, "Autobahn");
  ASSERT_NE(autobahn, nullptr);
  ASSERT_EQ(autobahn->types.size(), 1U);
  ASSERT_TRUE(autobahn->types[0].speed.has_value());

  // The verbatim spelling is the field that round-trips...
  EXPECT_EQ(autobahn->types[0].speed->max_str, "no limit");
  // ...and the derived number is correctly ABSENT rather than 0.0. A model
  // that stored `double max = 0.0` would pass every other assertion here.
  EXPECT_FALSE(autobahn->types[0].speed->max.has_value());

  const std::string written = reemit(parsed->network, "road_type_speed");
  EXPECT_NE(written.find(R"(max="no limit")"), std::string::npos) << written;
  EXPECT_EQ(written.find(R"(max="0")"), std::string::npos) << written;
}

TEST(RoadType, UndefinedSurvivesARoundTripToo) {
  const auto parsed = roadmaker::load_xodr(corpus("road_type_speed.xodr"));
  ASSERT_TRUE(parsed.has_value());

  const Road* mixed = road_named(parsed->network, "Mixed");
  ASSERT_NE(mixed, nullptr);
  ASSERT_EQ(mixed->types.size(), 2U);

  // Records keep DOCUMENT order, not sorted order — see the asc-order test.
  ASSERT_TRUE(mixed->types[0].speed.has_value());
  EXPECT_EQ(mixed->types[0].speed->max_str, "undefined");
  EXPECT_FALSE(mixed->types[0].speed->max.has_value());

  EXPECT_NE(reemit(parsed->network, "x").find(R"(max="undefined")"), std::string::npos);
}

TEST(RoadType, ANumericMaxParsesAndKeepsItsUnit) {
  const auto parsed = roadmaker::load_xodr(corpus("road_type_speed.xodr"));
  ASSERT_TRUE(parsed.has_value());

  const Road* mixed = road_named(parsed->network, "Mixed");
  ASSERT_NE(mixed, nullptr);
  ASSERT_EQ(mixed->types.size(), 2U);
  ASSERT_TRUE(mixed->types[1].speed.has_value());

  EXPECT_EQ(mixed->types[1].speed->max_str, "35");
  ASSERT_TRUE(mixed->types[1].speed->max.has_value());
  EXPECT_DOUBLE_EQ(*mixed->types[1].speed->max, 35.0);
  // mph deliberately bypasses the units layer — the settled policy since the
  // sign work. The unit travels with the value; nothing converts it.
  EXPECT_EQ(mixed->types[1].speed->unit, "mph");
}

TEST(RoadType, AMaxThatIsNeitherNumberNorLiteralIsPreservedAndReported) {
  const auto parsed = roadmaker::load_xodr(corpus("road_type_speed.xodr"));
  ASSERT_TRUE(parsed.has_value());

  const Road* foreign = road_named(parsed->network, "Foreign");
  ASSERT_NE(foreign, nullptr);
  ASSERT_EQ(foreign->types.size(), 1U);
  ASSERT_TRUE(foreign->types[0].speed.has_value());

  EXPECT_EQ(foreign->types[0].speed->max_str, "schnell");
  EXPECT_FALSE(foreign->types[0].speed->max.has_value());
  EXPECT_TRUE(mentions(parsed->diagnostics, "neither a number nor a t_maxSpeed literal"));
  EXPECT_NE(reemit(parsed->network, "x").find(R"(max="schnell")"), std::string::npos);
}

// --- the preserved tier -----------------------------------------------------

TEST(RoadType, UnmodeledAttributesAndChildrenSurviveVerbatim) {
  const auto parsed = roadmaker::load_xodr(corpus("road_type_speed.xodr"));
  ASSERT_TRUE(parsed.has_value());

  const std::string written = reemit(parsed->network, "road_type_speed");
  EXPECT_NE(written.find(R"(acme:grade="steep")"), std::string::npos) << written;
  EXPECT_NE(written.find(R"(acme:source="survey")"), std::string::npos) << written;
  EXPECT_NE(written.find("acme:note"), std::string::npos) << written;
  EXPECT_NE(written.find("unmodeled child"), std::string::npos) << written;
}

TEST(RoadType, AnUnknownTypeSpellingIsKeptRatherThanFlattenedToUnknown) {
  const auto parsed = roadmaker::load_xodr(corpus("road_type_speed.xodr"));
  ASSERT_TRUE(parsed.has_value());

  const Road* foreign = road_named(parsed->network, "Foreign");
  ASSERT_NE(foreign, nullptr);
  ASSERT_EQ(foreign->types.size(), 1U);
  EXPECT_EQ(foreign->types[0].type, "autobahnPlus");
  EXPECT_TRUE(mentions(parsed->diagnostics, "is not an e_roadType literal"));

  // "unknown" is itself a legal e_roadType, so writing it back would be
  // indistinguishable from a file that really said "unknown".
  const std::string written = reemit(parsed->network, "x");
  EXPECT_NE(written.find(R"(type="autobahnPlus")"), std::string::npos) << written;
}

TEST(RoadType, AnAlpha3CountryIsReportedAgainstTheNamedRule) {
  const auto parsed = roadmaker::load_xodr(corpus("road_type_speed.xodr"));
  ASSERT_TRUE(parsed.has_value());

  EXPECT_TRUE(has_rule(parsed->diagnostics, roadmaker::rules::kRoadTypeAlpha2Country));
  const Road* foreign = road_named(parsed->network, "Foreign");
  ASSERT_NE(foreign, nullptr);
  // Reported, not repaired: "DEU" is what the file said.
  EXPECT_EQ(foreign->types[0].country, "DEU");
}

TEST(RoadType, OutOfOrderRecordsAreReportedAndNotSilentlyReordered) {
  const auto parsed = roadmaker::load_xodr(corpus("road_type_speed.xodr"));
  ASSERT_TRUE(parsed.has_value());

  EXPECT_TRUE(has_rule(parsed->diagnostics, roadmaker::rules::kRoadTypeAscOrder));

  const Road* mixed = road_named(parsed->network, "Mixed");
  ASSERT_NE(mixed, nullptr);
  ASSERT_EQ(mixed->types.size(), 2U);
  // Document order preserved. Re-sorting would change bytes we were asked to
  // keep without changing what the file means.
  EXPECT_DOUBLE_EQ(mixed->types[0].s, 100.0);
  EXPECT_DOUBLE_EQ(mixed->types[1].s, 0.0);
}

TEST(RoadType, ARoadWithNoTypeWritesNoTypeElement) {
  const auto parsed = roadmaker::load_xodr(corpus("road_type_speed.xodr"));
  ASSERT_TRUE(parsed.has_value());

  const Road* untyped = road_named(parsed->network, "Untyped");
  ASSERT_NE(untyped, nullptr);
  EXPECT_TRUE(untyped->types.empty());

  // Multiplicity is 0..*, so absent must round-trip as absent — not as an
  // empty <type/> materialised by a writer that always emits the container.
  // Counted across the whole document rather than asserted on one road,
  // because "no <type> anywhere" would also pass a per-road absence check on
  // a writer that emitted none at all. Autobahn 1 + Mixed 2 + Foreign 1 = 4,
  // and Untyped contributes none.
  const std::string written = reemit(parsed->network, "road_type_speed");
  std::size_t count = 0;
  for (std::size_t at = written.find("<type"); at != std::string::npos;
       at = written.find("<type", at + 1)) {
    ++count;
  }
  EXPECT_EQ(count, 4U) << written;
}

// --- the enumerations -------------------------------------------------------

TEST(RoadType, TheRoadTypeEnumerationIsExactlyTheStandardsThirteen) {
  // §16 A.6.2 Table 194 (1.9.0), byte-identical to A.6.3 Table 188 (1.8.1).
  for (const std::string_view literal : {"bicycle",
                                         "lowSpeed",
                                         "motorway",
                                         "pedestrian",
                                         "rural",
                                         "townArterial",
                                         "townCollector",
                                         "townExpressway",
                                         "townLocal",
                                         "townPlayStreet",
                                         "townPrivate",
                                         "town",
                                         "unknown"}) {
    EXPECT_TRUE(roadmaker::is_known_road_type(literal)) << literal;
  }
  EXPECT_FALSE(roadmaker::is_known_road_type("motorWay"));
  EXPECT_FALSE(roadmaker::is_known_road_type("townarterial"));
  EXPECT_FALSE(roadmaker::is_known_road_type(""));
}

TEST(RoadType, TheSpeedUnitEnumerationIsExactlyTheStandardsThree) {
  // §16 A.1.5 Table 158.
  EXPECT_TRUE(roadmaker::is_known_speed_unit("km/h"));
  EXPECT_TRUE(roadmaker::is_known_speed_unit("m/s"));
  EXPECT_TRUE(roadmaker::is_known_speed_unit("mph"));
  EXPECT_FALSE(roadmaker::is_known_speed_unit("kmh"));
  EXPECT_FALSE(roadmaker::is_known_speed_unit("MPH"));
}

TEST(RoadType, EveryRoadClassNamesAStandardRoadType) {
  // The §1.7 mapping may only ever produce literals the standard defines —
  // a typo here would write an out-of-enum @type into every authored road.
  using roadmaker::defaults::RoadClass;
  for (const RoadClass road_class :
       {RoadClass::Freeway, RoadClass::Arterial, RoadClass::Collector, RoadClass::Local}) {
    EXPECT_TRUE(roadmaker::is_known_road_type(roadmaker::defaults::road_type_name(road_class)))
        << roadmaker::defaults::road_type_name(road_class);
  }
  EXPECT_STREQ(roadmaker::defaults::road_type_name(RoadClass::Freeway), "motorway");
  EXPECT_STREQ(roadmaker::defaults::road_type_name(RoadClass::Arterial), "townArterial");
  EXPECT_STREQ(roadmaker::defaults::road_type_name(RoadClass::Collector), "townCollector");
  EXPECT_STREQ(roadmaker::defaults::road_type_name(RoadClass::Local), "townLocal");
}

// --- authoring the type from the road class (#454's second half) ------------
//
// The mapping above existed since p7-s4 but had exactly two callers: the OSM
// importer and the doc generator. A road authored INTERACTIVELY still exported
// with no <type> at all, which is the "a freeway exports indistinguishable from
// a local street" symptom the issue opens with.

namespace {

roadmaker::RoadId author_with(roadmaker::RoadNetwork& network,
                              const roadmaker::LaneProfile& profile,
                              const char* odr_id) {
  const std::vector<roadmaker::Waypoint> waypoints{roadmaker::Waypoint{.x = 0.0, .y = 0.0},
                                                   roadmaker::Waypoint{.x = 100.0, .y = 0.0}};
  auto road = roadmaker::author_clothoid_road(network, waypoints, profile, "", odr_id);
  EXPECT_TRUE(road.has_value());
  return road.value_or(roadmaker::RoadId{});
}

} // namespace

TEST(RoadTypeAuthoring, EachTemplateStampsItsClassesType) {
  using roadmaker::defaults::RoadClass;
  const std::vector<std::pair<roadmaker::LaneProfile, const char*>> cases{
      {roadmaker::LaneProfile::freeway(), "motorway"},
      {roadmaker::LaneProfile::arterial(), "townArterial"},
      {roadmaker::LaneProfile::collector(), "townCollector"},
      {roadmaker::LaneProfile::local_road(), "townLocal"},
  };
  int index = 0;
  for (const auto& [profile, expected] : cases) {
    roadmaker::RoadNetwork network;
    const roadmaker::RoadId road = author_with(network, profile, std::to_string(++index).c_str());
    ASSERT_TRUE(road.is_valid());
    ASSERT_EQ(network.road(road)->types.size(), 1U) << expected;
    EXPECT_EQ(network.road(road)->types[0].type, expected);
    EXPECT_DOUBLE_EQ(network.road(road)->types[0].s, 0.0);
    // §1.7: no per-class default speed. Inventing one would be a guess where
    // the honest answer is silence.
    EXPECT_FALSE(network.road(road)->types[0].speed.has_value());
    // And it reaches the FILE, not only the model.
    const auto written = roadmaker::write_xodr(network, "authored");
    ASSERT_TRUE(written.has_value());
    EXPECT_NE(written->find(std::string(R"(type=")") + expected + R"(")"), std::string::npos);
  }
}

TEST(RoadTypeAuthoring, AHandBuiltProfileClaimsNoClassAndStampsNothing) {
  // A LaneProfile is caller-assemblable, and a hand-built cross section is not
  // entitled to claim it is a motorway. This is also what keeps every existing
  // hand-built fixture byte-identical.
  roadmaker::LaneProfile bespoke;
  bespoke.right.push_back(roadmaker::LaneSpec{.type = roadmaker::LaneType::Driving, .width = 3.0});
  EXPECT_FALSE(bespoke.road_class.has_value());

  roadmaker::RoadNetwork network;
  const roadmaker::RoadId road = author_with(network, bespoke, "1");
  ASSERT_TRUE(road.is_valid());
  EXPECT_TRUE(network.road(road)->types.empty());

  const auto written = roadmaker::write_xodr(network, "bespoke");
  ASSERT_TRUE(written.has_value());
  EXPECT_EQ(written->find("<type"), std::string::npos);
}

TEST(RoadTypeAuthoring, RestylingARoadRewritesItsTypeAndUndoRestoresIt) {
  roadmaker::RoadNetwork network;
  const roadmaker::RoadId road = author_with(network, roadmaker::LaneProfile::local_road(), "1");
  ASSERT_TRUE(road.is_valid());
  ASSERT_EQ(network.road(road)->types.size(), 1U);
  ASSERT_EQ(network.road(road)->types[0].type, "townLocal");

  auto command = roadmaker::edit::apply_road_style(network, road, roadmaker::RoadStyle::freeway());
  ASSERT_NE(command, nullptr);
  ASSERT_TRUE(command->apply(network).has_value());
  ASSERT_EQ(network.road(road)->types.size(), 1U);
  EXPECT_EQ(network.road(road)->types[0].type, "motorway")
      << "restyling changed the cross section but not what the file says the road IS";

  ASSERT_TRUE(command->revert(network).has_value());
  ASSERT_EQ(network.road(road)->types.size(), 1U);
  EXPECT_EQ(network.road(road)->types[0].type, "townLocal") << "undo did not restore the type";
}

TEST(RoadTypeAuthoring, RestylingKeepsTheSpeedTheRoadAlreadyDeclared) {
  // ★ The data loss this fix would otherwise introduce. §1.7: a speed limit is
  // a fact about a particular road, not about its class — so the class may be
  // rewritten while @country, the <speed> and any preserved extras must not be.
  roadmaker::RoadNetwork network;
  const roadmaker::RoadId road = author_with(network, roadmaker::LaneProfile::local_road(), "1");
  ASSERT_TRUE(road.is_valid());
  network.road(road)->types[0].country = "DE";
  network.road(road)->types[0].speed =
      roadmaker::RoadSpeed{.max_str = "50", .max = 50.0, .unit = "km/h"};

  auto command = roadmaker::edit::apply_road_style(network, road, roadmaker::RoadStyle::arterial());
  ASSERT_NE(command, nullptr);
  ASSERT_TRUE(command->apply(network).has_value());

  ASSERT_EQ(network.road(road)->types.size(), 1U);
  const RoadTypeRecord& record = network.road(road)->types[0];
  EXPECT_EQ(record.type, "townArterial") << "the class did not follow the style";
  EXPECT_EQ(record.country, "DE") << "the country was invented away";
  ASSERT_TRUE(record.speed.has_value()) << "the posted limit was destroyed by a restyle";
  EXPECT_EQ(record.speed->max_str, "50");
  EXPECT_EQ(record.speed->unit, "km/h");
}

TEST(RoadTypeAuthoring, AStyleWithNoClassLeavesAForeignRoadsTypesAlone) {
  // A hand-built style must not silently erase the s-varying type records of an
  // imported road just because it was applied to it.
  const auto parsed = roadmaker::load_xodr(corpus("road_type_speed.xodr"));
  ASSERT_TRUE(parsed.has_value());
  roadmaker::RoadNetwork network = std::move(parsed->network);

  const Road* mixed = road_named(network, "Mixed");
  ASSERT_NE(mixed, nullptr);
  const std::vector<RoadTypeRecord> before = mixed->types;
  ASSERT_GT(before.size(), 1U)
      << "the fixture must carry an s-varying road for this to mean anything";
  const roadmaker::RoadId road = network.find_road(mixed->odr_id);

  roadmaker::RoadStyle bespoke;
  bespoke.right.push_back(roadmaker::StyleLane{});
  ASSERT_FALSE(bespoke.road_class.has_value());

  auto command = roadmaker::edit::apply_road_style(network, road, bespoke);
  ASSERT_NE(command, nullptr);
  ASSERT_TRUE(command->apply(network).has_value());
  EXPECT_EQ(network.road(road)->types, before) << "a classless style rewrote the road's types";
}

// --- the vacuity guards -----------------------------------------------------
//
// Everything above would pass on a build that read no <type> at all if no
// committed file carried one. These are what make the fixtures load-bearing.

TEST(CommittedFixtures, ASampleSceneCarriesARoadTypeAndASpeed) {
  const auto parsed =
      roadmaker::load_xodr(std::filesystem::path(RM_SAMPLES_DIR) / "straight_road.xodr");
  ASSERT_TRUE(parsed.has_value());

  const Road* main_street = road_named(parsed->network, "Main Street");
  ASSERT_NE(main_street, nullptr);
  ASSERT_EQ(main_street->types.size(), 1U);
  EXPECT_EQ(main_street->types[0].type, "townArterial");
  ASSERT_TRUE(main_street->types[0].speed.has_value());
  EXPECT_EQ(main_street->types[0].speed->max_str, "50");
  EXPECT_EQ(main_street->types[0].speed->unit, "km/h");

  // And it survives being written back — this sample is also what the
  // esmini round-trip job loads, so the element is under an external
  // consumer's parser too.
  EXPECT_NE(reemit(parsed->network, "straight_road").find(R"(type="townArterial")"),
            std::string::npos);
}

TEST(CommittedFixtures, TheCorpusExercisesEveryRoadTypePath) {
  const auto parsed = roadmaker::load_xodr(corpus("road_type_speed.xodr"));
  ASSERT_TRUE(parsed.has_value());

  int typed = 0;
  int with_speed = 0;
  int literal_max = 0;
  parsed->network.for_each_road([&](roadmaker::RoadId, const Road& road) {
    typed += road.types.empty() ? 0 : 1;
    for (const RoadTypeRecord& record : road.types) {
      if (record.speed) {
        ++with_speed;
        if (!record.speed->max) {
          ++literal_max;
        }
      }
    }
  });

  EXPECT_EQ(typed, 3) << "three typed roads plus one deliberately untyped";
  EXPECT_EQ(with_speed, 4);
  // "no limit", "undefined" and the unparseable "schnell" — the three cases a
  // double-typed @max would have destroyed.
  EXPECT_EQ(literal_max, 3);
}
