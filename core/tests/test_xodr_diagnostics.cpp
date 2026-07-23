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

// Audit 2026-07 gaps B2/B3 + the xodr/rules UID guard, three concerns in one
// file:
//
//  1. count_errors (roadmaker/xodr/diagnostic.hpp) — the one function on the
//     diagnostic type itself.
//  2. A structural guard over every checker-rule UID in
//     roadmaker/xodr/rules.hpp: the Annex E grammar
//     `<emanating-entity>:<standard>:<definition-setting>:<rule_set.rule_name>`
//     (OpenDRIVE 1.8.1 Annex E "Rule UID schema"), uniqueness, and the vendor
//     namespace convention for RoadMaker-authored rules.
//  3. The first tranche of the OpenDRIVE reader's diagnostic paths
//     (core/src/xodr/reader.cpp): a parameterized malformed-input suite
//     pinning, for each emit site, the severity, the cited rule (or its
//     deliberate absence), and — the reader's core contract — that the parser
//     NEVER silently drops the document: parse_xodr still returns a
//     best-effort network, the surviving entities are still there, and the
//     skipped entity is verifiably absent (or verifiably degraded).

#include "roadmaker/road/network.hpp"
#include "roadmaker/xodr/diagnostic.hpp"
#include "roadmaker/xodr/reader.hpp"
#include "roadmaker/xodr/rules.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <regex>
#include <set>
#include <string>
#include <string_view>
#include <vector>

using roadmaker::Diagnostic;
using roadmaker::RoadNetwork;
using roadmaker::Severity;
using roadmaker::XodrParseResult;

// --- 1. count_errors ---------------------------------------------------------

TEST(XodrDiagnostics, CountErrorsOnEmptyVectorIsZero) {
  EXPECT_EQ(roadmaker::count_errors({}), 0U);
}

TEST(XodrDiagnostics, CountErrorsCountsOnlyErrorSeverity) {
  const std::vector<Diagnostic> diagnostics{
      Diagnostic{.severity = Severity::Info, .message = "i0"},
      Diagnostic{.severity = Severity::Warning, .message = "w0"},
      Diagnostic{.severity = Severity::Error, .message = "e0"},
      Diagnostic{.severity = Severity::Warning, .message = "w1"},
      Diagnostic{.severity = Severity::Error, .message = "e1"},
      Diagnostic{.severity = Severity::Info, .message = "i1"},
  };
  EXPECT_EQ(roadmaker::count_errors(diagnostics), 2U);
}

// --- 2. rule UID structural guard --------------------------------------------

