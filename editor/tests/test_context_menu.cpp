// Context-menu descriptor builder (M3a): build_context_menu() is pure logic —
// the tests drive a MenuContext per pick-context and assert on the item texts,
// enabled states, and that invoke() lands the command. No QMenu needed.

#include "roadmaker/edit/operations.hpp"
#include "roadmaker/road/junction.hpp"
#include "roadmaker/road/network.hpp"
#include "roadmaker/xodr/writer.hpp"

#include <gtest/gtest.h>

#include <QAction>
#include <QMenu>
#include <QString>
#include <QWidget>
#include <algorithm>
#include <array>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "app/actions.hpp"
#include "app/context_menu.hpp"
#include "document/document.hpp"
#include "document/selection_model.hpp"
#include "viewport/picking.hpp"

using roadmaker::LaneId;
using roadmaker::RoadId;
using roadmaker::Waypoint;
using roadmaker::editor::Actions;
using roadmaker::editor::assemble_context_menu;
using roadmaker::editor::build_context_menu;
using roadmaker::editor::ContextMenuDeps;
using roadmaker::editor::Document;
using roadmaker::editor::menu_context_for_pick;
using roadmaker::editor::MenuContext;
using roadmaker::editor::MenuItem;
using roadmaker::editor::PickHit;
using roadmaker::editor::SelectionModel;
using roadmaker::editor::station_to_world;
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

namespace {

roadmaker::ObjectId place_tree(Fixture& fx) {
  roadmaker::Object tree;
  tree.odr_id = "1";
  tree.name = "tree_pine";
  tree.type = roadmaker::ObjectType::Tree;
  tree.s = 40.0;
  tree.t = 4.0;
  tree.radius = 1.2;
  tree.height = 4.2;
  (void)fx.document.push_command(roadmaker::edit::add_object(fx.document.network(), fx.road, tree));
  roadmaker::ObjectId id;
  fx.document.network().for_each_object(
      [&](roadmaker::ObjectId oid, const roadmaker::Object&) { id = oid; });
  return id;
}

} // namespace

TEST(ContextMenu, ObjectMenuHasDeleteFrameDuplicate) {
  Fixture fx;
  const roadmaker::ObjectId object = place_tree(fx);
  ASSERT_TRUE(object.is_valid());

  MenuContext context;
  context.pick = PickHit{.road = fx.road, .object = object};
  const std::vector<MenuItem> items = build_context_menu(context, fx.deps);

  EXPECT_NE(fx.find(items, "Frame"), nullptr);
  EXPECT_NE(fx.find(items, "Duplicate"), nullptr);
  EXPECT_NE(fx.find(items, "Delete object"), nullptr);
  // The road-body items must NOT appear — the prop wins over the body.
  EXPECT_EQ(fx.find(items, "Split road here"), nullptr);
}

namespace {

/// Builds a 3-arm junction in `fx.document` (separate from fx.road) and returns
/// its id.
roadmaker::JunctionId build_junction(Fixture& fx) {
  const auto arm = [&](std::vector<Waypoint> waypoints) {
    (void)fx.document.push_command(roadmaker::edit::create_road(
        std::move(waypoints), roadmaker::LaneProfile::two_lane_default(), ""));
  };
  arm({Waypoint{.x = -40.0, .y = 0.0}, Waypoint{.x = -6.0, .y = 0.0}});
  arm({Waypoint{.x = 40.0, .y = 0.0}, Waypoint{.x = 6.0, .y = 0.0}});
  arm({Waypoint{.x = 0.0, .y = -40.0}, Waypoint{.x = 0.0, .y = -6.0}});
  std::vector<roadmaker::RoadEnd> ends;
  fx.document.network().for_each_road([&](RoadId id, const roadmaker::Road&) {
    if (id != fx.road) {
      ends.push_back(roadmaker::RoadEnd{.road = id, .contact = roadmaker::ContactPoint::End});
    }
  });
  (void)fx.document.push_command(
      roadmaker::edit::create_junction(fx.document.network(), std::move(ends)));
  roadmaker::JunctionId junction;
  fx.document.network().for_each_junction(
      [&](roadmaker::JunctionId id, const roadmaker::Junction&) { junction = id; });
  return junction;
}

std::size_t object_count(const Fixture& fx) {
  std::size_t count = 0;
  fx.document.network().for_each_object(
      [&](roadmaker::ObjectId, const roadmaker::Object&) { ++count; });
  return count;
}

} // namespace

