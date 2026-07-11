// Delete tool (issue #11, docs/design/m2/02_editing_tools.md §7): headless
// ToolEvent sequences drive click-deletion; the tests assert on the network,
// the undo stack, and the serialized round-trip — including the referential
// closure when the picked road feeds a junction.

#include "roadmaker/edit/operations.hpp"
#include "roadmaker/road/road.hpp"
#include "roadmaker/xodr/writer.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>

#include "document/document.hpp"
#include "tools/delete_tool.hpp"

using roadmaker::JunctionId;
using roadmaker::LaneId;
using roadmaker::RoadId;
using roadmaker::Waypoint;
using roadmaker::editor::DeleteTool;
using roadmaker::editor::Document;
using roadmaker::editor::PickHit;
using roadmaker::editor::ToolEvent;

namespace {

std::string xodr(const Document& document) {
  auto text = roadmaker::write_xodr(document.network());
  if (!text) {
    throw std::runtime_error(text.error().message);
  }
  return *text;
}

ToolEvent click_on(RoadId road) {
  ToolEvent event;
  event.buttons = Qt::LeftButton;
  event.pick = PickHit{.road = road, .lane = LaneId{}};
  return event;
}

/// Two disjoint roads; base state captured after the creates.
struct Scene {
  Document document;
  RoadId first;
  RoadId second;
  int base_count = 0;
  std::string base_xodr;

  Scene() {
    auto one = document.push_command(
        roadmaker::edit::create_road({Waypoint{.x = 0.0, .y = 0.0}, Waypoint{.x = 100.0, .y = 0.0}},
                                     roadmaker::LaneProfile::two_lane_default(),
                                     "First"));
    auto two = document.push_command(roadmaker::edit::create_road(
        {Waypoint{.x = 0.0, .y = 40.0}, Waypoint{.x = 100.0, .y = 40.0}},
        roadmaker::LaneProfile::two_lane_default(),
        "Second"));
    if (!one || !two) {
      throw std::runtime_error("scene setup failed");
    }
    document.network().for_each_road([&](RoadId id, const roadmaker::Road& road) {
      if (road.name == "First") {
        first = id;
      } else if (road.name == "Second") {
        second = id;
      }
    });
    if (!first.is_valid() || !second.is_valid()) {
      throw std::runtime_error("scene roads not found");
    }
    base_count = document.undo_stack()->count();
    base_xodr = xodr(document);
  }
};

} // namespace

TEST(DeleteTool, ClickDeletesThePickedRoadAndUndoRestores) {
  Scene scene;
  DeleteTool tool(scene.document);

  ASSERT_TRUE(tool.mouse_press(click_on(scene.first)));

  EXPECT_EQ(scene.document.network().road(scene.first), nullptr);
  ASSERT_NE(scene.document.network().road(scene.second), nullptr);
  EXPECT_EQ(scene.document.undo_stack()->count(), scene.base_count + 1);

  scene.document.undo_stack()->undo();
  ASSERT_NE(scene.document.network().road(scene.first), nullptr);
  EXPECT_EQ(xodr(scene.document), scene.base_xodr);
}

TEST(DeleteTool, ClickOnEmptySpaceConsumesWithoutDeleting) {
  Scene scene;
  DeleteTool tool(scene.document);

  ToolEvent miss;
  miss.buttons = Qt::LeftButton;
  EXPECT_TRUE(tool.mouse_press(miss)); // LMB belongs to the tool (button map)
  EXPECT_EQ(scene.document.undo_stack()->count(), scene.base_count);
  EXPECT_EQ(xodr(scene.document), scene.base_xodr);
}

TEST(DeleteTool, DeletingAJunctionArmCarriesTheClosureAndUndoRestores) {
  // t_junction.xodr: incoming road "1" feeds connecting roads "10" and "11"
  // inside junction "100" (legs "2" and "3" exit it).
  Document document;
  ASSERT_TRUE(document.load(std::filesystem::path(RM_SAMPLES_DIR) / "t_junction.xodr"));
  const RoadId incoming = document.network().find_road("1");
  const JunctionId junction = document.network().find_junction("100");
  ASSERT_TRUE(incoming.is_valid());
  ASSERT_TRUE(junction.is_valid());
  const std::string base_xodr = xodr(document);

  DeleteTool tool(document);
  ASSERT_TRUE(tool.mouse_press(click_on(incoming)));

  // The closure took both connecting roads; the junction survives, emptied.
  EXPECT_EQ(document.network().road(incoming), nullptr);
  EXPECT_FALSE(document.network().find_road("10").is_valid());
  EXPECT_FALSE(document.network().find_road("11").is_valid());
  ASSERT_NE(document.network().junction(junction), nullptr);
  EXPECT_TRUE(document.network().junction(junction)->connections.empty());
  ASSERT_TRUE(document.network().find_road("2").is_valid());
  EXPECT_EQ(document.undo_stack()->count(), 1);

  document.undo_stack()->undo();
  EXPECT_EQ(xodr(document), base_xodr);
}
