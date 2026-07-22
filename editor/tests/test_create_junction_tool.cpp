// Copyright 2026 Robomous
// SPDX-License-Identifier: Apache-2.0

// Create Junction tool (issue #17, docs/design/m2/02_editing_tools.md §6):
// headless ToolEvent sequences select road ends and generate a junction; the
// tests assert on the network, the undo stack, and the regeneration an
// incoming-road edit triggers.

#include "roadmaker/edit/connection.hpp"
#include "roadmaker/edit/operations.hpp"
#include "roadmaker/road/road.hpp"

#include <gtest/gtest.h>

#include <QSignalSpy>
#include <QUndoStack>
#include <cmath>
#include <stdexcept>

#include "app/actions.hpp"
#include "document/document.hpp"
#include "tools/create_junction_tool.hpp"

using roadmaker::JunctionId;
using roadmaker::RoadId;
using roadmaker::Waypoint;
using roadmaker::editor::CreateJunctionTool;
using roadmaker::editor::Document;
using roadmaker::editor::ToolEvent;

namespace {

ToolEvent click_at(double x, double y) {
  ToolEvent event;
  event.buttons = Qt::LeftButton;
  event.world_x = x;
  event.world_y = y;
  return event;
}

/// Three straight two-lane arms whose ends meet near the origin (west, east,
/// south), each authored through a command so the base state is undoable.
struct Scene {
  Document document;
  int base_count = 0;

  Scene() {
    const auto arm = [&](Waypoint a, Waypoint b) {
      if (!document.push_command(roadmaker::edit::create_road(
              {a, b}, roadmaker::LaneProfile::two_lane_default(), ""))) {
        throw std::runtime_error("scene setup failed");
      }
    };
    arm(Waypoint{-40.0, 0.0}, Waypoint{-6.0, 0.0}); // road "1", end at (-6, 0)
    arm(Waypoint{40.0, 0.0}, Waypoint{6.0, 0.0});   // road "2", end at (6, 0)
    arm(Waypoint{0.0, -40.0}, Waypoint{0.0, -6.0}); // road "3", end at (0, -6)
    base_count = document.undo_stack()->count();
  }
};

} // namespace

TEST(CreateJunctionTool, SelectsEndsAndGeneratesConnectingRoads) {
  Scene scene;
  CreateJunctionTool tool(scene.document);
  tool.activate();

  ASSERT_TRUE(tool.mouse_press(click_at(-6.0, 0.0)));
  ASSERT_TRUE(tool.mouse_press(click_at(6.0, 0.0)));
  ASSERT_TRUE(tool.mouse_press(click_at(0.0, -6.0)));
  EXPECT_EQ(tool.selected_count(), 3U);

  ASSERT_TRUE(tool.key_press(Qt::Key_Return, Qt::NoModifier));
  EXPECT_EQ(tool.selected_count(), 0U); // session reset after commit

  EXPECT_EQ(scene.document.network().junction_count(), 1U);
  const JunctionId junction = scene.document.network().find_junction("1");
  ASSERT_TRUE(junction.is_valid());
  EXPECT_EQ(scene.document.network().junction(junction)->connections.size(), 6U);
  EXPECT_EQ(scene.document.undo_stack()->count(), scene.base_count + 1);

  scene.document.undo_stack()->undo();
  EXPECT_EQ(scene.document.network().junction_count(), 0U);
}

TEST(CreateJunctionTool, ClickingASelectedEndDeselectsIt) {
  Scene scene;
  CreateJunctionTool tool(scene.document);
  tool.activate();

  ASSERT_TRUE(tool.mouse_press(click_at(-6.0, 0.0)));
  EXPECT_EQ(tool.selected_count(), 1U);
  ASSERT_TRUE(tool.mouse_press(click_at(-6.0, 0.0))); // same end again
  EXPECT_EQ(tool.selected_count(), 0U);
}

