// Copyright 2026 Robomous
// SPDX-License-Identifier: Apache-2.0

// Marking Curve tool (p3-s4, issue #223): headless ToolEvent sequences draw a
// free-form marking along a road and assert the network, ONE-undo-entry
// semantics, byte-identical undo, Backspace/Esc, and the GW-5 s6 curved striped
// crossing. Runs under QT_QPA_PLATFORM=offscreen.

#include "roadmaker/edit/operations.hpp"
#include "roadmaker/road/network.hpp"
#include "roadmaker/road/object.hpp"
#include "roadmaker/xodr/writer.hpp"

#include <gtest/gtest.h>

#include <QSignalSpy>
#include <QUndoStack>
#include <stdexcept>

#include "document/document.hpp"
#include "document/library_manifest.hpp"
#include "document/selection_model.hpp"
#include "tools/marking_curve_tool.hpp"

namespace roadmaker::editor {
namespace {

using roadmaker::Waypoint;

LibraryItem marking_item() {
  LibraryItem item;
  item.key = "marking.solid_white";
  item.kind = LibraryItem::Kind::Marking;
  item.mark_type = "solid";
  item.mark_color = "white";
  item.mark_width = 0.15;
  return item;
}

LibraryItem crosswalk_item() {
  LibraryItem item;
  item.key = "crosswalk.zebra";
  item.kind = LibraryItem::Kind::Crosswalk;
  item.crosswalk_width = 3.0;
  item.crosswalk_dash = 0.5;
  item.crosswalk_gap = 0.5;
  item.crosswalk_material = "material.paint_white";
  return item;
}

ToolEvent event_at(double x, double y) {
  ToolEvent event;
  event.buttons = Qt::LeftButton;
  event.world_x = x;
  event.world_y = y;
  return event;
}

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
};

TEST(MarkingCurveTool, MultiClickCommitsOneMarkingAsOneUndoEntry) {
  Scene scene;
  SelectionModel selection(scene.document);
  MarkingCurveTool tool(scene.document, selection);
  tool.set_params_provider([] { return marking_item(); });
  tool.activate();

  ASSERT_TRUE(tool.mouse_press(event_at(-10.0, 0.0)));
  ASSERT_TRUE(tool.mouse_press(event_at(0.0, 0.0)));
  ASSERT_TRUE(tool.mouse_press(event_at(10.0, 0.0)));
  EXPECT_EQ(tool.point_count(), 3U);
  EXPECT_EQ(scene.object_count(), 0); // nothing in the network until commit
  EXPECT_TRUE(tool.key_press(Qt::Key_Return, Qt::NoModifier));

  EXPECT_EQ(scene.object_count(), 1);
  EXPECT_EQ(scene.document.undo_stack()->count(), scene.base_count + 1);
  EXPECT_EQ(selection.selected_objects().size(), 1U);

  scene.document.undo_stack()->undo();
  EXPECT_EQ(scene.object_count(), 0);
}

TEST(MarkingCurveTool, UndoRoundTripsByteIdentically) {
  Scene scene;
  SelectionModel selection(scene.document);
  MarkingCurveTool tool(scene.document, selection);
  tool.set_params_provider([] { return marking_item(); });
  tool.activate();

  const auto before = roadmaker::write_xodr(scene.document.network(), "marking curve");
  ASSERT_TRUE(before.has_value());

  ASSERT_TRUE(tool.mouse_press(event_at(-10.0, 0.0)));
  ASSERT_TRUE(tool.mouse_press(event_at(10.0, 0.0)));
  ASSERT_TRUE(tool.key_press(Qt::Key_Return, Qt::NoModifier));
  scene.document.undo_stack()->undo();

  const auto after = roadmaker::write_xodr(scene.document.network(), "marking curve");
  ASSERT_TRUE(after.has_value());
  EXPECT_EQ(*before, *after);
}

TEST(MarkingCurveTool, BackspaceRemovesLastPointAndEscCancels) {
  Scene scene;
  SelectionModel selection(scene.document);
  MarkingCurveTool tool(scene.document, selection);
  tool.set_params_provider([] { return marking_item(); });
  tool.activate();

  ASSERT_TRUE(tool.mouse_press(event_at(-10.0, 0.0)));
  ASSERT_TRUE(tool.mouse_press(event_at(0.0, 0.0)));
  EXPECT_TRUE(tool.key_press(Qt::Key_Backspace, Qt::NoModifier));
  EXPECT_EQ(tool.point_count(), 1U);
  EXPECT_TRUE(tool.key_press(Qt::Key_Escape, Qt::NoModifier));
  EXPECT_EQ(tool.point_count(), 0U);
  EXPECT_EQ(scene.object_count(), 0);
}

TEST(MarkingCurveTool, FirstClickOffRoadPlacesNoAnchor) {
  Scene scene;
  SelectionModel selection(scene.document);
  MarkingCurveTool tool(scene.document, selection);
  tool.set_params_provider([] { return marking_item(); });
  tool.activate();

  ASSERT_TRUE(tool.mouse_press(event_at(0.0, 80.0))); // far off any road
  EXPECT_EQ(tool.point_count(), 0U);
}

TEST(MarkingCurveTool, NoCompatibleAssetToastsOnFirstClick) {
  Scene scene;
  SelectionModel selection(scene.document);
  MarkingCurveTool tool(scene.document, selection);
  tool.set_params_provider([] { return LibraryItem{}; }); // Kind::Unknown
  tool.activate();

  QSignalSpy toast(&tool, &Tool::toast_requested);
  ASSERT_TRUE(tool.mouse_press(event_at(-10.0, 0.0)));
  EXPECT_EQ(tool.point_count(), 0U);
  EXPECT_GE(toast.count(), 1);
}

// GW-5 step 6: a crosswalk.zebra asset drawn as a gentle curve authors a striped
// crossing band that follows the drawn centreline.
TEST(MarkingCurveTool, CrosswalkAssetAuthorsAStripedBand) {
  Scene scene;
  SelectionModel selection(scene.document);
  MarkingCurveTool tool(scene.document, selection);
  tool.set_params_provider([] { return crosswalk_item(); });
  tool.activate();

  ASSERT_TRUE(tool.mouse_press(event_at(-10.0, 0.0)));
  ASSERT_TRUE(tool.mouse_press(event_at(0.0, 0.3)));
  ASSERT_TRUE(tool.mouse_press(event_at(10.0, 0.0)));
  ASSERT_TRUE(tool.key_press(Qt::Key_Return, Qt::NoModifier));

  ASSERT_EQ(scene.object_count(), 1);
  bool striped = false;
  scene.document.network().for_each_object([&](ObjectId, const Object& object) {
    if (object.marking_curve.has_value()) {
      striped = object.marking_curve->striped;
    }
  });
  EXPECT_TRUE(striped);
}

} // namespace
} // namespace roadmaker::editor