namespace {

struct NamedRule {
  std::string_view name;
  std::string_view uid;
};

// Every constant in roadmaker/xodr/rules.hpp. The header exposes no iterable
// list, so this table is maintained by hand: WHEN A NEW RULE CONSTANT IS ADDED
// TO rules.hpp IT MUST BE ADDED HERE TOO (the uniqueness and grammar checks
// below then cover it for free).
constexpr NamedRule kAllRules[] = {
    {"kIdUniqueInClass", roadmaker::rules::kIdUniqueInClass},
    {"kIdUniqueInLaneSection", roadmaker::rules::kIdUniqueInLaneSection},
    {"kOnlyRefDefinedIds", roadmaker::rules::kOnlyRefDefinedIds},
    {"kReflineExists", roadmaker::rules::kReflineExists},
    {"kReflineNoGaps", roadmaker::rules::kReflineNoGaps},
    {"kRoadLengthSumGeometries", roadmaker::rules::kRoadLengthSumGeometries},
    {"kLaneSectionRequired", roadmaker::rules::kLaneSectionRequired},
    {"kLaneSectionValidLength", roadmaker::rules::kLaneSectionValidLength},
    {"kWidthDefinedWholeSection", roadmaker::rules::kWidthDefinedWholeSection},
    {"kMaterialCenterLaneNone", roadmaker::rules::kMaterialCenterLaneNone},
    {"kMaterialElemAscOrder", roadmaker::rules::kMaterialElemAscOrder},
    {"kRoadLinkAttributeUsage", roadmaker::rules::kRoadLinkAttributeUsage},
    {"kJunctionNotOnlyTwo", roadmaker::rules::kJunctionNotOnlyTwo},
    {"kJunctionVirtualAttributes", roadmaker::rules::kJunctionVirtualAttributes},
    {"kJunctionOneLineElement", roadmaker::rules::kJunctionOneLineElement},
    {"kJunctionRefLineDefinition", roadmaker::rules::kJunctionRefLineDefinition},
    {"kJunctionOneElevGrid", roadmaker::rules::kJunctionOneElevGrid},
    {"kJunctionElevGridPerpendicular", roadmaker::rules::kJunctionElevGridPerpendicular},
    {"kObjectTypeAttr", roadmaker::rules::kObjectTypeAttr},
    {"kObjectOrientation", roadmaker::rules::kObjectOrientation},
    {"kObjectStTCoords", roadmaker::rules::kObjectStTCoords},
    {"kObjectCircularVsAngular", roadmaker::rules::kObjectCircularVsAngular},
    {"kOutlineFollowedByCorner", roadmaker::rules::kOutlineFollowedByCorner},
    {"kOutlineExactlyOneOuter", roadmaker::rules::kOutlineExactlyOneOuter},
    {"kCornerRoadMinAmount", roadmaker::rules::kCornerRoadMinAmount},
    {"kCornerLocalMinAmount", roadmaker::rules::kCornerLocalMinAmount},
    {"kCornerRoadLocalExcl", roadmaker::rules::kCornerRoadLocalExcl},
    {"kCornerRoadIdWithMarkings", roadmaker::rules::kCornerRoadIdWithMarkings},
    {"kCornerLocalIdWithMarkings", roadmaker::rules::kCornerLocalIdWithMarkings},
    {"kObjectMarkingColour", roadmaker::rules::kObjectMarkingColour},
    {"kObjectMarkingNoOutlineSide", roadmaker::rules::kObjectMarkingNoOutlineSide},
    {"kSignalType", roadmaker::rules::kSignalType},
    {"kSignalUseCountryCode", roadmaker::rules::kSignalUseCountryCode},
    {"kControllerValidForSignals", roadmaker::rules::kControllerValidForSignals},
    {"kVirtualNoControllers", roadmaker::rules::kVirtualNoControllers},
    {"kJunctionBoundaryCloseGap", roadmaker::rules::kJunctionBoundaryCloseGap},
    {"kJunctionBoundaryOnlyCommon", roadmaker::rules::kJunctionBoundaryOnlyCommon},
    {"kJunctionBoundaryCcwOrder", roadmaker::rules::kJunctionBoundaryCcwOrder},
    {"kJunctionBoundaryClosed", roadmaker::rules::kJunctionBoundaryClosed},
    {"kJunctionBoundaryReachAllRoads", roadmaker::rules::kJunctionBoundaryReachAllRoads},
    {"kJunctionArmSingleOwner", roadmaker::rules::kJunctionArmSingleOwner},
};

} // namespace

TEST(RuleUid, EveryConstantMatchesTheAnnexEGrammar) {
  // OpenDRIVE 1.8.1 Annex E, "Rule UID schema":
  //   <emanating-entity>:<standard>:x.y.z:rule_set.for_rules.rule_name
  // with the full rule name a dot-separated snake_lower_case string of at
  // least rule_set + rule_name.
  const std::regex grammar(
      R"(^[a-z0-9.\-]+:[a-z]+:[0-9]+\.[0-9]+\.[0-9]+:[a-z0-9_]+(\.[a-z0-9_]+)+$)");
  for (const NamedRule& rule : kAllRules) {
    EXPECT_FALSE(rule.uid.empty()) << rule.name;
    EXPECT_TRUE(std::regex_match(rule.uid.begin(), rule.uid.end(), grammar))
        << rule.name << " = '" << rule.uid << "'";
  }
}

