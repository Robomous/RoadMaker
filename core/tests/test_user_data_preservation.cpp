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

// Foreign and unknown-rm: <userData> preservation (fmt-s2, #326; ADR-0008
// Layer-1 policy; §7.2 of OpenDRIVE 1.8.1 and 1.9.0 alike — the two chapters
// are textually identical — allows <userData> at any element). Three scopes
// historically LOST these elements: <road> and <junction> warn-and-dropped,
// and the <OpenDRIVE> root dropped them with no diagnostic at all. The gate
// here is preserve-and-warn at all three, plus the newer-RoadMaker warning for
// unregistered rm: codes at every preserved tier, plus write→parse→write
// byte-identity with the fragments on board.

#include "roadmaker/road/junction.hpp"
#include "roadmaker/road/lane.hpp"
#include "roadmaker/road/network.hpp"
#include "roadmaker/road/road.hpp"
#include "roadmaker/xodr/reader.hpp"
#include "roadmaker/xodr/writer.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

using roadmaker::Junction;
using roadmaker::JunctionId;
using roadmaker::Road;
using roadmaker::RoadId;
using roadmaker::RoadNetwork;

namespace {

/// One straight road, one connection-less (foreign-style) junction. The three
/// slots inject <userData> at road scope (after <lanes>), junction scope, and
/// the document root.
std::string document_with(std::string_view road_user_data,
                          std::string_view junction_user_data,
                          std::string_view root_user_data,
                          std::string_view lane_user_data = {}) {
  std::string doc = R"(<?xml version="1.0" encoding="UTF-8"?>
<OpenDRIVE>
  <header revMajor="1" revMinor="8" name="preserve-test"/>
  <road name="r" length="100" id="1" junction="-1">
    <planView>
      <geometry s="0" x="0" y="0" hdg="0" length="100"><line/></geometry>
    </planView>
    <lanes>
      <laneSection s="0">
        <center>
          <lane id="0" type="none" level="false"/>
        </center>
        <right>
          <lane id="-1" type="driving" level="false">
            <width sOffset="0" a="3.5" b="0" c="0" d="0"/>
)";
  doc += lane_user_data;
  doc += R"(          </lane>
        </right>
      </laneSection>
    </lanes>
)";
  doc += road_user_data;
  doc += R"(  </road>
  <junction id="9" name="j">
)";
  doc += junction_user_data;
  doc += R"(  </junction>
)";
  doc += root_user_data;
  doc += "</OpenDRIVE>\n";
  return doc;
}

roadmaker::XodrParseResult parse(std::string_view xml) {
  auto result = roadmaker::parse_xodr(xml, "test");
  EXPECT_TRUE(result.has_value());
  return std::move(*result);
}

std::vector<const roadmaker::Diagnostic*>
warnings_mentioning(const std::vector<roadmaker::Diagnostic>& diagnostics,
                    std::string_view needle) {
  std::vector<const roadmaker::Diagnostic*> found;
  for (const roadmaker::Diagnostic& d : diagnostics) {
    if (d.message.find(needle) != std::string::npos) {
      found.push_back(&d);
    }
  }
  return found;
}

const Road& only_road(const RoadNetwork& network) {
  return *network.road(network.find_road("1"));
}

const Junction& only_junction(const RoadNetwork& network) {
  const Junction* found = nullptr;
  network.for_each_junction([&](JunctionId, const Junction& junction) {
    if (junction.odr_id == "9") {
      found = &junction;
    }
  });
  EXPECT_NE(found, nullptr);
  return *found;
}

/// write→parse→write must reproduce the first write byte for byte.
void expect_write_is_a_fixed_point(const RoadNetwork& network) {
  const auto written = roadmaker::write_xodr(network, "preserve-test");
  ASSERT_TRUE(written.has_value());
  const auto reparsed = parse(*written);
  const auto rewritten = roadmaker::write_xodr(reparsed.network, "preserve-test");
  ASSERT_TRUE(rewritten.has_value());
  EXPECT_EQ(*written, *rewritten);
}

} // namespace

TEST(UserDataPreservation, RoadForeignUserDataIsPreservedVerbatimAndWarned) {
  const auto parsed = parse(document_with(
      "    <userData code=\"vendor:road\" value=\"1\"><nested a=\"b\"/></userData>\n", "", ""));

  const Road& road = only_road(parsed.network);
  ASSERT_EQ(road.preserved_user_data.size(), 1U);
  EXPECT_NE(road.preserved_user_data.front().find("vendor:road"), std::string::npos);
  EXPECT_NE(road.preserved_user_data.front().find("<nested"), std::string::npos);

  const auto warned = warnings_mentioning(parsed.diagnostics, "'vendor:road'");
  ASSERT_EQ(warned.size(), 1U);
  EXPECT_EQ(warned.front()->severity, roadmaker::Severity::Warning);
  EXPECT_NE(warned.front()->message.find("preserved verbatim"), std::string::npos);
  EXPECT_TRUE(warned.front()->rule_id.empty()); // tool limitation, not a rule
  EXPECT_TRUE(warnings_mentioning(parsed.diagnostics, "was ignored").empty());

  const auto written = roadmaker::write_xodr(parsed.network, "preserve-test");
  ASSERT_TRUE(written.has_value());
  EXPECT_NE(written->find("code=\"vendor:road\""), std::string::npos);
  EXPECT_NE(written->find("<nested a=\"b\""), std::string::npos);
  expect_write_is_a_fixed_point(parsed.network);
}

