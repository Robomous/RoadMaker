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

// Elevation editor (issue #16, docs/design/m2/02_editing_tools.md §5): the
// ElevationTool routes node picks into an active-node selection, and the
// PropertiesPanel's Elevation section edits that node's height through one
// edit::set_node_elevation command per commit. Headless: ToolEvents are fed
// directly and widget signals invoked without a running event loop.

#include "roadmaker/edit/operations.hpp"
#include "roadmaker/geometry/poly3.hpp"
#include "roadmaker/road/authoring.hpp"
#include "roadmaker/road/network.hpp"
#include "roadmaker/tol.hpp"
#include "roadmaker/xodr/writer.hpp"

#include <gtest/gtest.h>

#include <QDoubleSpinBox>
#include <QLabel>
#include <QSignalSpy>
#include <algorithm>
#include <filesystem>
#include <stdexcept>
#include <string>

#include "document/document.hpp"
#include "document/selection_model.hpp"
#include "panels/properties_panel.hpp"
#include "tools/elevation_tool.hpp"

using roadmaker::LaneProfile;
using roadmaker::RoadId;
using roadmaker::Waypoint;
using roadmaker::editor::Document;
using roadmaker::editor::ElevationTool;
using roadmaker::editor::PickHit;
using roadmaker::editor::PropertiesPanel;
using roadmaker::editor::SelectionModel;
using roadmaker::editor::ToolEvent;

namespace {

std::string xodr(const Document& document) {
  auto text = roadmaker::write_xodr(document.network());
  if (!text) {
    throw std::runtime_error(text.error().message);
  }
  return *text;
}

/// One straight three-node road with the panel and Elevation tool wired up.
struct Scene {
  Document document;
  SelectionModel selection{document};
  PropertiesPanel panel{document, selection};
  ElevationTool tool{document, selection};
  RoadId road;
  int base_count = 0;

  Scene() {
    auto created =
        document.push_command(roadmaker::edit::create_road({Waypoint{.x = 0.0, .y = 0.0},
                                                            Waypoint{.x = 40.0, .y = 0.0},
                                                            Waypoint{.x = 80.0, .y = 0.0}},
                                                           LaneProfile::two_lane_default(),
                                                           "Subject"));
    if (!created) {
      throw std::runtime_error("scene setup failed");
    }
    document.network().for_each_road([&](RoadId id, const roadmaker::Road&) { road = id; });
    panel.set_elevation_tool(&tool);
    base_count = document.undo_stack()->count();
  }

  [[nodiscard]] Waypoint node(std::size_t index) const {
    return roadmaker::edit::effective_waypoints(*document.network().road(road)).at(index);
  }

  /// Feeds the tool a left click at a world position.
  [[nodiscard]] bool click(double x, double y) {
    ToolEvent event;
    event.buttons = Qt::LeftButton;
    event.world_x = x;
    event.world_y = y;
    return tool.mouse_press(event);
  }

  template <typename T>
  [[nodiscard]] T* editor(const char* name) {
    T* widget = panel.findChild<T*>(QString::fromLatin1(name));
    if (widget == nullptr) {
      throw std::runtime_error("editor widget not found");
    }
    return widget;
  }
};

} // namespace

TEST(ElevationTool, ClickingANodeMakesItActiveAndSelectsItsRoad) {
  Scene scene;
  QSignalSpy spy(&scene.tool, &ElevationTool::active_node_changed);
  scene.selection.select({.road = scene.road, .lane = roadmaker::LaneId{}});

  const Waypoint mid = scene.node(1);
  ASSERT_TRUE(scene.click(mid.x, mid.y));
  ASSERT_TRUE(scene.tool.active_node().has_value());
  EXPECT_EQ(scene.tool.active_node()->first, scene.road);
  EXPECT_EQ(scene.tool.active_node()->second, 1U);
  EXPECT_EQ(scene.selection.primary().road, scene.road);
  EXPECT_GE(spy.count(), 1);

  // Clicking empty space clears both the active node and the selection.
  ASSERT_TRUE(scene.click(1000.0, 1000.0));
  EXPECT_FALSE(scene.tool.active_node().has_value());
  EXPECT_TRUE(scene.selection.empty());
}

TEST(ElevationTool, PreviewDrawsNodeHandlesAndTheActiveHighlight) {
  Scene scene;
  scene.selection.select({.road = scene.road, .lane = roadmaker::LaneId{}});
  // Node handles for the three waypoints, no active highlight yet.
  EXPECT_EQ(scene.tool.preview().handles.size(), 3U);
  EXPECT_TRUE(scene.tool.preview().line_positions.empty());

  const Waypoint first = scene.node(0);
  ASSERT_TRUE(scene.click(first.x, first.y));
  // The active node is now carried by its handle's Hovered state (no lines);
  // still three handles, exactly one of them highlighted.
  const auto preview = scene.tool.preview();
  EXPECT_EQ(preview.handles.size(), 3U);
  EXPECT_TRUE(preview.line_positions.empty());
  EXPECT_EQ(std::count_if(preview.handles.begin(),
                          preview.handles.end(),
                          [](const roadmaker::editor::Handle& handle) {
                            return handle.state == roadmaker::editor::HandleState::Hovered;
                          }),
            1);
}

