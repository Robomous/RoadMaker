// Copyright 2026 Robomous
// SPDX-License-Identifier: Apache-2.0

// Edit Nodes tool (issue #10): headless ToolEvent sequences drive node
// activation, midpoint-marker inserts, Delete-key removal, and node drags;
// the tests assert on the undo stack, the serialized network, and preview
// geometry — the M2 tool test seam (docs/design/m2/01_editing_framework.md
// §4). The foreign-road tests load the curved_road.xodr sample to cover the
// §2.5 waypoint derivation through the editor seam.

#include "roadmaker/edit/operations.hpp"
#include "roadmaker/road/road.hpp"
#include "roadmaker/tol.hpp"
#include "roadmaker/xodr/writer.hpp"

#include <gtest/gtest.h>

#include <QSignalSpy>
#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "document/document.hpp"
#include "document/selection_model.hpp"
#include "tools/edit_nodes_tool.hpp"

using roadmaker::LaneId;
using roadmaker::PathPoint;
using roadmaker::RoadId;
using roadmaker::Waypoint;
using roadmaker::editor::Document;
using roadmaker::editor::EditNodesTool;
using roadmaker::editor::PickHit;
using roadmaker::editor::SelectionEntry;
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

/// Document with road "Edited" (0,0)-(50,10)-(100,0) and a 2-node road
/// "Other" from (120,0); base state captured after both creates.
/// Document/SelectionModel are QObjects (pinned in place), hence setup in
/// the constructor.
struct Scene {
  Document document;
  SelectionModel selection{document};
  RoadId edited;
  RoadId other;
  int base_count = 0;
  std::string base_xodr;

  Scene() {
    auto road = document.push_command(
        roadmaker::edit::create_road({Waypoint{.x = 0.0, .y = 0.0},
                                      Waypoint{.x = 50.0, .y = 10.0},
                                      Waypoint{.x = 100.0, .y = 0.0}},
                                     roadmaker::LaneProfile::two_lane_default(),
                                     "Edited"));
    auto second = document.push_command(roadmaker::edit::create_road(
        {Waypoint{.x = 120.0, .y = 0.0}, Waypoint{.x = 200.0, .y = 0.0}},
        roadmaker::LaneProfile::two_lane_default(),
        "Other"));
    if (!road || !second) {
      throw std::runtime_error("scene setup failed");
    }
    document.network().for_each_road([&](RoadId id, const roadmaker::Road& r) {
      if (r.name == "Edited") {
        edited = id;
      } else if (r.name == "Other") {
        other = id;
      }
    });
    if (!edited.is_valid() || !other.is_valid()) {
      throw std::runtime_error("scene roads not found");
    }
    base_count = document.undo_stack()->count();
    base_xodr = xodr(document);
  }

  /// The on-curve midpoint marker of segment `segment` — computed exactly
  /// like the tool computes it.
  [[nodiscard]] Waypoint marker(RoadId road_id, std::size_t segment) const {
    const roadmaker::Road* road = document.network().road(road_id);
    if (road == nullptr) {
      throw std::runtime_error("road lost");
    }
    const auto stations = roadmaker::edit::waypoint_stations(*road);
    if (!stations.has_value() || segment + 1 >= stations->size()) {
      throw std::runtime_error("no such segment");
    }
    const PathPoint mid =
        road->plan_view.evaluate((stations->at(segment) + stations->at(segment + 1)) / 2.0);
    return Waypoint{.x = mid.x, .y = mid.y};
  }

  [[nodiscard]] std::vector<Waypoint> nodes(RoadId road_id) const {
    const roadmaker::Road* road = document.network().road(road_id);
    if (road == nullptr) {
      throw std::runtime_error("road lost");
    }
    return roadmaker::edit::effective_waypoints(*road);
  }
};

} // namespace

// --- preview geometry --------------------------------------------------------

TEST(EditNodesTool, PreviewShowsNodesTangentsAndMarkers) {
  Scene scene;
  EditNodesTool tool(scene.document, scene.selection);

  EXPECT_TRUE(tool.preview().empty());

  scene.selection.select({.road = scene.edited, .lane = LaneId{}});
  const auto preview = tool.preview();
  // 3 node handles + 2 segment-midpoint handles.
  EXPECT_EQ(preview.handles.size(), 5U);
  EXPECT_EQ(std::count_if(preview.handles.begin(),
                          preview.handles.end(),
                          [](const roadmaker::editor::Handle& handle) {
                            return handle.kind == roadmaker::editor::HandleKind::Node;
                          }),
            3);
  // 3 tangent whiskers (1 segment each) · 6 doubles (midpoints are now knobs).
  EXPECT_EQ(preview.line_positions.size(), 18U);
}

// --- double-click bend insert --------------------------------------------------