TEST(ContextMenu, JunctionFloorPickReachesTheJunctionMenu) {
  Fixture fx;
  const roadmaker::JunctionId junction = build_junction(fx);
  ASSERT_TRUE(junction.is_valid());

  // What picking.cpp reports for a junction floor: the JunctionId, road/lane
  // invalid. Nothing forwarded it into the MenuContext, so build_context_menu's
  // junction block was unreachable in the shipped app.
  const PickHit floor{.junction = junction, .position = {0.0, 0.0, 0.0}};
  const MenuContext context = menu_context_for_pick(fx.document.network(), floor);
  ASSERT_TRUE(context.junction.has_value());
  EXPECT_EQ(*context.junction, junction);
  EXPECT_FALSE(context.station.has_value()); // no road under a floor hit

  const std::vector<MenuItem> items = build_context_menu(context, fx.deps);
  EXPECT_NE(fx.find(items, "Add crosswalks to all arms"), nullptr);
  EXPECT_NE(fx.find(items, "Add lane arrows to all arms"), nullptr);
  EXPECT_NE(fx.find(items, "Add centre lines to all arms"), nullptr);
  EXPECT_NE(fx.find(items, "Delete junction"), nullptr);
}

TEST(ContextMenu, RoadPickCarriesTheStationAndNoJunction) {
  Fixture fx;
  const roadmaker::Road* road = fx.document.network().road(fx.road);
  ASSERT_NE(road, nullptr);
  const auto world = station_to_world(road->plan_view, 40.0, 0.0);

  const PickHit hit{.road = fx.road, .position = {world[0], world[1], 0.0}};
  const MenuContext context = menu_context_for_pick(fx.document.network(), hit);
  EXPECT_FALSE(context.junction.has_value());
  ASSERT_TRUE(context.station.has_value());
  EXPECT_NEAR(*context.station, 40.0, 0.5);
}

TEST(ContextMenu, AddCrosswalksToAllArmsIsOneUndoableMacro) {
  Fixture fx;
  const roadmaker::JunctionId junction = build_junction(fx);
  ASSERT_TRUE(junction.is_valid());

  MenuContext context;
  context.junction = junction;
  const std::vector<MenuItem> items = build_context_menu(context, fx.deps);
  const MenuItem* add = fx.find(items, "Add crosswalks to all arms");
  ASSERT_NE(add, nullptr);
  ASSERT_TRUE(add->enabled);

  ASSERT_EQ(object_count(fx), 0U);
  add->invoke();
  EXPECT_EQ(object_count(fx), 3U); // one crosswalk per arm

  // The whole batch is a single undo step (QUndoStack macro).
  fx.document.undo_stack()->undo();
  EXPECT_EQ(object_count(fx), 0U);
  fx.document.undo_stack()->redo();
  EXPECT_EQ(object_count(fx), 3U);
}

TEST(ContextMenu, AddCrosswalksAlsoLinksEachArmsStopLine) {
  // "Add stop lines to all arms" is gone (p4-s3, #318): every arm already HAS a
  // stop line, so the action would be a no-op. What the crosswalk action does
  // now is link each placed crosswalk to its arm's derived line, in the SAME
  // macro, so one undo still removes the whole batch.
  Fixture fx;
  const roadmaker::JunctionId junction = build_junction(fx);
  ASSERT_TRUE(junction.is_valid());

  MenuContext context;
  context.junction = junction;
  const std::vector<MenuItem> items = build_context_menu(context, fx.deps);
  EXPECT_EQ(fx.find(items, "Add stop lines to all arms"), nullptr)
      << "the retired action must not linger in the menu";

  const MenuItem* add = fx.find(items, "Add crosswalks to all arms");
  ASSERT_NE(add, nullptr);
  ASSERT_TRUE(add->enabled);

  add->invoke();
  EXPECT_EQ(object_count(fx), 3U); // one crosswalk per arm, no stop-line objects
  const roadmaker::Junction* record = fx.document.network().junction(junction);
  ASSERT_NE(record, nullptr);
  EXPECT_EQ(record->stoplines.size(), 3U) << "each arm's line records its crosswalk";
  for (const roadmaker::StopLine& line : record->stoplines) {
    EXPECT_FALSE(line.crosswalk_odr_id.empty());
  }

  fx.document.undo_stack()->undo();
  EXPECT_EQ(object_count(fx), 0U);
  EXPECT_TRUE(fx.document.network().junction(junction)->stoplines.empty());
}