TEST(CreateJunctionTool, FewerThanTwoEndsDoesNotGenerate) {
  Scene scene;
  CreateJunctionTool tool(scene.document);
  tool.activate();

  ASSERT_TRUE(tool.mouse_press(click_at(-6.0, 0.0)));
  ASSERT_TRUE(tool.key_press(Qt::Key_Return, Qt::NoModifier));
  EXPECT_EQ(scene.document.network().junction_count(), 0U);
  EXPECT_EQ(scene.document.undo_stack()->count(), scene.base_count);
}

TEST(CreateJunctionTool, EscapeClearsSelection) {
  Scene scene;
  CreateJunctionTool tool(scene.document);
  tool.activate();

  ASSERT_TRUE(tool.mouse_press(click_at(-6.0, 0.0)));
  ASSERT_TRUE(tool.mouse_press(click_at(6.0, 0.0)));
  ASSERT_TRUE(tool.key_press(Qt::Key_Escape, Qt::NoModifier));
  EXPECT_EQ(tool.selected_count(), 0U);
  EXPECT_EQ(scene.document.network().junction_count(), 0U);
}

TEST(CreateJunctionTool, ReselectingTheSameEndsRegeneratesInPlace) {
  Scene scene;
  CreateJunctionTool tool(scene.document);
  tool.activate();
  ASSERT_TRUE(tool.mouse_press(click_at(-6.0, 0.0)));
  ASSERT_TRUE(tool.mouse_press(click_at(6.0, 0.0)));
  ASSERT_TRUE(tool.mouse_press(click_at(0.0, -6.0)));
  ASSERT_TRUE(tool.key_press(Qt::Key_Return, Qt::NoModifier));
  ASSERT_EQ(scene.document.network().junction_count(), 1U);
  const JunctionId junction = scene.document.network().find_junction("1");

  QSignalSpy toast(&tool, &CreateJunctionTool::toast_requested);
  // Re-selecting ends that overlap the existing junction and generating must
  // NEVER superimpose a second junction (finding 5) — it regenerates in place
  // or refuses, and surfaces the outcome as a toast either way.
  ASSERT_TRUE(tool.mouse_press(click_at(-6.0, 0.0)));
  ASSERT_TRUE(tool.mouse_press(click_at(6.0, 0.0)));
  ASSERT_TRUE(tool.mouse_press(click_at(0.0, -6.0)));
  ASSERT_TRUE(tool.key_press(Qt::Key_Return, Qt::NoModifier));

  EXPECT_EQ(scene.document.network().junction_count(), 1U);         // no duplicate
  EXPECT_EQ(scene.document.network().find_junction("1"), junction); // still the same one
  EXPECT_EQ(tool.selected_count(), 0U);                             // session reset
  EXPECT_GE(toast.count(), 1);                                      // surfaced, not silent
}

TEST(CreateJunctionTool, ExactReselectionRegeneratesInPlaceNotADuplicate) {
  // Drive the idempotency-match path deterministically: build the junction, then
  // regenerate it from its recorded arms through the same routing generate uses.
  Scene scene;
  CreateJunctionTool tool(scene.document);
  tool.activate();
  ASSERT_TRUE(tool.mouse_press(click_at(-6.0, 0.0)));
  ASSERT_TRUE(tool.mouse_press(click_at(6.0, 0.0)));
  ASSERT_TRUE(tool.mouse_press(click_at(0.0, -6.0)));
  ASSERT_TRUE(tool.key_press(Qt::Key_Return, Qt::NoModifier));
  const JunctionId junction = scene.document.network().find_junction("1");
  ASSERT_TRUE(junction.is_valid());

  // matching_junction over the recorded arms finds exactly this junction, so a
  // regenerate lands (not a create) — no duplicate id is minted.
  const auto& arms = scene.document.network().junction(junction)->arms;
  EXPECT_EQ(roadmaker::edit::matching_junction(scene.document.network(), arms), junction);
  ASSERT_TRUE(scene.document.push_command(
      roadmaker::edit::regenerate_junction(scene.document.network(), junction)));
  EXPECT_EQ(scene.document.network().junction_count(), 1U);
}

