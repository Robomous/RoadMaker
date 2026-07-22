// Copyright 2026 Robomous
// SPDX-License-Identifier: Apache-2.0

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

TEST(JunctionCrosswalks, ParametricAssetAuthorsOutlineMarkingsAndUserData) {
  ArmedJunction fx;
  roadmaker::edit::CrosswalkParams params;
  params.dash_length_m = 0.4;
  params.dash_gap_m = 0.6;
  params.border_width_m = 0.2;
  params.material = "material.paint_white";
  params.color = "white";
  params.asset = "crosswalk.zebra";
  params.category = "crosswalk";
  const auto crosswalks = roadmaker::edit::junction_crosswalks(fx.network, fx.junction, params);
  ASSERT_FALSE(crosswalks.empty());
  const roadmaker::Object& cw = crosswalks.front().second;

  // Closed cornerRoad outline, ids 0..3 CCW, painted.
  ASSERT_EQ(cw.outlines.size(), 1U);
  const roadmaker::ObjectOutline& outline = cw.outlines.front();
  EXPECT_TRUE(outline.road_coords);
  ASSERT_TRUE(outline.closed.has_value());
  EXPECT_TRUE(*outline.closed);
  ASSERT_TRUE(outline.fill_type.has_value());
  EXPECT_EQ(*outline.fill_type, "paint");
  ASSERT_EQ(outline.corners.size(), 4U);
  for (std::size_t i = 0; i < 4; ++i) {
    ASSERT_TRUE(outline.corners[i].id.has_value());
    EXPECT_EQ(*outline.corners[i].id, static_cast<int>(i));
  }
  // One dashed stripes marking (full ring) + two solid border markings.
  ASSERT_EQ(outline.markings.size(), 3U);
  EXPECT_EQ(outline.markings[0].corner_refs, (std::vector<int>{0, 1, 2, 3}));
  EXPECT_DOUBLE_EQ(outline.markings[0].line_length, 0.4);
  EXPECT_DOUBLE_EQ(outline.markings[0].space_length, 0.6);
  for (std::size_t i = 1; i < 3; ++i) {
    ASSERT_TRUE(outline.markings[i].width.has_value());
    EXPECT_DOUBLE_EQ(*outline.markings[i].width, 0.2);       // border width
    EXPECT_DOUBLE_EQ(outline.markings[i].space_length, 0.0); // solid
  }
  // rm:crosswalk userData carries the asset params.
  ASSERT_TRUE(cw.crosswalk.has_value());
  EXPECT_EQ(cw.crosswalk->asset, "crosswalk.zebra");
  EXPECT_DOUBLE_EQ(cw.crosswalk->dash_length, 0.4);
  EXPECT_DOUBLE_EQ(cw.crosswalk->dash_gap, 0.6);
  EXPECT_DOUBLE_EQ(cw.crosswalk->border_width, 0.2);
  EXPECT_EQ(cw.crosswalk->material, "material.paint_white");
  EXPECT_FALSE(cw.crosswalk->material_override);
}

TEST(JunctionCrosswalks, SolidAssetEncodesZeroSpaceLength) {
  ArmedJunction fx;
  roadmaker::edit::CrosswalkParams params;
  params.dash_length_m = 0.0; // solid
  params.border_width_m = 0.0;
  const auto crosswalks = roadmaker::edit::junction_crosswalks(fx.network, fx.junction, params);
  ASSERT_FALSE(crosswalks.empty());
  const roadmaker::ObjectOutline& outline = crosswalks.front().second.outlines.front();
  ASSERT_EQ(outline.markings.size(), 1U);                  // no borders
  EXPECT_DOUBLE_EQ(outline.markings[0].space_length, 0.0); // solid: no gap
  EXPECT_GT(outline.markings[0].line_length, 0.0);         // lineLength stays > 0 (t_grZero)
  ASSERT_TRUE(crosswalks.front().second.crosswalk.has_value());
  EXPECT_DOUBLE_EQ(crosswalks.front().second.crosswalk->dash_length, 0.0);
}

TEST(JunctionCrosswalks, AuthoredCrosswalkExportValidatesBothVersions) {
  ArmedJunction fx;
  roadmaker::edit::CrosswalkParams params;
  params.border_width_m = 0.15;
  auto crosswalks = roadmaker::edit::junction_crosswalks(fx.network, fx.junction, params);
  ASSERT_FALSE(crosswalks.empty());
  for (auto& [road, cw] : crosswalks) {
    ASSERT_TRUE(roadmaker::edit::add_object(fx.network, road, cw)->apply(fx.network).has_value());
  }
  for (const auto version : {roadmaker::XodrVersion::v1_9_0, roadmaker::XodrVersion::v1_8_1}) {
    const auto written = roadmaker::write_xodr(fx.network, "cw", {.target_version = version});
    ASSERT_TRUE(written.has_value());
    EXPECT_NE(written->find("rm:crosswalk"), std::string::npos);
    EXPECT_NE(written->find("<marking"), std::string::npos);
  }
}

TEST(JunctionCrosswalks, StaleJunctionYieldsNone) {
  ArmedJunction fx;
  fx.network.erase_junction(fx.junction);
  EXPECT_TRUE(roadmaker::edit::junction_crosswalks(fx.network, fx.junction).empty());
}

// The two JunctionStopLines cases that used to live here are gone with the
// edit::junction_stop_lines generator they exercised (p4-s3, #318). Stop lines
// are now a derived entity, not an object batch: their derivation, authoring,
// persistence and meshing are covered by test_junction_stoplines.cpp,
// test_stopline_operations.cpp and test_stopline_persistence.cpp.

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
