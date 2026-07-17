// Lane Carve tool (p2-s6): a press-drag-release along one lane carves a turn
// lane through edit::carve_lane — ONE preview session, ONE undo entry. The
// width ramps 0 -> full over the dragged span and holds full to the terminus.
// The acceptance-critical property is DeactivateMidDragCancelsPreview: a tool
// switch mid-drag must not leak the preview session (which would refuse the next
// tool's push_command). Offscreen; the tool is driven by ToolEvent literals.

#include "roadmaker/edit/operations.hpp"
#include "roadmaker/road/network.hpp"

#include <gtest/gtest.h>

#include <stdexcept>

#include "document/document.hpp"
#include "document/selection_model.hpp"
#include "tools/lane_carve_tool.hpp"

using roadmaker::LaneId;
using roadmaker::LaneProfile;
using roadmaker::RoadId;
using roadmaker::Waypoint;
using roadmaker::editor::Document;
using roadmaker::editor::LaneCarveTool;
using roadmaker::editor::PickHit;
using roadmaker::editor::SelectionModel;
using roadmaker::editor::StationCoord;
using roadmaker::editor::ToolEvent;

namespace {

struct Scene {
  Document document;
  SelectionModel selection{document};
  RoadId road;

  Scene() {
    if (!document.push_command(roadmaker::edit::create_road(
            {Waypoint{.x = 0.0, .y = 0.0}, Waypoint{.x = 120.0, .y = 0.0}},
            LaneProfile::two_lane_default(),
            "Road"))) {
      throw std::runtime_error("scene create failed");
    }
    document.network().for_each_road([&](RoadId id, const roadmaker::Road&) { road = id; });
  }

  [[nodiscard]] LaneId right_lane() const {
    const roadmaker::Road* r = document.network().road(road);
    for (const LaneId lane_id : document.network().lane_section(r->sections.front())->lanes) {
      if (document.network().lane(lane_id)->odr_id == -1) {
        return lane_id;
      }
    }
    throw std::runtime_error("no -1 lane");
  }

  /// A press/move/release event over the right (-1) lane of `road` at station s.
  [[nodiscard]] ToolEvent at(double s, Qt::MouseButtons buttons) const {
    ToolEvent event;
    event.world_x = s;
    event.world_y = -1.75;
    event.buttons = buttons;
    event.pick = PickHit{.road = road, .lane = right_lane(), .position = {s, -1.75, 0.0}};
    event.station = StationCoord{.s = s, .t = -1.75};
    return event;
  }
};

} // namespace

TEST(LaneCarveTool, DragCarvesTurnLaneInOneUndoStep) {
  Scene scene;
  LaneCarveTool tool(scene.document, scene.selection);
  const int before = scene.document.undo_stack()->count();

  // Drag upstream->toward the terminus; the turn lane runs to the road end.
  ASSERT_TRUE(tool.mouse_press(scene.at(40.0, Qt::LeftButton)));
  ASSERT_TRUE(tool.mouse_move(scene.at(90.0, Qt::LeftButton)));
  EXPECT_TRUE(scene.document.preview_active());
  ASSERT_TRUE(tool.mouse_release(scene.at(90.0, Qt::NoButton)));

  EXPECT_FALSE(scene.document.preview_active());
  EXPECT_EQ(scene.document.undo_stack()->count(), before + 1); // exactly one entry
  // One split at s_start — the carved lane runs in the final section.
  EXPECT_EQ(scene.document.network().road(scene.road)->sections.size(), 2U);

  scene.document.undo_stack()->undo();
  EXPECT_EQ(scene.document.network().road(scene.road)->sections.size(), 1U);
}

TEST(LaneCarveTool, DeactivateMidDragCancelsPreview) {
  Scene scene;
  LaneCarveTool tool(scene.document, scene.selection);
  const int before = scene.document.undo_stack()->count();

  ASSERT_TRUE(tool.mouse_press(scene.at(40.0, Qt::LeftButton)));
  ASSERT_TRUE(tool.mouse_move(scene.at(90.0, Qt::LeftButton)));
  ASSERT_TRUE(scene.document.preview_active());

  tool.deactivate();
  EXPECT_FALSE(scene.document.preview_active()) << "the drag preview must not leak";
  EXPECT_EQ(scene.document.undo_stack()->count(), before) << "a cancelled drag adds no entry";

  // The acceptance guarantee: a fresh command still applies (a leaked session
  // would have refused this).
  const bool ok = scene.document
                      .push_command(roadmaker::edit::split_lane_section(
                          scene.document.network(), scene.road, 60.0))
                      .has_value();
  EXPECT_TRUE(ok);
}

TEST(LaneCarveTool, ShortDragDoesNotCommit) {
  Scene scene;
  LaneCarveTool tool(scene.document, scene.selection);
  const int before = scene.document.undo_stack()->count();

  ASSERT_TRUE(tool.mouse_press(scene.at(40.0, Qt::LeftButton)));
  ASSERT_TRUE(tool.mouse_move(scene.at(40.4, Qt::LeftButton))); // < kMinSpan
  EXPECT_FALSE(scene.document.preview_active());
  ASSERT_TRUE(tool.mouse_release(scene.at(40.4, Qt::NoButton)));

  EXPECT_EQ(scene.document.undo_stack()->count(), before);
  EXPECT_EQ(scene.document.network().road(scene.road)->sections.size(), 1U);
}

TEST(LaneCarveTool, SpanGuardSameRoadOnly) {
  Scene scene;
  ASSERT_TRUE(scene.document.push_command(
      roadmaker::edit::create_road({Waypoint{.x = 0.0, .y = 50.0}, Waypoint{.x = 120.0, .y = 50.0}},
                                   LaneProfile::two_lane_default(),
                                   "Other")));
  RoadId other;
  scene.document.network().for_each_road([&](RoadId id, const roadmaker::Road&) {
    if (id != scene.road) {
      other = id;
    }
  });

  LaneCarveTool tool(scene.document, scene.selection);
  ASSERT_TRUE(tool.mouse_press(scene.at(40.0, Qt::LeftButton)));

  ToolEvent off = scene.at(90.0, Qt::LeftButton);
  off.pick->road = other; // the move wandered onto the other road
  ASSERT_TRUE(tool.mouse_move(off));
  EXPECT_FALSE(scene.document.preview_active()) << "a carve spans one road only";
}