TEST(ContextMenu, AddLaneArrowsToAllArmsIsOneUndoableMacro) {
  Fixture fx;
  const roadmaker::JunctionId junction = build_junction(fx);
  ASSERT_TRUE(junction.is_valid());

  MenuContext context;
  context.junction = junction;
  const std::vector<MenuItem> items = build_context_menu(context, fx.deps);
  const MenuItem* add = fx.find(items, "Add lane arrows to all arms");
  ASSERT_NE(add, nullptr);
  ASSERT_TRUE(add->enabled);

  add->invoke();
  EXPECT_EQ(object_count(fx), 3U); // one straight arrow per approach lane
  fx.document.undo_stack()->undo();
  EXPECT_EQ(object_count(fx), 0U);
}

TEST(ContextMenu, AddCentreLinesToAllArmsIsOneUndoableMacro) {
  Fixture fx;
  const roadmaker::JunctionId junction = build_junction(fx);
  ASSERT_TRUE(junction.is_valid());

  MenuContext context;
  context.junction = junction;
  const std::vector<MenuItem> items = build_context_menu(context, fx.deps);
  const MenuItem* add = fx.find(items, "Add centre lines to all arms");
  ASSERT_NE(add, nullptr);
  ASSERT_TRUE(add->enabled);

  // Centre lines are lane roadMarks, not objects: the object count never moves,
  // so this asserts on the document instead.
  const std::string before = xodr(fx.document);
  EXPECT_EQ(before.find("color=\"yellow\""), std::string::npos);

  add->invoke();
  const std::string after = xodr(fx.document);
  EXPECT_NE(after.find("type=\"solid solid\""), std::string::npos);
  EXPECT_NE(after.find("color=\"yellow\""), std::string::npos);
  EXPECT_EQ(object_count(fx), 0U);

  // One undo step for the whole batch, restoring the document byte-for-byte.
  fx.document.undo_stack()->undo();
  EXPECT_EQ(xodr(fx.document), before);
  fx.document.undo_stack()->redo();
  EXPECT_EQ(xodr(fx.document), after);
}

TEST(ContextMenu, DeleteObjectInvokeLandsTheCommand) {
  Fixture fx;
  const roadmaker::ObjectId object = place_tree(fx);
  MenuContext context;
  context.pick = PickHit{.road = fx.road, .object = object};

  const std::vector<MenuItem> items = build_context_menu(context, fx.deps);
  const MenuItem* del = fx.find(items, "Delete object");
  ASSERT_NE(del, nullptr);
  del->invoke();
  EXPECT_EQ(fx.document.network().object_count(), 0U);
  fx.document.undo_stack()->undo();
  EXPECT_EQ(fx.document.network().object_count(), 1U);
}

