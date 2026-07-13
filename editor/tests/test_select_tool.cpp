// Select/Move tool (issue #9): headless ToolEvent sequences drive
// click/modifier selection, rubber bands, and node drags; the tests assert
// on SelectionModel state, the undo stack, the serialized network, snapping,
// and preview geometry — the M2 tool test seam
// (docs/design/m2/01_editing_framework.md §4).

#include "roadmaker/edit/operations.hpp"
#include "roadmaker/edit/snap.hpp"
#include "roadmaker/tol.hpp"
#include "roadmaker/xodr/writer.hpp"

#include <gtest/gtest.h>

#include <QSignalSpy>
#include <QString>
#include <array>
#include <cstddef>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "document/document.hpp"
#include "document/selection_model.hpp"
#include "tools/select_tool.hpp"

using roadmaker::LaneId;
using roadmaker::RoadId;
using roadmaker::Waypoint;
using roadmaker::editor::Document;
using roadmaker::editor::PickHit;
using roadmaker::editor::SelectionEntry;
using roadmaker::editor::SelectionModel;
using roadmaker::editor::SelectTool;
using roadmaker::editor::ToolEvent;

namespace {

std::string xodr(const Document& document) {
  auto text = roadmaker::write_xodr(document.network());
  if (!text) {
    throw std::runtime_error(text.error().message);
  }
  return *text;
}

ToolEvent at(double x,
             double y,
             Qt::MouseButtons buttons = Qt::NoButton,
             Qt::KeyboardModifiers modifiers = Qt::NoModifier,
             std::optional<PickHit> pick = std::nullopt) {
  ToolEvent event;
  event.world_x = x;
  event.world_y = y;
  event.buttons = buttons;
  event.modifiers = modifiers;
  event.pick = pick;
  return event;
}

/// Document with road "Dragged" (0,0)-(50,10)-(100,0) and a snap-target road
/// "Other" starting at (120,0); base state captured after both creates.
/// Document/SelectionModel are QObjects (pinned in place), hence setup in
/// the constructor.
struct Scene {
  Document document;
  SelectionModel selection{document};
  RoadId dragged;
  RoadId other;
  int base_count = 0;
  std::string base_xodr;

  Scene() {
    auto road = document.push_command(
        roadmaker::edit::create_road({Waypoint{.x = 0.0, .y = 0.0},
                                      Waypoint{.x = 50.0, .y = 10.0},
                                      Waypoint{.x = 100.0, .y = 0.0}},
                                     roadmaker::LaneProfile::two_lane_default(),
                                     "Dragged"));
    auto second = document.push_command(roadmaker::edit::create_road(
        {Waypoint{.x = 120.0, .y = 0.0}, Waypoint{.x = 200.0, .y = 0.0}},
        roadmaker::LaneProfile::two_lane_default(),
        "Other"));
    if (!road || !second) {
      throw std::runtime_error("scene setup failed");
    }
    document.network().for_each_road([&](RoadId id, const roadmaker::Road& r) {
      if (r.name == "Dragged") {
        dragged = id;
      } else if (r.name == "Other") {
        other = id;
      }
    });
    if (!dragged.is_valid() || !other.is_valid()) {
      throw std::runtime_error("scene roads not found");
    }
    base_count = document.undo_stack()->count();
    base_xodr = xodr(document);
  }

  /// Synthetic lane-patch hit as the viewport would deliver it.
  [[nodiscard]] PickHit hit(RoadId road) const {
    const roadmaker::Road* road_ptr = document.network().road(road);
    if (road_ptr == nullptr || road_ptr->sections.empty()) {
      throw std::runtime_error("road has no sections");
    }
    const auto* section = document.network().lane_section(road_ptr->sections.front());
    if (section == nullptr || section->lanes.empty()) {
      throw std::runtime_error("section has no lanes");
    }
    return PickHit{.road = road, .lane = section->lanes.back()};
  }
};

Waypoint node(const Document& document, RoadId road, std::size_t index) {
  const roadmaker::Road* road_ptr = document.network().road(road);
  if (road_ptr == nullptr || !road_ptr->authoring_waypoints) {
    throw std::runtime_error("road lost its waypoints");
  }
  return (*road_ptr->authoring_waypoints)[index];
}

} // namespace

