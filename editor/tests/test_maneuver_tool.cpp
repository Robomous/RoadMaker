// Maneuver tool (p4-s6, issue #227): headless ToolEvent sequences select a
// junction, pick one of its turns, and reshape it — asserting that the preview
// is gated on the selection, that the sub-selection follows a viewport click, a
// panel row click and a road selection alike, that a drag is exactly ONE undo
// entry, that a gesture which changes nothing pushes NOTHING (a still-derived
// maneuver would otherwise be locked by an unchanged path), that Esc cancels
// mid-drag, and that undo restores the network bytes. Runs under
// QT_QPA_PLATFORM=offscreen like every other editor test.

#include "roadmaker/edit/operations.hpp"
#include "roadmaker/mesh/junction_maneuvers.hpp"
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
#include "tools/maneuver_tool.hpp"

namespace roadmaker::editor {
namespace {

using roadmaker::ContactPoint;
using roadmaker::JunctionId;
using roadmaker::JunctionManeuverInfo;
using roadmaker::LaneProfile;
using roadmaker::Maneuver;
using roadmaker::ManeuverSlide;
using roadmaker::RoadEnd;
using roadmaker::RoadId;
using roadmaker::TurnType;
using roadmaker::Waypoint;

std::string xodr(const Document& document) {
  auto text = roadmaker::write_xodr(document.network());
  if (!text) {
    throw std::runtime_error(text.error().message);
  }
  return *text;
}

/// Four roomy arms meeting at the origin, joined into a junction — a dozen
/// turns to pick from.
struct Scene {
  Document document;
  SelectionModel selection{document};
  JunctionId junction;
  int base_count = 0;
  std::string base_xodr;

