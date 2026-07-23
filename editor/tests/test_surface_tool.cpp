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

// Surface tool (p5-s1, issue #231): headless ToolEvent sequences select a
// ground surface, grab a boundary node, drag it and release, asserting that the
// whole gesture is ONE undo entry, that the release detaches the surface to
// authored (decision D3), that undo restores the network bytes AND the derived
// source, and that "revert to derived" is the way back. Runs under
// QT_QPA_PLATFORM=offscreen like every other editor test.

#include "roadmaker/edit/operations.hpp"
#include "roadmaker/mesh/surface_boundary.hpp"
#include "roadmaker/road/network.hpp"
#include "roadmaker/road/surface.hpp"
#include "roadmaker/xodr/writer.hpp"

#include <gtest/gtest.h>

#include <QSignalSpy>
#include <QUndoStack>
#include <cmath>
#include <stdexcept>
#include <string>
#include <vector>

#include "document/document.hpp"
#include "document/selection_model.hpp"
#include "tools/surface_tool.hpp"

namespace roadmaker::editor {
namespace {

using roadmaker::BoundarySource;
using roadmaker::LaneProfile;
using roadmaker::RoadId;
using roadmaker::Surface;
using roadmaker::SurfaceId;
using roadmaker::SurfaceNode;
using roadmaker::Waypoint;

std::string xodr(const Document& document) {
  auto text = roadmaker::write_xodr(document.network());
  if (!text) {
    throw std::runtime_error(text.error().message);
  }
  return *text;
}

/// A 60 m block of four roads, which derives exactly one ground surface.
struct Scene {
  Document document;
  SelectionModel selection{document};
  SurfaceId surface;
  int base_count = 0;
  std::string base_xodr;

  Scene() {
    make(0.0, 0.0, 60.0, 0.0, "1");
    make(60.0, 0.0, 60.0, 60.0, "2");
    make(60.0, 60.0, 0.0, 60.0, "3");
    make(0.0, 60.0, 0.0, 0.0, "4");
    document.network().for_each_surface([&](SurfaceId id, const Surface&) { surface = id; });
    if (!surface.is_valid()) {
      throw std::runtime_error("the block derived no surface");
    }
    base_count = document.undo_stack()->index();
    base_xodr = xodr(document);
  }

  void make(double x0, double y0, double x1, double y1, const char* odr) {
    if (!document.push_command(
            roadmaker::edit::create_road({Waypoint{.x = x0, .y = y0}, Waypoint{.x = x1, .y = y1}},
                                         LaneProfile::two_lane_default(),
                                         odr))) {
      throw std::runtime_error("road create failed");
    }
  }

  void select_surface() {
    selection.select(SelectionEntry{.surface = surface}, SelectMode::Replace);
  }

