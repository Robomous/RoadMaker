// Lane Form tool (p2-s5): a click forms an interior lane from the click station
// to the road end through edit::form_lane — press begins a preview, release
// commits ONE undo entry. The kernel guard (a form that would reach a downstream
// seam) is surfaced as a status message with no state left behind, and a tool
// switch mid-preview must not leak the session (acceptance-critical).

#include "roadmaker/edit/operations.hpp"
#include "roadmaker/road/network.hpp"

#include <gtest/gtest.h>

#include <optional>
#include <stdexcept>

#include "document/document.hpp"
#include "document/selection_model.hpp"
#include "tools/lane_form_tool.hpp"

using roadmaker::Lane;
using roadmaker::LaneId;
using roadmaker::LaneProfile;
using roadmaker::LaneSectionId;
using roadmaker::RoadId;
using roadmaker::Waypoint;
using roadmaker::editor::Document;
using roadmaker::editor::LaneFormTool;
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

  [[nodiscard]] ToolEvent at(double s, Qt::MouseButtons buttons) const {
    ToolEvent event;
    event.world_x = s;
    event.world_y = -1.75;
    event.buttons = buttons;
    event.pick = PickHit{.road = road, .lane = right_lane(), .position = {s, -1.75, 0.0}};
    event.station = StationCoord{.s = s, .t = -1.75};
    return event;
  }

  [[nodiscard]] const Lane* lane_in_last_section(int odr_id) const {
    const roadmaker::Road* r = document.network().road(road);
    const LaneSectionId last = r->sections.back();
    for (const LaneId lane_id : document.network().lane_section(last)->lanes) {
      if (document.network().lane(lane_id)->odr_id == odr_id) {
        return document.network().lane(lane_id);
      }
    }
    return nullptr;
  }
};

} // namespace

TEST(LaneFormTool, LaneFormClickGrowsInteriorLane) {
  Scene scene;
  LaneFormTool tool(scene.document, scene.selection);
  const int before = scene.document.undo_stack()->count();

  ASSERT_TRUE(tool.mouse_press(scene.at(60.0, Qt::LeftButton)));
  EXPECT_TRUE(scene.document.preview_active());
  ASSERT_TRUE(tool.mouse_release(scene.at(60.0, Qt::NoButton)));

  EXPECT_FALSE(scene.document.preview_active());
  EXPECT_EQ(scene.document.undo_stack()->count(), before + 1);
  EXPECT_EQ(scene.document.network().road(scene.road)->sections.size(), 2U);

  // The formed lane took the picked lane's position (-1), so it sits inboard of
  // the old -1 (now pushed out to -2), and it is backward-unlinked.
  const Lane* formed = scene.lane_in_last_section(-1);
  ASSERT_NE(formed, nullptr);
  EXPECT_FALSE(formed->predecessor.has_value());
  EXPECT_NE(scene.lane_in_last_section(-2), nullptr); // the displaced original

  scene.document.undo_stack()->undo();
  EXPECT_EQ(scene.document.network().road(scene.road)->sections.size(), 1U);
}

TEST(LaneFormTool, LaneFormOnNonLastSectionRefusesWithMessage) {
  Scene scene;
  // A boundary at 90 makes a form at 60 land in a non-final section, which the
  // kernel refuses (forward-linking is out of scope for p2-s5).
  ASSERT_TRUE(scene.document.push_command(
      roadmaker::edit::split_lane_section(scene.document.network(), scene.road, 90.0)));
  const int before = scene.document.undo_stack()->count();

  LaneFormTool tool(scene.document, scene.selection);
  QString message;
  QObject::connect(
      &tool, &LaneFormTool::status_message, [&](const QString& text) { message = text; });

  ASSERT_TRUE(tool.mouse_press(scene.at(60.0, Qt::LeftButton)));
  EXPECT_FALSE(scene.document.preview_active()) << "a refused form leaves no session";
  EXPECT_FALSE(message.isEmpty()) << "the guard message must surface";
  EXPECT_EQ(scene.document.undo_stack()->count(), before);
}

TEST(LaneFormTool, DeactivateMidDragCancelsPreview) {
  Scene scene;
  LaneFormTool tool(scene.document, scene.selection);
  const int before = scene.document.undo_stack()->count();

  ASSERT_TRUE(tool.mouse_press(scene.at(60.0, Qt::LeftButton)));
  ASSERT_TRUE(scene.document.preview_active());

  tool.deactivate();
  EXPECT_FALSE(scene.document.preview_active()) << "the preview must not leak";
  EXPECT_EQ(scene.document.undo_stack()->count(), before);

  const bool ok = scene.document
                      .push_command(roadmaker::edit::split_lane_section(
                          scene.document.network(), scene.road, 30.0))
                      .has_value();
  EXPECT_TRUE(ok);
}