// --- selection: clicks -----------------------------------------------------

TEST(SelectTool, ClickSelectsPickAndMissClears) {
  Scene scene;
  SelectTool tool(scene.document, scene.selection);

  const PickHit hit = scene.hit(scene.dragged);
  ASSERT_TRUE(tool.mouse_press(at(50.0, 3.0, Qt::LeftButton, Qt::NoModifier, hit)));
  ASSERT_TRUE(tool.mouse_release(at(50.0, 3.0, Qt::NoButton, Qt::NoModifier, hit)));
  ASSERT_EQ(scene.selection.entries().size(), 1U);
  EXPECT_EQ(scene.selection.primary(), (SelectionEntry{.road = hit.road, .lane = hit.lane}));

  // Click on empty space clears; the network itself is untouched.
  ASSERT_TRUE(tool.mouse_press(at(300.0, 300.0, Qt::LeftButton)));
  ASSERT_TRUE(tool.mouse_release(at(300.0, 300.0)));
  EXPECT_TRUE(scene.selection.empty());
  EXPECT_EQ(scene.document.undo_stack()->count(), scene.base_count);
  EXPECT_EQ(xodr(scene.document), scene.base_xodr);
}

TEST(SelectTool, ShiftAddsAndCtrlToggles) {
  Scene scene;
  SelectTool tool(scene.document, scene.selection);
  const PickHit first = scene.hit(scene.dragged);
  const PickHit second = scene.hit(scene.other);

  ASSERT_TRUE(tool.mouse_press(at(50.0, 3.0, Qt::LeftButton, Qt::NoModifier, first)));
  ASSERT_TRUE(tool.mouse_release(at(50.0, 3.0, Qt::NoButton, Qt::NoModifier, first)));
  ASSERT_TRUE(tool.mouse_press(at(160.0, -3.0, Qt::LeftButton, Qt::ShiftModifier, second)));
  ASSERT_TRUE(tool.mouse_release(at(160.0, -3.0, Qt::NoButton, Qt::ShiftModifier, second)));
  EXPECT_EQ(scene.selection.entries().size(), 2U);

  // Ctrl-click toggles the present entry back out.
  ASSERT_TRUE(tool.mouse_press(at(160.0, -3.0, Qt::LeftButton, Qt::ControlModifier, second)));
  ASSERT_TRUE(tool.mouse_release(at(160.0, -3.0, Qt::NoButton, Qt::ControlModifier, second)));
  ASSERT_EQ(scene.selection.entries().size(), 1U);
  EXPECT_EQ(scene.selection.primary().road, scene.dragged);

  // A modified click on empty space keeps the selection.
  ASSERT_TRUE(tool.mouse_press(at(300.0, 300.0, Qt::LeftButton, Qt::ShiftModifier)));
  ASSERT_TRUE(tool.mouse_release(at(300.0, 300.0, Qt::NoButton, Qt::ShiftModifier)));
  EXPECT_EQ(scene.selection.entries().size(), 1U);
}

// --- selection: rubber band --------------------------------------------------

TEST(SelectTool, RubberBandSelectsIntersectingRoads) {
  Scene scene;
  SelectTool tool(scene.document, scene.selection);

  // Band over "Dragged" only ("Other" starts at x=120, outside x<=105).
  ASSERT_TRUE(tool.mouse_press(at(-10.0, -20.0, Qt::LeftButton)));
  ASSERT_TRUE(tool.mouse_move(at(105.0, 25.0, Qt::LeftButton)));
  EXPECT_TRUE(tool.banding());
  EXPECT_FALSE(tool.preview().line_positions.empty()); // band rectangle drawn
  ASSERT_TRUE(tool.mouse_release(at(105.0, 25.0)));

  ASSERT_EQ(scene.selection.entries().size(), 1U);
  EXPECT_EQ(scene.selection.primary(),
            (SelectionEntry{.road = scene.dragged, .lane = LaneId{}})); // road-level

  // Shift-band over "Other" adds to the selection.
  ASSERT_TRUE(tool.mouse_press(at(110.0, -20.0, Qt::LeftButton, Qt::ShiftModifier)));
  ASSERT_TRUE(tool.mouse_move(at(250.0, 25.0, Qt::LeftButton, Qt::ShiftModifier)));
  ASSERT_TRUE(tool.mouse_release(at(250.0, 25.0, Qt::NoButton, Qt::ShiftModifier)));
  EXPECT_EQ(scene.selection.entries().size(), 2U);

  // Selection is view state: nothing on the stack, network untouched.
  EXPECT_EQ(scene.document.undo_stack()->count(), scene.base_count);
  EXPECT_EQ(xodr(scene.document), scene.base_xodr);
}

