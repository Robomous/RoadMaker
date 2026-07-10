#include "roadmaker/edit/edit_stack.hpp"
#include "roadmaker/edit/operations.hpp"
#include "roadmaker/road/authoring.hpp"
#include "roadmaker/road/network.hpp"
#include "roadmaker/tol.hpp"

#include <gtest/gtest.h>

#include <array>
#include <memory>
#include <string>
#include <vector>

#include "support/network_compare.hpp"

using roadmaker::ContactPoint;
using roadmaker::JunctionConnection;
using roadmaker::JunctionId;
using roadmaker::LaneId;
using roadmaker::LaneProfile;
using roadmaker::LaneSectionId;
using roadmaker::LaneType;
using roadmaker::RoadEnd;
using roadmaker::RoadId;
using roadmaker::RoadMark;
using roadmaker::RoadMarkType;
using roadmaker::RoadNetwork;
using roadmaker::Waypoint;
using roadmaker::edit::Command;
using roadmaker::test::expect_network_matches;
using roadmaker::test::snapshot_xodr;

namespace {

RoadId author(RoadNetwork& network, std::vector<Waypoint> waypoints, const char* odr_id) {
  auto road = roadmaker::author_clothoid_road(
      network, waypoints, LaneProfile::two_lane_default(), "", odr_id);
  if (!road.has_value()) {
    throw std::runtime_error("author: " + road.error().message);
  }
  return *road;
}

RoadId author_default(RoadNetwork& network, const char* odr_id, double y_offset = 0.0) {
  return author(network,
                {Waypoint{.x = 0.0, .y = y_offset},
                 Waypoint{.x = 60.0, .y = y_offset + 8.0},
                 Waypoint{.x = 120.0, .y = y_offset}},
                odr_id);
}

/// The §8 round-trip oracle for every factory: apply changes the document,
/// revert restores it byte-identically, re-apply reproduces the applied
/// state byte-identically, and a final revert leaves it pristine.
void expect_command_round_trip(RoadNetwork& network, Command& command) {
  const std::string before = snapshot_xodr(network);
  {
    SCOPED_TRACE("first apply");
    ASSERT_TRUE(command.apply(network).has_value());
  }
  const std::string after = snapshot_xodr(network);
  EXPECT_NE(before, after); // a command that changes nothing is a bug
  {
    SCOPED_TRACE("revert");
    ASSERT_TRUE(command.revert(network).has_value());
    expect_network_matches(network, before);
  }
  {
    SCOPED_TRACE("re-apply (idempotence)");
    ASSERT_TRUE(command.apply(network).has_value());
    expect_network_matches(network, after);
  }
  {
    SCOPED_TRACE("final revert");
    ASSERT_TRUE(command.revert(network).has_value());
    expect_network_matches(network, before);
  }
}

/// A failed apply must leave the serialized network untouched.
void expect_command_rejected(RoadNetwork& network, std::unique_ptr<Command> command) {
  const std::string before = snapshot_xodr(network);
  EXPECT_FALSE(command->apply(network).has_value());
  expect_network_matches(network, before);
}

} // namespace

// --- document / lane value edits ---------------------------------------------

TEST(EditOperations, RenameRoadRoundTripsAndRenames) {
  RoadNetwork network;
  const RoadId road = author_default(network, "1");
  auto command = roadmaker::edit::rename_road(network, road, "Main Street");
  expect_command_round_trip(network, *command);

  ASSERT_TRUE(command->apply(network).has_value());
  EXPECT_EQ(network.road(road)->name, "Main Street");
  EXPECT_FALSE(command->dirty().topology);
}

TEST(EditOperations, SetLaneTypeWidthAndMarkRoundTrip) {
  RoadNetwork network;
  const RoadId road = author_default(network, "1");
  const LaneSectionId section = network.road(road)->sections[0];
  const LaneId outer_right = network.lane_section(section)->lanes.back();

  auto set_type = roadmaker::edit::set_lane_type(network, outer_right, LaneType::Parking);
  expect_command_round_trip(network, *set_type);

  auto set_width = roadmaker::edit::set_lane_width(network, outer_right, 2.75);
  expect_command_round_trip(network, *set_width);

  auto set_mark = roadmaker::edit::set_road_mark(
      network, outer_right, RoadMark{.s_offset = 0.0, .type = RoadMarkType::Solid, .width = 0.25});
  expect_command_round_trip(network, *set_mark);

  ASSERT_TRUE(set_width->apply(network).has_value());
  EXPECT_NEAR(network.lane(outer_right)->widths.at(0).a, 2.75, 1e-12);
}

