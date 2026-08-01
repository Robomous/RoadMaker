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

// Round-trip invariants are first-class tests: author → write → re-parse →
// compare within rm::tol (position 1e-4 m, heading 1e-6 rad).

#include "roadmaker/edit/operations.hpp"
#include "roadmaker/road/authoring.hpp"
#include "roadmaker/xodr/diagnostic.hpp"
#include "roadmaker/xodr/reader.hpp"
#include "roadmaker/xodr/writer.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <ranges>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "support/network_compare.hpp"

using roadmaker::ContactPoint;
using roadmaker::JunctionId;
using roadmaker::LaneProfile;
using roadmaker::RoadEnd;
using roadmaker::RoadId;
using roadmaker::RoadNetwork;
using roadmaker::Waypoint;
using roadmaker::test::expect_same_geometry;

TEST(Authoring, ClothoidRoadIsG1Continuous) {
  RoadNetwork network;
  const std::array<Waypoint, 4> waypoints{
      Waypoint{.x = 0.0, .y = 0.0},
      Waypoint{.x = 50.0, .y = 10.0},
      Waypoint{.x = 90.0, .y = 50.0},
      Waypoint{.x = 100.0, .y = 100.0},
  };
  const auto road_id = roadmaker::author_clothoid_road(
      network, waypoints, LaneProfile::two_lane_default(), "Test Road", "1");
  ASSERT_TRUE(road_id.has_value());

  const roadmaker::Road& road = *network.road(*road_id);
  EXPECT_GE(road.plan_view.records().size(), 3U);
  EXPECT_GT(road.length, 100.0); // longer than the straight-line chain

  // The fitted path passes through every waypoint (record joints);
  // build_G1 makes one clothoid per waypoint pair.
  const auto& records = road.plan_view.records();
  ASSERT_EQ(records.size(), waypoints.size() - 1);
  for (std::size_t i = 0; i < records.size(); ++i) {
    EXPECT_NEAR(records[i].x, waypoints.at(i).x, 1e-9);
    EXPECT_NEAR(records[i].y, waypoints.at(i).y, 1e-9);
  }
  const auto end = road.plan_view.evaluate(road.length);
  EXPECT_NEAR(end.x, waypoints.back().x, 1e-6);
  EXPECT_NEAR(end.y, waypoints.back().y, 1e-6);

  // G1 continuity at every joint.
  for (std::size_t i = 1; i < records.size(); ++i) {
    const auto before = road.plan_view.evaluate(records[i].s - 1e-9);
    const auto after = road.plan_view.evaluate(records[i].s + 1e-9);
    EXPECT_NEAR(before.x, after.x, roadmaker::tol::kRoundTripPosition);
    EXPECT_NEAR(before.y, after.y, roadmaker::tol::kRoundTripPosition);
    EXPECT_NEAR(before.hdg, after.hdg, 1e-6);
  }

  // Lane structure per the default profile: 0, +1, -1, -2.
  const roadmaker::LaneSection& section = *network.lane_section(road.sections.at(0));
  EXPECT_EQ(section.lanes.size(), 4U);
}

TEST(Authoring, RejectsBadInput) {
  RoadNetwork network;
  const LaneProfile profile = LaneProfile::two_lane_default();

  EXPECT_FALSE(roadmaker::author_clothoid_road(
                   network, std::array<Waypoint, 1>{Waypoint{.x = 0, .y = 0}}, profile)
                   .has_value());
  EXPECT_FALSE(roadmaker::author_clothoid_road(
                   network,
                   std::array<Waypoint, 2>{Waypoint{.x = 1, .y = 1}, Waypoint{.x = 1, .y = 1}},
                   profile)
                   .has_value());
  EXPECT_FALSE(roadmaker::author_clothoid_road(
                   network,
                   std::array<Waypoint, 2>{Waypoint{.x = 0, .y = 0}, Waypoint{.x = 9, .y = 9}},
                   LaneProfile{})
                   .has_value());
}

TEST(RoundTrip, AuthorWriteParseWithinTolerance) {
  RoadNetwork authored;
  const std::array<Waypoint, 5> waypoints{
      Waypoint{.x = 0.0, .y = 0.0},
      Waypoint{.x = 40.0, .y = 5.0},
      Waypoint{.x = 80.0, .y = 30.0},
      Waypoint{.x = 100.0, .y = 70.0},
      Waypoint{.x = 90.0, .y = 120.0},
  };
  const auto road_id = roadmaker::author_clothoid_road(
      authored, waypoints, LaneProfile::two_lane_default(), "Loop", "7");
  ASSERT_TRUE(road_id.has_value());

  const auto xml = roadmaker::write_xodr(authored, "round_trip");
  ASSERT_TRUE(xml.has_value());

  const auto reparsed = roadmaker::parse_xodr(*xml, "round_trip");
  ASSERT_TRUE(reparsed.has_value());
  EXPECT_EQ(roadmaker::count_errors(reparsed->diagnostics), 0U);
  ASSERT_EQ(reparsed->network.road_count(), 1U);

  const roadmaker::Road& original = *authored.road(*road_id);
  const roadmaker::Road& round = *reparsed->network.road(reparsed->network.find_road("7"));
  expect_same_geometry(original, round);

  // Lane structure survives.
  const auto& section_a = *authored.lane_section(original.sections[0]);
  const auto& section_b = *reparsed->network.lane_section(round.sections[0]);
  ASSERT_EQ(section_a.lanes.size(), section_b.lanes.size());
  for (std::size_t i = 0; i < section_a.lanes.size(); ++i) {
    const auto& lane_a = *authored.lane(section_a.lanes[i]);
    const auto& lane_b = *reparsed->network.lane(section_b.lanes[i]);
    EXPECT_EQ(lane_a.odr_id, lane_b.odr_id);
    EXPECT_EQ(lane_a.type, lane_b.type);
    EXPECT_EQ(lane_a.widths.size(), lane_b.widths.size());
    EXPECT_EQ(lane_a.road_marks.size(), lane_b.road_marks.size());
  }
}