TEST(SelectTool, ZeroAreaBandDegeneratesToClick) {
  Scene scene;
  SelectTool tool(scene.document, scene.selection);
  const PickHit hit = scene.hit(scene.dragged);

  // Moves inside the click tolerance never open a band; the release picks.
  ASSERT_TRUE(tool.mouse_press(at(50.0, 3.0, Qt::LeftButton, Qt::NoModifier, hit)));
  ASSERT_TRUE(tool.mouse_move(at(50.1, 3.1, Qt::LeftButton, Qt::NoModifier, hit)));
  EXPECT_FALSE(tool.banding());
  ASSERT_TRUE(tool.mouse_release(at(50.1, 3.1, Qt::NoButton, Qt::NoModifier, hit)));
  ASSERT_EQ(scene.selection.entries().size(), 1U);
  EXPECT_EQ(scene.selection.primary().road, scene.dragged);
}

TEST(SelectTool, EscapeCancelsARubberBand) {
  Scene scene;
  SelectTool tool(scene.document, scene.selection);
  scene.selection.select({.road = scene.other, .lane = LaneId{}});

  ASSERT_TRUE(tool.mouse_press(at(-10.0, -20.0, Qt::LeftButton)));
  ASSERT_TRUE(tool.mouse_move(at(105.0, 25.0, Qt::LeftButton)));
  ASSERT_TRUE(tool.key_press(Qt::Key_Escape, Qt::NoModifier));

  EXPECT_FALSE(tool.banding());
  // The cancelled band changed nothing, and the release is no longer consumed.
  ASSERT_EQ(scene.selection.entries().size(), 1U);
  EXPECT_EQ(scene.selection.primary().road, scene.other);
  EXPECT_FALSE(tool.mouse_release(at(105.0, 25.0)));
}

// --- node drag (absorbed phase 0 gate prototype, issue #37) ------------------

TEST(SelectTool, NodeHandlesRequireASelectedRoad) {
  Scene scene;
  SelectTool tool(scene.document, scene.selection);

  // Right button is camera orbit in the M2 map — never consumed.
  EXPECT_FALSE(tool.mouse_press(at(50.5, 10.5, Qt::RightButton)));

  // Unselected road: a press at its node is a click, not a grab...
  const PickHit hit = scene.hit(scene.dragged);
  ASSERT_TRUE(tool.mouse_press(at(50.5, 10.5, Qt::LeftButton, Qt::NoModifier, hit)));
  EXPECT_FALSE(tool.dragging());
  ASSERT_TRUE(tool.mouse_release(at(50.5, 10.5, Qt::NoButton, Qt::NoModifier, hit)));
  ASSERT_FALSE(scene.selection.empty());

  // ...and once selected the same press grabs the node, even over a lane
  // patch (handles pick with priority, 02 §1).
  ASSERT_TRUE(tool.mouse_press(at(50.5, 10.5, Qt::LeftButton, Qt::NoModifier, hit)));
  EXPECT_TRUE(tool.dragging());
  EXPECT_FALSE(scene.document.preview_active()); // preview starts on first move
}

