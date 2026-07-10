// NodeDragTool prototype (gate issue #37): headless ToolEvent sequences
// drive press → drag → release/Esc and the tests assert on the undo stack,
// the serialized network, snapping, and preview geometry — the M2 tool test
// seam (docs/design/m2/01_editing_framework.md §4).

#include "roadmaker/edit/operations.hpp"
#include "roadmaker/edit/snap.hpp"
#include "roadmaker/tol.hpp"
#include "roadmaker/xodr/writer.hpp"

#include <gtest/gtest.h>

#include <QSignalSpy>
#include <cstddef>
#include <stdexcept>
#include <string>

#include "document/document.hpp"
#include "tools/node_drag_tool.hpp"

using roadmaker::RoadId;
using roadmaker::Waypoint;
using roadmaker::editor::Document;
using roadmaker::editor::NodeDragTool;
using roadmaker::editor::ToolEvent;

namespace {

std::string xodr(const Document& document) {
  auto text = roadmaker::write_xodr(document.network());
  if (!text) {
    throw std::runtime_error(text.error().message);
  }
  return *text;
}

ToolEvent at(double x, double y, Qt::MouseButtons buttons = Qt::NoButton) {
  ToolEvent event;
  event.world_x = x;
  event.world_y = y;
  event.buttons = buttons;
  return event;
}

/// Document with the dragged road (0,0)-(50,10)-(100,0) and a snap target
/// road starting at (120,0); base state captured after both creates.
/// Document is a QObject (pinned in place), hence setup in the constructor.
struct Scene {
  Document document;
  RoadId dragged;
  int base_count = 0;
  std::string base_xodr;

  Scene() {
    auto road = document.push_command(
        roadmaker::edit::create_road({Waypoint{.x = 0.0, .y = 0.0},
                                      Waypoint{.x = 50.0, .y = 10.0},
                                      Waypoint{.x = 100.0, .y = 0.0}},
                                     roadmaker::LaneProfile::two_lane_default(),
                                     "Dragged"));
    auto other = document.push_command(roadmaker::edit::create_road(
        {Waypoint{.x = 120.0, .y = 0.0}, Waypoint{.x = 200.0, .y = 0.0}},
        roadmaker::LaneProfile::two_lane_default(),
        "Other"));
    if (!road || !other) {
      throw std::runtime_error("scene setup failed");
    }
    document.network().for_each_road([&](RoadId id, const roadmaker::Road& r) {
      if (r.name == "Dragged") {
        dragged = id;
      }
    });
    if (!dragged.is_valid()) {
      throw std::runtime_error("dragged road not found");
    }
    base_count = document.undo_stack()->count();
    base_xodr = xodr(document);
  }
};

Waypoint node(const Document& document, RoadId road, std::size_t index) {
  const roadmaker::Road* road_ptr = document.network().road(road);
  if (road_ptr == nullptr || !road_ptr->authoring_waypoints) {
    throw std::runtime_error("road lost its waypoints");
  }
  return (*road_ptr->authoring_waypoints)[index];
}

} // namespace

TEST(NodeDragTool, PressConsumesOnlyNearANode) {
  Scene scene;
  NodeDragTool tool(scene.document);

  EXPECT_FALSE(tool.mouse_press(at(300.0, 300.0, Qt::LeftButton)));
  EXPECT_FALSE(tool.dragging());

  // Right button never starts a drag (RMB is camera orbit in the M2 map).
  EXPECT_FALSE(tool.mouse_press(at(50.5, 10.5, Qt::RightButton)));

  EXPECT_TRUE(tool.mouse_press(at(50.5, 10.5, Qt::LeftButton)));
  EXPECT_TRUE(tool.dragging());
  EXPECT_FALSE(scene.document.preview_active()); // preview starts on first move
}