  [[nodiscard]] const Surface* stored() const { return document.network().surface(surface); }
};

ToolEvent at(double x, double y, Qt::MouseButtons buttons = Qt::NoButton) {
  ToolEvent event;
  event.world_x = x;
  event.world_y = y;
  event.buttons = buttons;
  return event;
}

} // namespace

TEST(SurfaceTool, PreviewIsEmptyWithoutASelectedSurface) {
  Scene scene;
  SurfaceTool tool(scene.document, scene.selection);
  EXPECT_TRUE(tool.preview().empty());
  EXPECT_FALSE(tool.target().is_valid());
}

TEST(SurfaceTool, SelectingASurfaceShowsNodeAndTangentHandles) {
  Scene scene;
  SurfaceTool tool(scene.document, scene.selection);
  scene.select_surface();

  const std::vector<SurfaceNode> nodes = tool.nodes();
  ASSERT_GE(nodes.size(), 3U);
  EXPECT_EQ(tool.target(), scene.surface);

  const PreviewGeometry geometry = tool.preview();
  // One node knob + two tangent knobs + one midpoint marker per node.
  EXPECT_EQ(geometry.handles.size(), nodes.size() * 4);
  EXPECT_FALSE(geometry.line_positions.empty()) << "the boundary loop is drawn";
  // The surface is still DERIVED — showing handles must not touch the network.
  EXPECT_EQ(scene.stored()->source, BoundarySource::Derived);
  EXPECT_EQ(xodr(scene.document), scene.base_xodr);
}

TEST(SurfaceTool, DraggingANodeIsOneUndoEntryAndDetachesTheSurface) {
  Scene scene;
  SurfaceTool tool(scene.document, scene.selection);
  scene.select_surface();

  const std::vector<SurfaceNode> before = tool.nodes();
  ASSERT_GE(before.size(), 3U);
  const SurfaceNode grabbed = before.front();

  ASSERT_TRUE(tool.mouse_press(at(grabbed.x, grabbed.y, Qt::LeftButton)));
  EXPECT_TRUE(tool.dragging());
  // Several move frames — the whole drag must still be a single entry.
  EXPECT_TRUE(tool.mouse_move(at(grabbed.x + 1.0, grabbed.y + 1.0, Qt::LeftButton)));
  EXPECT_TRUE(tool.mouse_move(at(grabbed.x + 2.0, grabbed.y + 2.0, Qt::LeftButton)));
  EXPECT_TRUE(tool.mouse_move(at(grabbed.x + 3.0, grabbed.y + 3.0, Qt::LeftButton)));
  ASSERT_TRUE(tool.mouse_release(at(grabbed.x + 3.0, grabbed.y + 3.0)));
  EXPECT_FALSE(tool.dragging());

  EXPECT_EQ(scene.document.undo_stack()->index(), scene.base_count + 1)
      << "a drag is exactly one undo step";
  ASSERT_NE(scene.stored(), nullptr);
  EXPECT_EQ(scene.stored()->source, BoundarySource::Authored);
  ASSERT_EQ(scene.stored()->nodes.size(), before.size());
  EXPECT_NEAR(scene.stored()->nodes.front().x, grabbed.x + 3.0, 1e-9);
  EXPECT_NEAR(scene.stored()->nodes.front().y, grabbed.y + 3.0, 1e-9);
  // Provenance survives the detach.
  EXPECT_EQ(scene.stored()->bounding_roads.size(), 4U);
}

TEST(SurfaceTool, UndoRestoresTheBytesAndTheDerivedSource) {
  Scene scene;
  SurfaceTool tool(scene.document, scene.selection);
  scene.select_surface();

  const SurfaceNode grabbed = tool.nodes().front();
  ASSERT_TRUE(tool.mouse_press(at(grabbed.x, grabbed.y, Qt::LeftButton)));
  EXPECT_TRUE(tool.mouse_move(at(grabbed.x + 4.0, grabbed.y, Qt::LeftButton)));
  ASSERT_TRUE(tool.mouse_release(at(grabbed.x + 4.0, grabbed.y)));

  scene.document.undo_stack()->undo();
  EXPECT_EQ(scene.document.undo_stack()->index(), scene.base_count);
  EXPECT_EQ(xodr(scene.document), scene.base_xodr);
  ASSERT_NE(scene.stored(), nullptr);
  EXPECT_EQ(scene.stored()->source, BoundarySource::Derived);
  EXPECT_TRUE(scene.stored()->nodes.empty());
}

TEST(SurfaceTool, EscapeCancelsADragWithoutTouchingTheDocument) {
  Scene scene;
  SurfaceTool tool(scene.document, scene.selection);
  scene.select_surface();

  const SurfaceNode grabbed = tool.nodes().front();
  ASSERT_TRUE(tool.mouse_press(at(grabbed.x, grabbed.y, Qt::LeftButton)));
  EXPECT_TRUE(tool.mouse_move(at(grabbed.x + 5.0, grabbed.y, Qt::LeftButton)));
  EXPECT_TRUE(tool.key_press(Qt::Key_Escape, Qt::NoModifier));

  EXPECT_FALSE(tool.dragging());
  EXPECT_EQ(scene.document.undo_stack()->index(), scene.base_count);
  EXPECT_EQ(xodr(scene.document), scene.base_xodr);
  EXPECT_EQ(scene.stored()->source, BoundarySource::Derived);
}

TEST(SurfaceTool, DeactivateCancelsAnInFlightDrag) {
  Scene scene;
  SurfaceTool tool(scene.document, scene.selection);
  scene.select_surface();

  const SurfaceNode grabbed = tool.nodes().front();
  ASSERT_TRUE(tool.mouse_press(at(grabbed.x, grabbed.y, Qt::LeftButton)));
  EXPECT_TRUE(tool.mouse_move(at(grabbed.x + 5.0, grabbed.y, Qt::LeftButton)));
  tool.deactivate();

  EXPECT_FALSE(tool.dragging());
  EXPECT_EQ(scene.document.undo_stack()->index(), scene.base_count);
  EXPECT_EQ(xodr(scene.document), scene.base_xodr);
}

TEST(SurfaceTool, DraggingATangentTipCurvesTheEdge) {
  Scene scene;
  SurfaceTool tool(scene.document, scene.selection);
  scene.select_surface();

  // Detach first with a plain node nudge, so the tangent drag has stored nodes
  // to work against and the tip position is predictable.
  std::vector<SurfaceNode> seed = tool.nodes();
  ASSERT_GE(seed.size(), 3U);
  seed.front().tangent_out_x = 6.0;
  seed.front().tangent_out_y = 0.0;
  ASSERT_TRUE(scene.document.push_command(
      roadmaker::edit::set_surface_boundary(scene.document.network(), scene.surface, seed)));

  const SurfaceNode node = tool.nodes().front();
  const double tip_x = node.x + 6.0;
  const double tip_y = node.y;
  ASSERT_TRUE(tool.mouse_press(at(tip_x, tip_y, Qt::LeftButton)));
  EXPECT_TRUE(tool.mouse_move(at(tip_x, tip_y + 4.0, Qt::LeftButton)));
  ASSERT_TRUE(tool.mouse_release(at(tip_x, tip_y + 4.0)));

  const SurfaceNode after = scene.stored()->nodes.front();
  EXPECT_NEAR(after.x, node.x, 1e-9) << "the node itself did not move";
  EXPECT_NEAR(after.y, node.y, 1e-9);
  EXPECT_NEAR(after.tangent_out_x, 6.0, 1e-9);
  EXPECT_NEAR(after.tangent_out_y, 4.0, 1e-9);
}

TEST(SurfaceTool, ClickingAMidpointInsertsANode) {
  Scene scene;
  SurfaceTool tool(scene.document, scene.selection);
  scene.select_surface();

  const std::vector<SurfaceNode> before = tool.nodes();
  ASSERT_GE(before.size(), 3U);
  const double mx = 0.5 * (before[0].x + before[1].x);
  const double my = 0.5 * (before[0].y + before[1].y);

  ASSERT_TRUE(tool.mouse_press(at(mx, my, Qt::LeftButton)));
  ASSERT_TRUE(tool.mouse_release(at(mx, my)));

  EXPECT_EQ(scene.document.undo_stack()->index(), scene.base_count + 1);
  ASSERT_NE(scene.stored(), nullptr);
  EXPECT_EQ(scene.stored()->nodes.size(), before.size() + 1);
  EXPECT_EQ(scene.stored()->source, BoundarySource::Authored);
}

TEST(SurfaceTool, DeleteRemovesTheActiveNodeButNeverBelowThree) {
  Scene scene;
  SurfaceTool tool(scene.document, scene.selection);
  scene.select_surface();

  // Reduce to a triangle, then try to delete one more.
  const std::vector<SurfaceNode> triangle{SurfaceNode{.x = 15.0, .y = 15.0},
                                          SurfaceNode{.x = 45.0, .y = 15.0},
                                          SurfaceNode{.x = 30.0, .y = 45.0}};
  ASSERT_TRUE(scene.document.push_command(
      roadmaker::edit::set_surface_boundary(scene.document.network(), scene.surface, triangle)));
  const int after_setup = scene.document.undo_stack()->index();

  ASSERT_TRUE(tool.mouse_press(at(15.0, 15.0, Qt::LeftButton)));
  ASSERT_TRUE(tool.mouse_release(at(15.0, 15.0)));
  EXPECT_TRUE(tool.key_press(Qt::Key_Delete, Qt::NoModifier));
  EXPECT_EQ(scene.document.undo_stack()->index(), after_setup)
      << "a 3-node boundary refuses to shrink";
  EXPECT_EQ(scene.stored()->nodes.size(), 3U);

  // With a fourth node the same gesture succeeds.
  std::vector<SurfaceNode> quad = triangle;
  quad.push_back(SurfaceNode{.x = 20.0, .y = 40.0});
  ASSERT_TRUE(scene.document.push_command(
      roadmaker::edit::set_surface_boundary(scene.document.network(), scene.surface, quad)));
  const int before_delete = scene.document.undo_stack()->index();
  ASSERT_TRUE(tool.mouse_press(at(15.0, 15.0, Qt::LeftButton)));
  ASSERT_TRUE(tool.mouse_release(at(15.0, 15.0)));
  EXPECT_TRUE(tool.key_press(Qt::Key_Delete, Qt::NoModifier));
  EXPECT_EQ(scene.document.undo_stack()->index(), before_delete + 1);
  EXPECT_EQ(scene.stored()->nodes.size(), 3U);
}

TEST(SurfaceTool, RevertToDerivedReattachesTheSurface) {
  Scene scene;
  SurfaceTool tool(scene.document, scene.selection);
  scene.select_surface();

  const SurfaceNode grabbed = tool.nodes().front();
  ASSERT_TRUE(tool.mouse_press(at(grabbed.x, grabbed.y, Qt::LeftButton)));
  EXPECT_TRUE(tool.mouse_move(at(grabbed.x + 4.0, grabbed.y, Qt::LeftButton)));
  ASSERT_TRUE(tool.mouse_release(at(grabbed.x + 4.0, grabbed.y)));
  ASSERT_EQ(scene.stored()->source, BoundarySource::Authored);

  QSignalSpy status(&tool, &Tool::status_message);
  tool.revert_to_derived();
  EXPECT_EQ(scene.document.undo_stack()->index(), scene.base_count + 2);
  EXPECT_EQ(scene.stored()->source, BoundarySource::Derived);
  EXPECT_TRUE(scene.stored()->nodes.empty());
  EXPECT_EQ(status.count(), 1);

  // The whole round trip is byte-identical to where it started.
  EXPECT_EQ(xodr(scene.document), scene.base_xodr);
}

TEST(SurfaceTool, RevertOnADerivedSurfaceReportsRatherThanPushes) {
  Scene scene;
  SurfaceTool tool(scene.document, scene.selection);
  scene.select_surface();

  QSignalSpy status(&tool, &Tool::status_message);
  tool.revert_to_derived();
  EXPECT_EQ(scene.document.undo_stack()->index(), scene.base_count);
  EXPECT_EQ(status.count(), 1);
}

TEST(SurfaceTool, InstructionTracksWhetherASurfaceIsSelected) {
  Scene scene;
  SurfaceTool tool(scene.document, scene.selection);
  EXPECT_FALSE(tool.instruction().isEmpty());
  const QString unselected = tool.instruction();
  scene.select_surface();
  EXPECT_NE(tool.instruction(), unselected);
}

} // namespace roadmaker::editor
