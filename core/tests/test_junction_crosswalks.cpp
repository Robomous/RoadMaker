// Junction-arm crosswalk authoring (GS-1 WS-B): edit::junction_crosswalks derives
// one zebra crosswalk Object per arm road, spanning its driving lanes.

#include "roadmaker/edit/connection.hpp"
#include "roadmaker/edit/markings.hpp"
#include "roadmaker/edit/operations.hpp"
#include "roadmaker/mesh/mesh_builder.hpp"
#include "roadmaker/road/authoring.hpp"
#include "roadmaker/road/junction.hpp"
#include "roadmaker/road/network.hpp"
#include "roadmaker/xodr/writer.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <memory>
#include <numbers>
#include <ranges>
#include <set>
#include <string>
#include <utility>
#include <vector>

using roadmaker::ContactPoint;
using roadmaker::JunctionId;
using roadmaker::LaneProfile;
using roadmaker::RoadEnd;
using roadmaker::RoadId;
using roadmaker::RoadNetwork;
using roadmaker::Waypoint;

namespace {

RoadId author(RoadNetwork& network, std::vector<Waypoint> waypoints, const char* odr_id) {
  auto road = roadmaker::author_clothoid_road(
      network, waypoints, LaneProfile::two_lane_default(), "", odr_id);
  if (!road.has_value()) {
    throw std::runtime_error("author: " + road.error().message);
  }
  return *road;
}

/// Three two-lane arms pointing at the origin (all End contacts) → a generated
/// junction, mirroring test_connection.cpp's canonical layout.
struct ArmedJunction {
  RoadNetwork network;
  JunctionId junction;

  ArmedJunction() {
    const RoadId a = author(network, {Waypoint{-40.0, 0.0}, Waypoint{-6.0, 0.0}}, "1");
    const RoadId b = author(network, {Waypoint{40.0, 0.0}, Waypoint{6.0, 0.0}}, "2");
    const RoadId c = author(network, {Waypoint{0.0, -40.0}, Waypoint{0.0, -6.0}}, "3");
    const std::array<RoadEnd, 3> ends{RoadEnd{a, ContactPoint::End},
                                      RoadEnd{b, ContactPoint::End},
                                      RoadEnd{c, ContactPoint::End}};
    auto command = roadmaker::edit::create_junction(network, ends);
    if (!command->apply(network).has_value()) {
      throw std::runtime_error("junction apply failed");
    }
    network.for_each_junction([&](JunctionId id, const roadmaker::Junction&) { junction = id; });
  }
};

} // namespace

TEST(JunctionCrosswalks, OnePerArmSpanningDrivingLanes) {
  ArmedJunction fx;
  const auto crosswalks = roadmaker::edit::junction_crosswalks(fx.network, fx.junction);
  ASSERT_EQ(crosswalks.size(), 3U); // one per distinct arm road

  std::set<std::string> ids;
  std::set<std::uint32_t> arms;
  for (const auto& [road, cw] : crosswalks) {
    EXPECT_EQ(cw.type, roadmaker::ObjectType::Crosswalk);
    EXPECT_EQ(cw.subtype, "zebra");
    EXPECT_NEAR(cw.hdg, std::numbers::pi / 2.0, 1e-9); // across the road
    ASSERT_TRUE(cw.length.has_value());
    EXPECT_GT(*cw.length, 3.0); // spans two 3.5 m driving lanes
    ASSERT_TRUE(cw.width.has_value());
    EXPECT_DOUBLE_EQ(*cw.width, 3.0); // default depth
    const auto* road_ptr = fx.network.road(road);
    ASSERT_NE(road_ptr, nullptr);
    EXPECT_GE(cw.s, 0.0);
    EXPECT_LE(cw.s, road_ptr->plan_view.length());
    ids.insert(cw.odr_id);
    arms.insert(road.index);
  }
  EXPECT_EQ(ids.size(), 3U);  // unique odr ids across the batch
  EXPECT_EQ(arms.size(), 3U); // one per distinct arm
}

TEST(JunctionCrosswalks, AddedObjectsMeshAsZebra) {
  ArmedJunction fx;
  auto crosswalks = roadmaker::edit::junction_crosswalks(fx.network, fx.junction);
  ASSERT_FALSE(crosswalks.empty());
  for (auto& [road, cw] : crosswalks) {
    auto command = roadmaker::edit::add_object(fx.network, road, cw);
    ASSERT_TRUE(command->apply(fx.network).has_value());
  }

  const roadmaker::NetworkMesh mesh = roadmaker::build_network_mesh(fx.network);
  int zebra_meshes = 0;
  for (const auto& road : mesh.roads) {
    for (const auto& marking : road.markings) {
      if (marking.name.find("crosswalk") != std::string::npos && !marking.indices.empty()) {
        ++zebra_meshes;
      }
    }
  }
  EXPECT_EQ(zebra_meshes, 3); // one zebra marking per arm
}