TEST(CreateJunctionTool, AnEndAlreadyInAJunctionIsRefusedWithAWarning) {
  Scene scene;
  CreateJunctionTool tool(scene.document);
  tool.activate();
  ASSERT_TRUE(tool.mouse_press(click_at(-6.0, 0.0)));
  ASSERT_TRUE(tool.mouse_press(click_at(6.0, 0.0)));
  ASSERT_TRUE(tool.mouse_press(click_at(0.0, -6.0)));
  ASSERT_TRUE(tool.key_press(Qt::Key_Return, Qt::NoModifier));
  ASSERT_EQ(scene.document.network().junction_count(), 1U);

  // A fresh north arm; pairing its end with an already-owned junction arm must
  // be refused (the single-owner invariant), not overlaid as a second junction.
  ASSERT_TRUE(scene.document.push_command(roadmaker::edit::create_road(
      {Waypoint{0.0, 40.0}, Waypoint{0.0, 6.0}}, roadmaker::LaneProfile::two_lane_default(), "")));
  const int after_setup = scene.document.undo_stack()->count();

  QSignalSpy toast(&tool, &CreateJunctionTool::toast_requested);
  ASSERT_TRUE(tool.mouse_press(click_at(-6.0, 0.0))); // already a junction arm
  ASSERT_TRUE(tool.mouse_press(click_at(0.0, 6.0)));  // the fresh road's free end
  ASSERT_TRUE(tool.key_press(Qt::Key_Return, Qt::NoModifier));

  EXPECT_EQ(scene.document.network().junction_count(), 1U);     // no duplicate
  EXPECT_EQ(scene.document.undo_stack()->count(), after_setup); // no command pushed
  EXPECT_GE(toast.count(), 1);                                  // warning surfaced
}

TEST(CreateJunctionTool, EditingAnIncomingRoadRegeneratesInOneUndoStep) {
  Scene scene;
  CreateJunctionTool tool(scene.document);
  tool.activate();
  ASSERT_TRUE(tool.mouse_press(click_at(-6.0, 0.0)));
  ASSERT_TRUE(tool.mouse_press(click_at(6.0, 0.0)));
  ASSERT_TRUE(tool.mouse_press(click_at(0.0, -6.0)));
  ASSERT_TRUE(tool.key_press(Qt::Key_Return, Qt::NoModifier));
  const JunctionId junction = scene.document.network().find_junction("1");
  const RoadId west = scene.document.network().find_road("1");
  const int count_after_create = scene.document.undo_stack()->count();

  // Move the west arm's junction end; the document regenerates the junction
  // as part of the same undo entry (the move + regeneration is one Ctrl+Z).
  ASSERT_TRUE(scene.document.push_command(
      roadmaker::edit::move_waypoint(scene.document.network(), west, 1, Waypoint{-8.0, -1.0})));
  EXPECT_EQ(scene.document.undo_stack()->count(), count_after_create + 1);

  // A connecting road leaving the west arm now starts at the moved end.
  for (const auto& connection : scene.document.network().junction(junction)->connections) {
    if (connection.incoming_road != west) {
      continue;
    }
    const auto start =
        scene.document.network().road(connection.connecting_road)->plan_view.evaluate(0.0);
    EXPECT_NEAR(start.x, -8.0, 1e-6);
    EXPECT_NEAR(start.y, -1.0, 1e-6);
  }

  scene.document.undo_stack()->undo(); // one step reverts move + regeneration
  const auto restored = scene.document.network().road(west)->plan_view.evaluate(
      scene.document.network().road(west)->plan_view.length());
  EXPECT_NEAR(restored.x, -6.0, 1e-6);
  EXPECT_NEAR(restored.y, 0.0, 1e-6);
}

// ---- side attach (hardening sprint #92, docs/design/hardening/t_junction.md)

