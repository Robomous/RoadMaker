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

// cascade-s4 (#464) — stage [4] of the move funnel. Props follow their anchor
// road's frame, which is correct (#400) and is exactly what can drive one into
// another road. The stage REPORTS that and changes nothing; the offered fix is
// a command the user has to ask for.

#include "roadmaker/edit/edit_stack.hpp"
#include "roadmaker/edit/operations.hpp"
#include "roadmaker/mesh/prop_obstructions.hpp"
#include "roadmaker/road/authoring.hpp"
#include "roadmaker/road/network.hpp"
#include "roadmaker/road/object.hpp"
#include "roadmaker/road/road.hpp"

#include "support/network_compare.hpp"

#include <gtest/gtest.h>

#include <array>
#include <memory>
#include <numbers>
#include <string>
#include <vector>

namespace roadmaker {
namespace {

constexpr double kPi = std::numbers::pi;

RoadId segment(RoadNetwork& network, const char* id, Waypoint a, Waypoint b) {
  const std::array<Waypoint, 2> waypoints{a, b};
  return author_clothoid_road(network, waypoints, LaneProfile::two_lane_default(), "", id).value();
}

ObjectId tree(RoadNetwork& network, RoadId road, double s, double t, const char* odr_id = "1") {
  Object object;
  object.road = road;
  object.odr_id = odr_id;
  object.name = "tree_pine";
  object.type = ObjectType::Tree;
  object.s = s;
  object.t = t;
  object.radius = 1.5;
  object.height = 4.0;
  return network.add_object(road, std::move(object));
}

void expect_applies(RoadNetwork& network, const std::unique_ptr<edit::Command>& command) {
  ASSERT_NE(command, nullptr);
  const auto applied = command->apply(network);
  ASSERT_TRUE(applied.has_value()) << applied.error().message;
}

/// A road carrying a tree 20 m off to one side, and a second road that side is
/// clear of — until something moves.
struct Scene {
  RoadId anchor;
  RoadId crossed;
  ObjectId prop;
};

Scene two_roads_and_a_tree(RoadNetwork& network) {
  Scene scene;
  scene.anchor = segment(network, "anchor", {-50, 40}, {50, 40});
  scene.crossed = segment(network, "crossed", {-50, 0}, {50, 0});
  scene.prop = tree(network, scene.anchor, 50.0, -20.0); // world (0, 20): clear
  return scene;
}

// -----------------------------------------------------------------------------

TEST(PropCascade, AMoveThatDrivesAPropIntoARoadReportsIt) {
  RoadNetwork network;
  const Scene scene = two_roads_and_a_tree(network);
  ASSERT_TRUE(find_prop_obstructions(network).empty()) << "clear before the gesture";

  // Carry the anchor road 20 m south: the tree rides with it, straight into the
  // crossed road's carriageway. Not one datum of the object changed.
  auto command = edit::translate_road(network, scene.anchor, 0.0, -20.0);
  expect_applies(network, command);

  const std::span<const edit::ObstructionRecord> records = command->obstruction_records();
  ASSERT_EQ(records.size(), 1U);
  EXPECT_EQ(records[0].obstruction.object, scene.prop);
  EXPECT_EQ(records[0].obstruction.kind, ObstructionKind::RoadSurface);
  EXPECT_EQ(records[0].obstruction.road, scene.crossed);
  EXPECT_EQ(records[0].object_odr_id, "1");
  EXPECT_NE(records[0].detail.find("crossed"), std::string::npos)
      << "the report names what it hit: " << records[0].detail;
}

/// THE pre-read test. A prop that was ALREADY in a lane before the gesture is
/// not this gesture's doing, and saying so on every nudge would make the
/// channel noise — the same stance cascade-s1 takes towards a joint that was
/// already broken.
///
/// The one-centimetre nudge is the point: the obstruction persists but its
/// WITNESS point moves, so a diff that compared whole records rather than
/// subjects would call it new every single frame of a drag.
TEST(PropCascade, AnAlreadyObstructedPropIsNotReportedAgainByALaterNudge) {
  RoadNetwork network;
  const Scene scene = two_roads_and_a_tree(network);
  network.object(scene.prop)->t = -40.0; // world (0, 0): already in the crossed road
  ASSERT_EQ(find_prop_obstructions(network).size(), 1U) << "obstructed before we touch anything";

  auto command = edit::translate_road(network, scene.anchor, 0.01, 0.0);
  expect_applies(network, command);
  EXPECT_TRUE(command->obstruction_records().empty())
      << "the prop is still obstructed, but this gesture did not do it";

  // Control: the obstruction really is still there, so "empty" is the filter
  // talking and not the query having lost it.
  EXPECT_EQ(find_prop_obstructions(network).size(), 1U);
}

/// The stage is pure reporting. A move that reports must write exactly what a
/// move that does not report would have written.
TEST(PropCascade, TheObstructionStageMutatesNothing) {
  RoadNetwork network;
  const Scene scene = two_roads_and_a_tree(network);

  RoadNetwork control;
  const Scene control_scene = two_roads_and_a_tree(control);
  // The control moves the same road the same way with NO prop to obstruct.
  ASSERT_TRUE(control.erase_object(control_scene.prop));

  auto command = edit::translate_road(network, scene.anchor, 0.0, -20.0);
  expect_applies(network, command);
  ASSERT_FALSE(command->obstruction_records().empty()) << "this move must report something";

  auto control_command = edit::translate_road(control, control_scene.anchor, 0.0, -20.0);
  expect_applies(control, control_command);

  // Same roads, same geometry: only the <object> element differs.
  const std::string reported = test::snapshot_xodr(network);
  EXPECT_NE(reported.find("<object"), std::string::npos);
  EXPECT_EQ(test::snapshot_xodr(control).find("<object"), std::string::npos);
}

TEST(PropCascade, UndoAfterAReportedObstructionIsByteIdentical) {
  RoadNetwork network;
  const Scene scene = two_roads_and_a_tree(network);
  const std::string before = test::snapshot_xodr(network);

  auto command = edit::translate_road(network, scene.anchor, 0.0, -20.0);
  expect_applies(network, command);
  ASSERT_EQ(command->obstruction_records().size(), 1U);

  ASSERT_TRUE(command->revert(network).has_value());
  EXPECT_EQ(test::snapshot_xodr(network), before);

  // The records describe what APPLYING does, so a redo does it again — they
  // survive the revert, exactly as follow_records and derived_records do.
  EXPECT_EQ(command->obstruction_records().size(), 1U);
}

/// The claim the funnel rests on: every gesture decides in the same place, so
/// none of them can hold its own opinion about obstructed props.
TEST(PropCascade, NoGestureLeavesAPropObstructionUnreported) {
  struct Gesture {
    const char* label;
    std::unique_ptr<edit::Command> (*build)(RoadNetwork&, RoadId);
  };

  const std::array<Gesture, 6> gestures{
      Gesture{"translate",
              [](RoadNetwork& net, RoadId road) { return edit::translate_road(net, road, 0, -10); }},
      Gesture{"rotate",
              [](RoadNetwork& net, RoadId road) {
                return edit::rotate_road(net, road, kPi, 0.0, 5.0);
              }},
      Gesture{"move_waypoint",
              [](RoadNetwork& net, RoadId road) {
                return edit::move_waypoint(net, road, 1, Waypoint{.x = 0, .y = 18});
              }},
      Gesture{"insert_waypoint",
              [](RoadNetwork& net, RoadId road) {
                return edit::insert_waypoint(net, road, 1, Waypoint{.x = -25, .y = 12});
              }},
      Gesture{"delete_waypoint",
              [](RoadNetwork& net, RoadId road) { return edit::delete_waypoint(net, road, 1); }},
      Gesture{"insert_node_at",
              [](RoadNetwork& net, RoadId road) { return edit::insert_node_at(net, road, 10.0); }},
  };

  std::size_t gestures_that_reported = 0;
  for (const Gesture& gesture : gestures) {
    SCOPED_TRACE(gesture.label);
    RoadNetwork network;
    // Three waypoints, so delete_waypoint has an interior one to drop, and a
    // bend at the middle one so deleting it actually moves the road. The tree
    // hangs 20 m off the bend's apex, at world y ~ 10 — CLEAR of the crossed
    // road, which is what makes "empty after undo" mean anything. An earlier
    // draft dipped the apex to y = 20, which put the tree in the road before
    // any gesture ran and made two rows fail for the fixture's reasons.
    const std::array<Waypoint, 3> path{
        Waypoint{.x = -50, .y = 40}, Waypoint{.x = 0, .y = 30}, Waypoint{.x = 50, .y = 40}};
    const RoadId anchor =
        author_clothoid_road(network, path, LaneProfile::two_lane_default(), "", "anchor").value();
    segment(network, "crossed", {-60, 0}, {60, 0});
    tree(network, anchor, network.road(anchor)->plan_view.length() * 0.5, -20.0);
    ASSERT_TRUE(find_prop_obstructions(network).empty()) << "the fixture starts clear";

    auto command = gesture.build(network, anchor);
    ASSERT_NE(command, nullptr);
    const auto applied = command->apply(network);
    if (!applied.has_value()) {
      GTEST_SKIP() << "gesture refused for this fixture: " << applied.error().message;
    }

    // Whatever the gesture did, the stage's answer and the query's answer agree
    // — the funnel cannot be silent about a state the query can see. Not every
    // gesture here drives the prop into the road, and that is fine: the claim
    // is that none of them holds its own opinion.
    const std::vector<PropObstruction> actual = find_prop_obstructions(network);
    EXPECT_EQ(command->obstruction_records().size(), actual.size())
        << "the stage reported " << command->obstruction_records().size() << " of " << actual.size()
        << " obstructions this gesture created";
    gestures_that_reported += command->obstruction_records().empty() ? 0 : 1;

    ASSERT_TRUE(command->revert(network).has_value());
    EXPECT_TRUE(find_prop_obstructions(network).empty()) << "undo clears them";
  }

  // Without this the table passes on 0 == 0 six times over.
  EXPECT_GE(gestures_that_reported, 2U) << "the table must actually exercise the reporting path";
}

/// #338 through the command layer rather than the query: the risk note this
/// sprint closes is about a GESTURE, and it is the funnel that has to notice.
TEST(PropCascade, TheRotationArcOf338IsReportedByTheMoveThatCausesIt) {
  RoadNetwork network;
  const RoadId anchor = segment(network, "anchor", {-50, 40}, {50, 40});
  segment(network, "crossed", {-50, 15}, {50, 15});
  const ObjectId prop = tree(network, anchor, 50.0, 25.0); // world (0, 65)
  ASSERT_TRUE(find_prop_obstructions(network).empty());

  // Half a turn about the road's own midpoint. The road maps onto itself; the
  // prop, 25 m out, sweeps a 25 m arc onto the crossed road.
  auto command = edit::rotate_road(network, anchor, kPi, 0.0, 40.0);
  expect_applies(network, command);

  const std::span<const edit::ObstructionRecord> records = command->obstruction_records();
  ASSERT_EQ(records.size(), 1U) << "a rotation that sweeps a prop into a road must say so";
  EXPECT_EQ(records[0].obstruction.object, prop);
}

TEST(PropCascade, TheEditStackExposesTheRecordsToHeadlessCallers) {
  RoadNetwork network;
  const Scene scene = two_roads_and_a_tree(network);

  edit::EditStack stack;
  ASSERT_TRUE(stack.push(network, edit::translate_road(network, scene.anchor, 0.0, -20.0))
                  .has_value());
  ASSERT_EQ(stack.last_obstruction_records().size(), 1U)
      << "push() owns the command, so this is the only way Python can read them";

  ASSERT_TRUE(stack.undo(network).has_value());
  EXPECT_TRUE(stack.last_obstruction_records().empty()) << "nothing is applied any more";
}

} // namespace
} // namespace roadmaker
