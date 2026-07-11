// Lane Profile editor (issue #14, docs/design/m2/02_editing_tools.md §4):
// the PropertiesPanel's lane-profile section drives Document commands — one
// per discrete action — and the LaneProfileTool routes lane-granular picks
// into the SelectionModel. Headless: widget signals are invoked directly.

#include "roadmaker/edit/operations.hpp"
#include "roadmaker/road/authoring.hpp"
#include "roadmaker/road/lane.hpp"
#include "roadmaker/xodr/writer.hpp"

#include <gtest/gtest.h>

#include <QComboBox>
#include <QDoubleSpinBox>
#include <QPushButton>
#include <stdexcept>
#include <string>

#include "document/document.hpp"
#include "document/selection_model.hpp"
#include "panels/properties_panel.hpp"
#include "tools/lane_profile_tool.hpp"

using roadmaker::Lane;
using roadmaker::LaneId;
using roadmaker::LaneProfile;
using roadmaker::LaneType;
using roadmaker::RoadId;
using roadmaker::RoadMarkType;
using roadmaker::Waypoint;
using roadmaker::editor::Document;
using roadmaker::editor::LaneProfileTool;
using roadmaker::editor::PickHit;
using roadmaker::editor::PropertiesPanel;
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

/// One road (two_lane_default: left +1 driving; right -1 driving, -2
/// shoulder) with the panel wired up and lane -1 selected.
struct Scene {
  Document document;
  SelectionModel selection{document};
  PropertiesPanel panel{document, selection};
  RoadId road;
  int base_count = 0;

  Scene() {
    auto created = document.push_command(
        roadmaker::edit::create_road({Waypoint{.x = 0.0, .y = 0.0}, Waypoint{.x = 80.0, .y = 0.0}},
                                     LaneProfile::two_lane_default(),
                                     "Subject"));
    if (!created) {
      throw std::runtime_error("scene setup failed");
    }
    document.network().for_each_road([&](RoadId id, const roadmaker::Road&) { road = id; });
    base_count = document.undo_stack()->count();
  }

  [[nodiscard]] LaneId lane(int odr_id) const {
    const auto& section =
        *document.network().lane_section(document.network().road(road)->sections[0]);
    for (const LaneId lane_id : section.lanes) {
      if (document.network().lane(lane_id)->odr_id == odr_id) {
        return lane_id;
      }
    }
    throw std::runtime_error("lane not found");
  }

  void select_lane(int odr_id) { selection.select({.road = road, .lane = lane(odr_id)}); }

  template <typename T>
  [[nodiscard]] T* editor(const char* name) {
    T* widget = panel.findChild<T*>(QString::fromLatin1(name));
    if (widget == nullptr) {
      throw std::runtime_error("editor widget not found");
    }
    return widget;
  }
};

} // namespace

TEST(LaneProfilePanel, WidthSpinCommitsOneCommandAndUndoRestores) {
  Scene scene;
  scene.select_lane(-1);
  auto* spin = scene.editor<QDoubleSpinBox>("lane_width_spin");
  ASSERT_TRUE(spin->isEnabled());
  EXPECT_NEAR(spin->value(), 3.5, 1e-9); // synced from the network

  const std::string before = xodr(scene.document);
  spin->setValue(5.25);
  emit spin->editingFinished();

  EXPECT_EQ(scene.document.undo_stack()->count(), scene.base_count + 1);
  const Lane* lane = scene.document.network().lane(scene.lane(-1));
  EXPECT_NEAR(lane->widths.front().a, 5.25, 1e-12);

  // A second editingFinished without a change (focus-out) pushes nothing.
  emit spin->editingFinished();
  EXPECT_EQ(scene.document.undo_stack()->count(), scene.base_count + 1);

  scene.document.undo_stack()->undo();
  EXPECT_EQ(xodr(scene.document), before);
}

TEST(LaneProfilePanel, TypeComboPushesSetLaneType) {
  Scene scene;
  scene.select_lane(-1);
  auto* combo = scene.editor<QComboBox>("lane_type_combo");
  ASSERT_TRUE(combo->isEnabled());
  EXPECT_EQ(combo->currentData().toInt(), static_cast<int>(LaneType::Driving));

  const int sidewalk = combo->findData(static_cast<int>(LaneType::Sidewalk));
  ASSERT_GE(sidewalk, 0);
  combo->setCurrentIndex(sidewalk);
  emit combo->activated(sidewalk);

  EXPECT_EQ(scene.document.undo_stack()->count(), scene.base_count + 1);
  EXPECT_EQ(scene.document.network().lane(scene.lane(-1))->type, LaneType::Sidewalk);
}