/// A 120 m main road and a side road ending 10 m south of its midpoint.
struct TeeScene {
  Document document;

  TeeScene() {
    const auto road = [&](Waypoint a, Waypoint b) {
      if (!document.push_command(roadmaker::edit::create_road(
              {a, b}, roadmaker::LaneProfile::two_lane_default(), ""))) {
        throw std::runtime_error("tee scene setup failed");
      }
    };
    road(Waypoint{-60.0, 0.0}, Waypoint{60.0, 0.0});  // road "1", the target
    road(Waypoint{0.0, -50.0}, Waypoint{0.0, -10.0}); // road "2", attaching end (0,-10)
  }
};

TEST(CreateJunctionTool, SideClickAfterOneEndTeesIntoTheRoadBody) {
  TeeScene scene;
  CreateJunctionTool tool(scene.document);
  tool.activate();

  // Select the side road's end, then click the main road's BODY near its
  // midpoint — the side anchor, not an endpoint toggle.
  ASSERT_TRUE(tool.mouse_press(click_at(0.0, -10.0)));
  EXPECT_EQ(tool.selected_count(), 1U);
  ASSERT_TRUE(tool.mouse_press(click_at(1.0, 1.5))); // ~s=61 on road "1"
  EXPECT_EQ(tool.selected_count(), 1U);              // an anchor, not an end

  ASSERT_TRUE(tool.key_press(Qt::Key_Return, Qt::NoModifier));
  EXPECT_EQ(scene.document.network().junction_count(), 1U);
  const JunctionId junction = scene.document.network().find_junction("1");
  ASSERT_TRUE(junction.is_valid());
  EXPECT_EQ(scene.document.network().junction(junction)->arms.size(), 3U);

  // ONE undo entry returns to the pre-tee network.
  const std::size_t roads_after = scene.document.network().road_count();
  EXPECT_GT(roads_after, 2U);
  scene.document.undo_stack()->undo();
  EXPECT_EQ(scene.document.network().junction_count(), 0U);
  EXPECT_EQ(scene.document.network().road_count(), 2U);
  scene.document.undo_stack()->redo();
  EXPECT_EQ(scene.document.network().junction_count(), 1U);
  EXPECT_EQ(scene.document.network().road_count(), roads_after);
}

TEST(CreateJunctionTool, EscClearsTheSideAnchor) {
  TeeScene scene;
  CreateJunctionTool tool(scene.document);
  tool.activate();

  ASSERT_TRUE(tool.mouse_press(click_at(0.0, -10.0)));
  ASSERT_TRUE(tool.mouse_press(click_at(1.0, 1.5)));
  ASSERT_TRUE(tool.key_press(Qt::Key_Escape, Qt::NoModifier));

  // Enter after Esc must do nothing — the whole session was cleared.
  ASSERT_TRUE(tool.key_press(Qt::Key_Return, Qt::NoModifier));
  EXPECT_EQ(scene.document.network().junction_count(), 0U);
  EXPECT_EQ(scene.document.network().road_count(), 2U);
  EXPECT_TRUE(scene.document.undo_stack()->isClean() ||
              scene.document.network().junction_count() == 0U);
}

TEST(CreateJunctionTool, EndpointSnapWinsOverTheSideAnchor) {
  TeeScene scene;
  CreateJunctionTool tool(scene.document);
  tool.activate();

  // Clicking ON an endpoint with one end already selected toggles ends —
  // it must never read as a tee into the road body.
  ASSERT_TRUE(tool.mouse_press(click_at(0.0, -10.0)));
  ASSERT_TRUE(tool.mouse_press(click_at(-60.0, 0.0))); // main road's START
  EXPECT_EQ(tool.selected_count(), 2U);
}

// ---- tee discoverability (issue #103 §8): preview overlay + status span ----