TEST(EditOperations, LaneEditsRejectBadInput) {
  RoadNetwork network;
  const RoadId road = author_default(network, "1");
  const LaneSectionId section = network.road(road)->sections[0];
  LaneId center;
  for (const LaneId lane_id : network.lane_section(section)->lanes) {
    if (network.lane(lane_id)->odr_id == 0) {
      center = lane_id;
    }
  }

  expect_command_rejected(network, roadmaker::edit::set_lane_width(network, center, 3.0));
  const LaneId outer = network.lane_section(section)->lanes.back();
  expect_command_rejected(network, roadmaker::edit::set_lane_width(network, outer, 0.0));
  expect_command_rejected(network,
                          roadmaker::edit::set_lane_type(network, LaneId{}, LaneType::Driving));
}

// --- waypoint edits ------------------------------------------------------------

TEST(EditOperations, MoveWaypointRefitsAndRoundTrips) {
  RoadNetwork network;
  const RoadId road = author_default(network, "1");
  const double old_length = network.road(road)->length;

  auto command = roadmaker::edit::move_waypoint(network, road, 1, Waypoint{.x = 60.0, .y = 30.0});
  expect_command_round_trip(network, *command);

  ASSERT_TRUE(command->apply(network).has_value());
  EXPECT_NE(network.road(road)->length, old_length);
  ASSERT_TRUE(network.road(road)->authoring_waypoints.has_value());
  EXPECT_NEAR(network.road(road)->authoring_waypoints->at(1).y, 30.0, 1e-12);
  // The fitted path still passes through the moved waypoint.
  const auto& records = network.road(road)->plan_view.records();
  EXPECT_NEAR(records.at(1).x, 60.0, 1e-9);
  EXPECT_NEAR(records.at(1).y, 30.0, 1e-9);
}

TEST(EditOperations, InsertAndDeleteWaypointRoundTrip) {
  RoadNetwork network;
  const RoadId road = author_default(network, "1");

  auto insert = roadmaker::edit::insert_waypoint(network, road, 1, Waypoint{.x = 30.0, .y = 20.0});
  expect_command_round_trip(network, *insert);

  auto erase = roadmaker::edit::delete_waypoint(network, road, 1);
  expect_command_round_trip(network, *erase);
}

TEST(EditOperations, WaypointEditsRejectBadInput) {
  RoadNetwork network;
  const RoadId road =
      author(network, {Waypoint{.x = 0.0, .y = 0.0}, Waypoint{.x = 50.0, .y = 0.0}}, "1");

  expect_command_rejected(network, roadmaker::edit::move_waypoint(network, road, 5, {}));
  // 2 waypoints is the minimum — deleting one must fail.
  expect_command_rejected(network, roadmaker::edit::delete_waypoint(network, road, 0));
  // Moving onto the neighbor creates coincident waypoints.
  expect_command_rejected(
      network, roadmaker::edit::move_waypoint(network, road, 0, Waypoint{.x = 50.0, .y = 0.0}));
}

TEST(EditOperations, FirstEditOfForeignRoadDerivesWaypoints) {
  // Build a road without authoring waypoints (as loaded from a foreign file).
  RoadNetwork network;
  const RoadId road = network.create_road("foreign", "1");
  network.road(road)->plan_view.append(
      {.x = 0.0, .y = 0.0, .hdg = 0.0, .length = 40.0, .shape = roadmaker::LineGeom{}});
  network.road(road)->plan_view.append({.x = 40.0,
                                        .y = 0.0,
                                        .hdg = 0.0,
                                        .length = 30.0,
                                        .shape = roadmaker::ArcGeom{.curvature = 0.01}});
  network.road(road)->length = network.road(road)->plan_view.length();
  const LaneSectionId section = network.add_lane_section(road, 0.0);
  network.add_lane(section, 0, LaneType::None);
  network.add_lane(section, -1, LaneType::Driving);
  network.lane(network.lane_section(section)->lanes.back())->widths.push_back({.a = 3.5});

  auto command = roadmaker::edit::move_waypoint(network, road, 2, Waypoint{.x = 75.0, .y = 8.0});
  ASSERT_TRUE(command->apply(network).has_value());
  // Derived waypoints: 2 record starts + endpoint.
  ASSERT_TRUE(network.road(road)->authoring_waypoints.has_value());
  EXPECT_EQ(network.road(road)->authoring_waypoints->size(), 3U);
  ASSERT_TRUE(command->revert(network).has_value());
  EXPECT_FALSE(network.road(road)->authoring_waypoints.has_value());
}