TEST(SelectTool, DragRefitsLiveAndReleaseCommitsOnce) {
  Scene scene;
  SelectTool tool(scene.document, scene.selection);
  scene.selection.select({.road = scene.dragged, .lane = LaneId{}});
  QSignalSpy preview_spy(&tool, &roadmaker::editor::Tool::preview_changed);

  ASSERT_TRUE(tool.mouse_press(at(50.5, 10.5, Qt::LeftButton)));
  ASSERT_TRUE(tool.mouse_move(at(50.0, 25.0, Qt::LeftButton)));
  EXPECT_EQ(node(scene.document, scene.dragged, 1), (Waypoint{.x = 50.0, .y = 25.0}));
  ASSERT_TRUE(tool.mouse_move(at(50.0, 40.0, Qt::LeftButton)));
  EXPECT_EQ(node(scene.document, scene.dragged, 1), (Waypoint{.x = 50.0, .y = 40.0}));

  EXPECT_TRUE(scene.document.preview_active());
  EXPECT_EQ(scene.document.undo_stack()->count(), scene.base_count);
  EXPECT_FALSE(tool.preview().line_positions.empty()); // original→current tether
  EXPECT_GE(preview_spy.count(), 3);                   // press + each move

  ASSERT_TRUE(tool.mouse_release(at(50.0, 40.0)));
  EXPECT_FALSE(tool.dragging());
  EXPECT_FALSE(scene.document.preview_active());
  EXPECT_EQ(scene.document.undo_stack()->count(), scene.base_count + 1);
  const std::string committed = xodr(scene.document);

  scene.document.undo_stack()->undo();
  EXPECT_EQ(xodr(scene.document), scene.base_xodr);
  scene.document.undo_stack()->redo();
  EXPECT_EQ(xodr(scene.document), committed);
}

TEST(SelectTool, EscapeCancelsDragByteIdentical) {
  Scene scene;
  SelectTool tool(scene.document, scene.selection);
  scene.selection.select({.road = scene.dragged, .lane = LaneId{}});

  ASSERT_TRUE(tool.mouse_press(at(50.5, 10.5, Qt::LeftButton)));
  ASSERT_TRUE(tool.mouse_move(at(70.0, 55.0, Qt::LeftButton)));
  ASSERT_TRUE(tool.key_press(Qt::Key_Escape, Qt::NoModifier));

  EXPECT_FALSE(tool.dragging());
  EXPECT_FALSE(scene.document.preview_active());
  EXPECT_EQ(xodr(scene.document), scene.base_xodr);
  EXPECT_EQ(scene.document.undo_stack()->count(), scene.base_count);

  // The interaction is over: further events are not consumed.
  EXPECT_FALSE(tool.mouse_move(at(75.0, 60.0, Qt::LeftButton)));
  EXPECT_FALSE(tool.mouse_release(at(75.0, 60.0)));
  EXPECT_FALSE(tool.key_press(Qt::Key_Escape, Qt::NoModifier));
}

TEST(SelectTool, GrabWithoutMovePushesNothing) {
  Scene scene;
  SelectTool tool(scene.document, scene.selection);
  scene.selection.select({.road = scene.dragged, .lane = LaneId{}});

  ASSERT_TRUE(tool.mouse_press(at(50.5, 10.5, Qt::LeftButton)));
  ASSERT_TRUE(tool.mouse_release(at(50.5, 10.5)));

  EXPECT_EQ(scene.document.undo_stack()->count(), scene.base_count);
  EXPECT_EQ(xodr(scene.document), scene.base_xodr);
}

TEST(SelectTool, SnapEngagesOnAnotherRoadsEndpoint) {
  Scene scene;
  SelectTool tool(scene.document, scene.selection);
  scene.selection.select({.road = scene.dragged, .lane = LaneId{}});
  tool.set_snap_options({.radius = 2.0});

  // Drag the END node (100,0); without SnapOptions::exclude_road its own
  // moving endpoint would be the closest candidate every frame.
  ASSERT_TRUE(tool.mouse_press(at(100.0, 0.0, Qt::LeftButton)));
  ASSERT_TRUE(tool.mouse_move(at(119.0, 0.5, Qt::LeftButton)));

  const Waypoint snapped = node(scene.document, scene.dragged, 2);
  EXPECT_NEAR(snapped.x, 120.0, roadmaker::tol::kRoundTripPosition);
  EXPECT_NEAR(snapped.y, 0.0, roadmaker::tol::kRoundTripPosition);
  // 3 handles of the selected road + the snap hint marker.
  EXPECT_EQ(tool.preview().handles.size(), 4U);

  ASSERT_TRUE(tool.mouse_release(at(119.0, 0.5)));
  EXPECT_EQ(scene.document.undo_stack()->count(), scene.base_count + 1);
  scene.document.undo_stack()->undo();
  EXPECT_EQ(xodr(scene.document), scene.base_xodr);
}