TEST(CreateJunctionTool, TeePreviewShowsAnchorGhostAndReplacedSpan) {
  TeeScene scene;
  CreateJunctionTool tool(scene.document);
  tool.activate();

  ASSERT_TRUE(tool.mouse_press(click_at(0.0, -10.0))); // select the side end
  ASSERT_TRUE(tool.mouse_press(click_at(1.0, 1.5)));   // anchor ~s=61 on "1"

  const auto preview = tool.preview();
  // Anchor marker on the reference line plus the selected end's marker.
  ASSERT_GE(preview.handles.size(), 2U);
  // Ghost dashes + span polyline + two end ticks all land in line_positions.
  ASSERT_FALSE(preview.line_positions.empty());

  // The span highlight runs along the target's reference line (y = 0) and
  // covers [s−gap, s+gap] around the anchor.
  const RoadId target = scene.document.network().find_road("1");
  const roadmaker::RoadEnd side_end{scene.document.network().find_road("2"),
                                    roadmaker::ContactPoint::End};
  const double gap =
      roadmaker::edit::t_attach_gap(scene.document.network(), side_end, target, 61.0);
  ASSERT_GT(gap, 0.0);
  double span_min_x = 1e9;
  double span_max_x = -1e9;
  bool ghost_seen = false;
  for (std::size_t i = 0; i + 2 < preview.line_positions.size(); i += 3) {
    const double x = preview.line_positions[i];
    const double y = preview.line_positions[i + 1];
    if (std::abs(y) < 1e-6) { // on the target's reference line
      span_min_x = std::min(span_min_x, x);
      span_max_x = std::max(span_max_x, x);
    }
    if (y < -1.0) { // between the branch end and the anchor
      ghost_seen = true;
    }
  }
  // Anchor x = 1 (s = 61 on a road starting at x = −60).
  EXPECT_NEAR(span_min_x, 1.0 - gap, 1.5);
  EXPECT_NEAR(span_max_x, 1.0 + gap, 1.5);
  EXPECT_TRUE(ghost_seen) << "dashed ghost line from the selected end is missing";
}

TEST(CreateJunctionTool, TeePreviewClearsOnEscape) {
  TeeScene scene;
  CreateJunctionTool tool(scene.document);
  tool.activate();

  ASSERT_TRUE(tool.mouse_press(click_at(0.0, -10.0)));
  ASSERT_TRUE(tool.mouse_press(click_at(1.0, 1.5)));
  ASSERT_FALSE(tool.preview().line_positions.empty());

  ASSERT_TRUE(tool.key_press(Qt::Key_Escape, Qt::NoModifier));
  const auto preview = tool.preview();
  EXPECT_TRUE(preview.line_positions.empty());
  EXPECT_TRUE(preview.handles.empty());
}

TEST(CreateJunctionTool, TeeStatusMessageNamesTheReplacedSpan) {
  TeeScene scene;
  CreateJunctionTool tool(scene.document);
  tool.activate();

  QSignalSpy status_spy(&tool, &roadmaker::editor::Tool::status_message);
  ASSERT_TRUE(tool.mouse_press(click_at(0.0, -10.0)));
  ASSERT_TRUE(tool.mouse_press(click_at(1.0, 1.5)));
  ASSERT_FALSE(status_spy.isEmpty());
  const QString last = status_spy.constLast().constFirst().toString();
  EXPECT_TRUE(last.contains("replaces s=")) << last.toStdString();
}

// The toolbar tooltip documents BOTH flows (endpoint junction and tee) — the
// GW-1 gate found the tee flow undiscoverable without it.
TEST(CreateJunctionTool, TooltipDocumentsBothFlows) {
  QUndoStack undo_stack;
  roadmaker::editor::Actions actions(undo_stack);
  const QString tip = actions.tool_create_junction->toolTip();
  EXPECT_TRUE(tip.contains("2+ road ends")) << tip.toStdString();
  EXPECT_TRUE(tip.contains("tee into it")) << tip.toStdString();
  EXPECT_TRUE(tip.contains("Esc cancels")) << tip.toStdString();
}