TEST(RoundTrip, ParsedSampleWriteParsePreservesTopologyAndGeometry) {
  auto first = roadmaker::load_xodr(std::filesystem::path(RM_SAMPLES_DIR) / "t_junction.xodr");
  ASSERT_TRUE(first.has_value());

  const auto xml = roadmaker::write_xodr(first->network, "t_junction");
  ASSERT_TRUE(xml.has_value());
  const auto second = roadmaker::parse_xodr(*xml, "rewritten");
  ASSERT_TRUE(second.has_value());
  EXPECT_EQ(roadmaker::count_errors(second->diagnostics), 0U);

  EXPECT_EQ(second->network.road_count(), first->network.road_count());
  EXPECT_EQ(second->network.junction_count(), first->network.junction_count());

  first->network.for_each_road([&](RoadId, const roadmaker::Road& road) {
    const RoadId other_id = second->network.find_road(road.odr_id);
    ASSERT_TRUE(other_id.is_valid());
    expect_same_geometry(road, *second->network.road(other_id));
  });

  // Junction connections survive with lane links.
  const auto j1 = *first->network.junction(first->network.find_junction("100"));
  const auto j2 = *second->network.junction(second->network.find_junction("100"));
  ASSERT_EQ(j1.connections.size(), j2.connections.size());
  EXPECT_EQ(j2.connections[0].lane_links, j1.connections[0].lane_links);
}

TEST(RoundTrip, GeneratedJunctionArmsSurviveWriteParseAndRegenerate) {
  RoadNetwork network;
  const auto arm = [&](std::vector<Waypoint> waypoints, const char* id) {
    return *roadmaker::author_clothoid_road(
        network, waypoints, LaneProfile::two_lane_default(), "", id);
  };
  const RoadId west = arm({Waypoint{-40.0, 0.0}, Waypoint{-6.0, 0.0}}, "1");
  const RoadId east = arm({Waypoint{40.0, 0.0}, Waypoint{6.0, 0.0}}, "2");
  const RoadId south = arm({Waypoint{0.0, -40.0}, Waypoint{0.0, -6.0}}, "3");
  const std::array<RoadEnd, 3> ends{RoadEnd{.road = west, .contact = ContactPoint::End},
                                    RoadEnd{.road = east, .contact = ContactPoint::End},
                                    RoadEnd{.road = south, .contact = ContactPoint::End}};
  ASSERT_TRUE(roadmaker::edit::create_junction(network, ends)->apply(network).has_value());

  const auto xml = roadmaker::write_xodr(network, "gen_junction");
  ASSERT_TRUE(xml.has_value());
  EXPECT_NE(xml->find("rm:arms"), std::string::npos);

  auto reparsed = roadmaker::parse_xodr(*xml, "gen_junction");
  ASSERT_TRUE(reparsed.has_value());
  EXPECT_EQ(roadmaker::count_errors(reparsed->diagnostics), 0U);

  // The arm list survives the round trip, so the reloaded junction still
  // regenerates — and a no-op regeneration reproduces the document.
  const JunctionId junction = reparsed->network.find_junction("1");
  ASSERT_TRUE(junction.is_valid());
  EXPECT_EQ(reparsed->network.junction(junction)->arms.size(), 3U);

  const auto before = roadmaker::write_xodr(reparsed->network, "gen_junction");
  ASSERT_TRUE(before.has_value());
  auto regen = roadmaker::edit::regenerate_junction(reparsed->network, junction);
  ASSERT_TRUE(regen->apply(reparsed->network).has_value());
  const auto after = roadmaker::write_xodr(reparsed->network, "gen_junction");
  ASSERT_TRUE(after.has_value());
  EXPECT_EQ(*before, *after);
}

TEST(RoundTrip, AuthoringWaypointsSurviveWriteParse) {
  RoadNetwork authored;
  const std::array<Waypoint, 3> waypoints{
      Waypoint{.x = 0.0, .y = 0.0},
      Waypoint{.x = 50.5, .y = 10.25},
      Waypoint{.x = 100.0, .y = -3.125},
  };
  const auto road_id = roadmaker::author_clothoid_road(
      authored, waypoints, LaneProfile::two_lane_default(), "WP", "1");
  ASSERT_TRUE(road_id.has_value());
  ASSERT_TRUE(authored.road(*road_id)->authoring_waypoints.has_value());

  const auto xml = roadmaker::write_xodr(authored, "wp");
  ASSERT_TRUE(xml.has_value());
  EXPECT_NE(xml->find("rm:waypoints"), std::string::npos);

  const auto reparsed = roadmaker::parse_xodr(*xml, "wp");
  ASSERT_TRUE(reparsed.has_value());
  const roadmaker::Road& round = *reparsed->network.road(reparsed->network.find_road("1"));
  ASSERT_TRUE(round.authoring_waypoints.has_value());
  // The writer's shortest-round-trip formatting reproduces doubles exactly.
  EXPECT_EQ(*round.authoring_waypoints,
            (std::vector<Waypoint>(waypoints.begin(), waypoints.end())));
}

TEST(RoundTrip, LaneDirectionSurvivesWriteParseWrite) {
  RoadNetwork authored;
  const std::array<Waypoint, 3> waypoints{
      Waypoint{.x = 0.0, .y = 0.0},
      Waypoint{.x = 60.0, .y = 8.0},
      Waypoint{.x = 120.0, .y = 0.0},
  };
  const auto road_id = roadmaker::author_clothoid_road(
      authored, waypoints, LaneProfile::two_lane_default(), "Dir", "1");
  ASSERT_TRUE(road_id.has_value());

  // Give the two non-center lanes distinct non-Standard directions.
  const roadmaker::LaneSection& section =
      *authored.lane_section(authored.road(*road_id)->sections.front());
  for (const roadmaker::LaneId lane_id : section.lanes) {
    roadmaker::Lane& lane = *authored.lane(lane_id);
    if (lane.odr_id == -1) {
      lane.direction = roadmaker::LaneDirection::Reversed;
    } else if (lane.odr_id == 1) {
      lane.direction = roadmaker::LaneDirection::Both;
    }
  }

  const auto xml = roadmaker::write_xodr(authored, "dir");
  ASSERT_TRUE(xml.has_value());
  const auto reparsed = roadmaker::parse_xodr(*xml, "dir");
  ASSERT_TRUE(reparsed.has_value());

  const roadmaker::Road& round = *reparsed->network.road(reparsed->network.find_road("1"));
  const roadmaker::LaneSection& round_section = *reparsed->network.lane_section(round.sections[0]);
  for (const roadmaker::LaneId lane_id : round_section.lanes) {
    const roadmaker::Lane& lane = *reparsed->network.lane(lane_id);
    if (lane.odr_id == -1) {
      EXPECT_EQ(lane.direction, roadmaker::LaneDirection::Reversed);
    } else if (lane.odr_id == 1) {
      EXPECT_EQ(lane.direction, roadmaker::LaneDirection::Both);
    } else {
      EXPECT_EQ(lane.direction, roadmaker::LaneDirection::Standard);
    }
  }

  // Byte-stable second pass: write→parse→write reproduces the same bytes.
  const auto again = roadmaker::write_xodr(reparsed->network, "dir");
  ASSERT_TRUE(again.has_value());
  EXPECT_EQ(*xml, *again);
}