TEST(EditNodesTool, DoubleClickOnBodyInsertsABendAndGrabsIt) {
  Scene scene;
  EditNodesTool tool(scene.document, scene.selection);
  scene.selection.select({.road = scene.edited, .lane = LaneId{}});
  const std::size_t before = scene.nodes(scene.edited).size();

  // Double-click on the road body (a lane-patch pick carries the road).
  const PickHit pick{.road = scene.edited, .lane = LaneId{}};
  ASSERT_TRUE(tool.mouse_double_click(at(30.0, 6.0, Qt::LeftButton, Qt::NoModifier, pick)));

  // One committed insert, the new node is active AND grabbed for shaping.
  EXPECT_EQ(scene.document.undo_stack()->count(), scene.base_count + 1);
  EXPECT_EQ(scene.nodes(scene.edited).size(), before + 1);
  EXPECT_TRUE(tool.dragging());
  ASSERT_TRUE(tool.active_node().has_value());
  EXPECT_EQ(tool.active_node()->first, scene.edited);

  // Undo removes the bend; the base bytes return.
  ASSERT_TRUE(tool.key_press(Qt::Key_Escape, Qt::NoModifier)); // drop the grab first
  scene.document.undo_stack()->undo();
  EXPECT_EQ(xodr(scene.document), scene.base_xodr);
}

// --- insert via midpoint markers ----------------------------------------------

TEST(EditNodesTool, ClickOnMidpointMarkerInsertsANodeOnTheCurve) {
  Scene scene;
  EditNodesTool tool(scene.document, scene.selection);
  scene.selection.select({.road = scene.edited, .lane = LaneId{}});

  const Waypoint before_start = scene.nodes(scene.edited).front();
  const Waypoint before_end = scene.nodes(scene.edited).back();
  const Waypoint marker = scene.marker(scene.edited, 0);

  ASSERT_TRUE(tool.mouse_press(at(marker.x + 0.2, marker.y - 0.2, Qt::LeftButton)));
  ASSERT_TRUE(tool.mouse_release(at(marker.x + 0.2, marker.y - 0.2)));

  // One command; the new node sits ON the curve at the marker.
  EXPECT_EQ(scene.document.undo_stack()->count(), scene.base_count + 1);
  const std::vector<Waypoint> nodes = scene.nodes(scene.edited);
  ASSERT_EQ(nodes.size(), 4U);
  EXPECT_NEAR(nodes[1].x, marker.x, roadmaker::tol::kRoundTripPosition);
  EXPECT_NEAR(nodes[1].y, marker.y, roadmaker::tol::kRoundTripPosition);
  ASSERT_TRUE(tool.active_node().has_value());
  EXPECT_EQ(tool.active_node()->second, 1U);

  // Endpoints survive the re-fit; undo restores the base bytes.
  EXPECT_NEAR(nodes.front().x, before_start.x, roadmaker::tol::kRoundTripPosition);
  EXPECT_NEAR(nodes.back().x, before_end.x, roadmaker::tol::kRoundTripPosition);
  scene.document.undo_stack()->undo();
  EXPECT_EQ(xodr(scene.document), scene.base_xodr);
}

TEST(EditNodesTool, MarkerPressThenDragPlacesTheNewNode) {
  Scene scene;
  EditNodesTool tool(scene.document, scene.selection);
  scene.selection.select({.road = scene.edited, .lane = LaneId{}});
  const Waypoint marker = scene.marker(scene.edited, 1);

  ASSERT_TRUE(tool.mouse_press(at(marker.x, marker.y, Qt::LeftButton)));
  EXPECT_TRUE(tool.dragging());
  ASSERT_TRUE(tool.mouse_move(at(80.0, 30.0, Qt::LeftButton)));
  ASSERT_TRUE(tool.mouse_release(at(80.0, 30.0)));

  // Insert + move: two undo entries, and two undos restore the base bytes.
  EXPECT_EQ(scene.document.undo_stack()->count(), scene.base_count + 2);
  const std::vector<Waypoint> nodes = scene.nodes(scene.edited);
  ASSERT_EQ(nodes.size(), 4U);
  EXPECT_EQ(nodes[2], (Waypoint{.x = 80.0, .y = 30.0}));
  scene.document.undo_stack()->undo();
  scene.document.undo_stack()->undo();
  EXPECT_EQ(xodr(scene.document), scene.base_xodr);
}

// --- delete ------------------------------------------------------------------