TEST(UserDataPreservation, JunctionForeignUserDataIsPreservedVerbatimAndWarned) {
  const auto parsed =
      parse(document_with("", "    <userData code=\"vendor:junction\" value=\"opaque\"/>\n", ""));

  const Junction& junction = only_junction(parsed.network);
  ASSERT_EQ(junction.preserved_user_data.size(), 1U);
  EXPECT_NE(junction.preserved_user_data.front().find("vendor:junction"), std::string::npos);

  const auto warned = warnings_mentioning(parsed.diagnostics, "'vendor:junction'");
  ASSERT_EQ(warned.size(), 1U);
  EXPECT_NE(warned.front()->message.find("preserved verbatim"), std::string::npos);
  EXPECT_TRUE(warned.front()->rule_id.empty());

  const auto written = roadmaker::write_xodr(parsed.network, "preserve-test");
  ASSERT_TRUE(written.has_value());
  EXPECT_NE(written->find("code=\"vendor:junction\""), std::string::npos);
  expect_write_is_a_fixed_point(parsed.network);
}

TEST(UserDataPreservation, RootForeignUserDataIsPreservedVerbatimAndWarned) {
  // This scope historically lost the element with NO diagnostic at all —
  // parse_surfaces/parse_terrain_reference skipped it and the root
  // unsupported-children warning exempted <userData>.
  const auto parsed = parse(document_with(
      "", "", "  <userData code=\"vendor:root\" value=\"1\"><blob x=\"y\"/></userData>\n"));

  ASSERT_EQ(parsed.network.preserved_user_data().size(), 1U);
  EXPECT_NE(parsed.network.preserved_user_data().front().find("vendor:root"), std::string::npos);

  const auto warned = warnings_mentioning(parsed.diagnostics, "'vendor:root'");
  ASSERT_EQ(warned.size(), 1U);
  EXPECT_EQ(warned.front()->location, "OpenDRIVE");
  EXPECT_NE(warned.front()->message.find("preserved verbatim"), std::string::npos);
  EXPECT_TRUE(warned.front()->rule_id.empty());

  const auto written = roadmaker::write_xodr(parsed.network, "preserve-test");
  ASSERT_TRUE(written.has_value());
  EXPECT_NE(written->find("code=\"vendor:root\""), std::string::npos);
  EXPECT_NE(written->find("<blob x=\"y\""), std::string::npos);
  expect_write_is_a_fixed_point(parsed.network);
}

TEST(UserDataPreservation, UnknownRmCodesArePreservedWithTheNewerRoadMakerWarning) {
  // rm:-prefixed but unregistered = written by a newer RoadMaker. One per
  // scope, including a lane (whose catch-all already preserved silently — the
  // warning is what fmt-s2 adds there).
  const auto parsed =
      parse(document_with("    <userData code=\"rm:future_road\" value=\"1\"/>\n",
                          "    <userData code=\"rm:future_junction\" value=\"1\"/>\n",
                          "  <userData code=\"rm:future_root\" value=\"1\"/>\n",
                          "            <userData code=\"rm:future_lane\" value=\"1\"/>\n"));

  EXPECT_EQ(only_road(parsed.network).preserved_user_data.size(), 1U);
  EXPECT_EQ(only_junction(parsed.network).preserved_user_data.size(), 1U);
  EXPECT_EQ(parsed.network.preserved_user_data().size(), 1U);

  const auto warned = warnings_mentioning(parsed.diagnostics, "not known to this RoadMaker");
  ASSERT_EQ(warned.size(), 4U);
  for (const roadmaker::Diagnostic* d : warned) {
    EXPECT_NE(d->message.find("preserved verbatim"), std::string::npos);
    EXPECT_TRUE(d->rule_id.empty());
  }

  const auto written = roadmaker::write_xodr(parsed.network, "preserve-test");
  ASSERT_TRUE(written.has_value());
  for (const std::string_view code :
       {"rm:future_road", "rm:future_junction", "rm:future_root", "rm:future_lane"}) {
    EXPECT_NE(written->find(std::string(code)), std::string::npos) << code;
  }
  expect_write_is_a_fixed_point(parsed.network);
}

TEST(UserDataPreservation, ARegisteredCodeAtTheWrongScopeGetsTheGenericWarning) {
  // rm:corners is registered — for junctions. On a road it is "not understood"
  // there, not "unknown to this version": preserve with the generic wording.
  const auto parsed =
      parse(document_with("    <userData code=\"rm:corners\" value=\"whatever\"/>\n", "", ""));

  EXPECT_EQ(only_road(parsed.network).preserved_user_data.size(), 1U);
  const auto warned = warnings_mentioning(parsed.diagnostics, "'rm:corners'");
  ASSERT_EQ(warned.size(), 1U);
  EXPECT_NE(warned.front()->message.find("is not understood and was preserved verbatim"),
            std::string::npos);
  EXPECT_TRUE(warnings_mentioning(parsed.diagnostics, "not known to this RoadMaker").empty());
  expect_write_is_a_fixed_point(parsed.network);
}

TEST(UserDataPreservation, ADocumentWithoutUserDataGainsNoFragmentsAndStaysAFixedPoint) {
  // Layer-0 purity: the new preserved tiers must cost a plain document
  // nothing — no fragments materialize, no new diagnostics, same bytes.
  const auto parsed = parse(document_with("", "", ""));
  EXPECT_TRUE(only_road(parsed.network).preserved_user_data.empty());
  EXPECT_TRUE(only_junction(parsed.network).preserved_user_data.empty());
  EXPECT_TRUE(parsed.network.preserved_user_data().empty());
  EXPECT_TRUE(warnings_mentioning(parsed.diagnostics, "preserved verbatim").empty());
  expect_write_is_a_fixed_point(parsed.network);
}