// --- the warned-and-dropped spec areas (fmt-f2, #539) ------------------------
//
// These were LOUD and lossy: one diagnostic, then permanent data loss. Worse
// than a silent drop in one respect — the file looked like it had been
// understood. None of them is modeled by this build; the bar is that they
// round-trip byte-for-byte and that the diagnostic says so.

TEST(RoundTrip, TheWarnedAndDroppedScopesNowSurviveWriteParseWrite) {
  const std::filesystem::path sample =
      std::filesystem::path(RM_FUZZ_CORPUS_DIR) / "preserved_sweep.xodr";
  const auto loaded = roadmaker::load_xodr(sample);
  ASSERT_TRUE(loaded.has_value()) << (loaded ? "" : loaded.error().message);

  const auto written = roadmaker::write_xodr(loaded->network, "preserved_sweep");
  ASSERT_TRUE(written.has_value());

  for (const std::string_view marker : {
           "<shape",         // 1. lateralProfile <shape> (§10.5.1)
           "<crossfall",     // 1. the legacy <crossfall>
           "<surface",       // 2. road <surface> (§10.6)
           "<CRG",           // 2. its <CRG> child
           "<junctionGroup", // 3. §12.16 — the roundabout grouping (#495)
           "<railroad",      // 4. chapter 15, road child
           "<station",       // 4. chapter 15, root child
           "<include",       // 5. §7.1, outside <header>
       }) {
    EXPECT_NE(written->find(marker), std::string::npos) << "lost: " << marker;
  }
  // Content, not just the tag: a writer emitting an empty <shape/> would pass
  // a tag-only check while still having flattened the carriageway.
  EXPECT_NE(written->find(R"(file="pavement.crg")"), std::string::npos);
  EXPECT_NE(written->find(R"(junction="101")"), std::string::npos);
  EXPECT_NE(written->find(R"(file="extra_geometry.xodr")"), std::string::npos);

  // Fixed point.
  const auto reparsed = roadmaker::parse_xodr(*written, "preserved_sweep");
  ASSERT_TRUE(reparsed.has_value());
  const auto again = roadmaker::write_xodr(reparsed->network, "preserved_sweep");
  ASSERT_TRUE(again.has_value());
  EXPECT_EQ(*written, *again);
  EXPECT_EQ(roadmaker::count_errors(loaded->diagnostics), 0U);
}

TEST(RoundTrip, TheSweptScopesSayPreservedRatherThanIgnored) {
  // The diagnostic is half the fix. "not supported yet and was ignored" was
  // TRUE before and is a lie now; what the user still needs to know is that the
  // element has no effect, not that it was thrown away.
  const auto loaded =
      roadmaker::load_xodr(std::filesystem::path(RM_FUZZ_CORPUS_DIR) / "preserved_sweep.xodr");
  ASSERT_TRUE(loaded.has_value());

  const auto says = [&](std::string_view needle) {
    return std::ranges::any_of(loaded->diagnostics, [&](const roadmaker::Diagnostic& d) {
      return d.message.find(needle) != std::string::npos;
    });
  };
  EXPECT_TRUE(says("<shape> is preserved verbatim but not modeled"));
  EXPECT_TRUE(says("<junctionGroup> is preserved verbatim but not modeled"));
  EXPECT_TRUE(says("<railroad> is preserved verbatim but not modeled"));
  // And nothing claims to have ignored them any more.
  EXPECT_FALSE(says("<shape> is not supported yet and was ignored"));
  EXPECT_FALSE(says("<junctionGroup> is not supported yet and was ignored"));
}

// --- the remaining non-preserving parse scopes (fmt-f1, #453) ----------------
//
// Six scopes that each dropped unmodeled input in silence. They are asserted
// TOGETHER against one fixture because the failure mode is uniform — a scope
// with no preserved tier loses everything it does not model — and because a
// per-scope test would not catch a seventh scope being added without one.

TEST(RoundTrip, EveryPreservedScopeSurvivesWriteParseWrite) {
  const std::filesystem::path sample =
      std::filesystem::path(RM_FUZZ_CORPUS_DIR) / "preserved_scopes.xodr";
  const auto loaded = roadmaker::load_xodr(sample);
  ASSERT_TRUE(loaded.has_value()) << (loaded ? "" : loaded.error().message);

  const auto written = roadmaker::write_xodr(loaded->network, "preserved_scopes");
  ASSERT_TRUE(written.has_value());

  // Every marker the fixture plants, one per scope. Asserted on the BYTES:
  // a model-level check would need six different accessors and would still
  // miss a writer that read the field and emitted nothing.
  for (const std::string_view marker : {
           "acme:paint",         // 1. roadMark unknown attribute
           "acme:markNote",      // 1. roadMark unmodeled child
           "acme:sectionNote",   // 2. laneSection unmodeled child
           "singleSide",         // 2. laneSection unknown attribute
           "acme:lanesNote",     // 3. <lanes> container child
           "acme:elevationNote", // 4. <elevationProfile> child
           "<priority",          // 5. junction non-userData child (§12.9)
           "acme:surveyId",      // 6. unknown attributes on <lane> and <junction>
       }) {
    EXPECT_NE(written->find(marker), std::string::npos) << "lost: " << marker;
  }
  // Both @acme:surveyId values, not just one of the two scopes that carry it.
  EXPECT_NE(written->find(R"(acme:surveyId="L-7")"), std::string::npos) << "lane attribute lost";
  EXPECT_NE(written->find(R"(acme:surveyId="J-3")"), std::string::npos)
      << "junction attribute lost";

  // Fixed point: preserved content must not accumulate or reorder on re-save.
  const auto reparsed = roadmaker::parse_xodr(*written, "preserved_scopes");
  ASSERT_TRUE(reparsed.has_value());
  const auto again = roadmaker::write_xodr(reparsed->network, "preserved_scopes");
  ASSERT_TRUE(again.has_value());
  EXPECT_EQ(*written, *again);
  EXPECT_EQ(roadmaker::count_errors(loaded->diagnostics), 0U);
}

TEST(RoundTrip, PreservationDoesNotDuplicateModeledOrDerivedContent) {
  // The trap on the other side of this fix: name too few children "modeled"
  // and the writer emits them twice — once from the model, once from the
  // preserved tier. Counting is the only way to see it.
  const auto loaded =
      roadmaker::load_xodr(std::filesystem::path(RM_FUZZ_CORPUS_DIR) / "preserved_scopes.xodr");
  ASSERT_TRUE(loaded.has_value());
  const auto written = roadmaker::write_xodr(loaded->network, "preserved_scopes");
  ASSERT_TRUE(written.has_value());

  const auto count = [&](std::string_view needle) {
    std::size_t n = 0;
    for (std::size_t at = written->find(needle); at != std::string::npos;
         at = written->find(needle, at + 1)) {
      ++n;
    }
    return n;
  };
  EXPECT_EQ(count("<laneSection"), 2U) << "one per road";
  EXPECT_EQ(count("<elevationProfile"), 1U);
  EXPECT_EQ(count("<priority"), 1U);
  EXPECT_EQ(count("acme:markNote"), 1U);
  EXPECT_EQ(count("<roadMark"), 1U);
}

