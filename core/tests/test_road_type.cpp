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

#include "roadmaker/road/defaults.hpp"
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
