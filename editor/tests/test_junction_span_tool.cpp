// Copyright 2026 Robomous
// SPDX-License-Identifier: Apache-2.0

// Junction Span tool (p4-s4, issue #319): headless ToolEvent sequences drag an
// [s0, s1] range on a road, stage it, and confirm with Enter — asserting the
// stage-and-confirm discipline (nothing enters the network before Enter), the
// ONE command a confirm produces, that Esc leaves the network byte-identical,
// and that a connecting road is refused before the kernel ever sees it.
// Runs under QT_QPA_PLATFORM=offscreen like every other editor test.

#include "roadmaker/edit/operations.hpp"
#include "roadmaker/road/junction.hpp"
#include "roadmaker/road/network.hpp"
#include "roadmaker/xodr/writer.hpp"

#include <gtest/gtest.h>

#include <QSignalSpy>
#include <QUndoStack>
#include <array>
#include <stdexcept>
#include <string>
#include <vector>

#include "document/document.hpp"
#include "document/selection_model.hpp"
#include "tools/junction_span_tool.hpp"
#include "viewport/picking.hpp"

namespace roadmaker::editor {
namespace {

using roadmaker::ContactPoint;
using roadmaker::JunctionId;
using roadmaker::LaneProfile;
using roadmaker::RoadEnd;
using roadmaker::RoadId;
using roadmaker::SpanArm;
using roadmaker::Waypoint;

std::string xodr(const Document& document) {
  auto text = roadmaker::write_xodr(document.network());
  if (!text) {
    throw std::runtime_error(text.error().message);
  }
  return *text;
}

/// Two parallel straight roads along +x, 20 m apart — the parallel-carriageway
/// case a crossing span junction covers.
struct Scene {
  Document document;
  RoadId north;
  RoadId south;
  int base_count = 0;
  std::string base_xodr;

