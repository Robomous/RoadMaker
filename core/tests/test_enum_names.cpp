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

// The independent oracle for core/src/xodr/enum_names.hpp (#563).
//
// ★ THE OBVIOUS TEST IS WORTHLESS HERE, WHICH IS WHY THIS SPELLS IT ALL OUT.
//
// `value_of(name_of(v)) == v` over every value is one loop and total coverage,
// and it is a TAUTOLOGY: one table serves both directions, so any *consistent*
// relabelling satisfies it. Measured, not assumed — a differential probe
// re-emitting all 143 tracked .xodr/.xosc fixtures reported ZERO byte
// differences with `{Solid,"broken"}, {Broken,"solid"}` compiled in, for two
// compounding reasons:
//
//   1. A round trip is invariant under a consistent permutation. Read "+" as
//      Minus, write Minus as "+": identical bytes, every object facing backwards.
//   2. The preserved tier shields the writer. `Lane/RoadMark/Object::type_str`
//      hold the verbatim spelling for every PARSED element (the #476 fix), so
//      re-emitting a parsed file never consults these tables at all.
//
// The tables are reached by networks the editor, the bindings and the importers
// BUILD — which no fixture round trip exercises. So the oracle must come from
// outside the table: the literals below are transcribed from the ASAM OpenDRIVE
// 1.9.0 tables cited per case, not derived from the code.

#include "roadmaker/road/lane.hpp"
#include "roadmaker/road/object.hpp"
#include "roadmaker/road/traffic_rule.hpp"

#include <gtest/gtest.h>

#include <string>
#include <string_view>

#include "xodr/enum_names.hpp"