TEST(JunctionCrosswalks, StaleJunctionYieldsNone) {
  ArmedJunction fx;
  fx.network.erase_junction(fx.junction);
  EXPECT_TRUE(roadmaker::edit::junction_crosswalks(fx.network, fx.junction).empty());
}

TEST(JunctionStopLines, OnePerArmAcrossApproachLanesOnly) {
  ArmedJunction fx;
  const auto lines = roadmaker::edit::junction_stop_lines(fx.network, fx.junction);
  ASSERT_EQ(lines.size(), 3U); // one per arm

  const auto crosswalks = roadmaker::edit::junction_crosswalks(fx.network, fx.junction);
  ASSERT_EQ(crosswalks.size(), 3U);

  std::set<std::string> ids;
  for (const auto& [road, line] : lines) {
    EXPECT_EQ(line.type_str, "roadMark");
    EXPECT_EQ(line.subtype, "signalLines");
    ASSERT_TRUE(line.length.has_value());
    EXPECT_DOUBLE_EQ(*line.length, 0.3); // thin along the road
    ASSERT_TRUE(line.width.has_value());
    EXPECT_GT(*line.width, 2.0); // spans the approach lane(s)
    // Approach lanes are one travel direction, so the line is narrower than a
    // crosswalk that spans the full driving width (~7 m).
    EXPECT_LT(*line.width, *crosswalks.front().second.length);
    const auto* road_ptr = fx.network.road(road);
    ASSERT_NE(road_ptr, nullptr);
    EXPECT_GE(line.s, 0.0);
    EXPECT_LE(line.s, road_ptr->plan_view.length());
    ids.insert(line.odr_id);
  }
  EXPECT_EQ(ids.size(), 3U); // unique ids
}

TEST(JunctionStopLines, AddedObjectsMeshAsStopQuads) {
  ArmedJunction fx;
  auto lines = roadmaker::edit::junction_stop_lines(fx.network, fx.junction);
  ASSERT_FALSE(lines.empty());
  for (auto& [road, line] : lines) {
    ASSERT_TRUE(roadmaker::edit::add_object(fx.network, road, line)->apply(fx.network).has_value());
  }
  const roadmaker::NetworkMesh mesh = roadmaker::build_network_mesh(fx.network);
  int stop_meshes = 0;
  for (const auto& road : mesh.roads) {
    for (const auto& marking : road.markings) {
      if (marking.name.find("stop line") != std::string::npos && !marking.indices.empty()) {
        ++stop_meshes;
      }
    }
  }
  EXPECT_EQ(stop_meshes, 3);
}

TEST(JunctionLaneArrows, OnePerApproachLanePointingIntoTheJunction) {
  ArmedJunction fx;
  const auto arrows = roadmaker::edit::junction_lane_arrows(fx.network, fx.junction);
  ASSERT_EQ(arrows.size(), 3U); // one approach lane per two-way arm x 3 arms

  std::set<std::string> ids;
  for (const auto& [road, arrow] : arrows) {
    EXPECT_EQ(arrow.type_str, "roadMark");
    EXPECT_EQ(arrow.subtype, "arrowStraight");
    ASSERT_TRUE(arrow.length.has_value());
    EXPECT_DOUBLE_EQ(*arrow.length, 4.0);
    ASSERT_TRUE(arrow.width.has_value());
    EXPECT_GT(*arrow.width, 0.0);
    EXPECT_DOUBLE_EQ(arrow.hdg, 0.0); // End-facing arms point +s into the junction
    ids.insert(arrow.odr_id);
  }
  EXPECT_EQ(ids.size(), 3U);
}

TEST(JunctionLaneArrows, GlyphChooserPicksTurnVariantsPerLane) {
  ArmedJunction fx;
  const RoadId first_arm = fx.network.find_road("1");
  ASSERT_TRUE(first_arm.is_valid());

  // Turn intent is the caller's: arm 1 turns left, every other arm keeps the
  // straight default.
  roadmaker::edit::LaneArrowParams params;
  params.glyph = [first_arm](RoadId arm, const roadmaker::edit::ContactLane& lane) {
    EXPECT_NE(lane.width, 0.0); // the chooser sees the lane it is deciding for
    return arm == first_arm ? "arrowLeft" : "";
  };

  const auto arrows = roadmaker::edit::junction_lane_arrows(fx.network, fx.junction, params);
  ASSERT_EQ(arrows.size(), 3U);
  int left = 0;
  int straight = 0;
  for (const auto& [road, arrow] : arrows) {
    if (arrow.subtype == "arrowLeft") {
      EXPECT_EQ(road, first_arm);
      ++left;
    } else {
      // An empty choice is a decline, not an invalid object.
      EXPECT_EQ(arrow.subtype, "arrowStraight");
      ++straight;
    }
  }
  EXPECT_EQ(left, 1);
  EXPECT_EQ(straight, 2);
}