// --- elevation ------------------------------------------------------------------

TEST(EditOperations, SetNodeElevationBuildsPiecewiseLinearProfile) {
  RoadNetwork network;
  const RoadId road = author_default(network, "1");

  auto raise = roadmaker::edit::set_node_elevation(network, road, 1, 4.0);
  expect_command_round_trip(network, *raise);

  ASSERT_TRUE(raise->apply(network).has_value());
  const roadmaker::Road& value = *network.road(road);
  ASSERT_EQ(value.elevation.size(), 2U); // two linear segments through 3 nodes
  const double mid_s = value.plan_view.records().at(1).s;
  EXPECT_NEAR(roadmaker::eval_profile(value.elevation, 0.0), 0.0, 1e-9);
  EXPECT_NEAR(roadmaker::eval_profile(value.elevation, mid_s), 4.0, 1e-9);
  EXPECT_NEAR(roadmaker::eval_profile(value.elevation, value.length), 0.0, 1e-9);

  // Lowering the node back to zero drops the profile entirely.
  auto lower = roadmaker::edit::set_node_elevation(network, road, 1, 0.0);
  ASSERT_TRUE(lower->apply(network).has_value());
  EXPECT_TRUE(network.road(road)->elevation.empty());
}

// --- topology: create / delete / split roads --------------------------------------

TEST(EditOperations, CreateRoadRoundTripsWithStableIds) {
  RoadNetwork network;
  author_default(network, "1");

  auto command =
      roadmaker::edit::create_road({Waypoint{.x = 0.0, .y = 50.0}, Waypoint{.x = 80.0, .y = 60.0}},
                                   LaneProfile::two_lane_default(),
                                   "Second");
  expect_command_round_trip(network, *command);

  ASSERT_TRUE(command->apply(network).has_value());
  EXPECT_EQ(network.road_count(), 2U);
  const RoadId created = network.find_road("2"); // auto-assigned next free id
  ASSERT_TRUE(created.is_valid());
  EXPECT_TRUE(command->dirty().topology);
  ASSERT_EQ(command->dirty().roads.size(), 1U);
  EXPECT_EQ(command->dirty().roads[0], created);
}

TEST(EditOperations, CreateRoadRejectsDegenerateWaypoints) {
  RoadNetwork network;
  author_default(network, "1");
  expect_command_rejected(
      network,
      roadmaker::edit::create_road({Waypoint{.x = 1.0, .y = 1.0}, Waypoint{.x = 1.0, .y = 1.0}},
                                   LaneProfile::two_lane_default(),
                                   ""));
}

TEST(EditOperations, DeleteRoadDetachesAndUndoResurrectsOriginalIds) {
  RoadNetwork network;
  const RoadId incoming = author_default(network, "1");
  const RoadId doomed = author_default(network, "2", 40.0);
  const RoadId neighbor = author_default(network, "3", 80.0);
  network.road(neighbor)->predecessor =
      roadmaker::RoadLink{.target = doomed, .contact = ContactPoint::End};

  const JunctionId junction = network.create_junction("100", "X");
  network.junction(junction)->connections.push_back(JunctionConnection{
      .incoming_road = incoming,
      .connecting_road = doomed,
      .contact_point = ContactPoint::Start,
      .lane_links = {{-1, -1}},
  });

  auto command = roadmaker::edit::delete_road(network, doomed);
  expect_command_round_trip(network, *command);

  ASSERT_TRUE(command->apply(network).has_value());
  EXPECT_EQ(network.road(doomed), nullptr);
  EXPECT_TRUE(network.junction(junction)->connections.empty());
  EXPECT_FALSE(network.road(neighbor)->predecessor.has_value());

  ASSERT_TRUE(command->revert(network).has_value());
  // The acceptance criterion: ids held elsewhere are valid again.
  ASSERT_EQ(network.junction(junction)->connections.size(), 1U);
  const JunctionConnection& connection = network.junction(junction)->connections[0];
  ASSERT_NE(network.road(connection.connecting_road), nullptr);
  EXPECT_EQ(network.road(connection.connecting_road)->odr_id, "2");
  ASSERT_TRUE(network.road(neighbor)->predecessor.has_value());
  EXPECT_EQ(std::get<RoadId>(network.road(neighbor)->predecessor->target), doomed);
}