TEST(EditNodesTool, DeleteKeyRemovesTheActiveNode) {
  Scene scene;
  EditNodesTool tool(scene.document, scene.selection);
  scene.selection.select({.road = scene.edited, .lane = LaneId{}});

  // Click a node (no move): activates it, pushes nothing.
  ASSERT_TRUE(tool.mouse_press(at(50.2, 10.2, Qt::LeftButton)));
  ASSERT_TRUE(tool.mouse_release(at(50.2, 10.2)));
  EXPECT_EQ(scene.document.undo_stack()->count(), scene.base_count);
  ASSERT_TRUE(tool.active_node().has_value());
  EXPECT_EQ(tool.active_node()->second, 1U);
  // The active-node emphasis is carried by its handle's state now, so the
  // line overlay is just the 3 tangent whiskers (1 segment each · 6 doubles).
  EXPECT_EQ(tool.preview().line_positions.size(), 18U);

  ASSERT_TRUE(tool.key_press(Qt::Key_Delete, Qt::NoModifier));
  EXPECT_EQ(scene.document.undo_stack()->count(), scene.base_count + 1);
  EXPECT_EQ(scene.nodes(scene.edited).size(), 2U);
  EXPECT_FALSE(tool.active_node().has_value());

  scene.document.undo_stack()->undo();
  EXPECT_EQ(xodr(scene.document), scene.base_xodr);
}

TEST(EditNodesTool, DeleteIsRefusedAtTwoWaypoints) {
  Scene scene;
  EditNodesTool tool(scene.document, scene.selection);
  scene.selection.select({.road = scene.other, .lane = LaneId{}});
  QSignalSpy status_spy(&tool, &roadmaker::editor::Tool::status_message);

  ASSERT_TRUE(tool.mouse_press(at(120.0, 0.0, Qt::LeftButton)));
  ASSERT_TRUE(tool.mouse_release(at(120.0, 0.0)));
  ASSERT_TRUE(tool.active_node().has_value());

  // The factory refuses (a road needs 2 waypoints); nothing reaches the
  // stack and the user hears why.
  ASSERT_TRUE(tool.key_press(Qt::Key_Delete, Qt::NoModifier));
  EXPECT_EQ(scene.document.undo_stack()->count(), scene.base_count);
  EXPECT_EQ(xodr(scene.document), scene.base_xodr);
  ASSERT_FALSE(status_spy.isEmpty());
  EXPECT_TRUE(status_spy.last().first().toString().contains(QStringLiteral("Cannot delete")));
}

// --- node drags (shared machinery with Select/Move) ---------------------------

TEST(EditNodesTool, DragRefitsLiveAndReleaseCommitsOnce) {
  Scene scene;
  EditNodesTool tool(scene.document, scene.selection);
  scene.selection.select({.road = scene.edited, .lane = LaneId{}});

  ASSERT_TRUE(tool.mouse_press(at(50.2, 10.2, Qt::LeftButton)));
  EXPECT_TRUE(tool.dragging());
  ASSERT_TRUE(tool.mouse_move(at(50.0, 25.0, Qt::LeftButton)));
  EXPECT_EQ(scene.nodes(scene.edited)[1], (Waypoint{.x = 50.0, .y = 25.0}));
  EXPECT_TRUE(scene.document.preview_active());
  EXPECT_EQ(scene.document.undo_stack()->count(), scene.base_count);

  ASSERT_TRUE(tool.mouse_release(at(50.0, 25.0)));
  EXPECT_FALSE(scene.document.preview_active());
  EXPECT_EQ(scene.document.undo_stack()->count(), scene.base_count + 1);
  scene.document.undo_stack()->undo();
  EXPECT_EQ(xodr(scene.document), scene.base_xodr);
}

TEST(EditNodesTool, EscapeCancelsADragByteIdentical) {
  Scene scene;
  EditNodesTool tool(scene.document, scene.selection);
  scene.selection.select({.road = scene.edited, .lane = LaneId{}});

  ASSERT_TRUE(tool.mouse_press(at(50.2, 10.2, Qt::LeftButton)));
  ASSERT_TRUE(tool.mouse_move(at(70.0, 55.0, Qt::LeftButton)));
  ASSERT_TRUE(tool.key_press(Qt::Key_Escape, Qt::NoModifier));

  EXPECT_FALSE(tool.dragging());
  EXPECT_FALSE(scene.document.preview_active());
  EXPECT_EQ(xodr(scene.document), scene.base_xodr);
  EXPECT_EQ(scene.document.undo_stack()->count(), scene.base_count);
}

TEST(EditNodesTool, DeactivateCancelsARunningDrag) {
  Scene scene;
  EditNodesTool tool(scene.document, scene.selection);
  scene.selection.select({.road = scene.edited, .lane = LaneId{}});

  ASSERT_TRUE(tool.mouse_press(at(50.2, 10.2, Qt::LeftButton)));
  ASSERT_TRUE(tool.mouse_move(at(70.0, 55.0, Qt::LeftButton)));
  tool.deactivate();

  EXPECT_FALSE(tool.dragging());
  EXPECT_FALSE(scene.document.preview_active());
  EXPECT_EQ(xodr(scene.document), scene.base_xodr);
  EXPECT_EQ(scene.document.undo_stack()->count(), scene.base_count);
}