  Scene() {
    south = make(0.0, 0.0, 200.0, 0.0, "1");
    north = make(0.0, 20.0, 200.0, 20.0, "2");
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

  [[nodiscard]] std::vector<JunctionId> span_junctions() const {
    std::vector<JunctionId> found;
    document.network().for_each_junction([&](JunctionId id, const roadmaker::Junction& junction) {
      if (!junction.spans.empty()) {
        found.push_back(id);
      }
    });
    return found;
  }
};

ToolEvent at(double x, double y, Qt::MouseButtons buttons = Qt::NoButton) {
  ToolEvent event;
  event.world_x = x;
  event.world_y = y;
  event.buttons = buttons;
  return event;
}

/// One press-drag-release along a road at constant y.
void drag(JunctionSpanTool& tool, double x0, double x1, double y) {
  ASSERT_TRUE(tool.mouse_press(at(x0, y, Qt::LeftButton)));
  ASSERT_TRUE(tool.mouse_move(at((x0 + x1) / 2.0, y, Qt::LeftButton)));
  ASSERT_TRUE(tool.mouse_move(at(x1, y, Qt::LeftButton)));
  ASSERT_TRUE(tool.mouse_release(at(x1, y)));
}

} // namespace

TEST(JunctionSpanTool, DragStagesASpanWithoutTouchingTheNetwork) {
  Scene scene;
  SelectionModel selection(scene.document);
  JunctionSpanTool tool(scene.document, selection);
  tool.activate();

  drag(tool, 40.0, 60.0, 0.0);

  ASSERT_EQ(tool.staged_count(), 1U);
  EXPECT_EQ(tool.staged_spans().front().road, scene.south);
  EXPECT_NEAR(tool.staged_spans().front().s_start, 40.0, 0.5);
  EXPECT_NEAR(tool.staged_spans().front().s_end, 60.0, 0.5);
  EXPECT_FALSE(tool.preview().line_positions.empty()) << "the staged span is drawn";
  EXPECT_EQ(scene.document.undo_stack()->index(), scene.base_count)
      << "staging is tool-local — nothing enters the network before Enter";
  EXPECT_EQ(xodr(scene.document), scene.base_xodr);
}

TEST(JunctionSpanTool, DragDragEnterProducesExactlyOneCommand) {
  Scene scene;
  SelectionModel selection(scene.document);
  JunctionSpanTool tool(scene.document, selection);
  tool.activate();

  drag(tool, 40.0, 60.0, 0.0);
  drag(tool, 40.0, 60.0, 20.0);
  ASSERT_EQ(tool.staged_count(), 2U);

  ASSERT_TRUE(tool.key_press(Qt::Key_Return, Qt::NoModifier));

  EXPECT_EQ(scene.document.undo_stack()->index(), scene.base_count + 1)
      << "two staged spans still commit as ONE command";
  const std::vector<JunctionId> created = scene.span_junctions();
  ASSERT_EQ(created.size(), 1U);
  const roadmaker::Junction* junction = scene.document.network().junction(created.front());
  ASSERT_NE(junction, nullptr);
  EXPECT_EQ(junction->spans.size(), 2U);
  EXPECT_TRUE(junction->arms.empty()) << "arms-xor-spans";
  EXPECT_TRUE(junction->locked) << "a virtual junction is locked structurally";
  // The tool selects what it made, so the Properties pane lands on it.
  EXPECT_EQ(selection.primary().junction, created.front());
  EXPECT_EQ(tool.staged_count(), 0U) << "a confirm resets the session";

  // Undo/redo round-trips: restore-in-place reuses the JunctionId, so the held
  // selection survives.
  scene.document.undo_stack()->undo();
  EXPECT_TRUE(scene.span_junctions().empty());
  EXPECT_EQ(xodr(scene.document), scene.base_xodr);
  scene.document.undo_stack()->redo();
  ASSERT_EQ(scene.span_junctions().size(), 1U);
  EXPECT_EQ(scene.span_junctions().front(), created.front());
}

TEST(JunctionSpanTool, EscapeLeavesTheNetworkByteIdentical) {
  Scene scene;
  SelectionModel selection(scene.document);
  JunctionSpanTool tool(scene.document, selection);
  tool.activate();

  drag(tool, 40.0, 60.0, 0.0);
  drag(tool, 30.0, 90.0, 20.0);
  ASSERT_EQ(tool.staged_count(), 2U);

  ASSERT_TRUE(tool.key_press(Qt::Key_Escape, Qt::NoModifier));
  EXPECT_EQ(tool.staged_count(), 0U);
  EXPECT_TRUE(tool.preview().line_positions.empty());
  EXPECT_EQ(scene.document.undo_stack()->index(), scene.base_count);
  EXPECT_EQ(xodr(scene.document), scene.base_xodr);
}

TEST(JunctionSpanTool, ReDraggingAStagedRoadReplacesItsSpan) {
  Scene scene;
  SelectionModel selection(scene.document);
  JunctionSpanTool tool(scene.document, selection);
  tool.activate();

  drag(tool, 40.0, 60.0, 0.0);
  drag(tool, 100.0, 140.0, 0.0);

  ASSERT_EQ(tool.staged_count(), 1U) << "the same road stages once";
  EXPECT_NEAR(tool.staged_spans().front().s_start, 100.0, 0.5);
  EXPECT_NEAR(tool.staged_spans().front().s_end, 140.0, 0.5);
}

TEST(JunctionSpanTool, AClickWithoutTravelStagesNothing) {
  Scene scene;
  SelectionModel selection(scene.document);
  JunctionSpanTool tool(scene.document, selection);
  tool.activate();

  ASSERT_TRUE(tool.mouse_press(at(50.0, 0.0, Qt::LeftButton)));
  ASSERT_TRUE(tool.mouse_release(at(50.0, 0.0)));
  EXPECT_EQ(tool.staged_count(), 0U) << "a span needs a length";

  // Enter with nothing staged authors nothing rather than pushing a refused
  // command.
  ASSERT_TRUE(tool.key_press(Qt::Key_Return, Qt::NoModifier));
  EXPECT_EQ(scene.document.undo_stack()->index(), scene.base_count);
  EXPECT_EQ(xodr(scene.document), scene.base_xodr);
}

TEST(JunctionSpanTool, AConnectingRoadIsRefusedBeforeTheKernelSeesIt) {
  Scene scene;
  // Tee the two roads into a junction so the network holds connecting roads.
  const RoadId stem = scene.make(100.0, -60.0, 100.0, -12.0, "3");
  ASSERT_TRUE(scene.document.push_command(roadmaker::edit::attach_t_junction(
      scene.document.network(), RoadEnd{stem, ContactPoint::End}, scene.south, 100.0)));

  RoadId connecting;
  double cx = 0.0;
  double cy = 0.0;
  scene.document.network().for_each_road([&](RoadId id, const roadmaker::Road& road) {
    if (road.junction.is_valid() && !connecting.is_valid() && !road.plan_view.empty()) {
      connecting = id;
      const roadmaker::PathPoint mid = road.plan_view.evaluate(road.plan_view.length() / 2.0);
      cx = mid.x;
      cy = mid.y;
    }
  });
  ASSERT_TRUE(connecting.is_valid()) << "the tee must have generated connecting roads";

  SelectionModel selection(scene.document);
  JunctionSpanTool tool(scene.document, selection);
  tool.activate();
  QSignalSpy toasts(&tool, &Tool::toast_requested);

  // The press lands on the connecting road's own body — consumed, refused, and
  // no drag started.
  // The viewport's pick is what tells a connecting road from the through road
  // it overlaps, so the event carries one — exactly as ViewportWidget supplies.
  ToolEvent press = at(cx, cy, Qt::LeftButton);
  press.pick = PickHit{.road = connecting};
  ASSERT_TRUE(tool.mouse_press(press));
  EXPECT_FALSE(tool.dragging());
  EXPECT_EQ(tool.staged_count(), 0U);
  EXPECT_GE(toasts.count(), 1) << "the refusal is cued, not silent";
}

} // namespace roadmaker::editor
