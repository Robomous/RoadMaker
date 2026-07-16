// Lane Profile editor (issue #14, docs/design/m2/02_editing_tools.md §4):
// the PropertiesPanel's lane-profile section drives Document commands — one
// per discrete action — and the LaneProfileTool routes lane-granular picks
// into the SelectionModel. Headless: widget signals are invoked directly.

#include "roadmaker/edit/operations.hpp"
#include "roadmaker/geometry/poly3.hpp"
#include "roadmaker/road/authoring.hpp"
#include "roadmaker/road/lane.hpp"
#include "roadmaker/xodr/writer.hpp"

#include <gtest/gtest.h>

#include <QComboBox>
#include <QDoubleSpinBox>
#include <QGroupBox>
#include <QPushButton>
#include <QSignalSpy>
#include <stdexcept>
#include <string>

#include "document/document.hpp"
#include "document/selection_model.hpp"
#include "panels/properties_panel.hpp"
#include "panels/scrub_label.hpp"
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

TEST(LaneProfilePanel, RemoveSideButtonsActOnTheOutermostLaneOfEachSide) {
  Scene scene;
  // No lane selection needed — the per-side buttons act on the section.
  scene.selection.select({.road = scene.road, .lane = LaneId{}});
  auto* remove_left = scene.editor<QPushButton>("remove_left_lane_button");
  auto* remove_right = scene.editor<QPushButton>("remove_right_lane_button");

  const auto side_has = [&](int odr) {
    const auto& section = *scene.document.network().lane_section(
        scene.document.network().road(scene.road)->sections[0]);
    for (const LaneId lane_id : section.lanes) {
      if (scene.document.network().lane(lane_id)->odr_id == odr) {
        return true;
      }
    }
    return false;
  };

  // two_lane_default: left [+1 driving], right [-1 driving, -2 shoulder].
  EXPECT_FALSE(remove_left->isEnabled()); // sole driving lane — protected
  ASSERT_TRUE(remove_right->isEnabled()); // outermost right is the shoulder

  emit remove_right->clicked();
  EXPECT_EQ(scene.document.undo_stack()->count(), scene.base_count + 1);
  EXPECT_FALSE(side_has(-2));
  EXPECT_TRUE(side_has(-1));
  // Now only the driving lane remains on the right — the button disables.
  EXPECT_FALSE(remove_right->isEnabled());

  // Undo restores the same lane and re-enables the button.
  scene.document.undo_stack()->undo();
  EXPECT_TRUE(side_has(-2));
  EXPECT_TRUE(remove_right->isEnabled());
}

TEST(LaneProfilePanel, RemoveSideButtonEmitsAStatusMessageOnSuccess) {
  Scene scene;
  scene.selection.select({.road = scene.road, .lane = LaneId{}});
  QSignalSpy spy(&scene.panel, &PropertiesPanel::status_message);
  emit scene.editor<QPushButton>("remove_right_lane_button")->clicked();
  EXPECT_EQ(spy.count(), 1);
}

TEST(LaneProfilePanel, CenterLaneEditsAreRestrictedToTheRoadMark) {
  Scene scene;
  scene.select_lane(0);
  EXPECT_FALSE(scene.editor<QDoubleSpinBox>("lane_width_spin")->isEnabled());
  EXPECT_FALSE(scene.editor<QComboBox>("lane_type_combo")->isEnabled());

  // Lane 0's mark IS the center-line style — editable.
  auto* combo = scene.editor<QComboBox>("road_mark_combo");
  ASSERT_TRUE(combo->isEnabled());
  const int solid = combo->findData(static_cast<int>(RoadMarkType::Solid));
  combo->setCurrentIndex(solid);
  emit combo->activated(solid);
  EXPECT_EQ(scene.document.network().lane(scene.lane(0))->road_marks.front().type,
            RoadMarkType::Solid);
}

TEST(LaneProfilePanel, RoadLevelSelectionOffersAddAndPerSideRemove) {
  Scene scene;
  scene.selection.select({.road = scene.road, .lane = LaneId{}});
  EXPECT_TRUE(scene.editor<QPushButton>("add_left_lane_button")->isEnabled());
  EXPECT_TRUE(scene.editor<QPushButton>("add_right_lane_button")->isEnabled());
  // Per-side remove works at road level (no lane pick): right has a removable
  // shoulder, left has only its driving lane.
  EXPECT_TRUE(scene.editor<QPushButton>("remove_right_lane_button")->isEnabled());
  EXPECT_FALSE(scene.editor<QPushButton>("remove_left_lane_button")->isEnabled());
  // Lane editors stay disabled without a lane selection.
  EXPECT_FALSE(scene.editor<QComboBox>("lane_type_combo")->isEnabled());
  EXPECT_FALSE(scene.editor<QDoubleSpinBox>("lane_width_spin")->isEnabled());

  emit scene.editor<QPushButton>("add_right_lane_button")->clicked();
  EXPECT_EQ(scene.document.undo_stack()->count(), scene.base_count + 1);
}

