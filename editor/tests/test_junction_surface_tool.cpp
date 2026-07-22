// Copyright 2026 Robomous
// SPDX-License-Identifier: Apache-2.0

// Junction Surface tool (p4-s5, issue #320): headless ToolEvent sequences
// select a junction, pick one of its floor's surface spans, and drive the two
// controls, asserting that the preview is gated on the selection, that the
// tool-local sub-selection follows both a viewport click and a panel row click,
// that Space / PgUp / PgDn push EXACTLY one command each, and that undo restores
// both the network bytes and the preview. Runs under QT_QPA_PLATFORM=offscreen
// like every other editor test.

#include "roadmaker/edit/operations.hpp"
#include "roadmaker/mesh/junction_surface_spans.hpp"
#include "roadmaker/road/junction.hpp"
#include "roadmaker/road/network.hpp"
#include "roadmaker/xodr/writer.hpp"

#include <gtest/gtest.h>

#include <QSignalSpy>
#include <QUndoStack>
#include <algorithm>
#include <array>
#include <stdexcept>
#include <string>
#include <vector>

#include "document/document.hpp"
#include "document/selection_model.hpp"
#include "tools/junction_surface_tool.hpp"

namespace roadmaker::editor {
namespace {

using roadmaker::ContactPoint;
using roadmaker::JunctionId;
using roadmaker::JunctionSurfaceSpanInfo;
using roadmaker::LaneProfile;
using roadmaker::RoadEnd;
using roadmaker::RoadId;
using roadmaker::SurfaceSpan;
using roadmaker::Waypoint;

std::string xodr(const Document& document) {
  auto text = roadmaker::write_xodr(document.network());
  if (!text) {
    throw std::runtime_error(text.error().message);
  }
  return *text;
}

/// Four roomy arms meeting at the origin, joined into a junction whose floor
/// therefore unions a dozen turn ribbons.
struct Scene {
  Document document;
  SelectionModel selection{document};
  JunctionId junction;
  int base_count = 0;
  std::string base_xodr;