TEST(EditOperations, SplitRoadPreservesGeometryAndRoundTrips) {
  RoadNetwork network;
  const RoadId road = author_default(network, "1");
  const double length = network.road(road)->length;
  const double split_s = length * 0.4;
  const roadmaker::PathPoint at_split = network.road(road)->plan_view.evaluate(split_s);

  auto command = roadmaker::edit::split_road(network, road, split_s);
  expect_command_round_trip(network, *command);

  ASSERT_TRUE(command->apply(network).has_value());
  EXPECT_EQ(network.road_count(), 2U);
  const RoadId tail = network.find_road("2");
  ASSERT_TRUE(tail.is_valid());

  const roadmaker::Road& head_road = *network.road(road);
  const roadmaker::Road& tail_road = *network.road(tail);
  EXPECT_NEAR(head_road.length, split_s, roadmaker::tol::kRoundTripPosition);
  EXPECT_NEAR(head_road.length + tail_road.length, length, roadmaker::tol::kRoundTripPosition);

  // The seam is G1: tail starts exactly where the head ends.
  const roadmaker::PathPoint tail_start = tail_road.plan_view.evaluate(0.0);
  EXPECT_NEAR(tail_start.x, at_split.x, roadmaker::tol::kRoundTripPosition);
  EXPECT_NEAR(tail_start.y, at_split.y, roadmaker::tol::kRoundTripPosition);
  EXPECT_NEAR(tail_start.hdg, at_split.hdg, roadmaker::tol::kRoundTripHeading);

  // Linked head->tail with identity lane links on the seam.
  ASSERT_TRUE(head_road.successor.has_value());
  EXPECT_EQ(std::get<RoadId>(head_road.successor->target), tail);
  ASSERT_TRUE(tail_road.predecessor.has_value());
  EXPECT_EQ(std::get<RoadId>(tail_road.predecessor->target), road);
  ASSERT_EQ(tail_road.sections.size(), 1U);
  EXPECT_EQ(network.lane_section(tail_road.sections[0])->lanes.size(),
            network.lane_section(head_road.sections[0])->lanes.size());
}

TEST(EditOperations, SplitRoadRejectsBadStationsAndJunctionRoads) {
  RoadNetwork network;
  const RoadId road = author_default(network, "1");
  const double length = network.road(road)->length;

  expect_command_rejected(network, roadmaker::edit::split_road(network, road, 0.0));
  expect_command_rejected(network, roadmaker::edit::split_road(network, road, length));

  const JunctionId junction = network.create_junction("100", "X");
  network.road(road)->successor =
      roadmaker::RoadLink{.target = junction, .contact = ContactPoint::Start};
  expect_command_rejected(network, roadmaker::edit::split_road(network, road, length * 0.5));
}

// --- junctions ---------------------------------------------------------------------

TEST(EditOperations, CreateJunctionLinksArmsAndRoundTrips) {
  RoadNetwork network;
  const RoadId a = author_default(network, "1");
  const RoadId b = author_default(network, "2", 40.0);

  const std::array<RoadEnd, 2> ends{
      RoadEnd{.road = a, .contact = ContactPoint::End},
      RoadEnd{.road = b, .contact = ContactPoint::Start},
  };
  auto command = roadmaker::edit::create_junction(network, ends);
  expect_command_round_trip(network, *command);

  ASSERT_TRUE(command->apply(network).has_value());
  EXPECT_EQ(network.junction_count(), 1U);
  const JunctionId junction = network.find_junction("1");
  ASSERT_TRUE(junction.is_valid());
  ASSERT_TRUE(network.road(a)->successor.has_value());
  EXPECT_EQ(std::get<JunctionId>(network.road(a)->successor->target), junction);
  ASSERT_TRUE(network.road(b)->predecessor.has_value());
  EXPECT_EQ(std::get<JunctionId>(network.road(b)->predecessor->target), junction);
}

TEST(EditOperations, CreateJunctionRejectsBadEnds) {
  RoadNetwork network;
  const RoadId a = author_default(network, "1");
  const RoadId b = author_default(network, "2", 40.0);

  const std::array<RoadEnd, 1> too_few{RoadEnd{.road = a, .contact = ContactPoint::End}};
  expect_command_rejected(network, roadmaker::edit::create_junction(network, too_few));

  const std::array<RoadEnd, 2> duplicate{
      RoadEnd{.road = a, .contact = ContactPoint::End},
      RoadEnd{.road = a, .contact = ContactPoint::End},
  };
  expect_command_rejected(network, roadmaker::edit::create_junction(network, duplicate));

  network.road(b)->predecessor = roadmaker::RoadLink{.target = a, .contact = ContactPoint::End};
  const std::array<RoadEnd, 2> occupied{
      RoadEnd{.road = a, .contact = ContactPoint::End},
      RoadEnd{.road = b, .contact = ContactPoint::Start},
  };
  expect_command_rejected(network, roadmaker::edit::create_junction(network, occupied));
}

