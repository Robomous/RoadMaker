// Copyright 2026 Robomous
// SPDX-License-Identifier: Apache-2.0

// Prop Point tool (p6-s4, issue #238): headless ToolEvent sequences place a prop
// on/beside a road and assert the network, the ONE-undo-entry semantics,
// byte-identical undo, the drag = one move command path, the off-road no-op, the
// wrong-asset toast, selection, and the hover ghost. Runs under
// QT_QPA_PLATFORM=offscreen.

#include "roadmaker/edit/operations.hpp"
#include "roadmaker/road/network.hpp"
#include "roadmaker/road/object.hpp"
#include "roadmaker/xodr/writer.hpp"

#include <gtest/gtest.h>

#include <QSignalSpy>
#include <QUndoStack>
#include <optional>
#include <stdexcept>

#include "document/document.hpp"
#include "document/library_manifest.hpp"
#include "document/selection_model.hpp"
#include "tools/prop_point_tool.hpp"
#include "viewport/picking.hpp"

namespace roadmaker::editor {
namespace {

using roadmaker::Waypoint;

LibraryItem pine_item() {
  LibraryItem item;
  item.key = "prop.tree.pine";
  item.label = "Pine tree";
  item.kind = LibraryItem::Kind::Tree;
  item.model = "tree_pine";
  return item;
}

ToolEvent event_at(double x, double y) {
  ToolEvent event;
  event.buttons = Qt::LeftButton;
  event.world_x = x;
  event.world_y = y;
  return event;
}

/// A single straight two-lane road built through the Document, so placements
/// stack on a real undo stack.
struct Scene {
  Document document;
  int base_count = 0;

  Scene() {
    if (!document.push_command(
            roadmaker::edit::create_road({Waypoint{-40.0, 0.0}, Waypoint{40.0, 0.0}},
                                         roadmaker::LaneProfile::two_lane_rural(),
                                         ""))) {
      throw std::runtime_error("road setup failed");
    }
    base_count = document.undo_stack()->count();
  }

  int object_count() const {
    int count = 0;
    document.network().for_each_object([&](ObjectId, const Object&) { ++count; });
    return count;
  }

