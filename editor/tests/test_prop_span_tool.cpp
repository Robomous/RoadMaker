// Copyright 2026 Robomous
// SPDX-License-Identifier: Apache-2.0

// Prop Span tool (p6-s5, issue #239): headless ToolEvent sequences click two
// stations on one road, then Enter/double-click commits ONE `<object>` carrying
// ONE `<repeat>` — a single undo entry whose undo restores the file byte for
// byte. Asserts the single-command commit, the bracket-key spacing clamp, the
// off-anchor second-click rejection, Esc cancellation, and that the ghost count
// matches the kernel repeat expansion. Runs under QT_QPA_PLATFORM=offscreen.

#include "roadmaker/edit/operations.hpp"
#include "roadmaker/road/network.hpp"
#include "roadmaker/road/object.hpp"
#include "roadmaker/xodr/writer.hpp"

#include <gtest/gtest.h>

#include <QUndoStack>
#include <stdexcept>

#include "document/document.hpp"
#include "document/library_manifest.hpp"
#include "document/prop_placement.hpp"
#include "document/selection_model.hpp"
#include "tools/prop_span_tool.hpp"
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
    // A straight road from (-40, 0) to (40, 0): length 80 m, odr id "1".
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

/// Anchor at s ≈ 20 and set the far station at s ≈ 60 (a 40 m span on the road).
void place_two_stations(PropSpanTool& tool) {
  ASSERT_TRUE(tool.mouse_press(event_at(-20.0, 0.0)));
  ASSERT_TRUE(tool.mouse_press(event_at(20.0, 0.0)));
}

TEST(PropSpanTool, CommitAddsSingleObjectWithRepeat) {
  Scene scene;
  SelectionModel selection(scene.document);
  PropSpanTool tool(scene.document, selection);
  tool.set_params_provider([] { return pine_item(); });
  tool.activate();

  const auto before = roadmaker::write_xodr(scene.document.network(), "prop span");
  ASSERT_TRUE(before.has_value());

  place_two_stations(tool);
  EXPECT_EQ(scene.object_count(), 0); // nothing enters the network before the commit

  EXPECT_TRUE(tool.key_press(Qt::Key_Return, Qt::NoModifier));

  // Exactly ONE object, carrying ONE repeat, added as ONE undo entry.
  EXPECT_EQ(scene.object_count(), 1);
  EXPECT_EQ(scene.document.undo_stack()->count(), scene.base_count + 1);
  bool has_repeat = false;
  scene.document.network().for_each_object([&](ObjectId, const Object& object) {
    ASSERT_EQ(object.repeats.size(), 1U);
    EXPECT_GT(object.repeats.front().distance, 0.0);
    has_repeat = true;
  });
  EXPECT_TRUE(has_repeat);

  // Undo restores the file byte for byte.
  scene.document.undo_stack()->undo();
  EXPECT_EQ(scene.object_count(), 0);
  const auto after = roadmaker::write_xodr(scene.document.network(), "prop span");
  ASSERT_TRUE(after.has_value());
  EXPECT_EQ(*before, *after);
}

TEST(PropSpanTool, DoubleClickCommits) {
  Scene scene;
  SelectionModel selection(scene.document);
  PropSpanTool tool(scene.document, selection);
  tool.set_params_provider([] { return pine_item(); });
  tool.activate();

  place_two_stations(tool);
  EXPECT_TRUE(tool.mouse_double_click(event_at(20.0, 0.0)));
  EXPECT_EQ(scene.object_count(), 1);
  EXPECT_EQ(scene.document.undo_stack()->count(), scene.base_count + 1);
}

TEST(PropSpanTool, BracketKeysAdjustDistanceWithinClamp) {
  Scene scene;
  SelectionModel selection(scene.document);
  PropSpanTool tool(scene.document, selection);
  tool.set_params_provider([] { return pine_item(); });
  tool.activate();

  EXPECT_DOUBLE_EQ(tool.distance_m(), 5.0);
  EXPECT_TRUE(tool.key_press(Qt::Key_BracketLeft, Qt::NoModifier));
  EXPECT_DOUBLE_EQ(tool.distance_m(), 4.5);
  EXPECT_TRUE(tool.key_press(Qt::Key_BracketRight, Qt::NoModifier));
  EXPECT_DOUBLE_EQ(tool.distance_m(), 5.0);

  for (int i = 0; i < 20; ++i) {
    EXPECT_TRUE(tool.key_press(Qt::Key_BracketLeft, Qt::NoModifier));
  }
  EXPECT_DOUBLE_EQ(tool.distance_m(), 0.5);
  for (int i = 0; i < 200; ++i) {
    EXPECT_TRUE(tool.key_press(Qt::Key_BracketRight, Qt::NoModifier));
  }
  EXPECT_DOUBLE_EQ(tool.distance_m(), 50.0);
}

TEST(PropSpanTool, SecondClickOffAnchorRoadIsIgnored) {
  Scene scene;
  SelectionModel selection(scene.document);
  PropSpanTool tool(scene.document, selection);
  tool.set_params_provider([] { return pine_item(); });
  tool.activate();

  ASSERT_TRUE(tool.mouse_press(event_at(-20.0, 0.0)));
  EXPECT_EQ(tool.station_count(), 1U);
  // A second click well off the anchor road does not set the span's end.
  ASSERT_TRUE(tool.mouse_press(event_at(0.0, 80.0)));
  EXPECT_EQ(tool.station_count(), 1U);
}

TEST(PropSpanTool, EscapeCancelsWithoutCommand) {
  Scene scene;
  SelectionModel selection(scene.document);
  PropSpanTool tool(scene.document, selection);
  tool.set_params_provider([] { return pine_item(); });
  tool.activate();

  place_two_stations(tool);
  EXPECT_EQ(tool.station_count(), 2U);
  EXPECT_TRUE(tool.key_press(Qt::Key_Escape, Qt::NoModifier));
  EXPECT_EQ(tool.station_count(), 0U);
  EXPECT_EQ(scene.object_count(), 0);
}

TEST(PropSpanTool, PreviewGhostCountMatchesExpansion) {
  Scene scene;
  SelectionModel selection(scene.document);
  PropSpanTool tool(scene.document, selection);
  tool.set_params_provider([] { return pine_item(); });
  tool.activate();

  place_two_stations(tool); // s ≈ 20 → 60, default 5 m spacing

  const PreviewGeometry preview = tool.preview();
  std::size_t ghosts = 0;
  for (const Handle& handle : preview.handles) {
    if (handle.state == HandleState::Hovered) {
      ++ghosts;
    }
  }
  // The ghost count equals the kernel repeat expansion for the same span.
  const RoadId anchor = scene.document.network().find_road("1");
  const std::size_t expected =
      span_preview_points(scene.document.network(), anchor, make_span_repeat(20.0, 60.0, 0.0, 5.0))
          .size();
  EXPECT_GT(expected, 0U);
  EXPECT_EQ(ghosts, expected);
  EXPECT_EQ(scene.object_count(), 0); // still nothing committed
}

} // namespace
} // namespace roadmaker::editor