TEST(SelectTool, DeactivateCancelsARunningDrag) {
  Scene scene;
  SelectTool tool(scene.document, scene.selection);
  scene.selection.select({.road = scene.dragged, .lane = LaneId{}});

  ASSERT_TRUE(tool.mouse_press(at(50.5, 10.5, Qt::LeftButton)));
  ASSERT_TRUE(tool.mouse_move(at(70.0, 55.0, Qt::LeftButton)));
  tool.deactivate();

  EXPECT_FALSE(tool.dragging());
  EXPECT_FALSE(scene.document.preview_active());
  EXPECT_EQ(xodr(scene.document), scene.base_xodr);
  EXPECT_EQ(scene.document.undo_stack()->count(), scene.base_count);
}

// --- preview geometry --------------------------------------------------------

TEST(SelectTool, PreviewShowsHandlesForSelectedRoads) {
  Scene scene;
  SelectTool tool(scene.document, scene.selection);

  EXPECT_TRUE(tool.preview().empty());

  scene.selection.select({.road = scene.dragged, .lane = LaneId{}});
  EXPECT_EQ(tool.preview().handles.size(), 3U); // 3 waypoints

  // A lane entry of the other road adds ITS road's handles once.
  scene.selection.select({.road = scene.other, .lane = scene.hit(scene.other).lane},
                         roadmaker::editor::SelectMode::Add);
  EXPECT_EQ(tool.preview().handles.size(), 5U); // 3 + 2 waypoints
}

// --- Delete key (issue #11, 02 §7) --------------------------------------------

TEST(SelectTool, DeleteKeyDeletesMultiSelectionAsOneUndoStep) {
  Scene scene;
  SelectTool tool(scene.document, scene.selection);
  scene.selection.select_many(std::vector<SelectionEntry>{{.road = scene.dragged, .lane = LaneId{}},
                                                          {.road = scene.other, .lane = LaneId{}}},
                              roadmaker::editor::SelectMode::Replace);

  ASSERT_TRUE(tool.key_press(Qt::Key_Delete, Qt::NoModifier));

  EXPECT_EQ(scene.document.network().road(scene.dragged), nullptr);
  EXPECT_EQ(scene.document.network().road(scene.other), nullptr);
  // ONE macro on the stack; the selection pruned itself.
  EXPECT_EQ(scene.document.undo_stack()->count(), scene.base_count + 1);
  EXPECT_TRUE(scene.selection.empty());

  // One Ctrl+Z restores everything byte-identically under the original ids.
  scene.document.undo_stack()->undo();
  ASSERT_NE(scene.document.network().road(scene.dragged), nullptr);
  ASSERT_NE(scene.document.network().road(scene.other), nullptr);
  EXPECT_EQ(xodr(scene.document), scene.base_xodr);
}

TEST(SelectTool, DeleteKeySingleSelectionIsAPlainCommand) {
  Scene scene;
  SelectTool tool(scene.document, scene.selection);
  scene.selection.select({.road = scene.dragged, .lane = LaneId{}});

  ASSERT_TRUE(tool.key_press(Qt::Key_Backspace, Qt::NoModifier));

  EXPECT_EQ(scene.document.network().road(scene.dragged), nullptr);
  EXPECT_EQ(scene.document.undo_stack()->count(), scene.base_count + 1);
  scene.document.undo_stack()->undo();
  EXPECT_EQ(xodr(scene.document), scene.base_xodr);
}