TEST(RuleUid, AllConstantsAreUnique) {
  std::set<std::string_view> seen;
  for (const NamedRule& rule : kAllRules) {
    EXPECT_TRUE(seen.insert(rule.uid).second) << "duplicate UID: " << rule.uid;
  }
  EXPECT_EQ(seen.size(), std::size(kAllRules));
}

TEST(RuleUid, VendorRulesUseTheProjectNamespace) {
  // ASAM rules emanate from asam.net; everything else must be a RoadMaker
  // rule under the maintainer-approved robomous.ai:rm namespace.
  for (const NamedRule& rule : kAllRules) {
    if (!rule.uid.starts_with("asam.net:xodr:")) {
      EXPECT_TRUE(rule.uid.starts_with("robomous.ai:rm:"))
          << rule.name << " = '" << rule.uid << "'";
    }
  }
}

// --- 3. reader diagnostic paths (parameterized malformed-input suite) --------

namespace {

/// A valid, diagnostic-free base road every document keeps, so each row can
/// assert the rest of the network survived its malformed snippet.
constexpr const char* kBaseRoad = R"(
  <road id="1" length="10">
    <planView><geometry s="0" x="0" y="0" hdg="0" length="10"><line/></geometry></planView>
    <lanes><laneSection s="0"><center><lane id="0" type="none"/></center></laneSection></lanes>
  </road>)";

/// Wraps `body` (roads, junctions, controllers, root userData) after the base
/// road inside a well-formed document.
std::string doc(std::string_view body) {
  return std::string("<OpenDRIVE>\n  <header revMajor=\"1\" revMinor=\"7\"/>") + kBaseRoad +
         std::string(body) + "\n</OpenDRIVE>";
}

/// A second valid road ("2") carrying `inner` (objects/signals/userData), for
/// rows whose malformed payload rides an otherwise healthy road.
std::string road2(std::string_view inner) {
  return std::string(R"(
  <road id="2" length="10">
    <planView><geometry s="0" x="0" y="0" hdg="0" length="10"><line/></geometry></planView>
    <lanes><laneSection s="0"><center><lane id="0" type="none"/></center></laneSection></lanes>
)") + std::string(inner) +
         "\n  </road>";
}

const Diagnostic* find_diag(const std::vector<Diagnostic>& diagnostics, std::string_view needle) {
  const auto it = std::ranges::find_if(diagnostics, [&](const Diagnostic& d) {
    return d.message.find(needle) != std::string::npos;
  });
  return it == diagnostics.end() ? nullptr : &*it;
}

const roadmaker::Junction& junction9(const XodrParseResult& result) {
  const roadmaker::JunctionId id = result.network.find_junction("9");
  EXPECT_TRUE(id.is_valid());
  return *result.network.junction(id);
}

const roadmaker::Object& sole_object_on_road2(const XodrParseResult& result) {
  const std::vector<roadmaker::ObjectId> ids =
      roadmaker::objects_of(result.network, result.network.find_road("2"));
  EXPECT_EQ(ids.size(), 1U);
  return *result.network.object(ids.front());
}

/// One reader emit site: the document, the expected diagnostic (severity +
/// exact rule id, empty pinning "deliberately no normative rule"), and an
/// optional entity-level check that the skip-the-entity contract held while
/// the rest of the network survived.
struct DiagCase {
  const char* label;
  std::string xml;
  Severity severity;
  std::string_view rule;                  ///< expected rule_id verbatim; "" pins an empty one
  const char* needle;                     ///< message substring the emit site produces
  void (*verify)(const XodrParseResult&); ///< nullptr = no extra check
};