namespace {

using namespace roadmaker;
using roadmaker::xodr_names::name_of;
using roadmaker::xodr_names::value_of;

// --- what the WRITER emits (§11.7 Table 43) ---------------------------------

TEST(XodrEnumNames, LaneTypeSpellings) {
  const auto name = [](LaneType t) {
    return std::string(name_of(xodr_names::kLaneType, t, "none"));
  };
  EXPECT_EQ(name(LaneType::Driving), "driving");
  EXPECT_EQ(name(LaneType::Stop), "stop");
  EXPECT_EQ(name(LaneType::Shoulder), "shoulder");
  EXPECT_EQ(name(LaneType::Biking), "biking");
  EXPECT_EQ(name(LaneType::Sidewalk), "sidewalk");
  EXPECT_EQ(name(LaneType::Border), "border");
  EXPECT_EQ(name(LaneType::Restricted), "restricted");
  EXPECT_EQ(name(LaneType::Parking), "parking");
  EXPECT_EQ(name(LaneType::Median), "median");
  EXPECT_EQ(name(LaneType::Curb), "curb");
  EXPECT_EQ(name(LaneType::None), "none");
  // Other has no row: it is the unmodeled bucket, and the fallback stands in.
  EXPECT_EQ(name(LaneType::Other), "none");
  // ...and NEVER the deprecated alias, which is read-only. Emitting "walking"
  // for a sidewalk is the exact corruption #476 was filed for.
  EXPECT_NE(name(LaneType::Sidewalk), "walking");
}

TEST(XodrEnumNames, LaneTypeParsing) {
  const auto value = [](std::string_view s) { return value_of(xodr_names::kLaneType, s); };
  EXPECT_EQ(value("driving"), LaneType::Driving);
  EXPECT_EQ(value("stop"), LaneType::Stop);
  EXPECT_EQ(value("shoulder"), LaneType::Shoulder);
  EXPECT_EQ(value("biking"), LaneType::Biking);
  EXPECT_EQ(value("sidewalk"), LaneType::Sidewalk);
  EXPECT_EQ(value("border"), LaneType::Border);
  EXPECT_EQ(value("restricted"), LaneType::Restricted);
  EXPECT_EQ(value("parking"), LaneType::Parking);
  EXPECT_EQ(value("median"), LaneType::Median);
  EXPECT_EQ(value("curb"), LaneType::Curb);
  EXPECT_EQ(value("none"), LaneType::None);
  // The pre-1.6 alias still parses (§11.7) — accepted, never emitted.
  EXPECT_EQ(value("walking"), LaneType::Sidewalk);
  // No guessing: unknown and empty are the CALLER's policy, not the table's.
  EXPECT_FALSE(value("bus").has_value());
  EXPECT_FALSE(value("").has_value());
  EXPECT_FALSE(value("Driving").has_value()) << "e_laneType is case-sensitive";
}

// --- e_roadMarkType (§11.9, Table 47) ---------------------------------------

TEST(XodrEnumNames, RoadMarkTypeSpellings) {
  const auto name = [](RoadMarkType t) {
    return std::string(name_of(xodr_names::kRoadMarkType, t, "solid"));
  };
  EXPECT_EQ(name(RoadMarkType::None), "none");
  EXPECT_EQ(name(RoadMarkType::Solid), "solid");
  EXPECT_EQ(name(RoadMarkType::Broken), "broken");
  // The multi-token spellings carry exactly one ASCII space — it is part of the
  // enumerator, and a tab or a double space is a different (invalid) token.
  EXPECT_EQ(name(RoadMarkType::SolidSolid), "solid solid");
  EXPECT_EQ(name(RoadMarkType::SolidBroken), "solid broken");
  EXPECT_EQ(name(RoadMarkType::BrokenSolid), "broken solid");
  EXPECT_EQ(name(RoadMarkType::BrokenBroken), "broken broken");
  EXPECT_EQ(name(RoadMarkType::Other), "solid") << "the lossy stand-in, via the fallback";
}

TEST(XodrEnumNames, RoadMarkTypeParsing) {
  const auto value = [](std::string_view s) { return value_of(xodr_names::kRoadMarkType, s); };
  EXPECT_EQ(value("none"), RoadMarkType::None);
  EXPECT_EQ(value("solid"), RoadMarkType::Solid);
  EXPECT_EQ(value("broken"), RoadMarkType::Broken);
  EXPECT_EQ(value("solid solid"), RoadMarkType::SolidSolid);
  EXPECT_EQ(value("solid broken"), RoadMarkType::SolidBroken);
  EXPECT_EQ(value("broken solid"), RoadMarkType::BrokenSolid);
  EXPECT_EQ(value("broken broken"), RoadMarkType::BrokenBroken);
  // The five modelled-as-Other spellings from Table 47 must NOT resolve to a
  // value — each has to reach the caller as "unknown" so it lands in Other with
  // its text preserved. A row for any of these would repaint a kerb as a line.
  for (const std::string_view exotic : {"curb", "grass", "botts dots", "edge", "custom"}) {
    EXPECT_FALSE(value(exotic).has_value()) << exotic << " must stay unmodelled";
  }
}

// --- e_roadMarkColor (§11.9, Table 48) --------------------------------------

TEST(XodrEnumNames, RoadMarkColorSpellings) {
  const auto name = [](RoadMarkColor c) {
    return std::string(name_of(xodr_names::kRoadMarkColor, c, "standard"));
  };
  EXPECT_EQ(name(RoadMarkColor::Standard), "standard");
  EXPECT_EQ(name(RoadMarkColor::White), "white");
  EXPECT_EQ(name(RoadMarkColor::Yellow), "yellow");
  EXPECT_EQ(name(RoadMarkColor::Red), "red");
  EXPECT_EQ(name(RoadMarkColor::Blue), "blue");
  EXPECT_EQ(name(RoadMarkColor::Green), "green");
  EXPECT_EQ(name(RoadMarkColor::Orange), "orange");
  EXPECT_EQ(name(RoadMarkColor::Other), "standard");
}

TEST(XodrEnumNames, RoadMarkColorParsing) {
  const auto value = [](std::string_view s) { return value_of(xodr_names::kRoadMarkColor, s); };
  EXPECT_EQ(value("standard"), RoadMarkColor::Standard);
  EXPECT_EQ(value("white"), RoadMarkColor::White);
  EXPECT_EQ(value("yellow"), RoadMarkColor::Yellow);
  EXPECT_EQ(value("red"), RoadMarkColor::Red);
  EXPECT_EQ(value("blue"), RoadMarkColor::Blue);
  EXPECT_EQ(value("green"), RoadMarkColor::Green);
  EXPECT_EQ(value("orange"), RoadMarkColor::Orange);
  EXPECT_FALSE(value("violet").has_value());
  EXPECT_FALSE(value("").has_value());
}

// --- e_lane_direction (§11.7, 1.8.0+) ---------------------------------------

TEST(XodrEnumNames, LaneDirectionSpellings) {
  const auto name = [](LaneDirection d) {
    return std::string(name_of(xodr_names::kLaneDirection, d, "standard"));
  };
  EXPECT_EQ(name(LaneDirection::Standard), "standard");
  EXPECT_EQ(name(LaneDirection::Reversed), "reversed");
  EXPECT_EQ(name(LaneDirection::Both), "both");
}

TEST(XodrEnumNames, LaneDirectionParsing) {
  const auto value = [](std::string_view s) { return value_of(xodr_names::kLaneDirection, s); };
  EXPECT_EQ(value("standard"), LaneDirection::Standard);
  EXPECT_EQ(value("reversed"), LaneDirection::Reversed);
  EXPECT_EQ(value("both"), LaneDirection::Both);
  EXPECT_FALSE(value("forward").has_value());
}

// --- e_objectType (§13.2, Table 92) -----------------------------------------

TEST(XodrEnumNames, ObjectTypeSpellings) {
  const auto name = [](ObjectType t) {
    return std::string(name_of(xodr_names::kObjectType, t, "none"));
  };
  EXPECT_EQ(name(ObjectType::Crosswalk), "crosswalk");
  EXPECT_EQ(name(ObjectType::Tree), "tree");
  EXPECT_EQ(name(ObjectType::Vegetation), "vegetation");
  EXPECT_EQ(name(ObjectType::Pole), "pole");
  EXPECT_EQ(name(ObjectType::Barrier), "barrier");
  EXPECT_EQ(name(ObjectType::Building), "building");
  EXPECT_EQ(name(ObjectType::Obstacle), "obstacle");
  EXPECT_EQ(name(ObjectType::None), "none");
  EXPECT_EQ(name(ObjectType::Other), "none");
}

TEST(XodrEnumNames, ObjectTypeParsing) {
  const auto value = [](std::string_view s) { return value_of(xodr_names::kObjectType, s); };
  EXPECT_EQ(value("crosswalk"), ObjectType::Crosswalk);
  EXPECT_EQ(value("tree"), ObjectType::Tree);
  EXPECT_EQ(value("vegetation"), ObjectType::Vegetation);
  EXPECT_EQ(value("pole"), ObjectType::Pole);
  EXPECT_EQ(value("barrier"), ObjectType::Barrier);
  EXPECT_EQ(value("building"), ObjectType::Building);
  EXPECT_EQ(value("obstacle"), ObjectType::Obstacle);
  EXPECT_EQ(value("none"), ObjectType::None);
  EXPECT_FALSE(value("streetLamp").has_value()) << "modelled as Other, text preserved";
}

// --- @orientation on <object> and <signal> (§13) ----------------------------

TEST(XodrEnumNames, ObjectOrientationSpellings) {
  const auto name = [](ObjectOrientation o) {
    return std::string(name_of(xodr_names::kObjectOrientation, o, "none"));
  };
  // Sign-inverting these is undetectable by any round trip — read "+" as Minus,
  // write Minus as "+", identical bytes, every object facing backwards. This is
  // the assertion that catches it.
  EXPECT_EQ(name(ObjectOrientation::Plus), "+");
  EXPECT_EQ(name(ObjectOrientation::Minus), "-");
  EXPECT_EQ(name(ObjectOrientation::None), "none");
}

TEST(XodrEnumNames, ObjectOrientationParsing) {
  const auto value = [](std::string_view s) { return value_of(xodr_names::kObjectOrientation, s); };
  EXPECT_EQ(value("+"), ObjectOrientation::Plus);
  EXPECT_EQ(value("-"), ObjectOrientation::Minus);
  EXPECT_EQ(value("none"), ObjectOrientation::None);
  EXPECT_FALSE(value("").has_value()) << "absent is the reader's policy, not a row";
}

// --- e_trafficRule (§10.2, Table 23) ----------------------------------------

TEST(XodrEnumNames, TrafficRuleParsing) {
  const auto value = [](std::string_view s) { return value_of(xodr_names::kTrafficRule, s); };
  EXPECT_EQ(value("RHT"), TrafficRule::RightHandTraffic);
  EXPECT_EQ(value("LHT"), TrafficRule::LeftHandTraffic);
  // Upper-case in the standard; the lower-case spelling is not a synonym.
  EXPECT_FALSE(value("rht").has_value());
  EXPECT_FALSE(value("").has_value());
}

// --- coverage: a new enumerator must not slip through unnamed ---------------
//
// A `switch` with no `default` is warned on (and -Werror'd) when an enumerator
// is added, so these force the author of a new value to visit this file. The
// assertion is that every value the switch enumerates has a real row — Other,
// which deliberately has none, is the single exception per enum.

TEST(XodrEnumNames, EveryLaneTypeIsNamedExceptOther) {
  for (const LaneType type : {LaneType::Driving,
                              LaneType::Stop,
                              LaneType::Shoulder,
                              LaneType::Biking,
                              LaneType::Sidewalk,
                              LaneType::Border,
                              LaneType::Restricted,
                              LaneType::Parking,
                              LaneType::Median,
                              LaneType::Curb,
                              LaneType::None}) {
    const char* sentinel = "!!unnamed!!";
    EXPECT_STRNE(name_of(xodr_names::kLaneType, type, sentinel), sentinel)
        << "LaneType value " << static_cast<int>(type) << " has no row in kLaneType";
  }
}

TEST(XodrEnumNames, EveryRoadMarkTypeIsNamedExceptOther) {
  for (const RoadMarkType type : {RoadMarkType::None,
                                  RoadMarkType::Solid,
                                  RoadMarkType::Broken,
                                  RoadMarkType::SolidSolid,
                                  RoadMarkType::SolidBroken,
                                  RoadMarkType::BrokenSolid,
                                  RoadMarkType::BrokenBroken}) {
    const char* sentinel = "!!unnamed!!";
    EXPECT_STRNE(name_of(xodr_names::kRoadMarkType, type, sentinel), sentinel)
        << "RoadMarkType value " << static_cast<int>(type) << " has no row";
  }
}

TEST(XodrEnumNames, EveryObjectTypeIsNamedExceptOther) {
  for (const ObjectType type : {ObjectType::None,
                                ObjectType::Crosswalk,
                                ObjectType::Tree,
                                ObjectType::Vegetation,
                                ObjectType::Pole,
                                ObjectType::Barrier,
                                ObjectType::Building,
                                ObjectType::Obstacle}) {
    const char* sentinel = "!!unnamed!!";
    EXPECT_STRNE(name_of(xodr_names::kObjectType, type, sentinel), sentinel)
        << "ObjectType value " << static_cast<int>(type) << " has no row";
  }
}

} // namespace