// --- direct and crossing junctions (#534) ------------------------------------
//
// A direct junction (§12.4) has NO connecting road: each <connection> carries
// @linkedRoad. The reader read @type only to test == "virtual" and read every
// connection expecting @connectingRoad, so `direct` was dropped AND every
// connection was skipped as "unknown road ''" — the file came back an EMPTY
// common junction. Destruction, not degradation.

TEST(RoundTrip, DirectAndCrossingJunctionsSurviveWriteParseWrite) {
  const std::filesystem::path sample =
      std::filesystem::path(RM_FUZZ_CORPUS_DIR) / "direct_junction.xodr";
  const auto loaded = roadmaker::load_xodr(sample);
  ASSERT_TRUE(loaded.has_value()) << (loaded ? "" : loaded.error().message);
  const RoadNetwork& network = loaded->network;

  // 1. The types survived.
  const roadmaker::Junction& direct = *network.junction(network.find_junction("100"));
  EXPECT_EQ(direct.type, roadmaker::JunctionType::Direct);
  const roadmaker::Junction& crossing = *network.junction(network.find_junction("200"));
  EXPECT_EQ(crossing.type, roadmaker::JunctionType::Crossing);

  // 2. The direct junction's connections exist, and are NOT in `connections` —
  // they have no connecting road, so the derived machinery must see none.
  EXPECT_TRUE(direct.connections.empty());
  ASSERT_EQ(direct.direct_connections.size(), 2U) << "connections were skipped again";
  EXPECT_EQ(direct.direct_connections[0].odr_id, "0");
  EXPECT_EQ(network.road(direct.direct_connections[0].linked_road)->odr_id, "2");
  EXPECT_EQ(network.road(direct.direct_connections[1].linked_road)->odr_id, "3");
  EXPECT_EQ(direct.direct_connections[0].contact_point,
            std::optional<roadmaker::ContactPoint>{roadmaker::ContactPoint::Start});

  // 3. @overlapZone and the layer attributes — never read anywhere before.
  ASSERT_EQ(direct.direct_connections[1].lane_links.size(), 1U);
  const roadmaker::DirectLaneLink& link = direct.direct_connections[1].lane_links[0];
  EXPECT_EQ(link.from, -2);
  EXPECT_EQ(link.to, -1);
  EXPECT_EQ(link.from_layer, "permanent");
  ASSERT_TRUE(link.overlap_zone.has_value());
  EXPECT_DOUBLE_EQ(*link.overlap_zone, 45.0);
  // Absent stays absent: the spec's default of 100 is not materialised.
  ASSERT_EQ(direct.direct_connections[0].lane_links.size(), 1U);
  EXPECT_FALSE(direct.direct_connections[0].lane_links[0].overlap_zone.has_value());

  // 4. A crossing junction uses @connectingRoad like a common one, so its
  // connection must land in the ORDINARY list. This is what stops the fix
  // meaning "anything that is not default is direct".
  EXPECT_TRUE(crossing.direct_connections.empty());
  EXPECT_EQ(crossing.connections.size(), 1U);

  // 5. The bytes. A model-only assertion would pass on a writer that still
  // emitted a common junction.
  const auto written = roadmaker::write_xodr(loaded->network, "direct_junction");
  ASSERT_TRUE(written.has_value());
  EXPECT_NE(written->find(R"(type="direct")"), std::string::npos);
  EXPECT_NE(written->find(R"(type="crossing")"), std::string::npos);
  EXPECT_NE(written->find(R"(linkedRoad="2")"), std::string::npos);
  EXPECT_NE(written->find(R"(linkedRoad="3")"), std::string::npos);
  EXPECT_NE(written->find(R"(overlapZone="45")"), std::string::npos);

  // 6. Fixed point, and the type is still Direct after the second trip rather
  // than decaying to the default.
  const auto reparsed = roadmaker::parse_xodr(*written, "direct_junction");
  ASSERT_TRUE(reparsed.has_value());
  EXPECT_EQ(reparsed->network.junction(reparsed->network.find_junction("100"))->type,
            roadmaker::JunctionType::Direct);
  const auto again = roadmaker::write_xodr(reparsed->network, "direct_junction");
  ASSERT_TRUE(again.has_value());
  EXPECT_EQ(*written, *again);
}

TEST(RoundTrip, ADirectJunctionRaisesNoUnknownRoadWarning) {
  // The old failure mode was loud AND wrong: every connection produced
  // "connection references unknown road ''" because @connectingRoad was absent
  // by design. A diagnostic that misnames the problem is worse than none.
  const auto loaded =
      roadmaker::load_xodr(std::filesystem::path(RM_FUZZ_CORPUS_DIR) / "direct_junction.xodr");
  ASSERT_TRUE(loaded.has_value());
  EXPECT_FALSE(std::ranges::any_of(loaded->diagnostics, [](const roadmaker::Diagnostic& d) {
    return d.message.find("unknown road ''") != std::string::npos;
  }));
  EXPECT_EQ(roadmaker::count_errors(loaded->diagnostics), 0U);
}

TEST(RoundTrip, AnAuthoredJunctionWritesNoTypeAttribute) {
  // `default` is what an absent @type means, so authoring must not start
  // stamping type="default" onto every junction in the suite.
  RoadNetwork network;
  const auto arm = [&](std::vector<Waypoint> waypoints, const char* id) {
    return *roadmaker::author_clothoid_road(
        network, waypoints, LaneProfile::two_lane_default(), "", id);
  };
  const RoadId west = arm({Waypoint{-40.0, 0.0}, Waypoint{-6.0, 0.0}}, "1");
  const RoadId east = arm({Waypoint{40.0, 0.0}, Waypoint{6.0, 0.0}}, "2");
  const RoadId south = arm({Waypoint{0.0, -40.0}, Waypoint{0.0, -6.0}}, "3");
  const std::array<RoadEnd, 3> ends{RoadEnd{.road = west, .contact = ContactPoint::End},
                                    RoadEnd{.road = east, .contact = ContactPoint::End},
                                    RoadEnd{.road = south, .contact = ContactPoint::End}};
  auto command = roadmaker::edit::create_junction(network, ends);
  ASSERT_NE(command, nullptr);
  ASSERT_TRUE(command->apply(network).has_value());

  const auto written = roadmaker::write_xodr(network, "plain_junction");
  ASSERT_TRUE(written.has_value());
  EXPECT_EQ(written->find(R"(type="default")"), std::string::npos);
}

