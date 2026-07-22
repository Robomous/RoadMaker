// Copyright 2026 Robomous
// SPDX-License-Identifier: Apache-2.0

// Split tool (M3a): a headless click splits the picked road at the cut station,
// selects both halves, and requests a return to Select.

#include "roadmaker/edit/operations.hpp"
#include "roadmaker/road/network.hpp"

#include <gtest/gtest.h>

#include <optional>
#include <stdexcept>
#include <vector>

#include "document/document.hpp"
#include "document/selection_model.hpp"
#include "tools/split_tool.hpp"

using roadmaker::LaneId;
using roadmaker::RoadId;
using roadmaker::Waypoint;
using roadmaker::editor::Document;
using roadmaker::editor::PickHit;
using roadmaker::editor::SelectionModel;
using roadmaker::editor::SplitTool;
using roadmaker::editor::ToolEvent;
using roadmaker::editor::ToolId;

namespace {

ToolEvent at(double x, double y, Qt::MouseButtons buttons, std::optional<PickHit> pick) {
  ToolEvent event;
  event.world_x = x;
  event.world_y = y;
  event.buttons = buttons;
  event.pick = pick;
  return event;
}

struct Scene {
  Document document;
  SelectionModel selection{document};
  RoadId road;

  Scene() {
    if (!document.push_command(roadmaker::edit::create_road(
            {Waypoint{.x = 0.0, .y = 0.0}, Waypoint{.x = 100.0, .y = 0.0}},
            roadmaker::LaneProfile::two_lane_default(),
            "Road"))) {
      throw std::runtime_error("scene create failed");
    }
    document.network().for_each_road([&](RoadId id, const roadmaker::Road&) { road = id; });
  }

  [[nodiscard]] PickHit hit() const {
    const roadmaker::Road* road_ptr = document.network().road(road);
    return PickHit{.road = road,
                   .lane =
                       document.network().lane_section(road_ptr->sections.front())->lanes.back(),
                   .position = {50.0, 0.0, 0.0}};
  }
};

} // namespace

TEST(SplitTool, ClickSplitsSelectsBothHalvesAndReturnsToSelect) {
  Scene scene;
  SplitTool tool(scene.document, scene.selection);

  ToolId requested = ToolId::Split;
  int requests = 0;
  QObject::connect(&tool, &SplitTool::request_tool, [&](ToolId id) {
    requested = id;
    ++requests;
  });

  // Hover then click at mid-road.
  ASSERT_TRUE(tool.mouse_move(at(50.0, 0.0, Qt::NoButton, scene.hit())));
  ASSERT_TRUE(tool.mouse_press(at(50.0, 0.0, Qt::LeftButton, scene.hit())));

  EXPECT_EQ(scene.document.network().road_count(), 2U);
  EXPECT_EQ(scene.selection.entries().size(), 2U); // both halves selected
  EXPECT_EQ(requests, 1);
  EXPECT_EQ(requested, ToolId::Select);

  scene.document.undo_stack()->undo();
  EXPECT_EQ(scene.document.network().road_count(), 1U);
}

TEST(SplitTool, EscapeReturnsToSelectWithoutSplitting) {
  Scene scene;
  SplitTool tool(scene.document, scene.selection);
  ToolId requested = ToolId::Split;
  QObject::connect(&tool, &SplitTool::request_tool, [&](ToolId id) { requested = id; });

  ASSERT_TRUE(tool.key_press(Qt::Key_Escape, Qt::NoModifier));
  EXPECT_EQ(requested, ToolId::Select);
  EXPECT_EQ(scene.document.network().road_count(), 1U);
}