std::vector<DiagCase> make_cases() {
  using roadmaker::rules::kControllerValidForSignals;
  using roadmaker::rules::kLaneSectionRequired;
  using roadmaker::rules::kObjectTypeAttr;
  using roadmaker::rules::kOnlyRefDefinedIds;
  using roadmaker::rules::kReflineExists;
  using roadmaker::rules::kSignalType;
  using roadmaker::rules::kSignalUseCountryCode;

  std::vector<DiagCase> cases;

  // --- structural OpenDRIVE errors (reader.cpp parse_* sites) ---------------

  cases.push_back({.label = "missing header",
                   .xml = std::string("<OpenDRIVE>") + kBaseRoad + "\n</OpenDRIVE>",
                   .severity = Severity::Warning,
                   .rule = "",
                   .needle = "missing <header>",
                   .verify = +[](const XodrParseResult& result) {
                     EXPECT_DOUBLE_EQ(result.revision, 0.0); // no header -> no revision
                   }});

  cases.push_back({.label = "road without id",
                   .xml = doc(R"(
  <road length="5">
    <planView><geometry s="0" x="0" y="0" hdg="0" length="5"><line/></geometry></planView>
    <lanes><laneSection s="0"><center><lane id="0" type="none"/></center></laneSection></lanes>
  </road>)"),
                   .severity = Severity::Error,
                   .rule = "",
                   .needle = "road without 'id' attribute skipped",
                   .verify = +[](const XodrParseResult& result) {
                     EXPECT_EQ(result.network.road_count(), 1U); // only the base road
                   }});

  // "Each road shall have a road reference line" (1.8.1 Annex E.5.9.6). The
  // road still LOADS — Error diagnostic, entity kept, zero-length refline.
  cases.push_back({.label = "road with no usable planView",
                   .xml = doc(R"(
  <road id="2" length="0">
    <planView/>
    <lanes><laneSection s="0"><center><lane id="0" type="none"/></center></laneSection></lanes>
  </road>)"),
                   .severity = Severity::Error,
                   .rule = kReflineExists,
                   .needle = "road has no usable planView geometry",
                   .verify = +[](const XodrParseResult& result) {
                     EXPECT_TRUE(result.network.find_road("2").is_valid()); // kept, not dropped
                   }});

  cases.push_back({.label = "geometry with nonpositive length",
                   .xml = doc(R"(
  <road id="2" length="10">
    <planView>
      <geometry s="0" x="0" y="0" hdg="0" length="10"><line/></geometry>
      <geometry s="10" x="10" y="0" hdg="0" length="0"><line/></geometry>
    </planView>
    <lanes><laneSection s="0"><center><lane id="0" type="none"/></center></laneSection></lanes>
  </road>)"),
                   .severity = Severity::Warning,
                   .rule = "",
                   .needle = "geometry record with length <= 0 skipped",
                   .verify = +[](const XodrParseResult& result) {
                     const roadmaker::Road& road =
                         *result.network.road(result.network.find_road("2"));
                     EXPECT_EQ(road.plan_view.records().size(), 1U); // bad record skipped
                     EXPECT_DOUBLE_EQ(road.length, 10.0);            // good one kept
                   }});

  // "Each road shall have at least one lane section" (Annex E.5.11.2), both
  // emit sites: no <lanes> element at all, and <lanes> with no laneSection.
  cases.push_back({.label = "road without lanes element",
                   .xml = doc(R"(
  <road id="2" length="10">
    <planView><geometry s="0" x="0" y="0" hdg="0" length="10"><line/></geometry></planView>
  </road>)"),
                   .severity = Severity::Warning,
                   .rule = kLaneSectionRequired,
                   .needle = "road has no <lanes> element",
                   .verify = +[](const XodrParseResult& result) {
                     EXPECT_TRUE(result.network.find_road("2").is_valid());
                   }});

  cases.push_back({.label = "road without lane sections",
                   .xml = doc(R"(
  <road id="2" length="10">
    <planView><geometry s="0" x="0" y="0" hdg="0" length="10"><line/></geometry></planView>
    <lanes/>
  </road>)"),
                   .severity = Severity::Warning,
                   .rule = kLaneSectionRequired,
                   .needle = "road has no lane sections",
                   .verify = +[](const XodrParseResult& result) {
                     EXPECT_TRUE(
                         result.network.road(result.network.find_road("2"))->sections.empty());
                   }});

  // Objects: the entity always loads; the missing attribute is diagnosed.
  cases.push_back({.label = "object without id",
                   .xml = doc(road2(R"(    <objects>
      <object type="pole" s="1" t="0" zOffset="0" orientation="none"/>
    </objects>)")),
                   .severity = Severity::Warning,
                   .rule = "",
                   .needle = "object without 'id' attribute",
                   .verify = +[](const XodrParseResult& result) {
                     EXPECT_TRUE(sole_object_on_road2(result).odr_id.empty());
                   }});

  // "The type of an object shall be given by the @type attribute"
  // (Annex E.5.14.5).
  cases.push_back({.label = "object without type",
                   .xml = doc(road2(R"(    <objects>
      <object id="o1" s="1" t="0" zOffset="0" orientation="none"/>
    </objects>)")),
                   .severity = Severity::Warning,
                   .rule = kObjectTypeAttr,
                   .needle = "object without 'type' attribute",
                   .verify = +[](const XodrParseResult& result) {
                     EXPECT_EQ(sole_object_on_road2(result).odr_id, "o1"); // still loaded
                   }});

  // Signals (§14.1 rules, Annex E.5.16).
  cases.push_back(
      {.label = "signal without id",
       .xml = doc(road2(R"(    <signals>
      <signal s="1" t="0" zOffset="0" orientation="none" type="1000001" subtype="-1"
              country="OpenDRIVE"/>
    </signals>)")),
       .severity = Severity::Warning,
       .rule = "",
       .needle = "signal without 'id' attribute",
       .verify = +[](const XodrParseResult& result) {
         EXPECT_EQ(roadmaker::signals_of(result.network, result.network.find_road("2")).size(), 1U);
       }});

  cases.push_back({.label = "signal without type",
                   .xml = doc(road2(R"(    <signals>
      <signal id="g1" s="1" t="0" zOffset="0" orientation="none" country="OpenDRIVE"/>
    </signals>)")),
                   .severity = Severity::Warning,
                   .rule = kSignalType,
                   .needle = "signal requires 'type' and 'subtype' attributes",
                   .verify = nullptr});

  cases.push_back({.label = "signal without country",
                   .xml = doc(road2(R"(    <signals>
      <signal id="g1" s="1" t="0" zOffset="0" orientation="none" type="1000001" subtype="-1"/>
    </signals>)")),
                   .severity = Severity::Warning,
                   .rule = kSignalUseCountryCode,
                   .needle = "signal without 'country' attribute",
                   .verify = nullptr});

  // Controllers (§14.6 Tables 128/129).
  cases.push_back({.label = "controller without id",
                   .xml = doc(R"(
  <controller><control signalId="s1" type=""/></controller>)"),
                   .severity = Severity::Warning,
                   .rule = "",
                   .needle = "controller without 'id' attribute",
                   .verify = +[](const XodrParseResult& result) {
                     EXPECT_EQ(result.network.controller_count(), 1U); // still loads
                   }});

  // "Controllers shall be valid for one or more signals" — <control> is 1..*.
  cases.push_back({.label = "controller with no controls",
                   .xml = doc(R"(
  <controller id="c9"/>)"),
                   .severity = Severity::Warning,
                   .rule = kControllerValidForSignals,
                   .needle = "controller has no <control> children",
                   .verify = +[](const XodrParseResult& result) {
                     EXPECT_EQ(result.network.controller_count(), 1U);
                   }});

  cases.push_back({.label = "control without signalId",
                   .xml = doc(R"(
  <controller id="c1"><control type="0"/></controller>)"),
                   .severity = Severity::Warning,
                   .rule = "",
                   .needle = "control without 'signalId' attribute",
                   .verify = +[](const XodrParseResult& result) {
                     EXPECT_EQ(result.network.controller_count(), 1U);
                   }});

  // Junctions.
  cases.push_back({.label = "junction without id",
                   .xml = doc(R"(
  <junction name="ghost"/>)"),
                   .severity = Severity::Error,
                   .rule = "",
                   .needle = "junction without 'id' attribute skipped",
                   .verify = +[](const XodrParseResult& result) {
                     EXPECT_EQ(result.network.junction_count(), 0U);
                   }});

  // "Only defined IDs may be referenced" (Annex E.3.3): the connection is
  // skipped, the junction itself survives.
  cases.push_back({.label = "connection references unknown road",
                   .xml = doc(R"(
  <junction id="9">
    <connection id="0" incomingRoad="404" connectingRoad="405" contactPoint="start"/>
  </junction>)"),
                   .severity = Severity::Warning,
                   .rule = kOnlyRefDefinedIds,
                   .needle = "connection references unknown road",
                   .verify = +[](const XodrParseResult& result) {
                     EXPECT_TRUE(junction9(result).connections.empty());
                   }});

  // --- malformed rm:* userData branches (warn-and-skip, never silent) -------

  cases.push_back(
      {.label = "malformed rm:waypoints",
       .xml = doc(road2(R"(    <userData code="rm:waypoints" value="0,zero;10,0"/>)")),
       .severity = Severity::Warning,
       .rule = "",
       .needle = "malformed rm:waypoints userData ignored",
       .verify = +[](const XodrParseResult& result) {
         EXPECT_FALSE(
             result.network.road(result.network.find_road("2"))->authoring_waypoints.has_value());
       }});

  cases.push_back({.label = "malformed rm:crosswalk",
                   .xml = doc(road2(R"(    <objects>
      <object id="o1" type="crosswalk" s="1" t="0" zOffset="0" orientation="none">
        <userData code="rm:crosswalk" borderWidth="wide"/>
      </object>
    </objects>)")),
                   .severity = Severity::Warning,
                   .rule = "",
                   .needle = "malformed rm:crosswalk userData ignored",
                   .verify = +[](const XodrParseResult& result) {
                     // The object survives; only the parametric record is dropped.
                     EXPECT_FALSE(sole_object_on_road2(result).crosswalk.has_value());
                   }});

  cases.push_back({.label = "malformed rm:markingCurve",
                   .xml = doc(road2(R"(    <objects>
      <object id="o1" type="none" s="1" t="0" zOffset="0" orientation="none">
        <userData code="rm:markingCurve" samples="0,0"/>
      </object>
    </objects>)")),
                   .severity = Severity::Warning,
                   .rule = "",
                   .needle = "malformed rm:markingCurve userData ignored",
                   .verify = +[](const XodrParseResult& result) {
                     EXPECT_FALSE(sole_object_on_road2(result).marking_curve.has_value());
                   }});

  cases.push_back({.label = "malformed rm:corners",
                   .xml = doc(R"(
  <junction id="9"><userData code="rm:corners" value="garbage"/></junction>)"),
                   .severity = Severity::Warning,
                   .rule = "",
                   .needle = "malformed rm:corners userData ignored",
                   .verify = +[](const XodrParseResult& result) {
                     EXPECT_TRUE(junction9(result).corners.empty());
                   }});

  cases.push_back({.label = "malformed rm:spans",
                   .xml = doc(R"(
  <junction id="9"><userData code="rm:spans" value="nope"/></junction>)"),
                   .severity = Severity::Warning,
                   .rule = "",
                   .needle = "malformed rm:spans userData ignored",
                   .verify = +[](const XodrParseResult& result) {
                     EXPECT_TRUE(junction9(result).spans.empty());
                   }});

  cases.push_back({.label = "malformed rm:phases",
                   .xml = doc(R"(
  <junction id="9"><userData code="rm:phases" value="dur=0"/></junction>)"),
                   .severity = Severity::Warning,
                   .rule = "",
                   .needle = "malformed rm:phases userData ignored",
                   .verify = +[](const XodrParseResult& result) {
                     EXPECT_TRUE(junction9(result).phases.empty());
                   }});

  cases.push_back({.label = "rm:junction bad material token",
                   .xml = doc(R"(
  <junction id="9"><userData code="rm:junction" value="mat=a b"/></junction>)"),
                   .severity = Severity::Warning,
                   .rule = "",
                   .needle = "malformed rm:junction userData ignored",
                   .verify = +[](const XodrParseResult& result) {
                     EXPECT_TRUE(junction9(result).material.empty());
                   }});

  cases.push_back({.label = "rm:junction bad lock value",
                   .xml = doc(R"(
  <junction id="9"><userData code="rm:junction" value="locked=2"/></junction>)"),
                   .severity = Severity::Warning,
                   .rule = "",
                   .needle = "malformed rm:junction userData ignored",
                   .verify = +[](const XodrParseResult& result) {
                     EXPECT_FALSE(junction9(result).locked);
                     EXPECT_FALSE(junction9(result).default_corner_radius.has_value());
                   }});

  cases.push_back({.label = "malformed rm:arms",
                   .xml = doc(R"(
  <junction id="9"><userData code="rm:arms" value="1:sideways"/></junction>)"),
                   .severity = Severity::Warning,
                   .rule = "",
                   .needle = "malformed rm:arms userData ignored",
                   .verify = +[](const XodrParseResult& result) {
                     EXPECT_TRUE(junction9(result).arms.empty());
                   }});

  cases.push_back({.label = "rm:surface with unresolvable road",
                   .xml = doc(R"(
  <userData code="rm:surface" value="1;404;405"/>)"),
                   .severity = Severity::Warning,
                   .rule = "",
                   .needle = "malformed rm:surface userData ignored",
                   .verify = +[](const XodrParseResult& result) {
                     EXPECT_EQ(result.network.surface_count(), 0U);
                   }});

  cases.push_back({.label = "rm:surface with malformed authored nodes",
                   .xml = doc(R"(
  <userData code="rm:surface" nodes="0,0,1" value=""/>)"),
                   .severity = Severity::Warning,
                   .rule = "",
                   .needle = "malformed rm:surface nodes userData ignored",
                   .verify = +[](const XodrParseResult& result) {
                     EXPECT_EQ(result.network.surface_count(), 0U);
                   }});

  cases.push_back({.label = "rm:terrain unsafe sidecar path",
                   .xml = doc(R"(
  <userData code="rm:terrain" value="../escape.asc"/>)"),
                   .severity = Severity::Warning,
                   .rule = "",
                   .needle = "rm:terrain names an unusable sidecar path",
                   .verify = +[](const XodrParseResult& result) {
                     EXPECT_TRUE(result.network.terrain().sidecar.empty());
                   }});

  return cases;
}

class XodrReaderDiag : public testing::TestWithParam<DiagCase> {};

} // namespace

TEST_P(XodrReaderDiag, EmitsAStructuredDiagnosticAndNeverDropsTheDocument) {
  const DiagCase& row = GetParam();
  const auto result = roadmaker::parse_xodr(row.xml, row.label);

  // The reader's contract (reader.hpp): a parse only fails outright on
  // structural XML problems. Everything here is a best-effort result plus
  // structured diagnostics — never a silent drop, never a hard failure.
  ASSERT_TRUE(result.has_value()) << row.label;
  EXPECT_TRUE(result->network.find_road("1").is_valid())
      << row.label << ": the healthy base road must survive";

  const Diagnostic* diagnostic = find_diag(result->diagnostics, row.needle);
  ASSERT_NE(diagnostic, nullptr) << row.label << ": no diagnostic contains '" << row.needle << "'";
  EXPECT_EQ(diagnostic->severity, row.severity) << row.label;
  EXPECT_EQ(diagnostic->rule_id, row.rule) << row.label;

  if (row.verify != nullptr) {
    row.verify(*result);
  }
}

INSTANTIATE_TEST_SUITE_P(Cases,
                         XodrReaderDiag,
                         testing::ValuesIn(make_cases()),
                         [](const testing::TestParamInfo<DiagCase>& param_info) {
                           std::string name = param_info.param.label;
                           for (char& c : name) {
                             if (std::isalnum(static_cast<unsigned char>(c)) == 0) {
                               c = '_';
                             }
                           }
                           return name;
                         });