  Scene() {
    const std::array<RoadEnd, 4> ends{RoadEnd{make(-80.0, 0.0, -20.0, 0.0, "1"), ContactPoint::End},
                                      RoadEnd{make(80.0, 0.0, 20.0, 0.0, "2"), ContactPoint::End},
                                      RoadEnd{make(0.0, -80.0, 0.0, -20.0, "3"), ContactPoint::End},
                                      RoadEnd{make(0.0, 80.0, 0.0, 20.0, "4"), ContactPoint::End}};
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

  [[nodiscard]] std::vector<JunctionManeuverInfo> maneuvers() const {
    return junction_maneuvers(document.network(), junction);
  }

  [[nodiscard]] JunctionManeuverInfo info(RoadId road) const {
    for (const JunctionManeuverInfo& candidate : maneuvers()) {
      if (candidate.road == road) {
        return candidate;
      }
    }
    throw std::runtime_error("no such maneuver");
  }

  void select_junction() {
    selection.select(SelectionEntry{.junction = junction}, SelectMode::Replace);
  }

  /// The authored record for `road`, or nullptr while the turn is still derived.
  [[nodiscard]] const Maneuver* authored(RoadId road) const {
    const roadmaker::Junction* junc = document.network().junction(junction);
    if (junc == nullptr) {
      return nullptr;
    }
    for (const Maneuver& stored : junc->maneuvers) {
      if (stored.road == road) {
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

/// A point ON a maneuver's drawn path — where a click grabs the turn itself.
std::array<double, 2> on_path(const JunctionManeuverInfo& info) {
  const std::array<double, 3>& sample = info.path[info.path.size() / 2];
  return {sample[0], sample[1]};
}

/// The tool's first insert marker: the midpoint of the endpoint chain, which on
/// a still-derived turn (no control points) is the only one there is.
std::array<double, 2> first_midpoint(const JunctionManeuverInfo& info) {
  return {(info.start_slide.anchor[0] + info.end_slide.anchor[0]) / 2.0,
          (info.start_slide.anchor[1] + info.end_slide.anchor[1]) / 2.0};
}

/// The far end of a slide: the bound that is NOT the anchor. A slide runs from
/// the anchor lane's inner boundary to its outer one, so one bound is always 0
/// (the anchor itself) and the other is the lane's signed width — which side
/// depends on whether the anchor lane is left or right of the reference line.
std::array<double, 2> slide_far_point(const ManeuverSlide& slide) {
  return std::abs(slide.max_offset) > std::abs(slide.min_offset) ? slide.max_point
                                                                 : slide.min_point;
}

double slide_far_offset(const ManeuverSlide& slide) {
  return std::abs(slide.max_offset) > std::abs(slide.min_offset) ? slide.max_offset
                                                                 : slide.min_offset;
}

/// Activates the first maneuver of the junction and returns it.
JunctionManeuverInfo activate_first(Scene& scene, ManeuverTool& tool) {
  scene.select_junction();
  const std::vector<JunctionManeuverInfo> all = scene.maneuvers();
  if (all.empty()) {
    throw std::runtime_error("the cross junction planned no turns");
  }
  const std::array<double, 2> point = on_path(all.front());
  if (!tool.mouse_press(at(point[0], point[1], Qt::LeftButton))) {
    throw std::runtime_error("clicking the path did not select a maneuver");
  }
  (void)tool.mouse_release(at(point[0], point[1]));
  return scene.info(all.front().road);
}

} // namespace

TEST(ManeuverTool, PreviewIsGatedOnAJunctionSelection) {
  Scene scene;
  ManeuverTool tool(scene.document, scene.selection);
  tool.activate();

  EXPECT_FALSE(tool.inspected_junction().is_valid());
  EXPECT_TRUE(tool.preview().empty());
  EXPECT_EQ(tool.instruction(), QStringLiteral("Select a junction to work on its turns"));

  scene.select_junction();
  EXPECT_EQ(tool.inspected_junction(), scene.junction);
  const PreviewGeometry geometry = tool.preview();
  EXPECT_FALSE(geometry.dashed_line_positions.empty()) << "every turn draws its path, dashed";
  EXPECT_TRUE(geometry.line_positions.empty()) << "nothing is active yet";
  EXPECT_TRUE(geometry.handles.empty()) << "handles belong to the active turn only";
}

TEST(ManeuverTool, ClickSelectsAManeuverAndMirrorsItsRoadIntoTheSelection) {
  Scene scene;
  ManeuverTool tool(scene.document, scene.selection);
  tool.activate();
  scene.select_junction();
  const std::vector<JunctionManeuverInfo> all = scene.maneuvers();
  ASSERT_FALSE(all.empty());

  QSignalSpy spy(&tool, &ManeuverTool::maneuver_selection_changed);
  const std::array<double, 2> point = on_path(all.front());
  EXPECT_TRUE(tool.mouse_press(at(point[0], point[1], Qt::LeftButton)));
  ASSERT_TRUE(tool.active_maneuver().has_value());
  EXPECT_EQ(tool.active_maneuver()->junction, scene.junction);
  EXPECT_EQ(spy.count(), 1);

  // The connecting ROAD joins the selection (so a maneuver is highlightable
  // like any other road) while the junction stays primary (so the Properties
  // pane keeps showing the junction's Maneuvers rows).
  const std::vector<RoadId> roads = scene.selection.selected_roads();
  EXPECT_NE(std::ranges::find(roads, all.front().road), roads.end());
  EXPECT_EQ(scene.selection.primary().junction, scene.junction);

  const PreviewGeometry geometry = tool.preview();
  EXPECT_FALSE(geometry.line_positions.empty()) << "the active turn is drawn solid";
  ASSERT_GE(geometry.handles.size(), 3U) << "two endpoints and at least one insert marker";
  // Selecting a maneuver authors nothing.
  EXPECT_EQ(scene.document.undo_stack()->index(), scene.base_count);
  EXPECT_EQ(xodr(scene.document), scene.base_xodr);
}

TEST(ManeuverTool, PressAndReleaseWithNoMovementPushesNothing) {
  Scene scene;
  ManeuverTool tool(scene.document, scene.selection);
  tool.activate();
  const JunctionManeuverInfo info = activate_first(scene, tool);
  const int base = scene.document.undo_stack()->index();

  // Grab the insert marker and let go without moving, then grab it and "drag"
  // right back to where it started. Neither may author anything: on a
  // still-derived maneuver an unchanged path STILL flips the implicit lock, so
  // the kernel would accept it — the tool has to refuse it first.
  const std::array<double, 2> mid = first_midpoint(info);
  ASSERT_TRUE(tool.mouse_press(at(mid[0], mid[1], Qt::LeftButton)));
  EXPECT_TRUE(tool.mouse_release(at(mid[0], mid[1])));
  EXPECT_EQ(scene.document.undo_stack()->index(), base);

  // Same for an ENDPOINT dragged away and back to its anchor: the offsets end
  // where they started, so nothing is authored. (An insert marker is different
  // on purpose — pressing one asks for a point, moved or not.)
  const std::array<double, 2> anchor = info.start_slide.anchor;
  const std::array<double, 2> far = slide_far_point(info.start_slide);
  ASSERT_TRUE(tool.mouse_press(at(anchor[0], anchor[1], Qt::LeftButton)));
  EXPECT_TRUE(tool.mouse_move(at(far[0], far[1], Qt::LeftButton)));
  EXPECT_TRUE(tool.mouse_move(at(anchor[0], anchor[1], Qt::LeftButton)));
  EXPECT_TRUE(tool.mouse_release(at(anchor[0], anchor[1])));
  EXPECT_EQ(scene.document.undo_stack()->index(), base) << "a drag walked back authors nothing";
  EXPECT_EQ(scene.authored(info.road), nullptr);
  EXPECT_EQ(xodr(scene.document), scene.base_xodr);
}

TEST(ManeuverTool, DraggingAnInsertMarkerShapesAndLocksInOneCommand) {
  Scene scene;
  ManeuverTool tool(scene.document, scene.selection);
  tool.activate();
  const JunctionManeuverInfo info = activate_first(scene, tool);
  const int base = scene.document.undo_stack()->index();

  const std::array<double, 2> mid = first_midpoint(info);
  ASSERT_TRUE(tool.mouse_press(at(mid[0], mid[1], Qt::LeftButton)));
  EXPECT_TRUE(tool.mouse_move(at(mid[0] + 1.0, mid[1] + 1.0, Qt::LeftButton)));
  EXPECT_TRUE(tool.mouse_move(at(mid[0] + 2.0, mid[1] + 2.0, Qt::LeftButton)));
  EXPECT_TRUE(tool.mouse_release(at(mid[0] + 2.0, mid[1] + 2.0)));

  // The insert AND every drag frame collapse into ONE undo entry.
  EXPECT_EQ(scene.document.undo_stack()->index(), base + 1);
  const Maneuver* record = scene.authored(info.road);
  ASSERT_NE(record, nullptr);
  EXPECT_EQ(record->control_points.size(), 1U);
  EXPECT_TRUE(record->locked) << "a hand-shaped path locks itself in the same undo step";

  scene.document.undo_stack()->undo();
  EXPECT_EQ(scene.authored(info.road), nullptr);
  EXPECT_EQ(xodr(scene.document), scene.base_xodr) << "undo restores the original bytes";
}

TEST(ManeuverTool, EscapeCancelsADragAndLeavesTheStackAlone) {
  Scene scene;
  ManeuverTool tool(scene.document, scene.selection);
  tool.activate();
  const JunctionManeuverInfo info = activate_first(scene, tool);
  const int base = scene.document.undo_stack()->index();

  const std::array<double, 2> mid = first_midpoint(info);
  ASSERT_TRUE(tool.mouse_press(at(mid[0], mid[1], Qt::LeftButton)));
  EXPECT_TRUE(tool.mouse_move(at(mid[0] + 2.0, mid[1] + 2.0, Qt::LeftButton)));
  EXPECT_TRUE(tool.dragging());
  EXPECT_TRUE(tool.key_press(Qt::Key_Escape, Qt::NoModifier));
  EXPECT_FALSE(tool.dragging());

  EXPECT_EQ(scene.document.undo_stack()->index(), base);
  EXPECT_EQ(scene.authored(info.road), nullptr);
  EXPECT_EQ(xodr(scene.document), scene.base_xodr);
}

TEST(ManeuverTool, DeleteRemovesTheFocusedControlPoint) {
  Scene scene;
  ManeuverTool tool(scene.document, scene.selection);
  tool.activate();
  const JunctionManeuverInfo info = activate_first(scene, tool);

  // Author one point first — Delete has nothing to do on a derived turn.
  EXPECT_FALSE(tool.key_press(Qt::Key_Delete, Qt::NoModifier));
  const std::array<double, 2> mid = first_midpoint(info);
  ASSERT_TRUE(tool.mouse_press(at(mid[0], mid[1], Qt::LeftButton)));
  EXPECT_TRUE(tool.mouse_move(at(mid[0] + 2.0, mid[1] + 2.0, Qt::LeftButton)));
  EXPECT_TRUE(tool.mouse_release(at(mid[0] + 2.0, mid[1] + 2.0)));
  ASSERT_EQ(scene.authored(info.road)->control_points.size(), 1U);
  const int after_shape = scene.document.undo_stack()->index();

  // The dragged point is the focused one, so Delete removes it.
  EXPECT_TRUE(tool.key_press(Qt::Key_Delete, Qt::NoModifier));
  EXPECT_EQ(scene.document.undo_stack()->index(), after_shape + 1);
  ASSERT_NE(scene.authored(info.road), nullptr);
  EXPECT_TRUE(scene.authored(info.road)->control_points.empty());
  EXPECT_TRUE(scene.authored(info.road)->locked) << "the implicit lock outlives the point";
}

TEST(ManeuverTool, EndpointDragSlidesWithinTheAnchorLaneBounds) {
  Scene scene;
  ManeuverTool tool(scene.document, scene.selection);
  tool.activate();
  const JunctionManeuverInfo info = activate_first(scene, tool);
  ASSERT_LT(info.start_slide.min_offset, info.start_slide.max_offset)
      << "the anchor lane must have room to slide in";
  const int base = scene.document.undo_stack()->index();

  // Grab the start endpoint and drag it a whole lane width PAST the slide's own
  // far bound: the tool clamps to the bounds the QUERY reports, never to bounds
  // of its own.
  const std::array<double, 2> anchor = info.start_slide.anchor;
  const std::array<double, 2> far = slide_far_point(info.start_slide);
  const std::array<double, 2> beyond{(far[0] * 2.0) - anchor[0], (far[1] * 2.0) - anchor[1]};
  ASSERT_TRUE(tool.mouse_press(at(anchor[0], anchor[1], Qt::LeftButton)));
  EXPECT_TRUE(tool.mouse_move(at(beyond[0], beyond[1], Qt::LeftButton)));
  EXPECT_TRUE(tool.mouse_release(at(beyond[0], beyond[1])));

  EXPECT_EQ(scene.document.undo_stack()->index(), base + 1);
  const Maneuver* record = scene.authored(info.road);
  ASSERT_NE(record, nullptr);
  ASSERT_TRUE(record->start_offset.has_value());
  EXPECT_LE(*record->start_offset, info.start_slide.max_offset);
  EXPECT_GE(*record->start_offset, info.start_slide.min_offset);
  EXPECT_DOUBLE_EQ(*record->start_offset, slide_far_offset(info.start_slide))
      << "the drag overshot, so it lands exactly ON the bound";
  EXPECT_TRUE(record->control_points.empty()) << "an endpoint slide authors no interior points";

  scene.document.undo_stack()->undo();
  EXPECT_EQ(xodr(scene.document), scene.base_xodr);
}

TEST(ManeuverTool, SelectManeuverIsTheSameSubSelectionAsAClick) {
  Scene scene;
  ManeuverTool tool(scene.document, scene.selection);
  tool.activate();
  scene.select_junction();
  const std::vector<JunctionManeuverInfo> all = scene.maneuvers();
  ASSERT_GE(all.size(), 2U);

  tool.select_maneuver(all[1].road);
  ASSERT_TRUE(tool.active_maneuver().has_value());
  EXPECT_EQ(tool.active_maneuver()->road, all[1].road);

  // Selecting the connecting road anywhere else in the editor lands on the same
  // sub-selection — that is what makes a maneuver an ordinary selectable thing.
  scene.selection.select(SelectionEntry{.road = all[0].road}, SelectMode::Replace);
  ASSERT_TRUE(tool.active_maneuver().has_value());
  EXPECT_EQ(tool.active_maneuver()->road, all[0].road);
  EXPECT_EQ(tool.inspected_junction(), scene.junction);

  // A road that is not a turn of this junction clears it rather than lying.
  tool.select_maneuver(scene.document.network().find_road("1"));
  EXPECT_FALSE(tool.active_maneuver().has_value());
  EXPECT_EQ(scene.document.undo_stack()->index(), scene.base_count) << "selecting authors nothing";
}

TEST(ManeuverTool, DeselectingTheJunctionClearsTheTool) {
  Scene scene;
  ManeuverTool tool(scene.document, scene.selection);
  tool.activate();
  (void)activate_first(scene, tool);
  ASSERT_TRUE(tool.active_maneuver().has_value());

  scene.selection.clear();
  EXPECT_FALSE(tool.inspected_junction().is_valid());
  EXPECT_FALSE(tool.active_maneuver().has_value());
  EXPECT_TRUE(tool.preview().empty());
}

} // namespace roadmaker::editor