TEST(SelectTool, DeleteKeyWithEmptySelectionIsNotConsumed) {
  Scene scene;
  SelectTool tool(scene.document, scene.selection);

  EXPECT_FALSE(tool.key_press(Qt::Key_Delete, Qt::NoModifier));
  EXPECT_EQ(scene.document.undo_stack()->count(), scene.base_count);
  EXPECT_EQ(xodr(scene.document), scene.base_xodr);
}

TEST(SelectTool, DeleteKeyIsInertDuringADrag) {
  Scene scene;
  SelectTool tool(scene.document, scene.selection);
  scene.selection.select({.road = scene.dragged, .lane = LaneId{}});

  ASSERT_TRUE(tool.mouse_press(at(50.5, 10.5, Qt::LeftButton)));
  ASSERT_TRUE(tool.mouse_move(at(70.0, 25.0, Qt::LeftButton)));
  EXPECT_FALSE(tool.key_press(Qt::Key_Delete, Qt::NoModifier));
  ASSERT_NE(scene.document.network().road(scene.dragged), nullptr);

  ASSERT_TRUE(tool.key_press(Qt::Key_Escape, Qt::NoModifier)); // clean exit
  EXPECT_EQ(xodr(scene.document), scene.base_xodr);
}

// --- whole-road body move (M3a) ---------------------------------------------

TEST(SelectTool, BodyDragMovesWholeRoadAutoSelectingItOneUndo) {
  Scene scene;
  SelectTool tool(scene.document, scene.selection);
  const PickHit hit = scene.hit(scene.dragged);
  const Waypoint before = node(scene.document, scene.dragged, 0);

  // Press on the body of an unselected road, then drag +20 in x.
  ASSERT_TRUE(tool.mouse_press(at(50.0, 3.0, Qt::LeftButton, Qt::NoModifier, hit)));
  ASSERT_TRUE(tool.mouse_move(at(70.0, 3.0, Qt::LeftButton, Qt::NoModifier, hit)));
  EXPECT_TRUE(tool.moving());
  ASSERT_TRUE(tool.mouse_release(at(70.0, 3.0, Qt::NoButton, Qt::NoModifier, hit)));

  // Auto-selected, and every node shifted by the drag delta.
  EXPECT_EQ(scene.selection.primary().road, scene.dragged);
  const Waypoint after = node(scene.document, scene.dragged, 0);
  EXPECT_NEAR(after.x, before.x + 20.0, roadmaker::tol::kRoundTripPosition);
  EXPECT_NEAR(after.y, before.y, roadmaker::tol::kRoundTripPosition);

  // Exactly one undo entry; undo restores byte-for-byte.
  EXPECT_EQ(scene.document.undo_stack()->count(), scene.base_count + 1);
  scene.document.undo_stack()->undo();
  EXPECT_EQ(xodr(scene.document), scene.base_xodr);
}

TEST(SelectTool, BodyDragMovesEverySelectedRoadTogether) {
  Scene scene;
  SelectTool tool(scene.document, scene.selection);
  scene.selection.select({.road = scene.dragged, .lane = LaneId{}});
  scene.selection.select({.road = scene.other, .lane = LaneId{}},
                         roadmaker::editor::SelectMode::Add);
  const Waypoint dragged_before = node(scene.document, scene.dragged, 0);
  const Waypoint other_before = node(scene.document, scene.other, 0);

  const PickHit hit = scene.hit(scene.dragged);
  ASSERT_TRUE(tool.mouse_press(at(50.0, 3.0, Qt::LeftButton, Qt::NoModifier, hit)));
  ASSERT_TRUE(tool.mouse_move(at(50.0, 33.0, Qt::LeftButton, Qt::NoModifier, hit)));
  ASSERT_TRUE(tool.mouse_release(at(50.0, 33.0, Qt::NoButton, Qt::NoModifier, hit)));

  // Both roads shifted by +30 in y; one undo entry covers the pair.
  EXPECT_NEAR(node(scene.document, scene.dragged, 0).y,
              dragged_before.y + 30.0,
              roadmaker::tol::kRoundTripPosition);
  EXPECT_NEAR(node(scene.document, scene.other, 0).y,
              other_before.y + 30.0,
              roadmaker::tol::kRoundTripPosition);
  EXPECT_EQ(scene.document.undo_stack()->count(), scene.base_count + 1);
}

