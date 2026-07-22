// Copyright 2026 Robomous
// SPDX-License-Identifier: Apache-2.0

// Stop Line tool (p4-s3, issue #318): headless ToolEvent sequences hover,
// select and drag a junction arm's stop line, asserting the tool-local
// sub-selection, the ONE-undo-entry drag discipline (press-with-no-move and Esc
// author nothing), the authored kernel values, and the F flip. Runs under
// QT_QPA_PLATFORM=offscreen like every other editor test.

#include "roadmaker/edit/operations.hpp"
#include "roadmaker/mesh/junction_stoplines.hpp"
#include "roadmaker/road/junction.hpp"
#include "roadmaker/road/network.hpp"
#include "roadmaker/xodr/writer.hpp"

#include <gtest/gtest.h>

#include <QSignalSpy>
#include <QUndoStack>
#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>
#include <string>
#include <vector>

#include "document/document.hpp"
#include "document/selection_model.hpp"
#include "tools/stopline_tool.hpp"

namespace roadmaker::editor {
namespace {

using roadmaker::ContactPoint;
using roadmaker::JunctionId;
using roadmaker::JunctionStopLineInfo;
using roadmaker::kStopLineDefaultDistance;
using roadmaker::LaneProfile;
using roadmaker::RoadEnd;
using roadmaker::RoadId;
using roadmaker::StopLine;
using roadmaker::Waypoint;

std::string xodr(const Document& document) {
  auto text = roadmaker::write_xodr(document.network());
  if (!text) {
    throw std::runtime_error(text.error().message);
  }
  return *text;
}

/// Four arms meeting at the origin, joined into a junction. Roomy arms (they
/// stop 12 m short) so an authored setback stays clear of the clamp.
struct Scene {
  Document document;
  JunctionId junction;
  int base_count = 0;
  std::string base_xodr;

  Scene() {
    const RoadId west = make(-60.0, 0.0, -12.0, 0.0, "1");
    const RoadId east = make(60.0, 0.0, 12.0, 0.0, "2");
    const RoadId south = make(0.0, -60.0, 0.0, -12.0, "3");
    const RoadId north = make(0.0, 60.0, 0.0, 12.0, "4");
    const std::array<RoadEnd, 4> ends{RoadEnd{west, ContactPoint::End},
                                      RoadEnd{east, ContactPoint::End},
                                      RoadEnd{south, ContactPoint::End},
                                      RoadEnd{north, ContactPoint::End}};
    if (!document.push_command(roadmaker::edit::create_junction(document.network(), ends))) {
      throw std::runtime_error("junction create failed");
    }
    document.network().for_each_junction(
        [&](JunctionId id, const roadmaker::Junction&) { junction = id; });
    base_count = document.undo_stack()->index();
    base_xodr = xodr(document);
  }

  RoadId make(double x0, double y0, double x1, double y1, const char* odr) {
    if (!document.push_command(
            roadmaker::edit::create_road({Waypoint{.x = x0, .y = y0}, Waypoint{.x = x1, .y = y1}},
                                         LaneProfile::two_lane_default(),
                                         odr))) {
      throw std::runtime_error("road create failed");
    }
    return document.network().find_road(odr);
  }

  [[nodiscard]] std::vector<JunctionStopLineInfo> lines() const {
    return junction_stoplines(document.network(), junction);
  }

  /// The first solved line (the tests only ever need one).
  [[nodiscard]] JunctionStopLineInfo line() const {
    const std::vector<JunctionStopLineInfo> all = lines();
    if (all.empty()) {
      throw std::runtime_error("the cross junction solved no stop lines");
    }
    return all.front();
  }

  /// The authored record for `info`'s arm, or nullptr when none exists.
  [[nodiscard]] const StopLine* authored(const JunctionStopLineInfo& info) const {
    const roadmaker::Junction* junc = document.network().junction(junction);
    if (junc == nullptr) {
      return nullptr;
    }
    for (const StopLine& stored : junc->stoplines) {
      if (stored.arm == info.arm) {
        return &stored;
      }
    }
    return nullptr;
  }

