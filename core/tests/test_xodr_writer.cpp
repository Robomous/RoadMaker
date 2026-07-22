// Copyright 2026 Robomous
// SPDX-License-Identifier: Apache-2.0

// Version-explicit writer target + checker-rule validation (issue #12,
// docs/design/m2/02_editing_tools.md §8): the header carries the selected
// revMinor, and validate_network cites normative rule UIDs — rules present
// in only one version's catalog are cited only for that target.

#include "roadmaker/road/authoring.hpp"
#include "roadmaker/xodr/reader.hpp"
#include "roadmaker/xodr/rules.hpp"
#include "roadmaker/xodr/writer.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <string>
#include <vector>

using roadmaker::ContactPoint;
using roadmaker::Diagnostic;
using roadmaker::JunctionConnection;
using roadmaker::JunctionId;
using roadmaker::LaneProfile;
using roadmaker::RoadId;
using roadmaker::RoadNetwork;
using roadmaker::Severity;
using roadmaker::Waypoint;
using roadmaker::WriterOptions;
using roadmaker::XodrVersion;

namespace {

RoadId author_default(RoadNetwork& network, const char* odr_id, double y = 0.0) {
  const std::array<Waypoint, 2> waypoints{Waypoint{.x = 0.0, .y = y}, Waypoint{.x = 100.0, .y = y}};
  const auto road = roadmaker::author_clothoid_road(
      network, waypoints, LaneProfile::two_lane_default(), "", odr_id);
  EXPECT_TRUE(road.has_value());
  return *road;
}

/// Two arms feeding one connecting road — the "only two roads meet" shape
/// (1.9.0 Annex F.4.5.3).
JunctionId make_two_arm_junction(RoadNetwork& network) {
  const RoadId incoming = author_default(network, "1");
  const RoadId connecting = author_default(network, "2", 40.0);
  const RoadId outgoing = author_default(network, "3", 80.0);
  const JunctionId junction = network.create_junction("100", "X");
  network.road(incoming)->successor =
      roadmaker::RoadLink{.target = junction, .contact = ContactPoint::Start};
  network.road(outgoing)->predecessor =
      roadmaker::RoadLink{.target = junction, .contact = ContactPoint::Start};
  roadmaker::Road& inner = *network.road(connecting);
  inner.junction = junction;
  inner.predecessor = roadmaker::RoadLink{.target = incoming, .contact = ContactPoint::End};
  inner.successor = roadmaker::RoadLink{.target = outgoing, .contact = ContactPoint::Start};
  network.junction(junction)->connections.push_back(JunctionConnection{
      .incoming_road = incoming,
      .connecting_road = connecting,
      .contact_point = ContactPoint::Start,
      .lane_links = {{-1, -1}},
  });
  return junction;
}

std::vector<Diagnostic> findings_with_rule(const std::vector<Diagnostic>& findings,
                                           std::string_view rule_id) {
  std::vector<Diagnostic> matched;
  for (const Diagnostic& finding : findings) {
    if (finding.rule_id == rule_id) {
      matched.push_back(finding);
    }
  }
  return matched;
}

} // namespace

TEST(XodrWriter, DefaultTargetWritesOpenDrive18Header) {
  RoadNetwork network;
  author_default(network, "1");

  const auto text = roadmaker::write_xodr(network);
  ASSERT_TRUE(text.has_value());
  EXPECT_NE(text->find("revMajor=\"1\""), std::string::npos);
  EXPECT_NE(text->find("revMinor=\"8\""), std::string::npos);

  const auto parsed = roadmaker::parse_xodr(*text);
  ASSERT_TRUE(parsed.has_value());
  EXPECT_DOUBLE_EQ(parsed->revision, 1.8);
}

TEST(XodrWriter, V190TargetWritesOpenDrive19Header) {
  RoadNetwork network;
  author_default(network, "1");

  const auto text =
      roadmaker::write_xodr(network, "roadmaker", {.target_version = XodrVersion::v1_9_0});
  ASSERT_TRUE(text.has_value());
  EXPECT_NE(text->find("revMinor=\"9\""), std::string::npos);

  const auto parsed = roadmaker::parse_xodr(*text);
  ASSERT_TRUE(parsed.has_value());
  EXPECT_DOUBLE_EQ(parsed->revision, 1.9);
}

TEST(XodrWriter, EmptyNetworkWritesHeaderOnlyDocumentThatReloads) {
  const RoadNetwork network;
  const auto text = roadmaker::write_xodr(network);
  ASSERT_TRUE(text.has_value());

  const auto parsed = roadmaker::parse_xodr(*text);
  ASSERT_TRUE(parsed.has_value());
  EXPECT_EQ(parsed->network.road_count(), 0U);
  EXPECT_TRUE(parsed->diagnostics.empty());
}

TEST(XodrWriter, ValidNetworkHasNoValidatorFindings) {
  RoadNetwork network;
  author_default(network, "1");
  EXPECT_TRUE(roadmaker::validate_network(network).empty());
  EXPECT_TRUE(
      roadmaker::validate_network(network, {.target_version = XodrVersion::v1_9_0}).empty());
}