TEST(SelectTool, EscapeCancelsAMoveLeavingTheNetworkPristine) {
  Scene scene;
  SelectTool tool(scene.document, scene.selection);
  const PickHit hit = scene.hit(scene.dragged);

  ASSERT_TRUE(tool.mouse_press(at(50.0, 3.0, Qt::LeftButton, Qt::NoModifier, hit)));
  ASSERT_TRUE(tool.mouse_move(at(90.0, 40.0, Qt::LeftButton, Qt::NoModifier, hit)));
  EXPECT_TRUE(tool.moving());
  ASSERT_TRUE(tool.key_press(Qt::Key_Escape, Qt::NoModifier));

  EXPECT_FALSE(tool.moving());
  EXPECT_EQ(scene.document.undo_stack()->count(), scene.base_count);
  EXPECT_EQ(xodr(scene.document), scene.base_xodr);
}

TEST(SelectTool, MoveEmitsSizeAllThenArrowCursor) {
  Scene scene;
  SelectTool tool(scene.document, scene.selection);
  std::vector<Qt::CursorShape> cursors;
  QObject::connect(&tool, &SelectTool::cursor_changed, [&cursors](Qt::CursorShape shape) {
    cursors.push_back(shape);
  });
  const PickHit hit = scene.hit(scene.dragged);

  ASSERT_TRUE(tool.mouse_press(at(50.0, 3.0, Qt::LeftButton, Qt::NoModifier, hit)));
  ASSERT_TRUE(tool.mouse_move(at(70.0, 3.0, Qt::LeftButton, Qt::NoModifier, hit)));
  ASSERT_TRUE(tool.mouse_release(at(70.0, 3.0, Qt::NoButton, Qt::NoModifier, hit)));

  ASSERT_EQ(cursors.size(), 2U);
  EXPECT_EQ(cursors.front(), Qt::SizeAllCursor);
  EXPECT_EQ(cursors.back(), Qt::ArrowCursor);
}

// Helpers that build real topology through the command layer (the network
// accessor is const — tests can't hand-set links).
namespace {

RoadId find_road(const Document& document, const char* name) {
  RoadId found;
  document.network().for_each_road([&](RoadId id, const roadmaker::Road& r) {
    if (r.name == name) {
      found = id;
    }
  });
  return found;
}

PickHit body_hit(const Document& document, RoadId road) {
  const roadmaker::Road* road_ptr = document.network().road(road);
  const auto* section = document.network().lane_section(road_ptr->sections.front());
  return PickHit{.road = road, .lane = section->lanes.back()};
}

} // namespace

TEST(SelectTool, JunctionRoadRefusesMoveWithAToast) {
  Document document;
  SelectionModel selection{document};
  // Two roads meeting at (50,0) joined by a common junction; the approach road
  // "A" then touches the junction and can't be moved as a free body.
  ASSERT_TRUE(document.push_command(
      roadmaker::edit::create_road({Waypoint{.x = 0.0, .y = 0.0}, Waypoint{.x = 50.0, .y = 0.0}},
                                   roadmaker::LaneProfile::two_lane_default(),
                                   "A")));
  ASSERT_TRUE(document.push_command(
      roadmaker::edit::create_road({Waypoint{.x = 50.0, .y = 0.0}, Waypoint{.x = 50.0, .y = 50.0}},
                                   roadmaker::LaneProfile::two_lane_default(),
                                   "B")));
  const RoadId a = find_road(document, "A");
  const RoadId b = find_road(document, "B");
  const std::array<roadmaker::RoadEnd, 2> ends{
      roadmaker::RoadEnd{.road = a, .contact = roadmaker::ContactPoint::End},
      roadmaker::RoadEnd{.road = b, .contact = roadmaker::ContactPoint::Start}};
  ASSERT_TRUE(document.push_command(roadmaker::edit::create_junction(document.network(), ends)));
  const std::string before = xodr(document);

  SelectTool tool(document, selection);
  std::vector<QString> toasts;
  QObject::connect(&tool, &SelectTool::status_message, [&toasts](const QString& text) {
    toasts.push_back(text);
  });
  const PickHit hit = body_hit(document, a);
  ASSERT_TRUE(tool.mouse_press(at(25.0, 0.0, Qt::LeftButton, Qt::NoModifier, hit)));
  ASSERT_TRUE(tool.mouse_move(at(25.0, 20.0, Qt::LeftButton, Qt::NoModifier, hit)));

  EXPECT_FALSE(tool.moving());
  EXPECT_EQ(xodr(document), before); // untouched
  ASSERT_FALSE(toasts.empty());
  EXPECT_TRUE(toasts.back().contains("can't be moved")) << toasts.back().toStdString();
}