  /// The world midpoint of a solved band — where the cursor grabs it.
  [[nodiscard]] static std::array<double, 2> grab(const JunctionStopLineInfo& info) {
    return {(info.left[0] + info.right[0]) / 2.0, (info.left[1] + info.right[1]) / 2.0};
  }
};

ToolEvent at(double x, double y, Qt::MouseButtons buttons = Qt::NoButton) {
  ToolEvent event;
  event.world_x = x;
  event.world_y = y;
  event.buttons = buttons;
  return event;
}

} // namespace

TEST(StopLineTool, EveryArmOffersALineWithNothingAuthored) {
  Scene scene;
  EXPECT_EQ(scene.lines().size(), 4U) << "stop lines are derived — nothing to create";
  for (const JunctionStopLineInfo& info : scene.lines()) {
    EXPECT_FALSE(info.authored);
    EXPECT_DOUBLE_EQ(info.distance, kStopLineDefaultDistance);
  }
}

TEST(StopLineTool, HoverResolvesTheBandUnderTheCursor) {
  Scene scene;
  SelectionModel selection(scene.document);
  StopLineTool tool(scene.document, selection);
  tool.activate();

  const JunctionStopLineInfo info = scene.line();
  const std::array<double, 2> on = Scene::grab(info);
  EXPECT_FALSE(tool.mouse_move(at(on[0], on[1]))) << "hover never consumes the event";
  EXPECT_FALSE(tool.preview().line_positions.empty()) << "the hovered band is drawn";

  // Far away: nothing hovered, nothing drawn.
  EXPECT_FALSE(tool.mouse_move(at(500.0, 500.0)));
  EXPECT_TRUE(tool.preview().line_positions.empty());
}

TEST(StopLineTool, ClickMakesALineActiveAndMirrorsItsJunction) {
  Scene scene;
  SelectionModel selection(scene.document);
  StopLineTool tool(scene.document, selection);
  tool.activate();
  QSignalSpy spy(&tool, &StopLineTool::stopline_selection_changed);

  const JunctionStopLineInfo info = scene.line();
  const std::array<double, 2> on = Scene::grab(info);
  ASSERT_TRUE(tool.mouse_press(at(on[0], on[1], Qt::LeftButton)));

  ASSERT_TRUE(tool.active_stopline().has_value());
  EXPECT_EQ(tool.active_stopline()->arm, info.arm);
  EXPECT_EQ(spy.count(), 1);
  // There is no stop-line selection entry, so the JUNCTION is what the rest of
  // the UI follows.
  EXPECT_EQ(selection.primary().junction, scene.junction);
  EXPECT_EQ(scene.document.undo_stack()->index(), scene.base_count) << "selecting authors nothing";
}

TEST(StopLineTool, DragCommitsExactlyOneCommand) {
  Scene scene;
  SelectionModel selection(scene.document);
  StopLineTool tool(scene.document, selection);
  tool.activate();

  const JunctionStopLineInfo info = scene.line();
  const std::array<double, 2> on = Scene::grab(info);
  ASSERT_TRUE(tool.mouse_press(at(on[0], on[1], Qt::LeftButton))); // selects
  // The band is its own handle: pressing it again arms the drag.
  ASSERT_TRUE(tool.mouse_press(at(on[0], on[1], Qt::LeftButton)));
  EXPECT_FALSE(tool.mouse_press(at(on[0], on[1], Qt::LeftButton)))
      << "a further press while already armed is ignored";

  // Drag 5 m further from the junction along the west arm (which runs -x → +x
  // and meets the junction at its End, so a smaller x is a bigger setback).
  ASSERT_TRUE(tool.mouse_move(at(on[0] - 5.0, on[1], Qt::LeftButton)));
  ASSERT_TRUE(tool.mouse_move(at(on[0] - 5.0, on[1], Qt::LeftButton)));
  ASSERT_TRUE(tool.mouse_release(at(on[0] - 5.0, on[1])));

  EXPECT_EQ(scene.document.undo_stack()->index(), scene.base_count + 1)
      << "a whole drag is ONE undo entry, however many move frames it took";

  const StopLine* record = scene.authored(info);
  ASSERT_NE(record, nullptr);
  ASSERT_TRUE(record->distance.has_value());
  EXPECT_NEAR(*record->distance, kStopLineDefaultDistance + 5.0, 0.2);

  // One undo restores the file byte for byte.
  scene.document.undo_stack()->undo();
  EXPECT_EQ(xodr(scene.document), scene.base_xodr);
}

TEST(StopLineTool, PressWithoutMovingAuthorsNothing) {
  Scene scene;
  SelectionModel selection(scene.document);
  StopLineTool tool(scene.document, selection);
  tool.activate();

  const std::array<double, 2> on = Scene::grab(scene.line());
  ASSERT_TRUE(tool.mouse_press(at(on[0], on[1], Qt::LeftButton))); // selects
  ASSERT_TRUE(tool.mouse_press(at(on[0], on[1], Qt::LeftButton))); // arms
  ASSERT_TRUE(tool.mouse_release(at(on[0], on[1])));

  EXPECT_EQ(scene.document.undo_stack()->index(), scene.base_count);
  EXPECT_EQ(xodr(scene.document), scene.base_xodr);
}

TEST(StopLineTool, EscapeCancelsADragByteIdentically) {
  Scene scene;
  SelectionModel selection(scene.document);
  StopLineTool tool(scene.document, selection);
  tool.activate();

  const std::array<double, 2> on = Scene::grab(scene.line());
  ASSERT_TRUE(tool.mouse_press(at(on[0], on[1], Qt::LeftButton))); // selects
  ASSERT_TRUE(tool.mouse_press(at(on[0], on[1], Qt::LeftButton))); // arms
  ASSERT_TRUE(tool.mouse_move(at(on[0] - 6.0, on[1], Qt::LeftButton)));
  EXPECT_TRUE(tool.dragging());

  ASSERT_TRUE(tool.key_press(Qt::Key_Escape, Qt::NoModifier));
  EXPECT_FALSE(tool.dragging());
  EXPECT_EQ(scene.document.undo_stack()->index(), scene.base_count);
  EXPECT_EQ(xodr(scene.document), scene.base_xodr) << "a cancelled drag leaves no trace";
}

TEST(StopLineTool, FFlipsTheActiveLine) {
  Scene scene;
  SelectionModel selection(scene.document);
  StopLineTool tool(scene.document, selection);
  tool.activate();

  const JunctionStopLineInfo info = scene.line();
  const std::array<double, 2> on = Scene::grab(info);
  ASSERT_TRUE(tool.mouse_press(at(on[0], on[1], Qt::LeftButton)));

  ASSERT_TRUE(tool.key_press(Qt::Key_F, Qt::NoModifier));
  EXPECT_EQ(scene.document.undo_stack()->index(), scene.base_count + 1);
  const StopLine* record = scene.authored(info);
  ASSERT_NE(record, nullptr);
  EXPECT_TRUE(record->flipped);
  ASSERT_TRUE(tool.active_stopline_info().has_value());
  EXPECT_TRUE(tool.active_stopline_info()->flipped);

  // Flipping back normalizes the record away, so the file matches the original.
  ASSERT_TRUE(tool.key_press(Qt::Key_F, Qt::NoModifier));
  EXPECT_EQ(scene.authored(info), nullptr);
  EXPECT_EQ(xodr(scene.document), scene.base_xodr);
}

TEST(StopLineTool, FDoesNothingWithoutAnActiveLine) {
  Scene scene;
  SelectionModel selection(scene.document);
  StopLineTool tool(scene.document, selection);
  tool.activate();

  EXPECT_FALSE(tool.key_press(Qt::Key_F, Qt::NoModifier));
  EXPECT_EQ(scene.document.undo_stack()->index(), scene.base_count);
}

TEST(StopLineTool, DragIsClampedToTheArmRoad) {
  Scene scene;
  SelectionModel selection(scene.document);
  StopLineTool tool(scene.document, selection);
  tool.activate();

  const JunctionStopLineInfo info = scene.line();
  const std::array<double, 2> on = Scene::grab(info);
  ASSERT_TRUE(tool.mouse_press(at(on[0], on[1], Qt::LeftButton))); // selects
  ASSERT_TRUE(tool.mouse_press(at(on[0], on[1], Qt::LeftButton))); // arms
  // Way past the far end of the arm.
  ASSERT_TRUE(tool.mouse_move(at(on[0] - 5000.0, on[1], Qt::LeftButton)));
  ASSERT_TRUE(tool.mouse_release(at(on[0] - 5000.0, on[1])));

  const StopLine* record = scene.authored(info);
  ASSERT_NE(record, nullptr);
  ASSERT_TRUE(record->distance.has_value());
  EXPECT_LE(*record->distance, info.max_distance + 1e-9)
      << "the gesture never ASKS for a setback the road cannot hold";
  EXPECT_GE(*record->distance, 0.0);
}

TEST(StopLineTool, EscapeWithNoDragClearsTheSelection) {
  Scene scene;
  SelectionModel selection(scene.document);
  StopLineTool tool(scene.document, selection);
  tool.activate();

  const std::array<double, 2> on = Scene::grab(scene.line());
  ASSERT_TRUE(tool.mouse_press(at(on[0], on[1], Qt::LeftButton)));
  ASSERT_TRUE(tool.active_stopline().has_value());

  ASSERT_TRUE(tool.key_press(Qt::Key_Escape, Qt::NoModifier));
  EXPECT_FALSE(tool.active_stopline().has_value());
}

// --- locked junctions (p4-s4, issue #319) ------------------------------------
//
// Locking guards the AUTOMATIC regeneration loop; it must not freeze the
// junction's authored values. Editing a locked junction's stop line is exactly
// the case that motivates the lock, so it keeps the same drag discipline.

TEST(StopLineTool, DragOnALockedJunctionStillCommitsOneCommand) {
  Scene scene;
  ASSERT_TRUE(scene.document.push_command(
      roadmaker::edit::set_junction_locked(scene.document.network(), scene.junction, true)));
  const int base = scene.document.undo_stack()->index();

  SelectionModel selection(scene.document);
  StopLineTool tool(scene.document, selection);
  tool.activate();

  const JunctionStopLineInfo info = scene.line();
  const std::array<double, 2> on = Scene::grab(info);
  ASSERT_TRUE(tool.mouse_press(at(on[0], on[1], Qt::LeftButton)));
  ASSERT_TRUE(tool.mouse_press(at(on[0], on[1], Qt::LeftButton)));
  ASSERT_TRUE(tool.mouse_move(at(on[0] - 5.0, on[1], Qt::LeftButton)));
  ASSERT_TRUE(tool.mouse_move(at(on[0] - 5.0, on[1], Qt::LeftButton)));
  ASSERT_TRUE(tool.mouse_release(at(on[0] - 5.0, on[1])));

  EXPECT_EQ(scene.document.undo_stack()->index(), base + 1);
  const StopLine* record = scene.authored(info);
  ASSERT_NE(record, nullptr);
  ASSERT_TRUE(record->distance.has_value());
  EXPECT_NEAR(*record->distance, kStopLineDefaultDistance + 5.0, 0.2);
  EXPECT_TRUE(scene.document.network().junction(scene.junction)->locked)
      << "editing an authored value never unlocks the junction";
}

TEST(StopLineTool, CancelledDragOnALockedJunctionIsByteIdentical) {
  Scene scene;
  ASSERT_TRUE(scene.document.push_command(
      roadmaker::edit::set_junction_locked(scene.document.network(), scene.junction, true)));
  const int base = scene.document.undo_stack()->index();
  const std::string locked_xodr = xodr(scene.document);

  SelectionModel selection(scene.document);
  StopLineTool tool(scene.document, selection);
  tool.activate();

  const JunctionStopLineInfo info = scene.line();
  const std::array<double, 2> on = Scene::grab(info);
  ASSERT_TRUE(tool.mouse_press(at(on[0], on[1], Qt::LeftButton)));
  ASSERT_TRUE(tool.mouse_press(at(on[0], on[1], Qt::LeftButton)));
  ASSERT_TRUE(tool.mouse_move(at(on[0] - 5.0, on[1], Qt::LeftButton)));
  ASSERT_TRUE(tool.key_press(Qt::Key_Escape, Qt::NoModifier));

  EXPECT_FALSE(scene.document.preview_active());
  EXPECT_EQ(scene.document.undo_stack()->index(), base);
  EXPECT_EQ(xodr(scene.document), locked_xodr);
}

} // namespace roadmaker::editor
