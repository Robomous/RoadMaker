// Corner tool (p4-s1, issue #225): headless ToolEvent sequences hover, select
// and drag a junction's fillet corner, asserting the tool-local sub-selection,
// the ONE-undo-entry drag discipline (press-with-no-move and Esc author
// nothing), the authored kernel values, and the dashed extent guides that make
// the corner legible. Runs under QT_QPA_PLATFORM=offscreen like every other
// editor test.

#include "roadmaker/edit/operations.hpp"
#include "roadmaker/mesh/junction_corners.hpp"
#include "roadmaker/road/junction.hpp"
#include "roadmaker/road/network.hpp"
#include "roadmaker/xodr/writer.hpp"

#include <gtest/gtest.h>

#include <QSignalSpy>
#include <QUndoStack>
#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>
#include <string>
#include <vector>

#include "document/document.hpp"
#include "document/selection_model.hpp"
#include "tools/corner_tool.hpp"

namespace roadmaker::editor {
namespace {

using roadmaker::ContactPoint;
using roadmaker::JunctionCornerInfo;
using roadmaker::JunctionId;
using roadmaker::LaneProfile;
using roadmaker::RoadEnd;
using roadmaker::RoadId;
using roadmaker::Waypoint;

std::string xodr(const Document& document) {
  auto text = roadmaker::write_xodr(document.network());
  if (!text) {
    throw std::runtime_error(text.error().message);
  }
  return *text;
}

/// Four arms meeting at the origin, joined into a junction — a cross has four
/// well-conditioned right-angle corners, so every corner solves.
struct Scene {
  Document document;
  JunctionId junction;
  int base_count = 0;
  std::string base_xodr;

  Scene() {
    const RoadId west = make(-60.0, 0.0, -12.0, 0.0, "1");
    const RoadId east = make(60.0, 0.0, 12.0, 0.0, "2");
    const RoadId south = make(0.0, -60.0, 0.0, -12.0, "3");
    const RoadId north = make(0.0, 60.0, 0.0, 12.0, "4");
    const std::array<RoadEnd, 4> ends{RoadEnd{west, ContactPoint::End},
                                      RoadEnd{east, ContactPoint::End},
                                      RoadEnd{south, ContactPoint::End},
                                      RoadEnd{north, ContactPoint::End}};
    if (!document.push_command(roadmaker::edit::create_junction(document.network(), ends))) {
      throw std::runtime_error("junction create failed");
    }
    document.network().for_each_junction(
        [&](JunctionId id, const roadmaker::Junction&) { junction = id; });
    base_count = document.undo_stack()->index();
    base_xodr = xodr(document);
  }

  RoadId make(double x0, double y0, double x1, double y1, const char* odr) {
    if (!document.push_command(
            roadmaker::edit::create_road({Waypoint{.x = x0, .y = y0}, Waypoint{.x = x1, .y = y1}},
                                         LaneProfile::two_lane_default(),
                                         odr))) {
      throw std::runtime_error("road create failed");
    }
    return document.network().find_road(odr);
  }

  [[nodiscard]] std::vector<JunctionCornerInfo> corners() const {
    return junction_corners(document.network(), junction);
  }

  /// The first solved corner (the tests only ever need one).
  [[nodiscard]] JunctionCornerInfo corner() const {
    const std::vector<JunctionCornerInfo> all = corners();
    if (all.empty()) {
      throw std::runtime_error("the cross junction solved no corners");
    }
    return all.front();
  }