TEST(LaneProfilePanel, WidthSpinDisabledOnTaperedLane) {
  Scene scene;
  // Author a width that varies along s on lane -1 (a taper) — set_lane_width
  // refuses it, so the constant-width spin must disable itself.
  ASSERT_TRUE(scene.document
                  .push_command(roadmaker::edit::set_lane_width_profile(
                      scene.document.network(),
                      scene.lane(-1),
                      {roadmaker::Poly3{.s = 0.0, .a = 3.0, .b = 0.02}}))
                  .has_value());
  scene.select_lane(-1);

  auto* spin = scene.editor<QDoubleSpinBox>("lane_width_spin");
  auto* scrub = scene.editor<roadmaker::editor::ScrubLabel>("lane_width_scrub");
  EXPECT_FALSE(spin->isEnabled()) << "a tapered lane's width is edited in the 2D Editor";
  EXPECT_FALSE(scrub->isEnabled());
  EXPECT_TRUE(spin->toolTip().contains(QStringLiteral("varies")));
  EXPECT_NEAR(spin->value(), 3.0, 1e-9); // shows widths.front().a

  // editingFinished must push nothing (the guard fires before set_lane_width).
  const int before = scene.document.undo_stack()->count();
  spin->setValue(5.0);
  emit spin->editingFinished();
  EXPECT_EQ(scene.document.undo_stack()->count(), before);

  // The scrub's baseline is nullopt on a tapered lane, so a drag is inert: no
  // preview session opens and nothing lands on the stack.
  emit scrub->scrub_started();
  emit scrub->scrub_moved(40.0);
  EXPECT_FALSE(scene.document.preview_active());
  EXPECT_EQ(scene.document.undo_stack()->count(), before);
  // The taper is untouched.
  EXPECT_NEAR(scene.document.network().lane(scene.lane(-1))->widths.front().b, 0.02, 1e-12);
}

TEST(LaneProfilePanel, WidthSpinEditsConstantLaneAndSkipsUnchanged) {
  Scene scene;
  scene.select_lane(-1);
  auto* spin = scene.editor<QDoubleSpinBox>("lane_width_spin");
  ASSERT_TRUE(spin->isEnabled());
  EXPECT_TRUE(spin->toolTip().isEmpty());

  const int before = scene.document.undo_stack()->count();
  spin->setValue(4.0);
  emit spin->editingFinished();
  EXPECT_EQ(scene.document.undo_stack()->count(), before + 1);
  EXPECT_NEAR(scene.document.network().lane(scene.lane(-1))->widths.front().a, 4.0, 1e-12);

  // Unchanged value skips the push (the re-entrancy guard).
  emit spin->editingFinished();
  EXPECT_EQ(scene.document.undo_stack()->count(), before + 1);
}

TEST(LaneProfilePanel, LaneEditorsDisabledWithoutPrimaryLane) {
  Scene scene;
  // A road selected but no lane: the group stays visible and its road-level
  // Add/Remove buttons keep working; the per-lane editors disable.
  scene.selection.select({.road = scene.road, .lane = LaneId{}});
  EXPECT_TRUE(scene.panel.findChild<QGroupBox*>() != nullptr);
  EXPECT_TRUE(scene.editor<QPushButton>("add_left_lane_button")->isEnabled());
  EXPECT_TRUE(scene.editor<QPushButton>("add_right_lane_button")->isEnabled());
  EXPECT_TRUE(scene.editor<QPushButton>("remove_right_lane_button")->isEnabled());
  EXPECT_FALSE(scene.editor<QComboBox>("lane_type_combo")->isEnabled());
  EXPECT_FALSE(scene.editor<QDoubleSpinBox>("lane_width_spin")->isEnabled());
  EXPECT_FALSE(scene.editor<QComboBox>("road_mark_combo")->isEnabled());
  EXPECT_FALSE(scene.editor<QDoubleSpinBox>("road_mark_width_spin")->isEnabled());
}

TEST(LaneProfileTool, ClickSelectsThePickedLaneAndEmptySpaceClears) {
  Scene scene;
  LaneProfileTool tool(scene.document, scene.selection);
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

TEST(LaneProfileTool, DeleteKeyRemovesTheOutermostSelectedLane) {
  Scene scene;
  LaneProfileTool tool(scene.document, scene.selection);
  tool.activate();
  // -2 is the outermost right lane (the shoulder) — removable.
  scene.select_lane(-2);
  const int before = scene.document.undo_stack()->count();

  ASSERT_TRUE(tool.key_press(Qt::Key_Delete, Qt::NoModifier));
  EXPECT_EQ(scene.document.undo_stack()->count(), before + 1);
  // The shoulder is gone; the driving lane remains.
  const auto& section = *scene.document.network().lane_section(
      scene.document.network().road(scene.road)->sections[0]);
  for (const LaneId id : section.lanes) {
    EXPECT_NE(scene.document.network().lane(id)->odr_id, -2);
  }
}

TEST(LaneProfileTool, DeleteOnACenterOrInteriorLaneEmitsStatusAndPushesNothing) {
  Scene scene;
  LaneProfileTool tool(scene.document, scene.selection);
  tool.activate();
  QSignalSpy spy(&tool, &roadmaker::editor::Tool::status_message);

  // The center lane cannot be removed.
  scene.select_lane(0);
  const int before = scene.document.undo_stack()->count();
  ASSERT_TRUE(tool.key_press(Qt::Key_Delete, Qt::NoModifier)); // consumed
  EXPECT_EQ(scene.document.undo_stack()->count(), before) << "no command pushed";
  EXPECT_EQ(spy.count(), 1) << "the gesture explains why it did nothing";

  // An interior (non-outermost) lane: -1 with -2 further out.
  scene.select_lane(-1);
  ASSERT_TRUE(tool.key_press(Qt::Key_Backspace, Qt::NoModifier));
  EXPECT_EQ(scene.document.undo_stack()->count(), before) << "still nothing pushed";
  EXPECT_EQ(spy.count(), 2);
}

TEST(LaneProfileTool, InstructionMentionsDeleteAndWidth) {
  Scene scene;
  LaneProfileTool tool(scene.document, scene.selection);
  const QString instruction = tool.instruction();
  EXPECT_TRUE(instruction.contains(QStringLiteral("Delete"), Qt::CaseInsensitive));
  EXPECT_TRUE(instruction.contains(QStringLiteral("Width"), Qt::CaseInsensitive));
}