TEST(ContextMenu, DuplicateObjectAddsASecondPropWithAFreshId) {
  Fixture fx;
  const roadmaker::ObjectId object = place_tree(fx);
  MenuContext context;
  context.pick = PickHit{.road = fx.road, .object = object};

  const std::vector<MenuItem> items = build_context_menu(context, fx.deps);
  const MenuItem* dup = fx.find(items, "Duplicate");
  ASSERT_NE(dup, nullptr);
  ASSERT_TRUE(dup->enabled);
  dup->invoke();
  EXPECT_EQ(fx.document.network().object_count(), 2U);
  // Both ids are unique in the file (id_unique_in_class holds after duplicate).
  std::vector<std::string> ids;
  fx.document.network().for_each_object(
      [&](roadmaker::ObjectId, const roadmaker::Object& o) { ids.push_back(o.odr_id); });
  ASSERT_EQ(ids.size(), 2U);
  EXPECT_NE(ids[0], ids[1]);
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

TEST(ContextMenu, EmptyContextOffersSceneWideItems) {
  Fixture fx;
  const std::vector<MenuItem> items = build_context_menu(MenuContext{}, fx.deps);
  EXPECT_NE(fx.find(items, "Create road here"), nullptr);
  EXPECT_NE(fx.find(items, "Frame all"), nullptr);
  const MenuItem* paste = fx.find(items, "Paste");
  ASSERT_NE(paste, nullptr);
  EXPECT_FALSE(paste->enabled); // stub
}

TEST(ContextMenu, JunctionMenuFramesAndDeletes) {
  Document document;
  SelectionModel selection{document};
  Actions actions{*document.undo_stack()};
  ContextMenuDeps deps{document, selection, actions};

  // Two arms meeting at (50,0) joined into a junction.
  ASSERT_TRUE(document.push_command(
      roadmaker::edit::create_road({Waypoint{.x = 0.0, .y = 0.0}, Waypoint{.x = 50.0, .y = 0.0}},
                                   roadmaker::LaneProfile::two_lane_default(),
                                   "A")));
  ASSERT_TRUE(document.push_command(
      roadmaker::edit::create_road({Waypoint{.x = 50.0, .y = 0.0}, Waypoint{.x = 50.0, .y = 50.0}},
                                   roadmaker::LaneProfile::two_lane_default(),
                                   "B")));
  RoadId a;
  RoadId b;
  document.network().for_each_road([&](RoadId id, const roadmaker::Road& r) {
    if (r.name == "A") {
      a = id;
    } else if (r.name == "B") {
      b = id;
    }
  });
  const std::array<roadmaker::RoadEnd, 2> ends{
      roadmaker::RoadEnd{.road = a, .contact = roadmaker::ContactPoint::End},
      roadmaker::RoadEnd{.road = b, .contact = roadmaker::ContactPoint::Start}};
  ASSERT_TRUE(document.push_command(roadmaker::edit::create_junction(document.network(), ends)));
  roadmaker::JunctionId junction;
  document.network().for_each_junction(
      [&](roadmaker::JunctionId id, const roadmaker::Junction&) { junction = id; });
  ASSERT_TRUE(junction.is_valid());

  MenuContext context;
  context.junction = junction;
  const std::vector<MenuItem> items = build_context_menu(context, deps);
  const MenuItem* del = nullptr;
  for (const MenuItem& item : items) {
    if (item.text == QString("Delete junction")) {
      del = &item;
    }
  }
  ASSERT_NE(del, nullptr);
  del->invoke();
  EXPECT_EQ(document.network().junction(junction), nullptr);
}

// Regression (gate finding 6, the hard blocker): the invoke closures must
// outlive the ContextMenuDeps bundle the caller assembled them from. MainWindow
// builds `ContextMenuDeps` as a stack local and shows the menu with the
// non-blocking QMenu::popup() — the deps local dies immediately, so a later
// click used to dereference freed stack memory (use-after-free that crashed the
// editor on right-click actions). The document/selection/actions it *references*
// are long-lived, so the closures must hold those by value. ASan turns the old
// bug into a hard failure here; both tests let `deps` die before the invoke.
TEST(ContextMenu, InvokeSurvivesDepsGoingOutOfScope) {
  Document document;
  SelectionModel selection{document};
  Actions actions{*document.undo_stack()};
  ASSERT_TRUE(
      document.push_command(roadmaker::edit::create_road({Waypoint{.x = 0.0, .y = 0.0},
                                                          Waypoint{.x = 50.0, .y = 10.0},
                                                          Waypoint{.x = 100.0, .y = 0.0}},
                                                         roadmaker::LaneProfile::two_lane_default(),
                                                         "Road")));
  RoadId road;
  document.network().for_each_road([&](RoadId id, const roadmaker::Road&) { road = id; });

  MenuContext context;
  context.pick = PickHit{.road = road, .lane = LaneId{}};
  context.station = 40.0;

  // Build the descriptor from a deps that dies at the end of this block — a
  // by-reference capture would now dangle.
  std::vector<MenuItem> items;
  {
    ContextMenuDeps deps{document, selection, actions};
    items = build_context_menu(context, deps);
  }

  const MenuItem* split = nullptr;
  for (const MenuItem& item : items) {
    if (item.text == QString("Split road here")) {
      split = &item;
    }
  }
  ASSERT_NE(split, nullptr);
  split->invoke();
  EXPECT_EQ(document.network().road_count(), 2U);
}

TEST(ContextMenu, AssembledMenuActionSurvivesDepsGoingOutOfScope) {
  Document document;
  SelectionModel selection{document};
  Actions actions{*document.undo_stack()};
  ASSERT_TRUE(
      document.push_command(roadmaker::edit::create_road({Waypoint{.x = 0.0, .y = 0.0},
                                                          Waypoint{.x = 50.0, .y = 10.0},
                                                          Waypoint{.x = 100.0, .y = 0.0}},
                                                         roadmaker::LaneProfile::two_lane_default(),
                                                         "Road")));
  RoadId road;
  document.network().for_each_road([&](RoadId id, const roadmaker::Road&) { road = id; });

  MenuContext context;
  context.pick = PickHit{.road = road, .lane = LaneId{}};
  context.station = 40.0;

  // The full QMenu path MainWindow uses: the menu is parented to a widget (as the
  // real WA_DeleteOnClose menu is to MainWindow) and outlives the deps local.
  QWidget parent;
  QMenu* menu = nullptr;
  {
    ContextMenuDeps deps{document, selection, actions};
    menu = assemble_context_menu(context, deps, &parent);
  }
  ASSERT_NE(menu, nullptr);

  QAction* split = nullptr;
  for (QAction* action : menu->actions()) {
    if (action->text() == QString("Split road here")) {
      split = action;
    }
  }
  ASSERT_NE(split, nullptr);
  split->trigger(); // the deferred click — this was the use-after-free
  EXPECT_EQ(document.network().road_count(), 2U);
}

TEST(ContextMenu, LaneBranchRemovesTheOutermostLaneAndKeepsRoadItems) {
  Fixture fx;
  // two_lane_default lanes: +1, -1, -2. Outermost right (-2) is removable;
  // the inner right lane (-1) is not.
  LaneId outer;
  LaneId inner;
  const auto& section =
      *fx.document.network().lane_section(fx.document.network().road(fx.road)->sections.front());
  for (const LaneId id : section.lanes) {
    const int odr = fx.document.network().lane(id)->odr_id;
    if (odr == -2) {
      outer = id;
    } else if (odr == -1) {
      inner = id;
    }
  }
  ASSERT_TRUE(outer.is_valid());
  ASSERT_TRUE(inner.is_valid());

  MenuContext context;
  context.pick = PickHit{.road = fx.road, .lane = outer};
  context.station = 40.0;
  const std::vector<MenuItem> items = build_context_menu(context, fx.deps);
  const MenuItem* remove = fx.find(items, "Remove this lane");
  ASSERT_NE(remove, nullptr);
  EXPECT_TRUE(remove->enabled);
  // The road-body items stay reachable alongside the lane item.
  EXPECT_NE(fx.find(items, "Split road here"), nullptr);
  EXPECT_NE(fx.find(items, "Delete road"), nullptr);

  // A non-outermost lane shows the item disabled.
  MenuContext inner_context;
  inner_context.pick = PickHit{.road = fx.road, .lane = inner};
  inner_context.station = 40.0;
  const std::vector<MenuItem> inner_items = build_context_menu(inner_context, fx.deps);
  const MenuItem* inner_remove = fx.find(inner_items, "Remove this lane");
  ASSERT_NE(inner_remove, nullptr);
  EXPECT_FALSE(inner_remove->enabled);

  // Invoke removes lane -2; undo restores it.
  const auto has_lane = [&](int odr) {
    const auto& sec =
        *fx.document.network().lane_section(fx.document.network().road(fx.road)->sections.front());
    for (const LaneId id : sec.lanes) {
      if (fx.document.network().lane(id)->odr_id == odr) {
        return true;
      }
    }
    return false;
  };
  remove->invoke();
  EXPECT_FALSE(has_lane(-2));
  fx.document.undo_stack()->undo();
  EXPECT_TRUE(has_lane(-2));
}

TEST(ContextMenu, MergeSelectedIsEnabledForTwoMergeableRoads) {
  Document document;
  SelectionModel selection{document};
  Actions actions{*document.undo_stack()};
  ContextMenuDeps deps{document, selection, actions};

  ASSERT_TRUE(document.push_command(
      roadmaker::edit::create_road({Waypoint{.x = 0.0, .y = 0.0}, Waypoint{.x = 50.0, .y = 0.0}},
                                   roadmaker::LaneProfile::two_lane_default(),
                                   "A")));
  ASSERT_TRUE(document.push_command(
      roadmaker::edit::create_road({Waypoint{.x = 50.0, .y = 0.0}, Waypoint{.x = 100.0, .y = 0.0}},
                                   roadmaker::LaneProfile::two_lane_default(),
                                   "B")));
  std::vector<RoadId> roads;
  document.network().for_each_road([&](RoadId id, const roadmaker::Road&) { roads.push_back(id); });
  ASSERT_EQ(roads.size(), 2U);
  selection.select({.road = roads[0], .lane = LaneId{}});
  selection.select({.road = roads[1], .lane = LaneId{}}, roadmaker::editor::SelectMode::Add);

  MenuContext context;
  context.pick = PickHit{.road = roads[0], .lane = LaneId{}};
  context.station = 25.0;
  const std::vector<MenuItem> items = build_context_menu(context, deps);
  const MenuItem* merge = nullptr;
  for (const MenuItem& item : items) {
    if (item.text == QString("Merge selected roads")) {
      merge = &item;
    }
  }
  ASSERT_NE(merge, nullptr);
  EXPECT_TRUE(merge->enabled);
  merge->invoke();
  EXPECT_EQ(document.network().road_count(), 1U);
}

TEST(ContextMenu, LinkEndsWeldsTwoSelectedRoadsWithNearbyFreeEnds) {
  Document document;
  SelectionModel selection{document};
  Actions actions{*document.undo_stack()};
  ContextMenuDeps deps{document, selection, actions};
  // Two roads with a 30 m gap between A's END and B's START — not mergeable
  // (ends don't coincide) but linkable via close_gap.
  ASSERT_TRUE(document.push_command(
      roadmaker::edit::create_road({Waypoint{.x = 0.0, .y = 0.0}, Waypoint{.x = 100.0, .y = 0.0}},
                                   roadmaker::LaneProfile::two_lane_default(),
                                   "A")));
  ASSERT_TRUE(document.push_command(
      roadmaker::edit::create_road({Waypoint{.x = 130.0, .y = 0.0}, Waypoint{.x = 230.0, .y = 0.0}},
                                   roadmaker::LaneProfile::two_lane_default(),
                                   "B")));
  std::vector<RoadId> roads;
  document.network().for_each_road([&](RoadId id, const roadmaker::Road&) { roads.push_back(id); });
  ASSERT_EQ(roads.size(), 2U);
  selection.select({.road = roads[0], .lane = LaneId{}});
  selection.select({.road = roads[1], .lane = LaneId{}}, roadmaker::editor::SelectMode::Add);

  MenuContext context;
  context.pick = PickHit{.road = roads[0], .lane = LaneId{}};
  context.station = 25.0;
  const std::vector<MenuItem> items = build_context_menu(context, deps);
  const MenuItem* link = nullptr;
  for (const MenuItem& item : items) {
    if (item.text == QString("Link Ends")) {
      link = &item;
    }
  }
  ASSERT_NE(link, nullptr);
  EXPECT_TRUE(link->enabled);
  const std::size_t before = document.network().road_count();
  link->invoke();
  EXPECT_EQ(document.network().road_count(), before + 1); // a connector road welds them
}