// --- lane <link> multiplicity (#536) -----------------------------------------
//
// <predecessor>/<successor> are 0..* and the standard MANDATES several where a
// lane splits abruptly. The reader took the first child only, so a spec-required
// split was halved on every round trip — with no diagnostic, because nothing
// looked wrong about a lane having one successor.

TEST(RoundTrip, MultipleLaneLinksSurviveWriteParseWrite) {
  const std::filesystem::path sample =
      std::filesystem::path(RM_FUZZ_CORPUS_DIR) / "lane_link_multi.xodr";
  const auto loaded = roadmaker::load_xodr(sample);
  ASSERT_TRUE(loaded.has_value()) << (loaded ? "" : loaded.error().message);
  const RoadNetwork& network = loaded->network;

  const roadmaker::Road& road = *network.road(network.find_road("1"));
  ASSERT_EQ(road.sections.size(), 2U);
  const auto lane_of = [&](std::size_t section, int odr_id) -> const roadmaker::Lane& {
    for (const roadmaker::LaneId id : network.lane_section(road.sections[section])->lanes) {
      if (network.lane(id)->odr_id == odr_id) {
        return *network.lane(id);
      }
    }
    throw std::runtime_error("lane missing from the corpus sample");
  };

  // 1. BOTH successors of the splitting lane survived. Before #536 this was 1.
  const roadmaker::Lane& splitting = lane_of(0, -1);
  ASSERT_EQ(splitting.successors.size(), 2U) << "the split was halved on read";
  EXPECT_EQ(splitting.successors[0].id, -1);
  EXPECT_EQ(splitting.successors[1].id, -2);
  // The compatibility accessor still answers what the scalar field used to.
  EXPECT_EQ(splitting.first_successor(), std::optional<int>{-1});

  // 2. @layer travels with each entry, and an absent one stays absent.
  const roadmaker::Lane& layered = lane_of(0, -3);
  ASSERT_EQ(layered.successors.size(), 2U);
  EXPECT_EQ(layered.successors[0].layer, "temporary");
  EXPECT_EQ(layered.successors[1].layer, "permanent");
  EXPECT_TRUE(splitting.successors[0].layer.empty()) << "an absent @layer was materialised";

  // 3. The bytes. Asserting on the model alone would pass on a writer that
  // still emitted one link per side.
  std::ifstream file(sample);
  ASSERT_TRUE(file.is_open());
  const std::string source((std::istreambuf_iterator<char>(file)),
                           std::istreambuf_iterator<char>());
  const auto written = roadmaker::write_xodr(loaded->network, "lane_link_multi");
  ASSERT_TRUE(written.has_value());

  const auto count = [](const std::string& text, std::string_view needle) {
    std::size_t n = 0;
    for (std::size_t at = text.find(needle); at != std::string::npos;
         at = text.find(needle, at + 1)) {
      ++n;
    }
    return n;
  };
  // `<successor id=` rather than `<successor`: the seed's own comment block
  // names the elements in prose, and a bare tag needle counts those too.
  EXPECT_EQ(count(*written, "<successor id="), count(source, "<successor id="));
  EXPECT_EQ(count(*written, "<predecessor id="), count(source, "<predecessor id="));
  EXPECT_EQ(count(source, "<successor id="), 4U) << "the seed must still carry the split";
  EXPECT_NE(written->find(R"(layer="temporary")"), std::string::npos);
  EXPECT_NE(written->find(R"(layer="permanent")"), std::string::npos);

  // 4. Fixed point.
  const auto reparsed = roadmaker::parse_xodr(*written, "lane_link_multi");
  ASSERT_TRUE(reparsed.has_value());
  const auto again = roadmaker::write_xodr(reparsed->network, "lane_link_multi");
  ASSERT_TRUE(again.has_value());
  EXPECT_EQ(*written, *again);
  EXPECT_EQ(roadmaker::count_errors(loaded->diagnostics), 0U);
}

TEST(RoundTrip, AnAuthoredLaneLinkIsASingleEntry) {
  // RoadMaker's own commands link one lane into one lane, so a multi-link list
  // only ever arrives from a file. This keeps every authored fixture's bytes.
  RoadNetwork network;
  const std::array<Waypoint, 2> waypoints{Waypoint{.x = 0.0, .y = 0.0},
                                          Waypoint{.x = 120.0, .y = 0.0}};
  const auto road_id = roadmaker::author_clothoid_road(
      network, waypoints, LaneProfile::two_lane_default(), "Split", "1");
  ASSERT_TRUE(road_id.has_value());
  auto split = roadmaker::edit::split_lane_section(network, *road_id, 60.0);
  ASSERT_NE(split, nullptr);
  ASSERT_TRUE(split->apply(network).has_value());

  const roadmaker::Road& road = *network.road(*road_id);
  ASSERT_EQ(road.sections.size(), 2U);
  for (const roadmaker::LaneId id : network.lane_section(road.sections.front())->lanes) {
    const roadmaker::Lane& lane = *network.lane(id);
    if (lane.odr_id != 0) {
      EXPECT_EQ(lane.successors.size(), 1U) << "lane " << lane.odr_id;
      EXPECT_TRUE(lane.successors[0].layer.empty()) << "an authored link invented a @layer";
    }
  }
}

// --- lane <border> (#538) ----------------------------------------------------
//
// The ALTERNATIVE encoding of lane geometry (§11.7.2): a border gives the
// lane's outer t-limit, not its width. It used to be warned about once and
// dropped, and the lane preserved tier excluded it too — so a border-authored
// lane came back with NO widths at all, meshed at zero width, and was written
// back width-less. Geometry loss, not markup loss.

namespace {

const roadmaker::Lane&
lane_by_odr_id(const RoadNetwork& network, const char* road_odr_id, int lane_odr_id) {
  const roadmaker::Road& road = *network.road(network.find_road(road_odr_id));
  const roadmaker::LaneSection& section = *network.lane_section(road.sections.front());
  for (const roadmaker::LaneId lane_id : section.lanes) {
    const roadmaker::Lane& lane = *network.lane(lane_id);
    if (lane.odr_id == lane_odr_id) {
      return lane;
    }
  }
  throw std::runtime_error("lane not found in the corpus sample");
}

} // namespace

