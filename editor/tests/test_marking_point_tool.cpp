// Copyright 2026 Robomous
// SPDX-License-Identifier: Apache-2.0

// Marking Point tool (p3-s4, issue #223): headless ToolEvent sequences place an
// arrow stencil on a lane and assert the network, the ONE-undo-entry semantics,
// byte-identical undo, the drag = one move command path, the no-asset toast, and
// the Library drop path. Runs under QT_QPA_PLATFORM=offscreen.

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
#include "document/library_drop.hpp"
#include "document/library_manifest.hpp"
#include "document/selection_model.hpp"
#include "tools/marking_point_tool.hpp"
#include "viewport/picking.hpp"

namespace roadmaker::editor {
namespace {

using roadmaker::Waypoint;

LibraryItem stencil_item() {
  LibraryItem item;
  item.key = "stencil.arrow_straight";
  item.kind = LibraryItem::Kind::Stencil;
  item.stencil_subtype = "arrowStraight";
  item.stencil_length = 4.0;
  item.stencil_width_frac = 0.5;
  item.stencil_material = "material.paint_white";
  item.stencil_segmentation = "road_marking";
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

TEST(MarkingPointTool, ClickPlacesOneStencilAsOneUndoEntry) {
  Scene scene;
  SelectionModel selection(scene.document);
  MarkingPointTool tool(scene.document, selection);
  tool.set_params_provider([] { return stencil_item(); });
  tool.activate();

  ASSERT_TRUE(tool.mouse_press(event_at(0.0, -1.75)));
  ASSERT_TRUE(tool.mouse_release(event_at(0.0, -1.75)));

  EXPECT_EQ(scene.object_count(), 1);
  EXPECT_EQ(scene.document.undo_stack()->count(), scene.base_count + 1);
  EXPECT_EQ(selection.selected_objects().size(), 1U);

  scene.document.undo_stack()->undo();
  EXPECT_EQ(scene.object_count(), 0);
}

TEST(MarkingPointTool, UndoRoundTripsByteIdentically) {
  Scene scene;
  SelectionModel selection(scene.document);
  MarkingPointTool tool(scene.document, selection);
  tool.set_params_provider([] { return stencil_item(); });
  tool.activate();

  const auto before = roadmaker::write_xodr(scene.document.network(), "marking point");
  ASSERT_TRUE(before.has_value());

  ASSERT_TRUE(tool.mouse_press(event_at(0.0, -1.75)));
  ASSERT_TRUE(tool.mouse_release(event_at(0.0, -1.75)));
  scene.document.undo_stack()->undo();

  const auto after = roadmaker::write_xodr(scene.document.network(), "marking point");
  ASSERT_TRUE(after.has_value());
  EXPECT_EQ(*before, *after);
}

TEST(MarkingPointTool, DragMovesStencilAsExactlyOneCommand) {
  Scene scene;
  SelectionModel selection(scene.document);
  MarkingPointTool tool(scene.document, selection);
  tool.set_params_provider([] { return stencil_item(); });
  tool.activate();

  // Place one stencil, then drag it along s.
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

TEST(MarkingPointTool, NoStencilAssetToastsAndPlacesNothing) {
  Scene scene;
  SelectionModel selection(scene.document);
  MarkingPointTool tool(scene.document, selection);
  tool.set_params_provider([] { return LibraryItem{}; }); // Kind::Unknown
  tool.activate();

  QSignalSpy toast(&tool, &Tool::toast_requested);
  ASSERT_TRUE(tool.mouse_press(event_at(0.0, -1.75)));
  ASSERT_TRUE(tool.mouse_release(event_at(0.0, -1.75)));
  EXPECT_EQ(scene.object_count(), 0);
  EXPECT_EQ(scene.document.undo_stack()->count(), scene.base_count);
  EXPECT_GE(toast.count(), 1);
}

TEST(MarkingPointTool, LibraryDropResolvesToAnAddObjectCommand) {
  Scene scene;
  const LibraryDropAction action =
      resolve_library_drop(stencil_item(), scene.document.network(), 0.0, -1.75);
  EXPECT_EQ(action.kind, LibraryDropKind::Stencil);
  ASSERT_NE(action.command, nullptr);
  EXPECT_TRUE(action.preview.valid);

  // A drop off any lane is rejected with a hint.
  const LibraryDropAction miss =
      resolve_library_drop(stencil_item(), scene.document.network(), 0.0, 60.0);
  EXPECT_EQ(miss.kind, LibraryDropKind::None);
  EXPECT_FALSE(miss.toast.isEmpty());
}

} // namespace
} // namespace roadmaker::editor
