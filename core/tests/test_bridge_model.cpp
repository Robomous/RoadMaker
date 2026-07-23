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

// Phase 2 (WP2) — the <bridge> span promoted from a preserved fragment
// (Road::object_extras) to a first-class record (§13.12). The span serializes
// and round-trips; the generated solids never do. These tests pin the parse,
// the byte-identical round-trip (including a foreign <laneValidity> and unknown
// attributes), and the required-attribute diagnostics.

#include "roadmaker/road/bridge.hpp"
#include "roadmaker/road/network.hpp"
#include "roadmaker/road/road.hpp"
#include "roadmaker/xodr/reader.hpp"
#include "roadmaker/xodr/rules.hpp"
#include "roadmaker/xodr/writer.hpp"

#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace roadmaker {
namespace {

/// A straight 120 m road (elevated to z=6) wrapping `objects_xml`.
std::string document_with_objects(std::string_view objects_xml) {
  std::string doc = R"(<?xml version="1.0" encoding="UTF-8"?>
<OpenDRIVE>
  <header revMajor="1" revMinor="8" name="bridge-test"/>
  <road name="r" length="120" id="1" junction="-1">
    <planView>
      <geometry s="0" x="0" y="0" hdg="0" length="120"><line/></geometry>
    </planView>
    <elevation s="0" a="6" b="0" c="0" d="0"/>
    <lanes>
      <laneSection s="0">
        <center><lane id="0" type="none" level="false"/></center>
        <right>
          <lane id="-1" type="driving" level="false"><width sOffset="0" a="3.5" b="0" c="0" d="0"/></lane>
        </right>
      </laneSection>
    </lanes>
    <objects>
)";
  doc += objects_xml;
  doc += R"(
    </objects>
  </road>
</OpenDRIVE>
)";
  return doc;
}

XodrParseResult parse(std::string_view xml) {
  auto result = parse_xodr(xml, "test");
  EXPECT_TRUE(result.has_value());
  return std::move(*result);
}

/// The first road's bridges (every fixture here has exactly one road).
std::vector<Bridge> bridges_of(const RoadNetwork& network) {
  std::vector<Bridge> out;
  network.for_each_road([&](RoadId, const Road& road) {
    out.insert(out.end(), road.bridges.begin(), road.bridges.end());
  });
  return out;
}

bool has_warning(const std::vector<Diagnostic>& diags, std::string_view needle) {
  for (const Diagnostic& d : diags) {
    if (d.message.find(needle) != std::string::npos) {
      return true;
    }
  }
  return false;
}