TEST(RoundTrip, BorderAuthoredLanesKeepTheirGeometry) {
  const auto loaded =
      roadmaker::load_xodr(std::filesystem::path(RM_FUZZ_CORPUS_DIR) / "lane_border.xodr");
  ASSERT_TRUE(loaded.has_value()) << (loaded ? "" : loaded.error().message);
  const RoadNetwork& network = loaded->network;

  // 1. Every bordered lane has geometry. Before the fix all five were empty.
  for (const int odr_id : {2, 1, -1, -2, -3}) {
    EXPECT_FALSE(lane_by_odr_id(network, "1", odr_id).widths.empty())
        << "lane " << odr_id << " lost its geometry";
  }

  // 2. The widths are the ones the borders imply, not merely non-empty.
  const roadmaker::Lane& inner_left = lane_by_odr_id(network, "1", 1);
  EXPECT_DOUBLE_EQ(roadmaker::eval_profile(inner_left.widths, 0.0), 3.5);
  const roadmaker::Lane& outer_left = lane_by_odr_id(network, "1", 2);
  EXPECT_DOUBLE_EQ(roadmaker::eval_profile(outer_left.widths, 0.0), 3.5)
      << "the outer lane's width is its border MINUS the inner lane's, not its border";

  // 3. ★ The lane whose own border never breaks, but whose inner neighbour's
  // does. It must narrow past s=50 as lane -2 widens beneath it; a conversion
  // that walked only each lane's own records would hold it at 2.0 forever.
  const roadmaker::Lane& shoulder = lane_by_odr_id(network, "1", -3);
  EXPECT_DOUBLE_EQ(roadmaker::eval_profile(shoulder.widths, 0.0), 2.0);
  EXPECT_DOUBLE_EQ(roadmaker::eval_profile(shoulder.widths, 50.0), 2.0);
  EXPECT_NEAR(roadmaker::eval_profile(shoulder.widths, 100.0), 1.0, 1e-9)
      << "the shoulder did not narrow as the lane inside it widened";
  const roadmaker::Lane& widening = lane_by_odr_id(network, "1", -2);
  EXPECT_NEAR(roadmaker::eval_profile(widening.widths, 100.0), 4.5, 1e-9);

  // 4. §11.7.2: where a lane declares both encodings, <width> wins.
  const roadmaker::Lane& both = lane_by_odr_id(network, "2", -1);
  EXPECT_DOUBLE_EQ(roadmaker::eval_profile(both.widths, 0.0), 3.0)
      << "the absurd border was used instead of the width";
}

TEST(RoundTrip, BorderAuthoredLanesSurviveWriteParseWrite) {
  const auto loaded =
      roadmaker::load_xodr(std::filesystem::path(RM_FUZZ_CORPUS_DIR) / "lane_border.xodr");
  ASSERT_TRUE(loaded.has_value());

  const auto written = roadmaker::write_xodr(loaded->network, "lane_border");
  ASSERT_TRUE(written.has_value());

  // The file changes ENCODING — borders become widths, which is what §11.7.2
  // sanctions and what the conversion is for — so the assertion is that the
  // geometry survives and that no <border> is re-emitted beside the <width>
  // it became (the two are mutually exclusive).
  EXPECT_NE(written->find("<width"), std::string::npos);
  EXPECT_EQ(written->find("<border"), std::string::npos)
      << "a border was re-emitted alongside the width it converted to";

  const auto reparsed = roadmaker::parse_xodr(*written, "lane_border");
  ASSERT_TRUE(reparsed.has_value());
  for (const int odr_id : {2, 1, -1, -2, -3}) {
    const roadmaker::Lane& before = lane_by_odr_id(loaded->network, "1", odr_id);
    const roadmaker::Lane& after = lane_by_odr_id(reparsed->network, "1", odr_id);
    for (double s = 0.0; s <= 100.0; s += 5.0) {
      EXPECT_NEAR(
          roadmaker::eval_profile(after.widths, s), roadmaker::eval_profile(before.widths, s), 1e-9)
          << "lane " << odr_id << " changed width at s = " << s;
    }
  }

  // And the second pass is a fixed point: once converted, nothing drifts.
  const auto again = roadmaker::write_xodr(reparsed->network, "lane_border");
  ASSERT_TRUE(again.has_value());
  EXPECT_EQ(*written, *again);
}

TEST(RoundTrip, BorderDiagnosticsSayWhatHappened) {
  const auto loaded =
      roadmaker::load_xodr(std::filesystem::path(RM_FUZZ_CORPUS_DIR) / "lane_border.xodr");
  ASSERT_TRUE(loaded.has_value());

  const auto mentions = [&](std::string_view needle) {
    return std::ranges::any_of(loaded->diagnostics, [&](const roadmaker::Diagnostic& d) {
      return d.message.find(needle) != std::string::npos;
    });
  };
  // The lane carrying both encodings is told its border was ignored...
  EXPECT_TRUE(mentions("declares both <width> and <border>"));
  // ...and the converted lanes are NOT reported as missing geometry, which is
  // the warning they used to draw on the way to being emptied.
  EXPECT_FALSE(mentions("non-center lane without <width> records"));
  EXPECT_EQ(roadmaker::count_errors(loaded->diagnostics), 0U) << "warnings, never errors";
}

// --- road @rule, LHT/RHT (#535) ---------------------------------------------
//
// Read nowhere before #535, so a left-hand-traffic network became right-hand
// traffic on its first save. Like #476 that is a REWRITE rather than a drop:
// the output makes an affirmative wrong claim about which way traffic runs,
// and §10.2 makes the absent attribute mean RHT, so nothing looks amiss.

namespace {

/// The `<road ...>` opening tags of an .xodr, trimmed — the granularity the
/// defect lives at, since @rule is a road attribute and the geometry below it
/// is unaffected either way.
std::vector<std::string> road_lines(const std::string& xml) {
  std::vector<std::string> out;
  std::istringstream stream(xml);
  std::string line;
  while (std::getline(stream, line)) {
    const std::size_t first = line.find_first_not_of(" \t");
    if (first == std::string::npos) {
      continue;
    }
    const std::string trimmed = line.substr(first);
    if (trimmed.starts_with("<road ")) {
      out.push_back(trimmed);
    }
  }
  return out;
}

} // namespace