  Scene() {
    const RoadId west = make(-80.0, 0.0, -20.0, 0.0, "1");
    const RoadId east = make(80.0, 0.0, 20.0, 0.0, "2");
    const RoadId south = make(0.0, -80.0, 0.0, -20.0, "3");
    const RoadId north = make(0.0, 80.0, 0.0, 20.0, "4");
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

  [[nodiscard]] std::vector<JunctionSurfaceSpanInfo> spans() const {
    return junction_surface_spans(document.network(), junction);
  }

  void select_junction() {
    selection.select(SelectionEntry{.junction = junction}, SelectMode::Replace);
  }

  /// A world point strictly inside `info`'s footprint — where a click grabs it.
  /// The ring's centroid can fall outside a crescent-shaped turn ribbon, so this
  /// walks the ring for a point the tool's own hit test accepts.
  [[nodiscard]] static std::array<double, 2> grab(const JunctionSurfaceSpanInfo& info) {
    // A ribbon's footprint is left-border-forward + right-border-reversed, so
    // sample i and its mirror bracket the ribbon at one station: their midpoint
    // is on the centerline, comfortably inside.
    const std::size_t half = info.footprint.size() / 2;
    const std::array<double, 2>& a = info.footprint[half / 2];
    const std::array<double, 2>& b = info.footprint[info.footprint.size() - 1 - (half / 2)];
    return {(a[0] + b[0]) / 2.0, (a[1] + b[1]) / 2.0};
  }

  [[nodiscard]] const SurfaceSpan* authored(RoadId road) const {
    const roadmaker::Junction* junc = document.network().junction(junction);
    if (junc == nullptr) {
      return nullptr;
    }
    for (const SurfaceSpan& stored : junc->surface_spans) {
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

} // namespace

TEST(JunctionSurfaceTool, PreviewIsGatedOnAJunctionSelection) {
  Scene scene;
  JunctionSurfaceTool tool(scene.document, scene.selection);
  tool.activate();

  // Nothing selected: no rings, no samples, and the instruction says what to do.
  EXPECT_FALSE(tool.inspected_junction().is_valid());
  EXPECT_TRUE(tool.preview().empty());
  EXPECT_EQ(tool.instruction(), QStringLiteral("Select a junction to inspect its surface spans"));

  scene.select_junction();
  EXPECT_EQ(tool.inspected_junction(), scene.junction);
  const PreviewGeometry geometry = tool.preview();
  EXPECT_FALSE(geometry.line_positions.empty()) << "every included span draws its footprint ring";
  EXPECT_TRUE(geometry.dashed_line_positions.empty()) << "nothing is excluded yet";
  EXPECT_TRUE(geometry.handles.empty()) << "samples only show for the span under attention";
}

TEST(JunctionSurfaceTool, ClickSelectsASpanAndShowsItsSamples) {
  Scene scene;
  JunctionSurfaceTool tool(scene.document, scene.selection);
  tool.activate();
  scene.select_junction();

  const std::vector<JunctionSurfaceSpanInfo> spans = scene.spans();
  ASSERT_FALSE(spans.empty());
  QSignalSpy spy(&tool, &JunctionSurfaceTool::surface_span_selection_changed);

  const std::array<double, 2> point = Scene::grab(spans.front());
  EXPECT_TRUE(tool.mouse_press(at(point[0], point[1], Qt::LeftButton)));
  ASSERT_TRUE(tool.active_span().has_value());
  EXPECT_EQ(tool.active_span()->junction, scene.junction);
  EXPECT_EQ(spy.count(), 1);

  const std::optional<JunctionSurfaceSpanInfo> info = tool.active_span_info();
  ASSERT_TRUE(info.has_value());
  EXPECT_FALSE(tool.preview().handles.empty()) << "the active span shows its samples";
  EXPECT_EQ(tool.preview().handles.front().kind, HandleKind::Sample);

  // Selecting a span authors nothing.
  EXPECT_EQ(scene.document.undo_stack()->index(), scene.base_count);
  EXPECT_EQ(xodr(scene.document), scene.base_xodr);
}

TEST(JunctionSurfaceTool, SpaceTogglesSamplesInExactlyOneCommand) {
  Scene scene;
  JunctionSurfaceTool tool(scene.document, scene.selection);
  tool.activate();
  scene.select_junction();
  const std::array<double, 2> point = Scene::grab(scene.spans().front());
  ASSERT_TRUE(tool.mouse_press(at(point[0], point[1], Qt::LeftButton)));
  const RoadId road = tool.active_span()->road;

  EXPECT_TRUE(tool.key_press(Qt::Key_Space, Qt::NoModifier));
  EXPECT_EQ(scene.document.undo_stack()->index(), scene.base_count + 1) << "exactly one undo entry";
  ASSERT_NE(scene.authored(road), nullptr);
  EXPECT_FALSE(scene.authored(road)->included);
  // The excluded span's ring moves to the dashed pass: its footprint is still
  // in the union, so the pavement has not moved — only its samples dropped out.
  const PreviewGeometry excluded = tool.preview();
  EXPECT_FALSE(excluded.dashed_line_positions.empty());

  scene.document.undo_stack()->undo();
  EXPECT_EQ(xodr(scene.document), scene.base_xodr);
  EXPECT_EQ(scene.authored(road), nullptr);
  EXPECT_TRUE(tool.preview().dashed_line_positions.empty()) << "undo restores the preview";
}

TEST(JunctionSurfaceTool, PageUpAndPageDownNudgeTheSortIndexByOne) {
  Scene scene;
  JunctionSurfaceTool tool(scene.document, scene.selection);
  tool.activate();
  scene.select_junction();
  const std::array<double, 2> point = Scene::grab(scene.spans().front());
  ASSERT_TRUE(tool.mouse_press(at(point[0], point[1], Qt::LeftButton)));
  const RoadId road = tool.active_span()->road;

  EXPECT_TRUE(tool.key_press(Qt::Key_PageUp, Qt::NoModifier));
  EXPECT_TRUE(tool.key_press(Qt::Key_PageUp, Qt::NoModifier));
  EXPECT_EQ(scene.document.undo_stack()->index(), scene.base_count + 2);
  ASSERT_NE(scene.authored(road), nullptr);
  EXPECT_EQ(scene.authored(road)->sort_index, 2);

  EXPECT_TRUE(tool.key_press(Qt::Key_PageDown, Qt::NoModifier));
  EXPECT_EQ(scene.authored(road)->sort_index, 1);

  // Back to zero erases the record, so the file matches the pristine one.
  EXPECT_TRUE(tool.key_press(Qt::Key_PageDown, Qt::NoModifier));
  EXPECT_EQ(scene.authored(road), nullptr);
  EXPECT_EQ(xodr(scene.document), scene.base_xodr);
}

TEST(JunctionSurfaceTool, KeysWithNoActiveSpanAuthorNothing) {
  Scene scene;
  JunctionSurfaceTool tool(scene.document, scene.selection);
  tool.activate();
  scene.select_junction();
  ASSERT_FALSE(tool.active_span().has_value());

  EXPECT_FALSE(tool.key_press(Qt::Key_Space, Qt::NoModifier));
  EXPECT_FALSE(tool.key_press(Qt::Key_PageUp, Qt::NoModifier));
  EXPECT_FALSE(tool.key_press(Qt::Key_PageDown, Qt::NoModifier));
  EXPECT_EQ(scene.document.undo_stack()->index(), scene.base_count);
  EXPECT_EQ(xodr(scene.document), scene.base_xodr);
}

TEST(JunctionSurfaceTool, TabCyclesAndSelectSpanIsTheSameSubSelection) {
  Scene scene;
  JunctionSurfaceTool tool(scene.document, scene.selection);
  tool.activate();
  scene.select_junction();
  const std::vector<JunctionSurfaceSpanInfo> spans = scene.spans();
  ASSERT_GE(spans.size(), 2U);

  EXPECT_TRUE(tool.key_press(Qt::Key_Tab, Qt::NoModifier));
  ASSERT_TRUE(tool.active_span().has_value());
  EXPECT_EQ(tool.active_span()->road, spans[0].road);
  EXPECT_TRUE(tool.key_press(Qt::Key_Tab, Qt::NoModifier));
  EXPECT_EQ(tool.active_span()->road, spans[1].road);

  // The panel's row click goes through the same sub-selection.
  tool.select_span(spans[0].road);
  EXPECT_EQ(tool.active_span()->road, spans[0].road);

  // A road that is not a span of this junction clears it rather than lying.
  tool.select_span(scene.document.network().find_road("1"));
  EXPECT_FALSE(tool.active_span().has_value());
  EXPECT_EQ(scene.document.undo_stack()->index(), scene.base_count) << "cycling authors nothing";
}

TEST(JunctionSurfaceTool, DeselectingTheJunctionClearsTheTool) {
  Scene scene;
  JunctionSurfaceTool tool(scene.document, scene.selection);
  tool.activate();
  scene.select_junction();
  const std::array<double, 2> point = Scene::grab(scene.spans().front());
  ASSERT_TRUE(tool.mouse_press(at(point[0], point[1], Qt::LeftButton)));
  ASSERT_TRUE(tool.active_span().has_value());

  scene.selection.clear();
  EXPECT_FALSE(tool.inspected_junction().is_valid());
  EXPECT_FALSE(tool.active_span().has_value());
  EXPECT_TRUE(tool.preview().empty());
}

} // namespace roadmaker::editor
