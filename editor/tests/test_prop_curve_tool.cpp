// Copyright 2026 Robomous
// SPDX-License-Identifier: Apache-2.0

// Prop Curve tool (p6-s4, issue #238): headless ToolEvent sequences click
// waypoints along a road, then Enter/double-click BAKES the distributed props as
// ONE undo macro (the GW-2 step-17 acceptance). Asserts anchoring/refusal, the
// single-undo bake, individually-editable baked props, Backspace/Esc, the
// bracket-key spacing clamp, and the pre-bake instance preview. Runs under
// QT_QPA_PLATFORM=offscreen.

#include "roadmaker/edit/operations.hpp"
#include "roadmaker/road/network.hpp"
#include "roadmaker/road/object.hpp"

#include <gtest/gtest.h>

#include <QUndoStack>
#include <stdexcept>

#include "document/document.hpp"
#include "document/library_manifest.hpp"
#include "document/selection_model.hpp"
#include "tools/prop_curve_tool.hpp"
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

/// Place two waypoints along the road (a 20 m straight curve), leaving the tool
/// ready to bake.
void place_two_points(PropCurveTool& tool) {
  ASSERT_TRUE(tool.mouse_press(event_at(-10.0, -1.75)));
  ASSERT_TRUE(tool.mouse_press(event_at(10.0, -1.75)));
}

TEST(PropCurveTool, FirstClickAnchorsOnRoadRefusesOffRoad) {
  Scene scene;
  SelectionModel selection(scene.document);
  PropCurveTool tool(scene.document, selection);
  tool.set_params_provider([] { return pine_item(); });
  tool.activate();

  // Off any road: nothing anchors.
  ASSERT_TRUE(tool.mouse_press(event_at(0.0, 80.0)));
  EXPECT_EQ(tool.point_count(), 0U);

  // On the road: the first point anchors.
  ASSERT_TRUE(tool.mouse_press(event_at(-10.0, -1.75)));
  EXPECT_EQ(tool.point_count(), 1U);
}

TEST(PropCurveTool, BakeCommitsAllPropsAsSingleUndoEntry) {
  Scene scene;
  SelectionModel selection(scene.document);
  PropCurveTool tool(scene.document, selection);
  tool.set_params_provider([] { return pine_item(); });
  tool.activate();

  place_two_points(tool);
  EXPECT_EQ(scene.object_count(), 0); // nothing enters the network before the bake

  EXPECT_TRUE(tool.key_press(Qt::Key_Return, Qt::NoModifier));

  // 20 m curve at the default 5 m spacing → props at s = 0, 5, 10, 15, 20.
  EXPECT_EQ(scene.object_count(), 5);
  // The whole bake is ONE undo entry.
  EXPECT_EQ(scene.document.undo_stack()->count(), scene.base_count + 1);
  scene.document.undo_stack()->undo();
  EXPECT_EQ(scene.object_count(), 0);
}

TEST(PropCurveTool, DoubleClickBakes) {
  Scene scene;
  SelectionModel selection(scene.document);
  PropCurveTool tool(scene.document, selection);
  tool.set_params_provider([] { return pine_item(); });
  tool.activate();

  place_two_points(tool);
  EXPECT_TRUE(tool.mouse_double_click(event_at(10.0, -1.75)));
  EXPECT_EQ(scene.object_count(), 5);
  EXPECT_EQ(scene.document.undo_stack()->count(), scene.base_count + 1);
}

TEST(PropCurveTool, BakedPropsAreIndividuallyEditable) {
  Scene scene;
  SelectionModel selection(scene.document);
  PropCurveTool tool(scene.document, selection);
  tool.set_params_provider([] { return pine_item(); });
  tool.activate();

  place_two_points(tool);
  EXPECT_TRUE(tool.key_press(Qt::Key_Return, Qt::NoModifier));

  // Each baked prop is a plain, distinctly-identified Tree object (no shared
  // marking/crosswalk userData) — so each can be selected and moved on its own.
  int count = 0;
  scene.document.network().for_each_object([&](ObjectId id, const Object& object) {
    ++count;
    EXPECT_EQ(object.type, ObjectType::Tree);
    EXPECT_FALSE(object.marking_curve.has_value());
    EXPECT_FALSE(object.crosswalk.has_value());
    EXPECT_FALSE(object.stencil.has_value());
    // move_object accepts it individually.
    auto move = roadmaker::edit::move_object(scene.document.network(), id, object.s, object.t);
    EXPECT_NE(move, nullptr);
  });
  EXPECT_EQ(count, 5);
}

TEST(PropCurveTool, BackspaceRemovesPointEscCancels) {
  Scene scene;
  SelectionModel selection(scene.document);
  PropCurveTool tool(scene.document, selection);
  tool.set_params_provider([] { return pine_item(); });
  tool.activate();

  place_two_points(tool);
  EXPECT_EQ(tool.point_count(), 2U);
  EXPECT_TRUE(tool.key_press(Qt::Key_Backspace, Qt::NoModifier));
  EXPECT_EQ(tool.point_count(), 1U);
  EXPECT_TRUE(tool.key_press(Qt::Key_Escape, Qt::NoModifier));
  EXPECT_EQ(tool.point_count(), 0U);
  EXPECT_EQ(scene.object_count(), 0);
}

TEST(PropCurveTool, BracketKeysAdjustSpacingWithinClamp) {
  Scene scene;
  SelectionModel selection(scene.document);
  PropCurveTool tool(scene.document, selection);
  tool.set_params_provider([] { return pine_item(); });
  tool.activate();

  EXPECT_DOUBLE_EQ(tool.spacing_m(), 5.0);
  EXPECT_TRUE(tool.key_press(Qt::Key_BracketLeft, Qt::NoModifier));
  EXPECT_DOUBLE_EQ(tool.spacing_m(), 4.5);
  EXPECT_TRUE(tool.key_press(Qt::Key_BracketRight, Qt::NoModifier));
  EXPECT_DOUBLE_EQ(tool.spacing_m(), 5.0);

  // Clamps at the low end.
  for (int i = 0; i < 20; ++i) {
    EXPECT_TRUE(tool.key_press(Qt::Key_BracketLeft, Qt::NoModifier));
  }
  EXPECT_DOUBLE_EQ(tool.spacing_m(), 0.5);
  // And at the high end.
  for (int i = 0; i < 200; ++i) {
    EXPECT_TRUE(tool.key_press(Qt::Key_BracketRight, Qt::NoModifier));
  }
  EXPECT_DOUBLE_EQ(tool.spacing_m(), 50.0);
}

TEST(PropCurveTool, PreviewShowsInstancesBeforeBake) {
  Scene scene;
  SelectionModel selection(scene.document);
  PropCurveTool tool(scene.document, selection);
  tool.set_params_provider([] { return pine_item(); });
  tool.activate();

  place_two_points(tool);
  const PreviewGeometry preview = tool.preview();
  // Two waypoint handles plus one handle per distributed prop (five) — so more
  // than the raw waypoints — and still nothing committed to the network.
  EXPECT_GT(preview.handles.size(), tool.point_count());
  EXPECT_EQ(scene.object_count(), 0);
}

} // namespace
} // namespace roadmaker::editor