TEST(RoundTrip, RoadRuleSurvivesWriteParseWrite) {
  const std::filesystem::path sample =
      std::filesystem::path(RM_FUZZ_CORPUS_DIR) / "left_hand_traffic.xodr";
  auto loaded = roadmaker::load_xodr(sample);
  ASSERT_TRUE(loaded.has_value()) << (loaded ? "" : loaded.error().message);

  // 1. The MODEL carries the rule, so behaviour downstream can consult it.
  const RoadNetwork& network = loaded->network;
  EXPECT_EQ(network.road(network.find_road("1"))->rule, roadmaker::TrafficRule::LeftHandTraffic);
  EXPECT_EQ(network.road(network.find_road("2"))->rule, roadmaker::TrafficRule::RightHandTraffic);
  // A spelling outside e_trafficRule resolves to the spec default, never to a
  // third state, and the reader says so (asserted in RoadRuleUnknownStillWarns).
  EXPECT_EQ(network.road(network.find_road("3"))->rule, roadmaker::TrafficRule::RightHandTraffic);

  // 2. The BYTES survive. Asserting on the parsed model alone would pass on a
  // writer that drops @rule entirely, because road 2's and road 3's values both
  // resolve to the same enum the default gives.
  std::ifstream file(sample);
  ASSERT_TRUE(file.is_open());
  const std::string source((std::istreambuf_iterator<char>(file)),
                           std::istreambuf_iterator<char>());
  const auto written = roadmaker::write_xodr(loaded->network, "left_hand_traffic");
  ASSERT_TRUE(written.has_value());

  const std::vector<std::string> before = road_lines(source);
  const std::vector<std::string> after = road_lines(*written);
  ASSERT_EQ(before.size(), 3U) << "the sample must still carry all three cases";
  ASSERT_EQ(after.size(), before.size());
  for (std::size_t i = 0; i < before.size(); ++i) {
    EXPECT_EQ(before[i], after[i]) << "road line " << i << " changed on write";
  }
  EXPECT_NE(written->find("rule=\"LHT\""), std::string::npos);
  EXPECT_NE(written->find("rule=\"RHT\""), std::string::npos);
  EXPECT_NE(written->find("rule=\"left\""), std::string::npos);

  // 3. Fixed point: write -> parse -> write reproduces the same bytes, and the
  // rule is still LHT after the second trip rather than decaying to the default.
  const auto reparsed = roadmaker::parse_xodr(*written, "left_hand_traffic");
  ASSERT_TRUE(reparsed.has_value());
  EXPECT_EQ(reparsed->network.road(reparsed->network.find_road("1"))->rule,
            roadmaker::TrafficRule::LeftHandTraffic);
  const auto again = roadmaker::write_xodr(reparsed->network, "left_hand_traffic");
  ASSERT_TRUE(again.has_value());
  EXPECT_EQ(*written, *again);
}

TEST(RoundTrip, RoadRuleUnknownStillWarns) {
  // Preserving the spelling must not silence the parser: a caller reading
  // `rule="left"` gets RHT semantics, and the never-drop contract is
  // diagnose-AND-keep, not keep-quietly.
  const auto loaded =
      roadmaker::load_xodr(std::filesystem::path(RM_FUZZ_CORPUS_DIR) / "left_hand_traffic.xodr");
  ASSERT_TRUE(loaded.has_value());
  EXPECT_TRUE(std::ranges::any_of(loaded->diagnostics, [](const roadmaker::Diagnostic& d) {
    return d.severity == roadmaker::Severity::Warning &&
           d.message.find("unknown road rule 'left'") != std::string::npos;
  }));
  // The two legal spellings must NOT warn, or the diagnostic is noise.
  EXPECT_FALSE(std::ranges::any_of(loaded->diagnostics, [](const roadmaker::Diagnostic& d) {
    return d.message.find("unknown road rule 'LHT'") != std::string::npos ||
           d.message.find("unknown road rule 'RHT'") != std::string::npos;
  }));
  EXPECT_EQ(roadmaker::count_errors(loaded->diagnostics), 0U) << "warnings, never errors";
}

TEST(RoundTrip, AuthoredRoadWritesNoRuleAttribute) {
  // The other half of the byte-stability claim: RHT is what an absent @rule
  // means, so a road RoadMaker authored must not start stamping rule="RHT"
  // onto every file in the suite.
  RoadNetwork network;
  const std::array<Waypoint, 2> waypoints{Waypoint{.x = 0.0, .y = 0.0},
                                          Waypoint{.x = 100.0, .y = 0.0}};
  const auto road_id = roadmaker::author_clothoid_road(
      network, waypoints, LaneProfile::two_lane_default(), "Plain", "1");
  ASSERT_TRUE(road_id.has_value());
  EXPECT_EQ(network.road(*road_id)->rule, roadmaker::TrafficRule::RightHandTraffic);
  EXPECT_TRUE(network.road(*road_id)->rule_str.empty());

  const auto written = roadmaker::write_xodr(network, "plain");
  ASSERT_TRUE(written.has_value());
  EXPECT_EQ(written->find("rule="), std::string::npos);
}

// --- foreign enum spellings (#476) ------------------------------------------
//
// The one place the kernel used to REWRITE foreign data into different
// semantics rather than dropping or preserving it. A rewrite is worse than a
// drop: the output makes an affirmative wrong claim, and nothing warns.

namespace {

/// Every `<lane>` / `<roadMark>` line of an .xodr, trimmed — the granularity the
/// defect lives at, and small enough that a failure names the offending line
/// instead of dumping the file.
std::vector<std::string> lane_lines(const std::string& xml) {
  std::vector<std::string> out;
  std::istringstream stream(xml);
  std::string line;
  while (std::getline(stream, line)) {
    const std::size_t first = line.find_first_not_of(" \t");
    if (first == std::string::npos) {
      continue;
    }
    const std::string trimmed = line.substr(first);
    if (trimmed.starts_with("<lane ") || trimmed.starts_with("<roadMark ")) {
      out.push_back(trimmed);
    }
  }
  return out;
}

} // namespace

TEST(RoundTrip, ForeignEnumSpellingsSurviveWriteUnchanged) {
  const std::filesystem::path sample =
      std::filesystem::path(RM_FUZZ_CORPUS_DIR) / "foreign_enum_spellings.xodr";
  auto loaded = roadmaker::load_xodr(sample);
  ASSERT_TRUE(loaded.has_value()) << (loaded ? "" : loaded.error().message);

  std::ifstream file(sample);
  ASSERT_TRUE(file.is_open());
  const std::string source((std::istreambuf_iterator<char>(file)),
                           std::istreambuf_iterator<char>());

  const auto written = roadmaker::write_xodr(loaded->network, "foreign_enum_spellings");
  ASSERT_TRUE(written.has_value());

  // ★ Line-for-line, not "the enum survived". The enum CANNOT survive — nothing
  // in LaneType spells `onRamp` — so a test asserting on the parsed model would
  // pass on the broken writer. The claim is about the BYTES the writer emits.
  const std::vector<std::string> before = lane_lines(source);
  const std::vector<std::string> after = lane_lines(*written);
  ASSERT_EQ(before.size(), after.size());
  ASSERT_GE(before.size(), 6U) << "the sample must still carry every case";
  for (std::size_t i = 0; i < before.size(); ++i) {
    EXPECT_EQ(before[i], after[i]) << "line " << i << " changed on write";
  }

  // Non-vacuity: the sample really does contain spellings this build does not
  // model, so the comparison above is not trivially satisfied by a file whose
  // every value happens to be in the enums.
  EXPECT_NE(written->find("type=\"onRamp\""), std::string::npos);
  EXPECT_NE(written->find("type=\"slipLane\""), std::string::npos);
  EXPECT_NE(written->find("type=\"curb\""), std::string::npos);
  EXPECT_NE(written->find("color=\"fuchsia\""), std::string::npos);
  EXPECT_NE(written->find("level=\"true\""), std::string::npos);
  EXPECT_NE(written->find("direction=\"backward\""), std::string::npos);
  // §11.8.1 deprecates `sidewalk` in favour of `walking`, and both parse to
  // LaneType::Sidewalk — so this one is a MODELED value that still changed
  // spelling on save, which is why the fix stores the spelling for every lane
  // rather than only for the unmodeled ones.
  EXPECT_NE(written->find("type=\"walking\""), std::string::npos);
  EXPECT_EQ(written->find("type=\"sidewalk\""), std::string::npos);
}