TEST(NodeDragTool, DragRefitsLiveAndReleaseCommitsOnce) {
  Scene scene;
  NodeDragTool tool(scene.document);
  QSignalSpy preview_spy(&tool, &roadmaker::editor::Tool::preview_changed);

  ASSERT_TRUE(tool.mouse_press(at(50.5, 10.5, Qt::LeftButton)));
  ASSERT_TRUE(tool.mouse_move(at(50.0, 25.0, Qt::LeftButton)));
  EXPECT_EQ(node(scene.document, scene.dragged, 1), (Waypoint{.x = 50.0, .y = 25.0}));
  ASSERT_TRUE(tool.mouse_move(at(50.0, 40.0, Qt::LeftButton)));
  EXPECT_EQ(node(scene.document, scene.dragged, 1), (Waypoint{.x = 50.0, .y = 40.0}));

  EXPECT_TRUE(scene.document.preview_active());
  EXPECT_EQ(scene.document.undo_stack()->count(), scene.base_count);
  EXPECT_FALSE(tool.preview().empty());
  EXPECT_GE(preview_spy.count(), 3); // press + each move

  ASSERT_TRUE(tool.mouse_release(at(50.0, 40.0)));
  EXPECT_FALSE(tool.dragging());
  EXPECT_FALSE(scene.document.preview_active());
  EXPECT_EQ(scene.document.undo_stack()->count(), scene.base_count + 1);
  EXPECT_TRUE(tool.preview().empty());
  const std::string committed = xodr(scene.document);

  scene.document.undo_stack()->undo();
  EXPECT_EQ(xodr(scene.document), scene.base_xodr);
  scene.document.undo_stack()->redo();
  EXPECT_EQ(xodr(scene.document), committed);
}

TEST(NodeDragTool, EscapeCancelsByteIdentical) {
  Scene scene;
  NodeDragTool tool(scene.document);

  ASSERT_TRUE(tool.mouse_press(at(50.5, 10.5, Qt::LeftButton)));
  ASSERT_TRUE(tool.mouse_move(at(70.0, 55.0, Qt::LeftButton)));
  ASSERT_TRUE(tool.key_press(Qt::Key_Escape, Qt::NoModifier));

  EXPECT_FALSE(tool.dragging());
  EXPECT_FALSE(scene.document.preview_active());
  EXPECT_EQ(xodr(scene.document), scene.base_xodr);
  EXPECT_EQ(scene.document.undo_stack()->count(), scene.base_count);

  // The drag is over: further events are not consumed.
  EXPECT_FALSE(tool.mouse_move(at(75.0, 60.0, Qt::LeftButton)));
  EXPECT_FALSE(tool.mouse_release(at(75.0, 60.0)));
  EXPECT_FALSE(tool.key_press(Qt::Key_Escape, Qt::NoModifier));
}

TEST(NodeDragTool, ClickWithoutMovePushesNothing) {
  Scene scene;
  NodeDragTool tool(scene.document);

  ASSERT_TRUE(tool.mouse_press(at(50.5, 10.5, Qt::LeftButton)));
  ASSERT_TRUE(tool.mouse_release(at(50.5, 10.5)));

  EXPECT_EQ(scene.document.undo_stack()->count(), scene.base_count);
  EXPECT_EQ(xodr(scene.document), scene.base_xodr);
}

TEST(NodeDragTool, SnapEngagesOnAnotherRoadsEndpoint) {
  Scene scene;
  NodeDragTool tool(scene.document);
  tool.set_snap_options({.radius = 2.0});

  // Drag the END node (100,0); without SnapOptions::exclude_road its own
  // moving endpoint would be the closest candidate every frame.
  ASSERT_TRUE(tool.mouse_press(at(100.0, 0.0, Qt::LeftButton)));
  ASSERT_TRUE(tool.mouse_move(at(119.0, 0.5, Qt::LeftButton)));

  const Waypoint snapped = node(scene.document, scene.dragged, 2);
  EXPECT_NEAR(snapped.x, 120.0, roadmaker::tol::kRoundTripPosition);
  EXPECT_NEAR(snapped.y, 0.0, roadmaker::tol::kRoundTripPosition);
  // Dragged node + snap hint marker.
  EXPECT_EQ(tool.preview().point_positions.size(), 6U);

  ASSERT_TRUE(tool.mouse_release(at(119.0, 0.5)));
  EXPECT_EQ(scene.document.undo_stack()->count(), scene.base_count + 1);
  scene.document.undo_stack()->undo();
  EXPECT_EQ(xodr(scene.document), scene.base_xodr);
}

TEST(NodeDragTool, DeactivateCancelsARunningDrag) {
  Scene scene;
  NodeDragTool tool(scene.document);

  ASSERT_TRUE(tool.mouse_press(at(50.5, 10.5, Qt::LeftButton)));
  ASSERT_TRUE(tool.mouse_move(at(70.0, 55.0, Qt::LeftButton)));
  tool.deactivate();

  EXPECT_FALSE(tool.dragging());
  EXPECT_FALSE(scene.document.preview_active());
  EXPECT_EQ(xodr(scene.document), scene.base_xodr);
  EXPECT_EQ(scene.document.undo_stack()->count(), scene.base_count);
}
