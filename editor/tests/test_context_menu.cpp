// Context-menu descriptor builder (M3a): build_context_menu() is pure logic —
// the tests drive a MenuContext per pick-context and assert on the item texts,
// enabled states, and that invoke() lands the command. No QMenu needed.

#include "roadmaker/edit/operations.hpp"
#include "roadmaker/road/network.hpp"
#include "roadmaker/xodr/writer.hpp"

#include <gtest/gtest.h>

#include <QString>
#include <algorithm>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "app/actions.hpp"
#include "app/context_menu.hpp"
#include "document/document.hpp"
#include "document/selection_model.hpp"

using roadmaker::LaneId;
using roadmaker::RoadId;
using roadmaker::Waypoint;
using roadmaker::editor::Actions;
using roadmaker::editor::build_context_menu;
using roadmaker::editor::ContextMenuDeps;
using roadmaker::editor::Document;
using roadmaker::editor::MenuContext;
using roadmaker::editor::MenuItem;
using roadmaker::editor::PickHit;
using roadmaker::editor::SelectionModel;
using roadmaker::editor::WaypointHit;

namespace {

std::string xodr(const Document& document) {
  auto text = roadmaker::write_xodr(document.network());
  if (!text) {
    throw std::runtime_error(text.error().message);
  }
  return *text;
}

struct Fixture {
  Document document;
  SelectionModel selection{document};
  Actions actions{*document.undo_stack()};
  ContextMenuDeps deps{document, selection, actions};
  RoadId road;

  Fixture() {
    if (!document.push_command(
            roadmaker::edit::create_road({Waypoint{.x = 0.0, .y = 0.0},
                                          Waypoint{.x = 50.0, .y = 10.0},
                                          Waypoint{.x = 100.0, .y = 0.0}},
                                         roadmaker::LaneProfile::two_lane_default(),
                                         "Road"))) {
      throw std::runtime_error("fixture create failed");
    }
    document.network().for_each_road([&](RoadId id, const roadmaker::Road&) { road = id; });
  }

  [[nodiscard]] const MenuItem* find(const std::vector<MenuItem>& items, const char* text) const {
    for (const MenuItem& item : items) {
      if (item.text == QString(text)) {
        return &item;
      }
    }
    return nullptr;
  }
};

} // namespace

TEST(ContextMenu, RoadBodyMenuHasTheExpectedItems) {
  Fixture fx;
  MenuContext context;
  context.pick = PickHit{.road = fx.road, .lane = LaneId{}};
  context.station = 40.0;

  const std::vector<MenuItem> items = build_context_menu(context, fx.deps);
  EXPECT_NE(fx.find(items, "Insert bend point here"), nullptr);
  EXPECT_NE(fx.find(items, "Split road here"), nullptr);
  EXPECT_NE(fx.find(items, "Edit lane profile"), nullptr);
  EXPECT_NE(fx.find(items, "Edit elevation profile"), nullptr);
  EXPECT_NE(fx.find(items, "Delete road"), nullptr);
}

TEST(ContextMenu, SplitRoadHereInvokeLandsTheCommand) {
  Fixture fx;
  MenuContext context;
  context.pick = PickHit{.road = fx.road, .lane = LaneId{}};
  context.station = 40.0;

  const std::vector<MenuItem> items = build_context_menu(context, fx.deps);
  const MenuItem* split = fx.find(items, "Split road here");
  ASSERT_NE(split, nullptr);
  ASSERT_TRUE(split->enabled);

  split->invoke();
  EXPECT_EQ(fx.document.network().road_count(), 2U);
  fx.document.undo_stack()->undo();
  EXPECT_EQ(fx.document.network().road_count(), 1U);
}

TEST(ContextMenu, InsertBendInvokeAddsANode) {
  Fixture fx;
  MenuContext context;
  context.pick = PickHit{.road = fx.road, .lane = LaneId{}};
  context.station = 25.0;

  const std::string before = xodr(fx.document);
  const std::vector<MenuItem> items = build_context_menu(context, fx.deps);
  const MenuItem* insert = fx.find(items, "Insert bend point here");
  ASSERT_NE(insert, nullptr);
  insert->invoke();
  EXPECT_NE(xodr(fx.document), before);
  fx.document.undo_stack()->undo();
  EXPECT_EQ(xodr(fx.document), before);
}

TEST(ContextMenu, NodeMenuSplitIsDisabledAtEndpointsEnabledInterior) {
  Fixture fx;
  // Endpoint node (index 0) — Split at this node is disabled.
  MenuContext endpoint;
  endpoint.node =
      WaypointHit{.road = fx.road, .index = 0, .position = Waypoint{.x = 0.0, .y = 0.0}};
  const std::vector<MenuItem> endpoint_items = build_context_menu(endpoint, fx.deps);
  const MenuItem* end_split = fx.find(endpoint_items, "Split at this node");
  ASSERT_NE(end_split, nullptr);
  EXPECT_FALSE(end_split->enabled);

  // Interior node (index 1) — enabled, and its invoke splits.
  MenuContext interior;
  interior.node =
      WaypointHit{.road = fx.road, .index = 1, .position = Waypoint{.x = 50.0, .y = 10.0}};
  const std::vector<MenuItem> items = build_context_menu(interior, fx.deps);
  const MenuItem* split = fx.find(items, "Split at this node");
  ASSERT_NE(split, nullptr);
  EXPECT_TRUE(split->enabled);
  EXPECT_NE(fx.find(items, "Delete node"), nullptr);
  split->invoke();
  EXPECT_EQ(fx.document.network().road_count(), 2U);
}

TEST(ContextMenu, EmptyContextOffersFrameAll) {
  Fixture fx;
  const std::vector<MenuItem> items = build_context_menu(MenuContext{}, fx.deps);
  EXPECT_NE(fx.find(items, "Frame all"), nullptr);
}
