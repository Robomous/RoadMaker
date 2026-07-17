// Round-trip invariants are first-class tests: author → write → re-parse →
// compare within rm::tol (position 1e-4 m, heading 1e-6 rad).

#include "roadmaker/edit/operations.hpp"
#include "roadmaker/road/authoring.hpp"
#include "roadmaker/xodr/reader.hpp"
#include "roadmaker/xodr/writer.hpp"

#include <gtest/gtest.h>

#include <array>
#include <filesystem>
#include <string>

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
  network.lane(lane)->successor = -5; // does not exist in next section

  const auto result = roadmaker::write_xodr(network);
  ASSERT_FALSE(result.has_value());
  EXPECT_NE(result.error().message.find("successor"), std::string::npos);
}
