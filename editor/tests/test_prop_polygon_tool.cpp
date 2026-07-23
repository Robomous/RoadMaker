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

// Prop Polygon tool (p6-s5, issue #239): headless ToolEvent sequences click a
// closed region, then Enter/double-click BAKES the scattered props as ONE undo
// macro whose undo restores the file byte for byte. Asserts the single-macro
// bake, density scaling, seeded determinism (set_seed), the three-vertex
// minimum, and Esc cancellation. Runs under QT_QPA_PLATFORM=offscreen.

#include "roadmaker/edit/operations.hpp"
#include "roadmaker/road/network.hpp"
#include "roadmaker/road/object.hpp"
#include "roadmaker/xodr/writer.hpp"

#include <gtest/gtest.h>

#include <QUndoStack>
#include <cstdint>
#include <stdexcept>
#include <vector>

#include "document/document.hpp"
#include "document/library_manifest.hpp"
#include "document/selection_model.hpp"
#include "tools/prop_polygon_tool.hpp"

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

/// Outline a square region straddling the road (16 m wide, 8 m tall).
void place_square(PropPolygonTool& tool) {
  ASSERT_TRUE(tool.mouse_press(event_at(-8.0, -4.0)));
  ASSERT_TRUE(tool.mouse_press(event_at(8.0, -4.0)));
  ASSERT_TRUE(tool.mouse_press(event_at(8.0, 4.0)));
  ASSERT_TRUE(tool.mouse_press(event_at(-8.0, 4.0)));
}

std::size_t ghost_count(const PropPolygonTool& tool) {
  std::size_t ghosts = 0;
  for (const Handle& handle : tool.preview().handles) {
    if (handle.state == HandleState::Hovered) {
      ++ghosts;
    }
  }
  return ghosts;
}

TEST(PropPolygonTool, BakePushesMacroAndUndoRestoresBytes) {
  Scene scene;
  SelectionModel selection(scene.document);
  PropPolygonTool tool(scene.document, selection);
  tool.set_params_provider([] { return pine_item(); });
  tool.activate();
  tool.set_seed(42);

  const auto before = roadmaker::write_xodr(scene.document.network(), "prop polygon");
  ASSERT_TRUE(before.has_value());

  place_square(tool);
  EXPECT_EQ(scene.object_count(), 0); // nothing enters the network before the bake

  EXPECT_TRUE(tool.key_press(Qt::Key_Return, Qt::NoModifier));
  EXPECT_GT(scene.object_count(), 0);
  // The whole bake is ONE undo entry (a macro).
  EXPECT_EQ(scene.document.undo_stack()->count(), scene.base_count + 1);

  scene.document.undo_stack()->undo();
  EXPECT_EQ(scene.object_count(), 0);
  const auto after = roadmaker::write_xodr(scene.document.network(), "prop polygon");
  ASSERT_TRUE(after.has_value());
  EXPECT_EQ(*before, *after);
}

TEST(PropPolygonTool, DensityKeysScalePlacedCount) {
  Scene scene;
  SelectionModel selection(scene.document);
  PropPolygonTool tool(scene.document, selection);
  tool.set_params_provider([] { return pine_item(); });
  tool.activate();
  tool.set_seed(1);

  place_square(tool);
  const std::size_t sparse = ghost_count(tool);
  EXPECT_GT(sparse, 0U);
  // Raise the density several steps; the scatter grows.
  for (int i = 0; i < 20; ++i) {
    EXPECT_TRUE(tool.key_press(Qt::Key_BracketRight, Qt::NoModifier));
  }
  EXPECT_GT(ghost_count(tool), sparse);
}

TEST(PropPolygonTool, RandomizeReDistributesDeterministically) {
  Scene scene;
  SelectionModel selection(scene.document);
  PropPolygonTool tool(scene.document, selection);
  tool.set_params_provider([] { return pine_item(); });
  tool.activate();

  const auto ghost_positions = [&] {
    std::vector<std::pair<double, double>> out;
    for (const Handle& handle : tool.preview().handles) {
      if (handle.state == HandleState::Hovered) {
        out.emplace_back(handle.x, handle.y);
      }
    }
    return out;
  };

  tool.set_seed(100);
  place_square(tool);
  const auto first = ghost_positions();
  EXPECT_FALSE(first.empty());

  // The same seed re-scatters identically.
  tool.set_seed(100);
  EXPECT_EQ(ghost_positions(), first);

  // A different seed scatters differently.
  tool.set_seed(200);
  EXPECT_NE(ghost_positions(), first);
}

TEST(PropPolygonTool, RequiresThreeVertices) {
  Scene scene;
  SelectionModel selection(scene.document);
  PropPolygonTool tool(scene.document, selection);
  tool.set_params_provider([] { return pine_item(); });
  tool.activate();

  ASSERT_TRUE(tool.mouse_press(event_at(-8.0, -4.0)));
  ASSERT_TRUE(tool.mouse_press(event_at(8.0, -4.0)));
  EXPECT_EQ(tool.vertex_count(), 2U);
  // Enter with only two vertices commits nothing.
  EXPECT_TRUE(tool.key_press(Qt::Key_Return, Qt::NoModifier));
  EXPECT_EQ(scene.object_count(), 0);
}

TEST(PropPolygonTool, EscapeCancelsWithoutCommand) {
  Scene scene;
  SelectionModel selection(scene.document);
  PropPolygonTool tool(scene.document, selection);
  tool.set_params_provider([] { return pine_item(); });
  tool.activate();

  place_square(tool);
  EXPECT_EQ(tool.vertex_count(), 4U);
  EXPECT_TRUE(tool.key_press(Qt::Key_Escape, Qt::NoModifier));
  EXPECT_EQ(tool.vertex_count(), 0U);
  EXPECT_EQ(scene.object_count(), 0);
}

} // namespace
} // namespace roadmaker::editor