TEST(BridgeModel, ParsesIntoTypedRecordNotObjectExtras) {
  const auto parsed = parse(document_with_objects(
      R"(<bridge s="50" length="24" id="b1" name="overpass" type="concrete">
        <userData code="rm:material.bridge_deck" value="rm:concrete"/>
      </bridge>)"));
  const std::vector<Bridge> bridges = bridges_of(parsed.network);
  ASSERT_EQ(bridges.size(), 1U);
  EXPECT_EQ(bridges[0].odr_id, "b1");
  EXPECT_EQ(bridges[0].name, "overpass");
  EXPECT_DOUBLE_EQ(bridges[0].s, 50.0);
  EXPECT_DOUBLE_EQ(bridges[0].length, 24.0);
  EXPECT_EQ(bridges[0].type, "concrete");
  EXPECT_EQ(bridges[0].deck_material, "rm:concrete");
  EXPECT_TRUE(bridges[0].extras.empty());
  // It is NOT left in the verbatim-fragment tier (object_extras).
  parsed.network.for_each_road(
      [&](RoadId, const Road& road) { EXPECT_TRUE(road.object_extras.empty()); });
}

TEST(BridgeModel, RoundTripsByteIdentically) {
  const auto parsed = parse(document_with_objects(
      R"(<bridge s="50" length="24" id="b1" name="overpass" type="concrete">
        <userData code="rm:material.bridge_deck" value="rm:concrete"/>
      </bridge>)"));
  const auto written = write_xodr(parsed.network, "bridge-test");
  ASSERT_TRUE(written.has_value());
  EXPECT_NE(written->find("<bridge"), std::string::npos);

  const auto reparsed = parse(*written);
  const auto rewritten = write_xodr(reparsed.network, "bridge-test");
  ASSERT_TRUE(rewritten.has_value());
  EXPECT_EQ(*written, *rewritten); // writer output is a fixed point
  EXPECT_EQ(bridges_of(reparsed.network), bridges_of(parsed.network));
}

TEST(BridgeModel, ForeignLaneValidityAndUnknownAttributesPreserved) {
  const auto parsed = parse(document_with_objects(
      R"(<bridge s="10" length="30" id="fb1" name="narrowed" type="steel" weight="heavy">
        <validity fromLane="-1" toLane="-1"/>
      </bridge>)"));
  const std::vector<Bridge> bridges = bridges_of(parsed.network);
  ASSERT_EQ(bridges.size(), 1U);
  // The unknown @weight and the whole <validity> child land in the raw tier.
  ASSERT_EQ(bridges[0].extras.attributes.size(), 1U);
  EXPECT_EQ(bridges[0].extras.attributes[0].first, "weight");
  EXPECT_EQ(bridges[0].extras.attributes[0].second, "heavy");
  ASSERT_EQ(bridges[0].extras.children.size(), 1U);
  EXPECT_NE(bridges[0].extras.children[0].find("validity"), std::string::npos);

  // And they survive a full round-trip byte-identically.
  const auto written = write_xodr(parsed.network, "bridge-test");
  ASSERT_TRUE(written.has_value());
  EXPECT_NE(written->find("weight=\"heavy\""), std::string::npos);
  EXPECT_NE(written->find("<validity"), std::string::npos);
  const auto reparsed = parse(*written);
  const auto rewritten = write_xodr(reparsed.network, "bridge-test");
  ASSERT_TRUE(rewritten.has_value());
  EXPECT_EQ(*written, *rewritten);
}

TEST(BridgeModel, OutOfEnumTypeSpellingSurvives) {
  const auto parsed =
      parse(document_with_objects(R"(<bridge s="5" length="40" id="cs" type="cableStayed"/>)"));
  const std::vector<Bridge> bridges = bridges_of(parsed.network);
  ASSERT_EQ(bridges.size(), 1U);
  EXPECT_EQ(bridges[0].type, "cableStayed"); // plain string, no enum clamp
}

TEST(BridgeModel, MissingRequiredLengthAndTypeWarnButLoad) {
  const auto parsed = parse(document_with_objects(R"(<bridge s="20" id="miss"/>)"));
  EXPECT_TRUE(has_warning(parsed.diagnostics, "length"));
  EXPECT_TRUE(has_warning(parsed.diagnostics, "type"));
  bool cites_rule = false;
  for (const Diagnostic& d : parsed.diagnostics) {
    if (d.rule_id == rules::kBridgeDefineType) {
      cites_rule = true;
    }
  }
  EXPECT_TRUE(cites_rule);
  // Parser never drops input: the bridge still loads.
  EXPECT_EQ(bridges_of(parsed.network).size(), 1U);
}

TEST(BridgeModel, NoBridgeWritesNoBridgeElement) {
  const auto parsed = parse(document_with_objects(
      R"(<object type="tree" id="t1" s="5" t="8" zOffset="0" orientation="none" height="6"/>)"));
  EXPECT_TRUE(bridges_of(parsed.network).empty());
  const auto written = write_xodr(parsed.network, "bridge-test");
  ASSERT_TRUE(written.has_value());
  EXPECT_EQ(written->find("<bridge"), std::string::npos);
}

} // namespace
} // namespace roadmaker