TEST(EditOperations, DeleteJunctionDetachesRoadsAndRoundTrips) {
  RoadNetwork network;
  const RoadId a = author_default(network, "1");
  const RoadId b = author_default(network, "2", 40.0);
  const std::array<RoadEnd, 2> ends{
      RoadEnd{.road = a, .contact = ContactPoint::End},
      RoadEnd{.road = b, .contact = ContactPoint::Start},
  };
  auto create = roadmaker::edit::create_junction(network, ends);
  ASSERT_TRUE(create->apply(network).has_value());
  const JunctionId junction = network.find_junction("1");

  auto command = roadmaker::edit::delete_junction(network, junction);
  expect_command_round_trip(network, *command);

  ASSERT_TRUE(command->apply(network).has_value());
  EXPECT_EQ(network.junction(junction), nullptr);
  EXPECT_FALSE(network.road(a)->successor.has_value());
  EXPECT_FALSE(network.road(b)->predecessor.has_value());
}

// --- lanes: add / remove --------------------------------------------------------

TEST(EditOperations, AddLaneAppendsOutermostAndRoundTrips) {
  RoadNetwork network;
  const RoadId road = author_default(network, "1");
  const LaneSectionId section = network.road(road)->sections[0];
  const std::size_t lanes_before = network.lane_section(section)->lanes.size();

  auto command = roadmaker::edit::add_lane(network, section, -1, LaneType::Biking);
  expect_command_round_trip(network, *command);

  ASSERT_TRUE(command->apply(network).has_value());
  ASSERT_EQ(network.lane_section(section)->lanes.size(), lanes_before + 1);
  const LaneId added = network.lane_section(section)->lanes.back(); // outermost right
  EXPECT_EQ(network.lane(added)->odr_id, -3);                       // default profile has -1, -2
  EXPECT_EQ(network.lane(added)->type, LaneType::Biking);
  // Width copied from the previous outermost (shoulder, 1.0 m).
  ASSERT_FALSE(network.lane(added)->widths.empty());
  EXPECT_NEAR(network.lane(added)->widths[0].a, 1.0, 1e-12);
}

TEST(EditOperations, RemoveLaneOutermostOnlyAndRoundTrips) {
  RoadNetwork network;
  const RoadId road = author_default(network, "1");
  const LaneSectionId section = network.road(road)->sections[0];
  const LaneId outer_right = network.lane_section(section)->lanes.back(); // -2 shoulder
  const LaneId inner_right = *std::prev(network.lane_section(section)->lanes.end(), 2); // -1

  expect_command_rejected(network, roadmaker::edit::remove_lane(network, inner_right));

  auto command = roadmaker::edit::remove_lane(network, outer_right);
  expect_command_round_trip(network, *command);

  ASSERT_TRUE(command->apply(network).has_value());
  EXPECT_EQ(network.lane(outer_right), nullptr);
  ASSERT_TRUE(command->revert(network).has_value());
  ASSERT_NE(network.lane(outer_right), nullptr); // original id resurrected
  EXPECT_EQ(network.lane(outer_right)->type, LaneType::Shoulder);
}

// --- integration with the stack ---------------------------------------------------

TEST(EditOperations, EditStackDrivesTopologyCommands) {
  RoadNetwork network;
  author_default(network, "1");
  const std::string pristine = snapshot_xodr(network);

  roadmaker::edit::EditStack stack;
  ASSERT_TRUE(stack
                  .push(network,
                        roadmaker::edit::create_road(
                            {Waypoint{.x = 0.0, .y = 50.0}, Waypoint{.x = 70.0, .y = 55.0}},
                            LaneProfile::two_lane_default(),
                            "Branch"))
                  .has_value());
  const RoadId created = network.find_road("2");
  ASSERT_TRUE(created.is_valid());
  ASSERT_TRUE(
      stack.push(network, roadmaker::edit::rename_road(network, created, "Renamed")).has_value());
  const std::string edited = snapshot_xodr(network);

  ASSERT_TRUE(stack.undo(network).has_value());
  ASSERT_TRUE(stack.undo(network).has_value());
  expect_network_matches(network, pristine);

  ASSERT_TRUE(stack.redo(network).has_value());
  ASSERT_TRUE(stack.redo(network).has_value());
  expect_network_matches(network, edited);
  EXPECT_EQ(network.road(created)->name, "Renamed"); // same generational id after redo
}