TEST(LaneProfilePanel, MarkComboAndWidthSpinPushSetRoadMark) {
  Scene scene;
  scene.select_lane(-1);
  auto* combo = scene.editor<QComboBox>("road_mark_combo");
  auto* width = scene.editor<QDoubleSpinBox>("road_mark_width_spin");

  const int solid = combo->findData(static_cast<int>(RoadMarkType::Solid));
  ASSERT_GE(solid, 0);
  combo->setCurrentIndex(solid);
  emit combo->activated(solid);
  {
    const Lane* lane = scene.document.network().lane(scene.lane(-1));
    ASSERT_FALSE(lane->road_marks.empty());
    EXPECT_EQ(lane->road_marks.front().type, RoadMarkType::Solid);
  }

  // Bold convention width (0.25 m — docs/domain/opendrive.md).
  width->setValue(PropertiesPanel::kMarkWidthBold);
  emit width->editingFinished();
  {
    const Lane* lane = scene.document.network().lane(scene.lane(-1));
    EXPECT_NEAR(lane->road_marks.front().width, 0.25, 1e-12);
    EXPECT_EQ(lane->road_marks.front().type, RoadMarkType::Solid);
  }
  EXPECT_EQ(scene.document.undo_stack()->count(), scene.base_count + 2);
}

TEST(LaneProfilePanel, AddLaneButtonsAppendOutermostOnEachSide) {
  Scene scene;
  scene.select_lane(-1);
  const auto lane_count = [&] {
    return scene.document.network()
        .lane_section(scene.document.network().road(scene.road)->sections[0])
        ->lanes.size();
  };
  const std::size_t before = lane_count();

  emit scene.editor<QPushButton>("add_left_lane_button")->clicked();
  EXPECT_EQ(lane_count(), before + 1);
  EXPECT_EQ(scene.document.network().lane(scene.lane(2))->type, LaneType::Driving);

  emit scene.editor<QPushButton>("add_right_lane_button")->clicked();
  EXPECT_EQ(lane_count(), before + 2);
  EXPECT_EQ(scene.document.network().lane(scene.lane(-3))->type, LaneType::Driving);

  EXPECT_EQ(scene.document.undo_stack()->count(), scene.base_count + 2);
}

TEST(LaneProfilePanel, RemoveLaneEnabledOnlyForTheOutermostLane) {
  Scene scene;
  auto* remove = scene.editor<QPushButton>("remove_lane_button");

  scene.select_lane(-1); // inner right — not removable in M2
  EXPECT_FALSE(remove->isEnabled());

  scene.select_lane(-2); // outermost right shoulder
  ASSERT_TRUE(remove->isEnabled());
  emit remove->clicked();
  EXPECT_EQ(scene.document.undo_stack()->count(), scene.base_count + 1);
  const auto& section = *scene.document.network().lane_section(
      scene.document.network().road(scene.road)->sections[0]);
  for (const LaneId lane_id : section.lanes) {
    EXPECT_NE(scene.document.network().lane(lane_id)->odr_id, -2);
  }
}

TEST(LaneProfilePanel, CenterLaneEditsAreRestrictedToTheRoadMark) {
  Scene scene;
  scene.select_lane(0);
  EXPECT_FALSE(scene.editor<QDoubleSpinBox>("lane_width_spin")->isEnabled());
  EXPECT_FALSE(scene.editor<QComboBox>("lane_type_combo")->isEnabled());
  EXPECT_FALSE(scene.editor<QPushButton>("remove_lane_button")->isEnabled());

  // Lane 0's mark IS the center-line style — editable.
  auto* combo = scene.editor<QComboBox>("road_mark_combo");
  ASSERT_TRUE(combo->isEnabled());
  const int solid = combo->findData(static_cast<int>(RoadMarkType::Solid));
  combo->setCurrentIndex(solid);
  emit combo->activated(solid);
  EXPECT_EQ(scene.document.network().lane(scene.lane(0))->road_marks.front().type,
            RoadMarkType::Solid);
}

TEST(LaneProfilePanel, RoadLevelSelectionOffersOnlyAddLane) {
  Scene scene;
  scene.selection.select({.road = scene.road, .lane = LaneId{}});
  EXPECT_TRUE(scene.editor<QPushButton>("add_left_lane_button")->isEnabled());
  EXPECT_TRUE(scene.editor<QPushButton>("add_right_lane_button")->isEnabled());
  EXPECT_FALSE(scene.editor<QComboBox>("lane_type_combo")->isEnabled());
  EXPECT_FALSE(scene.editor<QDoubleSpinBox>("lane_width_spin")->isEnabled());
  EXPECT_FALSE(scene.editor<QPushButton>("remove_lane_button")->isEnabled());

  emit scene.editor<QPushButton>("add_right_lane_button")->clicked();
  EXPECT_EQ(scene.document.undo_stack()->count(), scene.base_count + 1);
}

TEST(LaneProfileTool, ClickSelectsThePickedLaneAndEmptySpaceClears) {
  Scene scene;
  LaneProfileTool tool(scene.selection);
  tool.activate();

  ToolEvent click;
  click.buttons = Qt::LeftButton;
  click.pick = PickHit{.road = scene.road, .lane = scene.lane(-1)};
  ASSERT_TRUE(tool.mouse_press(click));
  ASSERT_EQ(scene.selection.entries().size(), 1U);
  EXPECT_EQ(scene.selection.primary().lane, scene.lane(-1));

  ToolEvent miss;
  miss.buttons = Qt::LeftButton;
  ASSERT_TRUE(tool.mouse_press(miss)); // LMB belongs to the tool even on a miss
  EXPECT_TRUE(scene.selection.empty());

  ToolEvent right_button;
  right_button.buttons = Qt::RightButton;
  EXPECT_FALSE(tool.mouse_press(right_button)); // camera keeps RMB
}
