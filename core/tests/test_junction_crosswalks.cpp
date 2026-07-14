// Junction-arm crosswalk authoring (GS-1 WS-B): edit::junction_crosswalks derives
// one zebra crosswalk Object per arm road, spanning its driving lanes.

#include "roadmaker/edit/connection.hpp"
#include "roadmaker/edit/markings.hpp"
#include "roadmaker/edit/operations.hpp"
#include "roadmaker/mesh/mesh_builder.hpp"
#include "roadmaker/road/authoring.hpp"
#include "roadmaker/road/junction.hpp"
#include "roadmaker/road/network.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <numbers>
#include <set>
#include <string>
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