TEST(RoundTrip, ForeignEnumSpellingsStillWarn) {
  // Preserving the spelling must not silence the parser: the user still has to
  // learn that this build renders a `curb` mark as a generic line, and the
  // never-drop contract is diagnose-AND-keep, not keep-quietly.
  const auto loaded = roadmaker::load_xodr(std::filesystem::path(RM_FUZZ_CORPUS_DIR) /
                                           "foreign_enum_spellings.xodr");
  ASSERT_TRUE(loaded.has_value());

  const auto mentions = [&](std::string_view needle) {
    return std::ranges::any_of(loaded->diagnostics, [&](const roadmaker::Diagnostic& d) {
      return d.severity == roadmaker::Severity::Warning &&
             d.message.find(needle) != std::string::npos;
    });
  };
  EXPECT_TRUE(mentions("onRamp"));
  EXPECT_TRUE(mentions("slipLane"));
  EXPECT_TRUE(mentions("curb"));
  EXPECT_TRUE(mentions("botts dots"));
  EXPECT_TRUE(mentions("fuchsia"));
  EXPECT_TRUE(mentions("backward"));
  EXPECT_EQ(roadmaker::count_errors(loaded->diagnostics), 0U) << "warnings, never errors";
}

TEST(RoundTrip, RetypingALaneDropsTheSpellingItNoLongerHas) {
  // ★ The trap this fix introduces if the setters are not updated. Keeping the
  // source spelling on a lane whose type the user CHANGED would re-export
  // `onRamp` for a lane that is now `driving` — trading one corruption for a
  // worse one, because this time RoadMaker itself authored the lie.
  auto loaded = roadmaker::load_xodr(std::filesystem::path(RM_FUZZ_CORPUS_DIR) /
                                     "foreign_enum_spellings.xodr");
  ASSERT_TRUE(loaded.has_value());
  RoadNetwork& network = loaded->network;

  const roadmaker::Road& road = *network.road(network.find_road("1"));
  const roadmaker::LaneSection& section = *network.lane_section(road.sections.front());
  roadmaker::LaneId ramp;
  for (const roadmaker::LaneId lane_id : section.lanes) {
    if (network.lane(lane_id)->odr_id == 1) {
      ramp = lane_id;
    }
  }
  ASSERT_TRUE(ramp.is_valid());
  ASSERT_EQ(network.lane(ramp)->type_str, "onRamp");

  auto retype = roadmaker::edit::set_lane_type(network, ramp, roadmaker::LaneType::Driving);
  ASSERT_TRUE(retype->apply(network).has_value());
  EXPECT_TRUE(network.lane(ramp)->type_str.empty());

  const auto written = roadmaker::write_xodr(network, "retyped");
  ASSERT_TRUE(written.has_value());
  EXPECT_EQ(written->find("type=\"onRamp\""), std::string::npos)
      << "the retyped lane kept the spelling of the type it no longer has";
  EXPECT_NE(written->find("type=\"driving\""), std::string::npos);

  // Undo restores the spelling with the type — the command captured both.
  ASSERT_TRUE(retype->revert(network).has_value());
  EXPECT_EQ(network.lane(ramp)->type_str, "onRamp");
}

TEST(RoundTrip, ForeignRoadsLoadWithoutAuthoringWaypoints) {
  auto loaded = roadmaker::load_xodr(std::filesystem::path(RM_SAMPLES_DIR) / "t_junction.xodr");
  ASSERT_TRUE(loaded.has_value());
  loaded->network.for_each_road([](RoadId, const roadmaker::Road& road) {
    EXPECT_FALSE(road.authoring_waypoints.has_value());
  });
}

TEST(XodrWriter, RefusesInvalidNetworks) {
  RoadNetwork network;
  network.create_road("empty", "1"); // no geometry, no sections
  const auto result = roadmaker::write_xodr(network);
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code, roadmaker::ErrorCode::InvalidArgument);
}

TEST(XodrWriter, RefusesDiscontinuousGeometry) {
  RoadNetwork network;
  const RoadId road_id = network.create_road("broken", "1");
  roadmaker::Road& road = *network.road(road_id);
  road.plan_view.append(
      {.x = 0.0, .y = 0.0, .hdg = 0.0, .length = 10.0, .shape = roadmaker::LineGeom{}});
  road.plan_view.append(
      {.x = 999.0, .y = 0.0, .hdg = 0.0, .length = 10.0, .shape = roadmaker::LineGeom{}});
  network.add_lane_section(road_id, 0.0);

  const auto result = roadmaker::write_xodr(network);
  ASSERT_FALSE(result.has_value());
  EXPECT_NE(result.error().message.find("discontinuity"), std::string::npos);
}

TEST(XodrWriter, RefusesDanglingLaneLinks) {
  RoadNetwork network;
  const RoadId road_id = network.create_road("links", "1");
  roadmaker::Road& road = *network.road(road_id);
  road.plan_view.append(
      {.x = 0.0, .y = 0.0, .hdg = 0.0, .length = 50.0, .shape = roadmaker::LineGeom{}});
  const auto s0 = network.add_lane_section(road_id, 0.0);
  const auto s1 = network.add_lane_section(road_id, 25.0);
  const auto lane = network.add_lane(s0, -1, roadmaker::LaneType::Driving);
  network.add_lane(s1, -1, roadmaker::LaneType::Driving);
  network.lane(lane)->set_successor(-5); // does not exist in next section

  const auto result = roadmaker::write_xodr(network);
  ASSERT_FALSE(result.has_value());
  EXPECT_NE(result.error().message.find("successor"), std::string::npos);
}