  /// The authored override for `info`'s pair, or nullptr when none exists.
  [[nodiscard]] const roadmaker::JunctionCorner* authored(const JunctionCornerInfo& info) const {
    const roadmaker::Junction* junc = document.network().junction(junction);
    if (junc == nullptr) {
      return nullptr;
    }
    for (const roadmaker::JunctionCorner& stored : junc->corners) {
      if (stored.arm_a == info.arm_a && stored.arm_b == info.arm_b) {
        return &stored;
      }
    }
    return nullptr;
  }
};

ToolEvent at(double x, double y, Qt::MouseButtons buttons = Qt::NoButton) {
  ToolEvent event;
  event.world_x = x;
  event.world_y = y;
  event.buttons = buttons;
  return event;
}

/// The same event with a junction-floor pick, the way the viewport reports a
/// click on the pavement inside the junction.
ToolEvent
on_junction(const Scene& scene, double x, double y, Qt::MouseButtons buttons = Qt::NoButton) {
  ToolEvent event = at(x, y, buttons);
  PickHit hit;
  hit.junction = scene.junction;
  event.pick = hit;
  return event;
}

/// Cursor position on the inward bisector that asks for fillet radius `radius`
/// (the inverse of CornerTool::radius_for).
std::array<double, 2> apex_cursor_for(const JunctionCornerInfo& info, double radius) {
  const double half_sin = std::sin(info.phi * 0.5);
  const double along = radius * (1.0 - half_sin) / half_sin;
  return {info.corner[0] + (info.bisector[0] * along), info.corner[1] + (info.bisector[1] * along)};
}

/// Cursor position on arm_a's edge line that asks for extent `extent`.
std::array<double, 2> extent_a_cursor_for(const JunctionCornerInfo& info, double extent) {
  return {info.corner[0] - (info.dir_a[0] * extent), info.corner[1] - (info.dir_a[1] * extent)};
}

/// Click-selects the corner (press + release, no movement) so its three
/// handles are live for the drag gestures that follow.
void select_corner(CornerTool& tool, const Scene& scene, const JunctionCornerInfo& info) {
  const std::array<double, 2> apex = info.apex();
  if (!tool.mouse_press(on_junction(scene, apex[0], apex[1], Qt::LeftButton))) {
    throw std::runtime_error("the corner click was not consumed");
  }
  static_cast<void>(tool.mouse_release(at(apex[0], apex[1])));
}

TEST(CornerTool, HoverHighlightsWithoutSelectingOrEditing) {
  Scene scene;
  SelectionModel selection(scene.document);
  CornerTool tool(scene.document, selection);
  tool.activate();

  const JunctionCornerInfo info = scene.corner();
  const std::array<double, 2> apex = info.apex();
  QSignalSpy spy(&tool, &CornerTool::corner_selection_changed);

  // Hovering never consumes (the viewport keeps its readout).
  EXPECT_FALSE(tool.mouse_move(on_junction(scene, apex[0], apex[1])));

  EXPECT_FALSE(tool.active_corner().has_value());
  EXPECT_EQ(spy.count(), 0);
  EXPECT_EQ(scene.document.undo_stack()->index(), scene.base_count);
  EXPECT_TRUE(selection.empty());

  // The corner reads as a highlighted curve plus its two dashed extent guides.
  const PreviewGeometry geometry = tool.preview();
  EXPECT_FALSE(geometry.empty());
  EXPECT_FALSE(geometry.line_positions.empty());
  ASSERT_EQ(geometry.dashed_line_positions.size(), 12U) << "two guide segments, xyz pairwise";
  EXPECT_TRUE(geometry.handles.empty()) << "handles belong to the ACTIVE corner only";
}

TEST(CornerTool, ClickSelectsCornerAndMirrorsTheJunction) {
  Scene scene;
  SelectionModel selection(scene.document);
  CornerTool tool(scene.document, selection);
  tool.activate();

  const JunctionCornerInfo info = scene.corner();
  const std::array<double, 2> apex = info.apex();
  QSignalSpy spy(&tool, &CornerTool::corner_selection_changed);

  ASSERT_TRUE(tool.mouse_press(on_junction(scene, apex[0], apex[1], Qt::LeftButton)));

  ASSERT_TRUE(tool.active_corner().has_value());
  EXPECT_EQ(tool.active_corner()->junction, scene.junction);
  EXPECT_EQ(tool.active_corner()->arm_a, info.arm_a);
  EXPECT_EQ(tool.active_corner()->arm_b, info.arm_b);
  EXPECT_EQ(spy.count(), 1);

  // The rest of the UI follows the owning junction.
  EXPECT_EQ(selection.primary().junction, scene.junction);
  EXPECT_EQ(scene.document.undo_stack()->index(), scene.base_count) << "selection is not an edit";

  // Three handles: apex, tangency a, tangency b.
  const PreviewGeometry geometry = tool.preview();
  ASSERT_EQ(geometry.handles.size(), 3U);
  EXPECT_FALSE(geometry.dashed_line_positions.empty());

  // The solved info the properties pane binds to.
  ASSERT_TRUE(tool.active_corner_info().has_value());
  EXPECT_NEAR(tool.active_corner_info()->radius, info.radius, 1e-9);
}

TEST(CornerTool, HoveringAHandleMarksItHovered) {
  Scene scene;
  SelectionModel selection(scene.document);
  CornerTool tool(scene.document, selection);
  tool.activate();

  const JunctionCornerInfo info = scene.corner();
  select_corner(tool, scene, info);

  EXPECT_FALSE(tool.mouse_move(on_junction(scene, info.tangent_a[0], info.tangent_a[1])));
  EXPECT_EQ(tool.hovered_handle(), CornerHandle::ExtentA);

  const PreviewGeometry geometry = tool.preview();
  ASSERT_EQ(geometry.handles.size(), 3U);
  EXPECT_EQ(geometry.handles[0].state, HandleState::Idle) << "apex";
  EXPECT_EQ(geometry.handles[1].state, HandleState::Hovered) << "tangency a";
  EXPECT_EQ(geometry.handles[2].state, HandleState::Idle) << "tangency b";
}

TEST(CornerTool, ApexDragAuthorsTheRadiusAsOneUndoEntry) {
  Scene scene;
  SelectionModel selection(scene.document);
  CornerTool tool(scene.document, selection);
  tool.activate();

  const JunctionCornerInfo info = scene.corner();
  ASSERT_GT(info.max_radius, 4.0) << "the cross must leave room for the dragged radius";
  select_corner(tool, scene, info);
  EXPECT_EQ(scene.document.undo_stack()->index(), scene.base_count) << "a click is not an edit";

  // Grab the apex handle, then drag along the bisector.
  const std::array<double, 2> apex = info.apex();
  ASSERT_TRUE(tool.mouse_press(at(apex[0], apex[1], Qt::LeftButton)));
  ASSERT_TRUE(tool.dragging());

  constexpr double kTarget = 3.5;
  const std::array<double, 2> cursor = apex_cursor_for(info, kTarget);
  ASSERT_TRUE(tool.mouse_move(at(cursor[0], cursor[1], Qt::LeftButton)));
  EXPECT_TRUE(scene.document.preview_active());
  ASSERT_TRUE(tool.mouse_release(at(cursor[0], cursor[1])));

  EXPECT_EQ(scene.document.undo_stack()->index(), scene.base_count + 1) << "exactly one entry";
  const roadmaker::JunctionCorner* stored = scene.authored(info);
  ASSERT_NE(stored, nullptr);
  ASSERT_TRUE(stored->radius.has_value());
  EXPECT_NEAR(*stored->radius, kTarget, 1e-6);
  EXPECT_FALSE(stored->extent_a.has_value()) << "a radius is symmetric — extents are cleared";

  // Undo restores the pre-drag document byte for byte.
  scene.document.undo_stack()->undo();
  EXPECT_EQ(scene.document.undo_stack()->index(), scene.base_count);
  EXPECT_EQ(xodr(scene.document), scene.base_xodr);
}

TEST(CornerTool, ExtentDragStoresBothSidesAsOneUndoEntry) {
  Scene scene;
  SelectionModel selection(scene.document);
  CornerTool tool(scene.document, selection);
  tool.activate();

  const JunctionCornerInfo info = scene.corner();
  select_corner(tool, scene, info);
  EXPECT_EQ(scene.document.undo_stack()->index(), scene.base_count) << "a click is not an edit";

  // Grab tangency A and pull it back along the edge.
  ASSERT_TRUE(tool.mouse_press(at(info.tangent_a[0], info.tangent_a[1], Qt::LeftButton)));
  const double target = std::min(info.max_extent_a * 0.5, info.extent_a + 2.0);
  ASSERT_GT(target, 0.1);
  const std::array<double, 2> cursor = extent_a_cursor_for(info, target);
  ASSERT_TRUE(tool.mouse_move(at(cursor[0], cursor[1], Qt::LeftButton)));
  ASSERT_TRUE(tool.mouse_release(at(cursor[0], cursor[1])));

  EXPECT_EQ(scene.document.undo_stack()->index(), scene.base_count + 1) << "exactly one entry";
  const roadmaker::JunctionCorner* stored = scene.authored(info);
  ASSERT_NE(stored, nullptr);
  ASSERT_TRUE(stored->extent_a.has_value());
  ASSERT_TRUE(stored->extent_b.has_value()) << "the first extent drag authors BOTH legs";
  EXPECT_NEAR(*stored->extent_a, target, 1e-6);
  EXPECT_NEAR(*stored->extent_b, info.extent_b, 1e-6);

  scene.document.undo_stack()->undo();
  EXPECT_EQ(xodr(scene.document), scene.base_xodr);
}

TEST(CornerTool, PressWithoutMovementAuthorsNothing) {
  Scene scene;
  SelectionModel selection(scene.document);
  CornerTool tool(scene.document, selection);
  tool.activate();

  const JunctionCornerInfo info = scene.corner();
  const std::array<double, 2> apex = info.apex();
  select_corner(tool, scene, info);

  // Second press grabs the apex handle; releasing without a move must not open
  // (or commit) a session.
  ASSERT_TRUE(tool.mouse_press(at(apex[0], apex[1], Qt::LeftButton)));
  EXPECT_FALSE(scene.document.preview_active());
  ASSERT_TRUE(tool.mouse_release(at(apex[0], apex[1])));

  EXPECT_EQ(scene.document.undo_stack()->index(), scene.base_count);
  EXPECT_EQ(xodr(scene.document), scene.base_xodr);
}

TEST(CornerTool, EscapeMidDragCancelsWithoutAnUndoEntry) {
  Scene scene;
  SelectionModel selection(scene.document);
  CornerTool tool(scene.document, selection);
  tool.activate();

  const JunctionCornerInfo info = scene.corner();
  const std::array<double, 2> apex = info.apex();
  select_corner(tool, scene, info);
  ASSERT_TRUE(tool.mouse_press(at(apex[0], apex[1], Qt::LeftButton)));
  const std::array<double, 2> cursor = apex_cursor_for(info, 3.0);
  ASSERT_TRUE(tool.mouse_move(at(cursor[0], cursor[1], Qt::LeftButton)));
  ASSERT_TRUE(scene.document.preview_active());

  EXPECT_TRUE(tool.key_press(Qt::Key_Escape, Qt::NoModifier));
  EXPECT_FALSE(scene.document.preview_active());
  EXPECT_FALSE(tool.dragging());
  EXPECT_EQ(scene.document.undo_stack()->index(), scene.base_count);
  EXPECT_EQ(xodr(scene.document), scene.base_xodr);

  // The corner stays selected — Esc cancelled the gesture, not the selection.
  EXPECT_TRUE(tool.active_corner().has_value());
  EXPECT_TRUE(tool.key_press(Qt::Key_Escape, Qt::NoModifier));
  EXPECT_FALSE(tool.active_corner().has_value());
}

TEST(CornerTool, LoadingADocumentClearsTheActiveCorner) {
  Scene scene;
  SelectionModel selection(scene.document);
  CornerTool tool(scene.document, selection);
  tool.activate();

  const JunctionCornerInfo info = scene.corner();
  select_corner(tool, scene, info);
  ASSERT_TRUE(tool.active_corner().has_value());

  QSignalSpy spy(&tool, &CornerTool::corner_selection_changed);
  scene.document.reset(); // emits Document::loaded

  EXPECT_FALSE(tool.active_corner().has_value()) << "a stale JunctionId is a crash";
  EXPECT_FALSE(tool.active_corner_info().has_value());
  EXPECT_FALSE(tool.dragging());
  EXPECT_EQ(spy.count(), 1);
  EXPECT_TRUE(tool.preview().empty());
}

TEST(CornerTool, DeactivateCancelsAnInFlightDrag) {
  Scene scene;
  SelectionModel selection(scene.document);
  CornerTool tool(scene.document, selection);
  tool.activate();

  const JunctionCornerInfo info = scene.corner();
  const std::array<double, 2> apex = info.apex();
  select_corner(tool, scene, info);
  ASSERT_TRUE(tool.mouse_press(at(apex[0], apex[1], Qt::LeftButton)));
  const std::array<double, 2> cursor = apex_cursor_for(info, 3.0);
  ASSERT_TRUE(tool.mouse_move(at(cursor[0], cursor[1], Qt::LeftButton)));

  tool.deactivate();

  EXPECT_FALSE(scene.document.preview_active());
  EXPECT_FALSE(tool.active_corner().has_value());
  EXPECT_EQ(scene.document.undo_stack()->index(), scene.base_count);
  EXPECT_EQ(xodr(scene.document), scene.base_xodr);
}

} // namespace
} // namespace roadmaker::editor