TEST(SelectTool, LinkBreakConfirmGatesTheMove) {
  Document document;
  SelectionModel selection{document};
  // A road split into two halves is genuinely road-road linked (head↔tail).
  ASSERT_TRUE(document.push_command(
      roadmaker::edit::create_road({Waypoint{.x = 0.0, .y = 0.0}, Waypoint{.x = 100.0, .y = 0.0}},
                                   roadmaker::LaneProfile::two_lane_default(),
                                   "Long")));
  const RoadId head = find_road(document, "Long");
  ASSERT_TRUE(document.push_command(roadmaker::edit::split_road(document.network(), head, 50.0)));
  ASSERT_TRUE(document.network().road(head)->successor.has_value()); // head→tail link
  const std::string linked = xodr(document);

  SelectTool tool(document, selection);
  const PickHit hit = body_hit(document, head);

  // Refusing the confirm leaves the network untouched.
  tool.set_link_break_confirm([] { return false; });
  ASSERT_TRUE(tool.mouse_press(at(25.0, 0.0, Qt::LeftButton, Qt::NoModifier, hit)));
  ASSERT_TRUE(tool.mouse_move(at(25.0, 20.0, Qt::LeftButton, Qt::NoModifier, hit)));
  EXPECT_FALSE(tool.moving());
  EXPECT_EQ(xodr(document), linked);

  // Accepting it moves the head and clears the head↔tail link on both sides.
  tool.set_link_break_confirm([] { return true; });
  ASSERT_TRUE(tool.mouse_press(at(25.0, 0.0, Qt::LeftButton, Qt::NoModifier, hit)));
  ASSERT_TRUE(tool.mouse_move(at(25.0, 20.0, Qt::LeftButton, Qt::NoModifier, hit)));
  ASSERT_TRUE(tool.moving());
  ASSERT_TRUE(tool.mouse_release(at(25.0, 20.0, Qt::NoButton, Qt::NoModifier, hit)));
  EXPECT_FALSE(document.network().road(head)->successor.has_value());
}

// --- double-click bend insert + Edit Nodes hand-off ----------------------------

TEST(SelectTool, DoubleClickOnBodyInsertsABendAndRequestsEditNodes) {
  Scene scene;
  SelectTool tool(scene.document, scene.selection);
  RoadId requested_road;
  std::size_t requested_index = 999;
  int emitted = 0;
  QObject::connect(&tool, &SelectTool::edit_nodes_requested, [&](RoadId road, std::size_t index) {
    requested_road = road;
    requested_index = index;
    ++emitted;
  });

  const roadmaker::Road* road = scene.document.network().road(scene.dragged);
  const std::size_t before = road->authoring_waypoints->size();
  const PickHit hit = scene.hit(scene.dragged);
  ASSERT_TRUE(tool.mouse_double_click(at(30.0, 6.0, Qt::LeftButton, Qt::NoModifier, hit)));

  // One committed insert; the app is asked to hand off to Edit Nodes.
  EXPECT_EQ(scene.document.undo_stack()->count(), scene.base_count + 1);
  EXPECT_EQ(scene.document.network().road(scene.dragged)->authoring_waypoints->size(), before + 1);
  EXPECT_EQ(emitted, 1);
  EXPECT_EQ(requested_road, scene.dragged);
  EXPECT_LT(requested_index, before + 1);
}