TEST(EditNodesTool, SnapEngagesOnAnotherRoadsEndpoint) {
  Scene scene;
  EditNodesTool tool(scene.document, scene.selection);
  scene.selection.select({.road = scene.edited, .lane = LaneId{}});
  tool.set_snap_options({.radius = 2.0});

  ASSERT_TRUE(tool.mouse_press(at(100.0, 0.0, Qt::LeftButton)));
  ASSERT_TRUE(tool.mouse_move(at(119.0, 0.5, Qt::LeftButton)));
  const Waypoint snapped = scene.nodes(scene.edited)[2];
  EXPECT_NEAR(snapped.x, 120.0, roadmaker::tol::kRoundTripPosition);
  EXPECT_NEAR(snapped.y, 0.0, roadmaker::tol::kRoundTripPosition);
  ASSERT_TRUE(tool.mouse_release(at(119.0, 0.5)));
  EXPECT_EQ(scene.document.undo_stack()->count(), scene.base_count + 1);
}

// --- selection routing ---------------------------------------------------------

TEST(EditNodesTool, ClickSelectsARoadAndEmptyClickClears) {
  Scene scene;
  EditNodesTool tool(scene.document, scene.selection);

  // RMB is camera orbit in the M2 map — never consumed.
  EXPECT_FALSE(tool.mouse_press(at(50.0, 3.0, Qt::RightButton)));

  const PickHit hit{.road = scene.other, .lane = LaneId{}};
  ASSERT_TRUE(tool.mouse_press(at(160.0, 0.0, Qt::LeftButton, Qt::NoModifier, hit)));
  ASSERT_EQ(scene.selection.entries().size(), 1U);
  EXPECT_EQ(scene.selection.primary().road, scene.other);

  ASSERT_TRUE(tool.mouse_press(at(300.0, 300.0, Qt::LeftButton)));
  EXPECT_TRUE(scene.selection.empty());
  EXPECT_EQ(scene.document.undo_stack()->count(), scene.base_count);
  EXPECT_EQ(xodr(scene.document), scene.base_xodr);
}

// --- foreign roads (§2.5 derivation through the editor seam) -------------------

TEST(EditNodesTool, ForeignRoadShowsDerivedNodesAndFirstEditRecordsThem) {
  Document document;
  SelectionModel selection{document};
  ASSERT_TRUE(
      document.load(std::filesystem::path(RM_SAMPLES_DIR) / "curved_road.xodr").has_value());
  const RoadId road = document.network().find_road("1");
  ASSERT_TRUE(document.network().road(road) != nullptr);
  ASSERT_FALSE(document.network().road(road)->authoring_waypoints.has_value());
  const std::string loaded_xodr = xodr(document);

  EditNodesTool tool(document, selection);
  selection.select({.road = road, .lane = LaneId{}});
  QSignalSpy status_spy(&tool, &roadmaker::editor::Tool::status_message);

  // Derived handles: 4 record starts + the endpoint = 5 node knobs.
  const auto derived = tool.preview();
  EXPECT_EQ(std::count_if(derived.handles.begin(),
                          derived.handles.end(),
                          [](const roadmaker::editor::Handle& handle) {
                            return handle.kind == roadmaker::editor::HandleKind::Node;
                          }),
            5);

  // Grab a derived node and nudge it: the derivation notice fires, the
  // commit records waypoints, and undo restores the loaded bytes (the
  // writer keeps never-edited foreign geometry untouched).
  const Waypoint node = roadmaker::edit::effective_waypoints(*document.network().road(road))[2];
  ASSERT_TRUE(tool.mouse_press(at(node.x, node.y, Qt::LeftButton)));
  ASSERT_FALSE(status_spy.isEmpty());
  EXPECT_TRUE(status_spy.last().first().toString().contains(QStringLiteral("First edit")));
  ASSERT_TRUE(tool.mouse_move(at(node.x + 2.0, node.y - 1.0, Qt::LeftButton)));
  ASSERT_TRUE(tool.mouse_release(at(node.x + 2.0, node.y - 1.0)));

  ASSERT_TRUE(document.network().road(road)->authoring_waypoints.has_value());
  EXPECT_EQ(document.network().road(road)->authoring_waypoints->size(), 5U);
  EXPECT_EQ(document.undo_stack()->count(), 1);
  document.undo_stack()->undo();
  EXPECT_FALSE(document.network().road(road)->authoring_waypoints.has_value());
  EXPECT_EQ(xodr(document), loaded_xodr);
}