TEST(ElevationTool, EscapeClearsTheActiveNode) {
  Scene scene;
  scene.selection.select({.road = scene.road, .lane = roadmaker::LaneId{}});
  const Waypoint mid = scene.node(1);
  ASSERT_TRUE(scene.click(mid.x, mid.y));
  ASSERT_TRUE(scene.tool.active_node().has_value());

  EXPECT_TRUE(scene.tool.key_press(Qt::Key_Escape, Qt::NoModifier));
  EXPECT_FALSE(scene.tool.active_node().has_value());
}

TEST(ElevationPanel, SpinDisabledUntilANodeIsActive) {
  Scene scene;
  scene.selection.select({.road = scene.road, .lane = roadmaker::LaneId{}});
  auto* spin = scene.editor<QDoubleSpinBox>("node_elevation_spin");
  EXPECT_FALSE(spin->isEnabled()); // no active node yet

  const Waypoint mid = scene.node(1);
  ASSERT_TRUE(scene.click(mid.x, mid.y));
  EXPECT_TRUE(spin->isEnabled());
  EXPECT_NEAR(spin->value(), 0.0, 1e-9); // flat road: node height 0
}

TEST(ElevationPanel, HeightSpinCommitsOneCommandAndUndoRestores) {
  Scene scene;
  scene.selection.select({.road = scene.road, .lane = roadmaker::LaneId{}});
  const Waypoint mid = scene.node(1);
  ASSERT_TRUE(scene.click(mid.x, mid.y));
  auto* spin = scene.editor<QDoubleSpinBox>("node_elevation_spin");

  const std::string before = xodr(scene.document);
  spin->setValue(4.0);
  emit spin->editingFinished();

  EXPECT_EQ(scene.document.undo_stack()->count(), scene.base_count + 1);
  const roadmaker::Road* road = scene.document.network().road(scene.road);
  ASSERT_FALSE(road->elevation.empty());
  const auto stations = roadmaker::edit::waypoint_stations(*road);
  ASSERT_TRUE(stations.has_value());
  EXPECT_NEAR(
      roadmaker::eval_profile(road->elevation, (*stations)[1]), 4.0, roadmaker::tol::kLength);

  // A second editingFinished without a change pushes nothing (re-entrancy guard).
  emit spin->editingFinished();
  EXPECT_EQ(scene.document.undo_stack()->count(), scene.base_count + 1);

  scene.document.undo_stack()->undo();
  EXPECT_EQ(xodr(scene.document), before);
  // After undo the panel re-synced the spin box back to zero.
  EXPECT_NEAR(spin->value(), 0.0, 1e-9);
}

TEST(ElevationPanel, EditingAForeignNodeDerivesWaypoints) {
  // A road with no authoring waypoints (as loaded from a foreign file) still
  // exposes editable nodes; the first elevation edit records the derived
  // waypoints, exactly as Edit Nodes does (01 §2.5).
  Document document;
  SelectionModel selection{document};
  PropertiesPanel panel{document, selection};
  ElevationTool tool{document, selection};
  panel.set_elevation_tool(&tool);

  ASSERT_TRUE(
      document.load(std::filesystem::path(RM_SAMPLES_DIR) / "curved_road.xodr").has_value());
  const RoadId road = document.network().find_road("1");
  ASSERT_TRUE(document.network().road(road) != nullptr);
  ASSERT_FALSE(document.network().road(road)->authoring_waypoints.has_value());

  selection.select({.road = road, .lane = roadmaker::LaneId{}});
  const Waypoint node = roadmaker::edit::effective_waypoints(*document.network().road(road))[2];
  ToolEvent event;
  event.buttons = Qt::LeftButton;
  event.world_x = node.x;
  event.world_y = node.y;
  ASSERT_TRUE(tool.mouse_press(event));
  ASSERT_TRUE(tool.active_node().has_value());

  auto* spin = panel.findChild<QDoubleSpinBox*>(QStringLiteral("node_elevation_spin"));
  ASSERT_NE(spin, nullptr);
  ASSERT_TRUE(spin->isEnabled());
  spin->setValue(2.5);
  emit spin->editingFinished();
  EXPECT_TRUE(document.network().road(road)->authoring_waypoints.has_value());
  EXPECT_FALSE(document.network().road(road)->elevation.empty());
}