  std::optional<ObjectId> first_object() const {
    std::optional<ObjectId> found;
    document.network().for_each_object([&](ObjectId id, const Object&) {
      if (!found.has_value()) {
        found = id;
      }
    });
    return found;
  }
};

TEST(PropPointTool, ClickPlacesOnePropAsOneUndoEntry) {
  Scene scene;
  SelectionModel selection(scene.document);
  PropPointTool tool(scene.document, selection);
  tool.set_params_provider([] { return pine_item(); });
  tool.activate();

  ASSERT_TRUE(tool.mouse_press(event_at(0.0, -1.75)));
  ASSERT_TRUE(tool.mouse_release(event_at(0.0, -1.75)));

  EXPECT_EQ(scene.object_count(), 1);
  EXPECT_EQ(scene.document.undo_stack()->count(), scene.base_count + 1);
  EXPECT_EQ(selection.selected_objects().size(), 1U);

  scene.document.undo_stack()->undo();
  EXPECT_EQ(scene.object_count(), 0);
}

TEST(PropPointTool, RepeatedClicksPlaceSeparateProps) {
  Scene scene;
  SelectionModel selection(scene.document);
  PropPointTool tool(scene.document, selection);
  tool.set_params_provider([] { return pine_item(); });
  tool.activate();

  for (double x : {-10.0, 0.0, 10.0}) {
    ASSERT_TRUE(tool.mouse_press(event_at(x, -1.75)));
    ASSERT_TRUE(tool.mouse_release(event_at(x, -1.75)));
  }
  EXPECT_EQ(scene.object_count(), 3);
  EXPECT_EQ(scene.document.undo_stack()->count(), scene.base_count + 3);
}

TEST(PropPointTool, UndoRoundTripsByteIdentically) {
  Scene scene;
  SelectionModel selection(scene.document);
  PropPointTool tool(scene.document, selection);
  tool.set_params_provider([] { return pine_item(); });
  tool.activate();

  const auto before = roadmaker::write_xodr(scene.document.network(), "prop point");
  ASSERT_TRUE(before.has_value());

  ASSERT_TRUE(tool.mouse_press(event_at(0.0, -1.75)));
  ASSERT_TRUE(tool.mouse_release(event_at(0.0, -1.75)));
  scene.document.undo_stack()->undo();

  const auto after = roadmaker::write_xodr(scene.document.network(), "prop point");
  ASSERT_TRUE(after.has_value());
  EXPECT_EQ(*before, *after);
}

TEST(PropPointTool, DragMovesPropAsExactlyOneCommand) {
  Scene scene;
  SelectionModel selection(scene.document);
  PropPointTool tool(scene.document, selection);
  tool.set_params_provider([] { return pine_item(); });
  tool.activate();

  ASSERT_TRUE(tool.mouse_press(event_at(0.0, -1.75)));
  ASSERT_TRUE(tool.mouse_release(event_at(0.0, -1.75)));
  const auto object = scene.first_object();
  ASSERT_TRUE(object.has_value());
  const RoadId road = scene.document.network().object(*object)->road;
  const int after_place = scene.document.undo_stack()->count();

  ToolEvent press = event_at(0.0, -1.75);
  press.pick = PickHit{.road = road, .object = *object};
  ASSERT_TRUE(tool.mouse_press(press));
  ASSERT_TRUE(tool.mouse_move(event_at(12.0, -1.75))); // beyond the drag tolerance
  ASSERT_TRUE(tool.mouse_release(event_at(12.0, -1.75)));

  // Exactly ONE more undo entry for the whole drag (no mergeWith).
  EXPECT_EQ(scene.document.undo_stack()->count(), after_place + 1);
}

TEST(PropPointTool, EscCancelsDragWithoutEntry) {
  Scene scene;
  SelectionModel selection(scene.document);
  PropPointTool tool(scene.document, selection);
  tool.set_params_provider([] { return pine_item(); });
  tool.activate();

  ASSERT_TRUE(tool.mouse_press(event_at(0.0, -1.75)));
  ASSERT_TRUE(tool.mouse_release(event_at(0.0, -1.75)));
  const auto object = scene.first_object();
  ASSERT_TRUE(object.has_value());
  const RoadId road = scene.document.network().object(*object)->road;
  const int after_place = scene.document.undo_stack()->count();

  ToolEvent press = event_at(0.0, -1.75);
  press.pick = PickHit{.road = road, .object = *object};
  ASSERT_TRUE(tool.mouse_press(press));
  ASSERT_TRUE(tool.mouse_move(event_at(12.0, -1.75)));
  EXPECT_TRUE(tool.key_press(Qt::Key_Escape, Qt::NoModifier));
  EXPECT_EQ(scene.document.undo_stack()->count(), after_place); // nothing committed
}

TEST(PropPointTool, OffRoadPlacesNothing) {
  Scene scene;
  SelectionModel selection(scene.document);
  PropPointTool tool(scene.document, selection);
  tool.set_params_provider([] { return pine_item(); });
  tool.activate();

  ASSERT_TRUE(tool.mouse_press(event_at(0.0, 80.0)));
  ASSERT_TRUE(tool.mouse_release(event_at(0.0, 80.0)));
  EXPECT_EQ(scene.object_count(), 0);
  EXPECT_EQ(scene.document.undo_stack()->count(), scene.base_count);
}

TEST(PropPointTool, WrongAssetToastsAndPlacesNothing) {
  Scene scene;
  SelectionModel selection(scene.document);
  PropPointTool tool(scene.document, selection);
  tool.set_params_provider([] { return LibraryItem{}; }); // Kind::Unknown
  tool.activate();

  QSignalSpy toast(&tool, &Tool::toast_requested);
  ASSERT_TRUE(tool.mouse_press(event_at(0.0, -1.75)));
  ASSERT_TRUE(tool.mouse_release(event_at(0.0, -1.75)));
  EXPECT_EQ(scene.object_count(), 0);
  EXPECT_EQ(scene.document.undo_stack()->count(), scene.base_count);
  EXPECT_GE(toast.count(), 1);
}

TEST(PropPointTool, HoverShowsGhost) {
  Scene scene;
  SelectionModel selection(scene.document);
  PropPointTool tool(scene.document, selection);
  tool.set_params_provider([] { return pine_item(); });
  tool.activate();

  // A plain hover over the road arms the ghost footprint.
  EXPECT_FALSE(tool.mouse_move(event_at(0.0, -1.75)));
  EXPECT_FALSE(tool.preview().empty());
}

} // namespace
} // namespace roadmaker::editor