TEST(XodrWriter, MissingLaneWidthCitesTheWidthRule) {
  RoadNetwork network;
  const RoadId road_id = author_default(network, "1");
  const roadmaker::LaneSection& section =
      *network.lane_section(network.road(road_id)->sections.front());
  for (const roadmaker::LaneId lane_id : section.lanes) {
    if (network.lane(lane_id)->odr_id == -1) {
      network.lane(lane_id)->widths.clear();
    }
  }

  const auto matched = findings_with_rule(roadmaker::validate_network(network),
                                          roadmaker::rules::kWidthDefinedWholeSection);
  ASSERT_EQ(matched.size(), 1U);
  EXPECT_EQ(matched.front().severity, Severity::Error);
  EXPECT_EQ(matched.front().road, road_id);
}

TEST(XodrWriter, LaneDirectionEmittedOnlyWhenNotStandard) {
  RoadNetwork network;
  const RoadId road_id = author_default(network, "1");
  const roadmaker::LaneSection& section =
      *network.lane_section(network.road(road_id)->sections.front());

  // Fresh lanes are Standard, so @direction must be absent for every lane.
  {
    const auto text = roadmaker::write_xodr(network);
    ASSERT_TRUE(text.has_value());
    EXPECT_EQ(text->find("direction="), std::string::npos);
  }

  // Set a non-center lane to Reversed and expect exactly that spelling.
  roadmaker::LaneId outer;
  for (const roadmaker::LaneId lane_id : section.lanes) {
    if (network.lane(lane_id)->odr_id == -1) {
      outer = lane_id;
    }
  }
  ASSERT_TRUE(outer.is_valid());
  network.lane(outer)->direction = roadmaker::LaneDirection::Reversed;

  const auto text = roadmaker::write_xodr(network);
  ASSERT_TRUE(text.has_value());
  EXPECT_NE(text->find("direction=\"reversed\""), std::string::npos);
  EXPECT_EQ(text->find("direction=\"standard\""), std::string::npos);
}

TEST(XodrWriter, CenterLaneDirectionIsAdvised) {
  RoadNetwork network;
  const RoadId road_id = author_default(network, "1");
  const roadmaker::LaneSection& section =
      *network.lane_section(network.road(road_id)->sections.front());
  for (const roadmaker::LaneId lane_id : section.lanes) {
    if (network.lane(lane_id)->odr_id == 0) {
      network.lane(lane_id)->direction = roadmaker::LaneDirection::Both;
    }
  }

  const auto findings = roadmaker::validate_network(network);
  const bool warned = std::any_of(findings.begin(), findings.end(), [](const Diagnostic& d) {
    return d.severity == Severity::Warning && d.message.find("center lane") != std::string::npos &&
           d.message.find("travel direction") != std::string::npos;
  });
  EXPECT_TRUE(warned);
}

TEST(XodrWriter, StructuralDefectsCarryRuleIdsAndRefuseTheWrite) {
  RoadNetwork network;
  const RoadId road_id = author_default(network, "1");
  network.road(road_id)->sections.clear();

  const auto matched = findings_with_rule(roadmaker::validate_network(network),
                                          roadmaker::rules::kLaneSectionRequired);
  ASSERT_EQ(matched.size(), 1U);
  EXPECT_EQ(matched.front().severity, Severity::Error);

  const auto text = roadmaker::write_xodr(network);
  ASSERT_FALSE(text.has_value());
  EXPECT_EQ(text.error().code, roadmaker::ErrorCode::InvalidArgument);
}

TEST(XodrWriter, TwoArmJunctionRuleIsCitedOnlyForV190) {
  RoadNetwork network;
  make_two_arm_junction(network);

  // 1.8.1's Annex E has no not_only_two rule — nothing to cite. (A common
  // junction still draws the boundary-omitted warning, so filter by rule
  // rather than asserting the finding set is empty.)
  EXPECT_TRUE(findings_with_rule(roadmaker::validate_network(network),
                                 roadmaker::rules::kJunctionNotOnlyTwo)
                  .empty());

  const auto findings =
      roadmaker::validate_network(network, {.target_version = XodrVersion::v1_9_0});
  const auto matched = findings_with_rule(findings, roadmaker::rules::kJunctionNotOnlyTwo);
  ASSERT_EQ(matched.size(), 1U);
  EXPECT_EQ(matched.front().severity, Severity::Warning);
  EXPECT_EQ(matched.front().location, "junction id=100");
}

TEST(XodrWriter, ThreeArmJunctionDoesNotTriggerTheNotOnlyTwoRule) {
  RoadNetwork network;
  const JunctionId junction = make_two_arm_junction(network);
  const RoadId third = author_default(network, "4", 120.0);
  network.road(third)->successor =
      roadmaker::RoadLink{.target = junction, .contact = ContactPoint::Start};

  const auto findings =
      roadmaker::validate_network(network, {.target_version = XodrVersion::v1_9_0});
  EXPECT_TRUE(findings_with_rule(findings, roadmaker::rules::kJunctionNotOnlyTwo).empty());
}
