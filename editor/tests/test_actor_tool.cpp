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

// The Actor tool's interaction (p8-s2, issue #246), driven headlessly through
// ToolEvent — the M2 contract that makes a tool testable without a viewport.
//
// WHAT IS ACTUALLY BEING PINNED HERE is the gesture grammar, because every one
// of these is a way the tool could look right and behave wrong:
//   - a click on empty space REFUSES with a hint rather than placing in space;
//   - a click on an actor SELECTS it rather than stacking a second one on it;
//   - a drag is ONE undo entry, and a click that jitters is not a drag at all;
//   - a placement is one entry, so one Ctrl+Z undoes the whole gesture.

#include "roadmaker/edit/operations.hpp"
#include "roadmaker/osc/edit.hpp"
#include "roadmaker/osc/writer.hpp"

#include <gtest/gtest.h>

#include <QSignalSpy>
#include <string>

#include "document/document.hpp"
#include "document/selection_model.hpp"
#include "tools/actor_tool.hpp"

namespace roadmaker::editor {
namespace {

/// A straight two-lane road along +x, 100 m. Lane -1 sits at negative y.
void author_straight_road(Document& document) {
  auto command = edit::create_road({Waypoint{.x = 0.0, .y = 0.0}, Waypoint{.x = 100.0, .y = 0.0}},
                                   LaneProfile::two_lane_default(),
                                   "main");
  ASSERT_NE(command, nullptr);
  ASSERT_TRUE(document.push_command(std::move(command)).has_value());
}

ToolEvent at(double x, double y) {
  return ToolEvent{.world_x = x, .world_y = y, .buttons = Qt::LeftButton};
}

/// One click: move (so the tool has a hover), press, release.
void click(ActorTool& tool, double x, double y) {
  (void)tool.mouse_move(at(x, y));
  (void)tool.mouse_press(at(x, y));
  (void)tool.mouse_release(at(x, y));
}

} // namespace

TEST(ActorTool, AClickOnALanePlacesAnActorThere) {
  Document document;
  author_straight_road(document);
  SelectionModel selection(document);
  ActorTool tool(document, selection);

  click(tool, 50.0, -1.75);

  ASSERT_EQ(document.scenario().entities.scenario_objects.size(), 1U);
  EXPECT_EQ(document.scenario().entities.scenario_objects[0].name, "Car1");

  // Placed ON THE LANE, as a <LanePosition> — not at a point in space.
  ASSERT_EQ(document.scenario().storyboard.init.actions.privates.size(), 1U);
  const osc::PrivateAction& action =
      document.scenario().storyboard.init.actions.privates[0].actions[0];
  ASSERT_TRUE(action.teleport.has_value());
  const auto* lane = std::get_if<osc::LanePosition>(&action.teleport->position);
  ASSERT_NE(lane, nullptr) << "the actor was placed with something other than a lane position";
  EXPECT_EQ(lane->lane_id, "-1") << "the actor snapped to the wrong lane";
  EXPECT_NEAR(lane->s, 50.0, 1e-6);
  EXPECT_FALSE(lane->road_id.empty());
}

TEST(ActorTool, APlacementIsOneUndoEntry) {
  // ★ One click, one Ctrl+Z. place_scenario_object exists precisely so this is
  // true; if the tool pushed add + set_pose the user would need two.
  Document document;
  author_straight_road(document);
  SelectionModel selection(document);
  ActorTool tool(document, selection);

  const int before = document.undo_stack()->count();
  click(tool, 50.0, -1.75);
  EXPECT_EQ(document.undo_stack()->count(), before + 1);

  document.undo_stack()->undo();
  EXPECT_TRUE(document.scenario().entities.scenario_objects.empty());
  EXPECT_TRUE(document.scenario().storyboard.init.actions.privates.empty());
}

TEST(ActorTool, AClickOffTheRoadIsRefusedWithAHintAndPlacesNothing) {
  Document document;
  author_straight_road(document);
  SelectionModel selection(document);
  ActorTool tool(document, selection);
  QSignalSpy toasts(&tool, &Tool::toast_requested);
  ASSERT_TRUE(toasts.isValid());

  click(tool, 5000.0, 5000.0);

  EXPECT_TRUE(document.scenario().entities.scenario_objects.empty())
      << "an actor was placed in open space";
  ASSERT_EQ(toasts.count(), 1) << "a refusal must say why";
  EXPECT_FALSE(toasts.at(0).at(0).toString().isEmpty());
}

TEST(ActorTool, TheInstructionShowsTheRefusalBeforeTheClick) {
  // A hint that only appears AFTER a failed click teaches nothing — the user
  // has already been refused. Hovering is where it belongs.
  Document document;
  author_straight_road(document);
  SelectionModel selection(document);
  ActorTool tool(document, selection);

  (void)tool.mouse_move(at(50.0, -1.75));
  const QString over_lane = tool.instruction();
  EXPECT_FALSE(over_lane.contains("No road within reach"));

  (void)tool.mouse_move(at(5000.0, 5000.0));
  EXPECT_TRUE(tool.instruction().contains("No road")) << tool.instruction().toStdString();
}

TEST(ActorTool, PlacingSelectsTheNewActor) {
  Document document;
  author_straight_road(document);
  SelectionModel selection(document);
  ActorTool tool(document, selection);

  click(tool, 50.0, -1.75);
  EXPECT_EQ(selection.selected_actors(), std::vector<std::string>{"Car1"});
}

TEST(ActorTool, EachClickMintsTheNextName) {
  Document document;
  author_straight_road(document);
  SelectionModel selection(document);
  ActorTool tool(document, selection);

  click(tool, 20.0, -1.75);
  click(tool, 60.0, -1.75);
  ASSERT_EQ(document.scenario().entities.scenario_objects.size(), 2U);
  EXPECT_EQ(document.scenario().entities.scenario_objects[0].name, "Car1");
  EXPECT_EQ(document.scenario().entities.scenario_objects[1].name, "Car2");
}

TEST(ActorTool, TheKindDecidesWhatIsPlaced) {
  Document document;
  author_straight_road(document);
  SelectionModel selection(document);
  ActorTool tool(document, selection);

  tool.set_kind(osc::ActorKind::Truck);
  click(tool, 50.0, -1.75);

  ASSERT_EQ(document.scenario().entities.scenario_objects.size(), 1U);
  EXPECT_EQ(document.scenario().entities.scenario_objects[0].name, "Truck1");
  const auto* vehicle =
      std::get_if<osc::Vehicle>(&document.scenario().entities.scenario_objects[0].entity_object);
  ASSERT_NE(vehicle, nullptr);
  EXPECT_EQ(vehicle->category, "truck");
}

// --- grabbing an existing actor ---------------------------------------------

TEST(ActorTool, AClickOnAPlacedActorSelectsItRatherThanStackingASecond) {
  // ★ Without this, clicking an actor to select it would place another one on
  // top of it — and the two would be indistinguishable in the viewport.
  Document document;
  author_straight_road(document);
  SelectionModel selection(document);
  ActorTool tool(document, selection);

  click(tool, 50.0, -1.75);
  ASSERT_EQ(document.scenario().entities.scenario_objects.size(), 1U);
  const int after_place = document.undo_stack()->count();

  click(tool, 50.0, -1.75); // the same spot: the actor is there now
  EXPECT_EQ(document.scenario().entities.scenario_objects.size(), 1U)
      << "a second actor was stacked on the first";
  EXPECT_EQ(document.undo_stack()->count(), after_place) << "a select pushed a command";
  EXPECT_EQ(selection.selected_actors(), std::vector<std::string>{"Car1"});
}

TEST(ActorTool, DraggingAPlacedActorMovesItAsOneUndoEntry) {
  Document document;
  author_straight_road(document);
  SelectionModel selection(document);
  ActorTool tool(document, selection);

  click(tool, 20.0, -1.75);
  const int after_place = document.undo_stack()->count();

  // Press on the actor, move well past the drag threshold, release elsewhere.
  (void)tool.mouse_move(at(20.0, -1.75));
  (void)tool.mouse_press(at(20.0, -1.75));
  (void)tool.mouse_move(at(45.0, -1.75));
  (void)tool.mouse_move(at(70.0, -1.75));
  (void)tool.mouse_release(at(70.0, -1.75));

  EXPECT_EQ(document.scenario().entities.scenario_objects.size(), 1U)
      << "the drag placed a new actor instead of moving the grabbed one";
  EXPECT_EQ(document.undo_stack()->count(), after_place + 1)
      << "a drag pushed more than one entry — the cursor path reached the undo stack";

  const osc::PrivateAction& action =
      document.scenario().storyboard.init.actions.privates[0].actions[0];
  ASSERT_TRUE(action.teleport.has_value());
  const auto* lane = std::get_if<osc::LanePosition>(&action.teleport->position);
  ASSERT_NE(lane, nullptr);
  EXPECT_NEAR(lane->s, 70.0, 0.5) << "the actor did not follow the drag";
}

TEST(ActorTool, AClickThatJittersIsStillAClickAndNotADrag) {
  // Below the drag threshold: a hand tremor must not re-anchor the actor to
  // where it already is and push a no-op onto the undo stack.
  Document document;
  author_straight_road(document);
  SelectionModel selection(document);
  ActorTool tool(document, selection);

  click(tool, 50.0, -1.75);
  const int after_place = document.undo_stack()->count();

  (void)tool.mouse_move(at(50.0, -1.75));
  (void)tool.mouse_press(at(50.0, -1.75));
  (void)tool.mouse_move(at(50.05, -1.75)); // 5 cm — under the threshold
  (void)tool.mouse_release(at(50.05, -1.75));

  EXPECT_EQ(document.undo_stack()->count(), after_place) << "a jittered click became a drag";
}

TEST(ActorTool, EscapeCancelsAPressWithoutPlacing) {
  Document document;
  author_straight_road(document);
  SelectionModel selection(document);
  ActorTool tool(document, selection);

  (void)tool.mouse_move(at(50.0, -1.75));
  (void)tool.mouse_press(at(50.0, -1.75));
  EXPECT_TRUE(tool.key_press(Qt::Key_Escape, Qt::NoModifier));
  (void)tool.mouse_release(at(50.0, -1.75));

  EXPECT_TRUE(document.scenario().entities.scenario_objects.empty())
      << "Escape did not cancel the press";
}

TEST(ActorTool, DeactivateClearsTheHoverAndThePress) {
  Document document;
  author_straight_road(document);
  SelectionModel selection(document);
  ActorTool tool(document, selection);

  (void)tool.mouse_move(at(50.0, -1.75));
  ASSERT_FALSE(tool.preview().empty());

  tool.deactivate();
  EXPECT_TRUE(tool.preview().empty()) << "a stale ghost survived deactivation";
}

TEST(ActorTool, TheGhostSitsWhereTheActorWillLand) {
  // ★ GHOST == COMMIT. Both go through actor_world_pose, so the preview cannot
  // drift from the placement — the failure would only ever show as "the ghost
  // lied".
  Document document;
  author_straight_road(document);
  SelectionModel selection(document);
  ActorTool tool(document, selection);

  (void)tool.mouse_move(at(50.0, -1.75));
  const PreviewGeometry ghost = tool.preview();
  ASSERT_EQ(ghost.handles.size(), 1U);
  const double ghost_x = ghost.handles[0].x;
  const double ghost_y = ghost.handles[0].y;

  click(tool, 50.0, -1.75);
  const osc::PrivateAction& action =
      document.scenario().storyboard.init.actions.privates[0].actions[0];
  const auto* lane = std::get_if<osc::LanePosition>(&action.teleport->position);
  ASSERT_NE(lane, nullptr);
  const auto pose = actor_world_pose(document.network(), *lane);
  ASSERT_TRUE(pose.has_value());

  EXPECT_NEAR(pose->position[0], ghost_x, 1e-9);
  EXPECT_NEAR(pose->position[1], ghost_y, 1e-9);
}

} // namespace roadmaker::editor