TEST(JunctionLaneArrows, AddedObjectsMeshAsArrowGlyphs) {
  ArmedJunction fx;
  auto arrows = roadmaker::edit::junction_lane_arrows(fx.network, fx.junction);
  ASSERT_FALSE(arrows.empty());
  for (auto& [road, arrow] : arrows) {
    ASSERT_TRUE(
        roadmaker::edit::add_object(fx.network, road, arrow)->apply(fx.network).has_value());
  }
  const roadmaker::NetworkMesh mesh = roadmaker::build_network_mesh(fx.network);
  int arrow_meshes = 0;
  for (const auto& road : mesh.roads) {
    for (const auto& marking : road.markings) {
      if (marking.name.find("arrow") != std::string::npos && !marking.indices.empty()) {
        ++arrow_meshes;
      }
    }
  }
  EXPECT_EQ(arrow_meshes, 3);
}

// --- centre lines (#193 gap 7) ----------------------------------------------

TEST(JunctionCenterMarks, DualYellowOnLaneZeroOfEveryArm) {
  ArmedJunction fx;
  const auto marks = roadmaker::edit::junction_center_marks(fx.network, fx.junction);
  ASSERT_EQ(marks.size(), 3U); // one lane 0 per arm, one section each

  for (const auto& [lane, mark] : marks) {
    EXPECT_EQ(fx.network.lane(lane)->odr_id, 0);
    EXPECT_EQ(mark.type, roadmaker::RoadMarkType::SolidSolid);
    EXPECT_EQ(mark.color, roadmaker::RoadMarkColor::Yellow);
    EXPECT_DOUBLE_EQ(mark.s_offset, 0.0);
    // Left empty on purpose: the writer keeps the compact single-@width form
    // and the mesh synthesizes the two stripes at +/-width.
    EXPECT_TRUE(mark.lines.empty());
  }
}

TEST(JunctionCenterMarks, ParamsChooseTypeAndColor) {
  ArmedJunction fx;
  const roadmaker::edit::CenterMarkParams params{.type = roadmaker::RoadMarkType::BrokenSolid,
                                                 .color = roadmaker::RoadMarkColor::White,
                                                 .width = 0.2};
  const auto marks = roadmaker::edit::junction_center_marks(fx.network, fx.junction, params);
  ASSERT_FALSE(marks.empty());
  for (const auto& [lane, mark] : marks) {
    EXPECT_EQ(mark.type, roadmaker::RoadMarkType::BrokenSolid);
    EXPECT_EQ(mark.color, roadmaker::RoadMarkColor::White);
    EXPECT_DOUBLE_EQ(mark.width, 0.2);
  }
}

TEST(JunctionCenterMarks, StaleJunctionYieldsNone) {
  ArmedJunction fx;
  fx.network.erase_junction(fx.junction);
  EXPECT_TRUE(roadmaker::edit::junction_center_marks(fx.network, fx.junction).empty());
}

TEST(JunctionCenterMarks, PushedThroughSetRoadMarkTheyReachTheFileAndUndoCleanly) {
  // Dual-strip *rendering* is resolve_stripes' contract, covered by
  // RoadMarks.SolidSolidRendersTwoStrips. What matters here is that the op's
  // marks survive set_road_mark and land in the file.
  ArmedJunction fx;
  const auto marks = roadmaker::edit::junction_center_marks(fx.network, fx.junction);
  ASSERT_FALSE(marks.empty());

  const auto before = roadmaker::write_xodr(fx.network, "center-marks");
  ASSERT_TRUE(before.has_value());
  EXPECT_EQ(before->find("color=\"yellow\""), std::string::npos); // profile paints broken white

  std::vector<std::unique_ptr<roadmaker::edit::Command>> pushed;
  for (const auto& [lane, mark] : marks) {
    auto command = roadmaker::edit::set_road_mark(fx.network, lane, mark);
    ASSERT_NE(command, nullptr);
    ASSERT_TRUE(command->apply(fx.network).has_value());
    pushed.push_back(std::move(command));
  }

  const auto written = roadmaker::write_xodr(fx.network, "center-marks");
  ASSERT_TRUE(written.has_value());
  EXPECT_NE(written->find("type=\"solid solid\""), std::string::npos);
  EXPECT_NE(written->find("color=\"yellow\""), std::string::npos);
  // Bare mark: the compact single-@width form, no <type>/<line> block.
  EXPECT_EQ(written->find("<type"), std::string::npos);

  // Undo is byte-identical, reverting in reverse push order.
  for (auto& command : std::ranges::reverse_view(pushed)) {
    ASSERT_TRUE(command->revert(fx.network).has_value());
  }
  const auto reverted = roadmaker::write_xodr(fx.network, "center-marks");
  ASSERT_TRUE(reverted.has_value());
  EXPECT_EQ(*reverted, *before);
}
