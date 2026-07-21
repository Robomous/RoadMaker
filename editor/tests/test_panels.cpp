// Offscreen smoke tests: panels instantiate, mirror the models, and route
// selection bidirectionally. Rendering itself is not asserted (no GL in the
// offscreen platform) — the ViewportWidget is deliberately absent here.

#include "roadmaker/assets/prop_library.hpp"
#include "roadmaker/edit/operations.hpp"
#include "roadmaker/mesh/junction_surface_spans.hpp"
#include "roadmaker/mesh/junction_corners.hpp"
#include "roadmaker/mesh/junction_stoplines.hpp"
#include "roadmaker/road/authoring.hpp"
#include "roadmaker/road/junction.hpp"
#include "roadmaker/road/surface_derivation.hpp"
#include "roadmaker/xodr/writer.hpp"

#include <gtest/gtest.h>

#include <QCheckBox>
#include <QDoubleSpinBox>
#include <QItemSelectionModel>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QToolButton>
#include <QTest>
#include <QUndoStack>
#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "document/diagnostics_model.hpp"
#include "document/document.hpp"
#include "document/library_list_model.hpp"
#include "document/library_manifest.hpp"
#include "document/scene_tree_model.hpp"
#include "document/selection_model.hpp"
#include "panels/diagnostics_panel.hpp"
#include "panels/properties_panel.hpp"
#include "panels/scene_tree_panel.hpp"
#include "panels/scrub_label.hpp"
#include "panels/slot_widget.hpp"
#include "tools/corner_tool.hpp"
#include "tools/stopline_tool.hpp"

namespace roadmaker::editor {
namespace {

const std::filesystem::path kSample = std::filesystem::path(RM_SAMPLES_DIR) / "t_junction.xodr";

/// Lane -1 of road 1 lacks <width> (lane-scoped diagnostic) and a duplicate
/// road 1 is skipped (diagnostic without entity ids).
constexpr const char* kDiagnosticXodr = R"(<OpenDRIVE>
  <header revMajor="1" revMinor="7"/>
  <road id="1" length="5">
    <planView><geometry s="0" x="0" y="0" hdg="0" length="5"><line/></geometry></planView>
    <lanes><laneSection s="0">
      <center><lane id="0" type="none"/></center>
      <right><lane id="-1" type="driving"/></right>
    </laneSection></lanes>
  </road>
  <road id="1" length="5">
    <planView><geometry s="0" x="0" y="0" hdg="0" length="5"><line/></geometry></planView>
    <lanes><laneSection s="0"><center><lane id="0" type="none"/></center></laneSection></lanes>
  </road>
</OpenDRIVE>)";

std::filesystem::path write_diagnostic_sample(const QTemporaryDir& dir) {
  const auto path = std::filesystem::path(dir.path().toStdString()) / "diagnostics.xodr";
  std::ofstream(path) << kDiagnosticXodr;
  return path;
}

struct Harness {
  Document document;
  SelectionModel selection{document};
  SceneTreeModel scene_tree_model{document};
  DiagnosticsModel diagnostics_model{document};
};

std::vector<RoadId> all_roads(const Document& document) {
  std::vector<RoadId> roads;
  document.network().for_each_road([&](RoadId id, const Road&) { roads.push_back(id); });
  return roads;
}

std::string xodr(const Document& document) {
  auto text = roadmaker::write_xodr(document.network());
  if (!text) {
    throw std::runtime_error(text.error().message);
  }
  return *text;
}

TEST(SceneTreePanel, ViewClickDrivesSelectionModel) {
  Harness h;
  ASSERT_TRUE(h.document.load(kSample).has_value());
  SceneTreePanel panel(h.scene_tree_model, h.selection);

  const RoadId road = all_roads(h.document).front();
  const QModelIndex road_index = h.scene_tree_model.index_for_road(road);
  ASSERT_TRUE(road_index.isValid());

  panel.view()->setCurrentIndex(road_index);
  EXPECT_EQ(h.selection.primary().road, road);
  EXPECT_FALSE(h.selection.primary().lane.is_valid());
}

TEST(SceneTreePanel, SelectionModelDrivesViewCurrentIndex) {
  Harness h;
  ASSERT_TRUE(h.document.load(kSample).has_value());
  SceneTreePanel panel(h.scene_tree_model, h.selection);

  const RoadId road = all_roads(h.document).front();
  h.selection.select({.road = road});
  EXPECT_EQ(panel.view()->currentIndex(), h.scene_tree_model.index_for_road(road));

  h.selection.clear();
  EXPECT_FALSE(panel.view()->currentIndex().isValid());
}

TEST(SceneTreePanel, MultiSelectMirrorsIntoTheView) {
  Harness h;
  ASSERT_TRUE(h.document.load(kSample).has_value());
  SceneTreePanel panel(h.scene_tree_model, h.selection);

  const std::vector<RoadId> roads = all_roads(h.document);
  ASSERT_GE(roads.size(), 2U);
  h.selection.select_many(std::vector<SelectionEntry>{{.road = roads[0]}, {.road = roads[1]}});

  const QModelIndexList selected = panel.view()->selectionModel()->selectedRows();
  EXPECT_EQ(selected.size(), 2);
  EXPECT_TRUE(selected.contains(h.scene_tree_model.index_for_road(roads[0])));
  EXPECT_TRUE(selected.contains(h.scene_tree_model.index_for_road(roads[1])));
  // The primary (last-selected) drives the current index.
  EXPECT_EQ(panel.view()->currentIndex(), h.scene_tree_model.index_for_road(roads[1]));
}

TEST(SceneTreePanel, ViewMultiSelectDrivesSelectionModel) {
  Harness h;
  ASSERT_TRUE(h.document.load(kSample).has_value());
  SceneTreePanel panel(h.scene_tree_model, h.selection);

  const std::vector<RoadId> roads = all_roads(h.document);
  ASSERT_GE(roads.size(), 2U);
  const QModelIndex first = h.scene_tree_model.index_for_road(roads[0]);
  const QModelIndex second = h.scene_tree_model.index_for_road(roads[1]);

  // Simulate Ctrl+click accumulation in the view.
  panel.view()->selectionModel()->setCurrentIndex(
      first, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
  panel.view()->selectionModel()->setCurrentIndex(
      second, QItemSelectionModel::Select | QItemSelectionModel::Rows);

  EXPECT_EQ(h.selection.entries().size(), 2U);
  EXPECT_TRUE(h.selection.contains({.road = roads[0]}));
  EXPECT_TRUE(h.selection.contains({.road = roads[1]}));
  EXPECT_EQ(h.selection.primary().road, roads[1]);
}

TEST(PropertiesPanel, ConstructsAndFollowsSelection) {
  Harness h;
  ASSERT_TRUE(h.document.load(kSample).has_value());
  PropertiesPanel panel(h.document, h.selection);
  // No crash on select/clear cycles; content assertions live in the models.
  h.selection.select({.road = all_roads(h.document).front()});
  h.selection.clear();
  ASSERT_TRUE(h.document.load(kSample).has_value()); // reload with panel alive
}

// A selected signal shows the Signal section (pose spinboxes populated from the
// network), and editing s commits exactly one move_signal; a no-op focus-out
// pushes nothing.
TEST(PropertiesPanel, SignalSelectionShowsPoseSectionAndEditCommitsMoveSignal) {
  Harness h;
  ASSERT_TRUE(h.document.load(kSample).has_value());
  PropertiesPanel panel(h.document, h.selection);
  auto* s_spin = panel.findChild<QDoubleSpinBox*>(QStringLiteral("signal_s_spin"));
  ASSERT_NE(s_spin, nullptr);
  EXPECT_FALSE(s_spin->isVisibleTo(&panel)); // nothing selected yet

  const RoadId road = all_roads(h.document).front();
  Signal sign;
  sign.odr_id = "p1";
  sign.type = "274";
  sign.subtype = "50";
  sign.country = "DE";
  sign.dynamic = false;
  sign.s = 2.0;
  sign.t = -3.0;
  ASSERT_TRUE(
      h.document.push_command(edit::add_signal(h.document.network(), road, sign)).has_value());
  SignalId signal;
  h.document.network().for_each_signal([&](SignalId id, const Signal&) { signal = id; });
  h.selection.select({.signal = signal});

  ASSERT_TRUE(s_spin->isVisibleTo(&panel));
  EXPECT_DOUBLE_EQ(s_spin->value(), 2.0); // synced from the network

  const int base = h.document.undo_stack()->count();
  s_spin->setValue(7.5);
  emit s_spin->editingFinished();
  EXPECT_EQ(h.document.undo_stack()->count(), base + 1);
  EXPECT_DOUBLE_EQ(h.document.network().signal(signal)->s, 7.5);

  // A focus-out with no change pushes nothing.
  emit s_spin->editingFinished();
  EXPECT_EQ(h.document.undo_stack()->count(), base + 1);
}

// --- scrub-editing (P1/GW-2) -------------------------------------------------
// ScrubLabel's own gesture is covered in test_scrub_label.cpp; what matters
// here is the consumer contract: a whole drag is ONE preview session and lands
// as exactly ONE undo entry, and a cancel restores the baseline exactly.

/// Selects a WIDTH-BEARING lane of the sample (the centre lane carries no
/// <width> records — scrubbing it is correctly inert) and returns the
/// ScrubLabel over Width.
ScrubLabel* scrub_lane_width(Harness& h, PropertiesPanel& panel, LaneId& lane_out) {
  const RoadId road = all_roads(h.document).front();
  const Road* road_ptr = h.document.network().road(road);
  const LaneSection* section = h.document.network().lane_section(road_ptr->sections.front());
  for (const LaneId lane : section->lanes) {
    const Lane* candidate = h.document.network().lane(lane);
    if (candidate != nullptr && !candidate->widths.empty()) {
      lane_out = lane;
      break;
    }
  }
  if (!lane_out.is_valid()) {
    return nullptr;
  }
  h.selection.select({.road = road, .lane = lane_out});

  for (ScrubLabel* label : panel.findChildren<ScrubLabel*>()) {
    if (label->text() == QStringLiteral("Width")) {
      return label;
    }
  }
  return nullptr;
}

/// Drives a scrub of `dx` pixels on `label`, releasing unless `hold`.
void scrub(ScrubLabel* label, int dx, bool hold = false) {
  QTest::mousePress(label, Qt::LeftButton, Qt::NoModifier, QPoint(30, 5));
  QTest::mouseMove(label, QPoint(30 + dx, 5));
  QMouseEvent move(QEvent::MouseMove,
                   QPointF(30 + dx, 5),
                   QPointF(30 + dx, 5),
                   Qt::NoButton,
                   Qt::LeftButton,
                   Qt::NoModifier);
  QCoreApplication::sendEvent(label, &move);
  if (!hold) {
    QTest::mouseRelease(label, Qt::LeftButton, Qt::NoModifier, QPoint(30 + dx, 5));
  }
}

TEST(PropertiesPanel, ScrubbingLaneWidthCommitsExactlyOneUndoEntry) {
  Harness h;
  ASSERT_TRUE(h.document.load(kSample).has_value());
  PropertiesPanel panel(h.document, h.selection);
  LaneId lane;
  ScrubLabel* label = scrub_lane_width(h, panel, lane);
  ASSERT_NE(label, nullptr);

  const double before = h.document.network().lane(lane)->widths.front().a;
  const std::string xodr_before = xodr(h.document);
  const int base = h.document.undo_stack()->count();

  scrub(label, 100); // ~+2 m at 0.02 m/px

  EXPECT_EQ(h.document.undo_stack()->count(), base + 1)
      << "a drag is one gesture and must be one undo entry, not one per frame";
  const double after = h.document.network().lane(lane)->widths.front().a;
  EXPECT_GT(after, before);

  // And that single entry reverses the whole gesture, byte-identically.
  h.document.undo_stack()->undo();
  EXPECT_DOUBLE_EQ(h.document.network().lane(lane)->widths.front().a, before);
  EXPECT_EQ(xodr(h.document), xodr_before);
}

TEST(PropertiesPanel, CancellingAScrubRestoresTheBaselineAndPushesNothing) {
  Harness h;
  ASSERT_TRUE(h.document.load(kSample).has_value());
  PropertiesPanel panel(h.document, h.selection);
  LaneId lane;
  ScrubLabel* label = scrub_lane_width(h, panel, lane);
  ASSERT_NE(label, nullptr);

  const std::string xodr_before = xodr(h.document);
  const int base = h.document.undo_stack()->count();

  scrub(label, 120, /*hold=*/true);
  ASSERT_NE(xodr(h.document), xodr_before) << "preview is live";
  QTest::keyClick(label, Qt::Key_Escape);

  EXPECT_EQ(h.document.undo_stack()->count(), base) << "a cancelled scrub pushes nothing";
  EXPECT_EQ(xodr(h.document), xodr_before);
  EXPECT_FALSE(h.document.preview_active());
}

TEST(PropertiesPanel, AScrubThatEndsWhereItStartedPushesNothing) {
  Harness h;
  ASSERT_TRUE(h.document.load(kSample).has_value());
  PropertiesPanel panel(h.document, h.selection);
  LaneId lane;
  ScrubLabel* label = scrub_lane_width(h, panel, lane);
  ASSERT_NE(label, nullptr);

  const std::string xodr_before = xodr(h.document);
  const int base = h.document.undo_stack()->count();

  QTest::mousePress(label, Qt::LeftButton, Qt::NoModifier, QPoint(30, 5));
  QTest::mouseMove(label, QPoint(130, 5)); // out...
  QTest::mouseMove(label, QPoint(30, 5));  // ...and back to the baseline
  QTest::mouseRelease(label, Qt::LeftButton, Qt::NoModifier, QPoint(30, 5));

  EXPECT_EQ(h.document.undo_stack()->count(), base) << "a net-zero drag is not an edit";
  EXPECT_EQ(xodr(h.document), xodr_before);
}

// --- the Model slot (P1/GW-3 mechanics) --------------------------------------

TEST(PropertiesPanel, DroppingOnTheModelSlotRetargetsThePropInOneCommand) {
  Harness h;
  ASSERT_TRUE(h.document.load(kSample).has_value());
  PropertiesPanel panel(h.document, h.selection);
  auto* slot = panel.findChild<SlotWidget*>(QStringLiteral("object_model_slot"));
  ASSERT_NE(slot, nullptr);
  EXPECT_FALSE(slot->isVisibleTo(&panel)); // nothing selected yet

  const RoadId road = all_roads(h.document).front();
  Object tree;
  tree.odr_id = "s1";
  tree.name = "tree_pine";
  tree.type = ObjectType::Tree;
  tree.s = 5.0;
  ASSERT_TRUE(
      h.document.push_command(edit::add_object(h.document.network(), road, tree)).has_value());
  ObjectId object;
  h.document.network().for_each_object([&](ObjectId id, const Object&) { object = id; });
  h.selection.select({.road = road, .object = object});

  ASSERT_TRUE(slot->isVisibleTo(&panel)) << "a selected prop shows the Prop section";
  const int base = h.document.undo_stack()->count();

  emit slot->item_dropped(QStringLiteral("shrub"));

  EXPECT_EQ(h.document.undo_stack()->count(), base + 1);
  EXPECT_EQ(h.document.network().object(object)->name, "shrub");

  h.document.undo_stack()->undo();
  EXPECT_EQ(h.document.network().object(object)->name, "tree_pine");
}

TEST(PropertiesPanel, AnUnknownDroppedModelIsRefusedWithoutAnUndoEntry) {
  Harness h;
  ASSERT_TRUE(h.document.load(kSample).has_value());
  PropertiesPanel panel(h.document, h.selection);
  auto* slot = panel.findChild<SlotWidget*>(QStringLiteral("object_model_slot"));
  ASSERT_NE(slot, nullptr);

  const RoadId road = all_roads(h.document).front();
  Object tree;
  tree.odr_id = "s1";
  tree.name = "tree_pine";
  tree.type = ObjectType::Tree;
  ASSERT_TRUE(
      h.document.push_command(edit::add_object(h.document.network(), road, tree)).has_value());
  ObjectId object;
  h.document.network().for_each_object([&](ObjectId id, const Object&) { object = id; });
  h.selection.select({.road = road, .object = object});

  const int base = h.document.undo_stack()->count();
  emit slot->item_dropped(QStringLiteral("urban_sidewalk")); // a road style, not a prop model

  EXPECT_EQ(h.document.undo_stack()->count(), base) << "a refusal must not reach the undo stack";
  EXPECT_EQ(h.document.network().object(object)->name, "tree_pine");
}

// --- per-instance marking material override (p3-s5, GW-2 s15 / GW-5 s8) ------

namespace {

// Adds a crosswalk instance carrying CrosswalkData and returns its id.
ObjectId add_crosswalk(Document& document, RoadId road, const char* odr_id, const char* material) {
  Object cw;
  cw.odr_id = odr_id;
  cw.type = ObjectType::Crosswalk;
  cw.s = 20.0;
  cw.length = 7.0;
  cw.crosswalk = CrosswalkData{.asset = "crosswalk.zebra", .material = material};
  if (!document.push_command(edit::add_object(document.network(), road, cw)).has_value()) {
    throw std::runtime_error("add_crosswalk failed");
  }
  ObjectId id;
  document.network().for_each_object([&](ObjectId oid, const Object& o) {
    if (o.odr_id == odr_id) {
      id = oid;
    }
  });
  return id;
}

} // namespace

TEST(PropertiesPanel, DroppingAMaterialOverridesOneCrosswalkInstanceInOneCommand) {
  Harness h;
  ASSERT_TRUE(h.document.load(kSample).has_value());
  PropertiesPanel panel(h.document, h.selection);
  auto* slot = panel.findChild<SlotWidget*>(QStringLiteral("object_material_slot"));
  ASSERT_NE(slot, nullptr);

  const RoadId road = all_roads(h.document).front();
  const ObjectId a = add_crosswalk(h.document, road, "cw_a", "material.paint_white");
  const ObjectId b = add_crosswalk(h.document, road, "cw_b", "material.paint_white");
  h.selection.select({.road = road, .object = a});
  ASSERT_TRUE(slot->isVisibleTo(&panel)) << "a selected marking shows the Material slot";

  const std::string before = xodr(h.document);
  const int base = h.document.undo_stack()->count();
  emit slot->item_dropped(QStringLiteral("material.asphalt"));

  EXPECT_EQ(h.document.undo_stack()->count(), base + 1) << "one undo entry";
  ASSERT_TRUE(h.document.network().object(a)->crosswalk.has_value());
  EXPECT_EQ(h.document.network().object(a)->crosswalk->material, "material.asphalt");
  EXPECT_TRUE(h.document.network().object(a)->crosswalk->material_override) << "override pinned";
  // The sibling instance is untouched.
  EXPECT_EQ(h.document.network().object(b)->crosswalk->material, "material.paint_white");
  EXPECT_FALSE(h.document.network().object(b)->crosswalk->material_override);

  h.document.undo_stack()->undo();
  EXPECT_EQ(xodr(h.document), before) << "undo is byte-identical";
  EXPECT_FALSE(h.document.network().object(a)->crosswalk->material_override);
}

TEST(PropertiesPanel, DroppingAMaterialOverridesAStencilInstance) {
  Harness h;
  ASSERT_TRUE(h.document.load(kSample).has_value());
  PropertiesPanel panel(h.document, h.selection);
  auto* slot = panel.findChild<SlotWidget*>(QStringLiteral("object_material_slot"));
  ASSERT_NE(slot, nullptr);

  const RoadId road = all_roads(h.document).front();
  Object arrow;
  arrow.odr_id = "arrow1";
  arrow.type_str = "roadMark";
  arrow.subtype = "arrowLeft";
  arrow.s = 15.0;
  arrow.stencil = StencilData{.asset = "stencil.arrow_left", .material = "material.paint_white"};
  ASSERT_TRUE(
      h.document.push_command(edit::add_object(h.document.network(), road, arrow)).has_value());
  ObjectId id;
  h.document.network().for_each_object([&](ObjectId oid, const Object& o) {
    if (o.odr_id == "arrow1") {
      id = oid;
    }
  });
  h.selection.select({.road = road, .object = id});
  ASSERT_TRUE(slot->isVisibleTo(&panel));

  const int base = h.document.undo_stack()->count();
  emit slot->item_dropped(QStringLiteral("material.asphalt"));

  EXPECT_EQ(h.document.undo_stack()->count(), base + 1);
  ASSERT_TRUE(h.document.network().object(id)->stencil.has_value());
  EXPECT_EQ(h.document.network().object(id)->stencil->material, "material.asphalt");
  EXPECT_TRUE(h.document.network().object(id)->stencil->material_override);
}

TEST(PropertiesPanel, AnUnknownMaterialDropIsRefusedWithoutAnUndoEntry) {
  Harness h;
  ASSERT_TRUE(h.document.load(kSample).has_value());
  PropertiesPanel panel(h.document, h.selection);
  auto* slot = panel.findChild<SlotWidget*>(QStringLiteral("object_material_slot"));
  ASSERT_NE(slot, nullptr);

  const RoadId road = all_roads(h.document).front();
  const ObjectId a = add_crosswalk(h.document, road, "cw_a", "material.paint_white");
  h.selection.select({.road = road, .object = a});

  const int base = h.document.undo_stack()->count();
  emit slot->item_dropped(QStringLiteral("not_a_material"));

  EXPECT_EQ(h.document.undo_stack()->count(), base) << "a refusal must not reach the undo stack";
  EXPECT_EQ(h.document.network().object(a)->crosswalk->material, "material.paint_white");
}

// --- the Road-style slot (p2-s8) --------------------------------------------

// The first non-connecting road (a road not owned by a junction).
RoadId first_plain_road(const Document& document) {
  RoadId plain;
  document.network().for_each_road([&](RoadId id, const Road& road) {
    if (!plain.is_valid() && !road.junction.is_valid()) {
      plain = id;
    }
  });
  return plain;
}

TEST(PropertiesPanel, DroppingOnTheRoadStyleSlotRestylesTheRoadInOneCommand) {
  Harness h;
  ASSERT_TRUE(h.document.load(kSample).has_value());
  PropertiesPanel panel(h.document, h.selection);
  auto* slot = panel.findChild<SlotWidget*>(QStringLiteral("road_style_slot"));
  ASSERT_NE(slot, nullptr);
  EXPECT_FALSE(slot->isVisibleTo(&panel)); // nothing selected yet

  const RoadId road = first_plain_road(h.document);
  ASSERT_TRUE(road.is_valid());
  h.selection.select({.road = road});
  ASSERT_TRUE(slot->isVisibleTo(&panel)) << "a selected road shows the Road-style section";

  const int base = h.document.undo_stack()->count();
  emit slot->item_dropped(QStringLiteral("style.urban"));

  EXPECT_EQ(h.document.undo_stack()->count(), base + 1);
  // Urban two-lane flattens the road to one section with 5 lanes (centre + 2/side).
  ASSERT_EQ(h.document.network().road(road)->sections.size(), 1U);
  const LaneSectionId section = h.document.network().road(road)->sections.front();
  EXPECT_EQ(h.document.network().lane_section(section)->lanes.size(), 5U);

  h.document.undo_stack()->undo();
  EXPECT_GT(h.document.network().road(road)->sections.size(), 0U);
}

TEST(PropertiesPanel, TheRoadStyleSlotIsHiddenWithoutARoadSelection) {
  Harness h;
  ASSERT_TRUE(h.document.load(kSample).has_value());
  PropertiesPanel panel(h.document, h.selection);
  auto* slot = panel.findChild<SlotWidget*>(QStringLiteral("road_style_slot"));
  ASSERT_NE(slot, nullptr);
  EXPECT_FALSE(slot->isVisibleTo(&panel));

  // Dropping with no road selected is a no-op (no crash, no undo entry).
  const int base = h.document.undo_stack()->count();
  emit slot->item_dropped(QStringLiteral("style.urban"));
  EXPECT_EQ(h.document.undo_stack()->count(), base);
}

// Editable Properties panel via manual binding (issue #15,
// docs/design/m2/01_editing_framework.md §7): the road-name line edit
// commits ONE rename command on editingFinished, skips no-op commits, and
// refresh-on-undo re-syncs the editor without echoing a command back.
TEST(PropertiesPanel, RoadNameEditCommitsOneRenameAndUndoRestores) {
  Harness h;
  ASSERT_TRUE(h.document.load(kSample).has_value());
  PropertiesPanel panel(h.document, h.selection);
  auto* name_edit = panel.findChild<QLineEdit*>(QStringLiteral("road_name_edit"));
  ASSERT_NE(name_edit, nullptr);
  EXPECT_FALSE(name_edit->isVisibleTo(&panel)); // nothing selected yet

  const RoadId road = all_roads(h.document).front();
  h.selection.select({.road = road});
  ASSERT_TRUE(name_edit->isVisibleTo(&panel));
  const std::string original = h.document.network().road(road)->name;
  EXPECT_EQ(name_edit->text().toStdString(), original); // synced from the network

  const std::string before = xodr(h.document);
  const int base = h.document.undo_stack()->count();
  name_edit->setText(QStringLiteral("Renamed by panel"));
  emit name_edit->editingFinished();
  EXPECT_EQ(h.document.undo_stack()->count(), base + 1);
  EXPECT_EQ(h.document.network().road(road)->name, "Renamed by panel");

  // Focus-out without a change (Qt fires editingFinished on both Return and
  // focus loss) must not push a second command.
  emit name_edit->editingFinished();
  EXPECT_EQ(h.document.undo_stack()->count(), base + 1);

  h.document.undo_stack()->undo();
  EXPECT_EQ(xodr(h.document), before);
  EXPECT_EQ(name_edit->text().toStdString(), original); // refresh re-synced the editor
}

TEST(PropertiesPanel, UndoRedoRefreshDoesNotEchoCommands) {
  Harness h;
  ASSERT_TRUE(h.document.load(kSample).has_value());
  PropertiesPanel panel(h.document, h.selection);
  const RoadId road = all_roads(h.document).front();
  h.selection.select({.road = road});

  auto* name_edit = panel.findChild<QLineEdit*>(QStringLiteral("road_name_edit"));
  ASSERT_NE(name_edit, nullptr);
  const int base = h.document.undo_stack()->count();
  name_edit->setText(QStringLiteral("Echo probe"));
  emit name_edit->editingFinished();
  ASSERT_EQ(h.document.undo_stack()->count(), base + 1);

  // Each undo/redo re-meshes once and refreshes the panel; the refresh must
  // not push commands back (count stable, index moves exactly one step).
  QSignalSpy mesh_spy(&h.document, &Document::mesh_changed);
  h.document.undo_stack()->undo();
  EXPECT_EQ(mesh_spy.count(), 1);
  EXPECT_EQ(h.document.undo_stack()->count(), base + 1);
  EXPECT_EQ(h.document.undo_stack()->index(), base);

  h.document.undo_stack()->redo();
  EXPECT_EQ(mesh_spy.count(), 2);
  EXPECT_EQ(h.document.undo_stack()->count(), base + 1);
  EXPECT_EQ(h.document.undo_stack()->index(), base + 1);
  EXPECT_EQ(h.document.network().road(road)->name, "Echo probe");
  EXPECT_EQ(name_edit->text(), QStringLiteral("Echo probe"));
}

TEST(DiagnosticsPanel, DoubleClickResolvableRowSelectsEntity) {
  Harness h;
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  ASSERT_TRUE(h.document.load(write_diagnostic_sample(dir)).has_value());
  DiagnosticsPanel panel(h.document, h.diagnostics_model, h.selection);
  ASSERT_NE(panel.view()->model(), nullptr);
  EXPECT_EQ(panel.view()->model()->rowCount(), static_cast<int>(h.document.diagnostics().size()));

  int lane_row = -1;
  int no_entity_row = -1;
  const auto& diagnostics = h.document.diagnostics();
  for (int row = 0; row < static_cast<int>(diagnostics.size()); ++row) {
    const auto& d = diagnostics[static_cast<std::size_t>(row)];
    if (d.lane.is_valid() && lane_row < 0) {
      lane_row = row;
    }
    if (!d.road.is_valid() && no_entity_row < 0) {
      no_entity_row = row;
    }
  }
  ASSERT_GE(lane_row, 0);
  ASSERT_GE(no_entity_row, 0);

  emit panel.view()->doubleClicked(h.diagnostics_model.index(lane_row, 0));
  const auto& lane_diag = diagnostics[static_cast<std::size_t>(lane_row)];
  EXPECT_EQ(h.selection.primary().road, lane_diag.road);
  EXPECT_EQ(h.selection.primary().lane, lane_diag.lane);

  // Rows without an attached entity leave the selection untouched.
  emit panel.view()->doubleClicked(h.diagnostics_model.index(no_entity_row, 0));
  EXPECT_EQ(h.selection.primary().road, lane_diag.road);
  EXPECT_EQ(h.selection.primary().lane, lane_diag.lane);
}

// --- the Materials slot (p6-s2) ---------------------------------------------

// Loads `h.document` from a square of four roads enclosing one ground surface.
void load_square_surface(Harness& h, const QTemporaryDir& dir) {
  RoadNetwork net;
  const auto seg = [&](const char* id, double x0, double y0, double x1, double y1) {
    const std::vector<Waypoint> wps{Waypoint{.x = x0, .y = y0}, Waypoint{.x = x1, .y = y1}};
    ASSERT_TRUE(roadmaker::author_clothoid_road(net, wps, LaneProfile::two_lane_rural(), "", id)
                    .has_value());
  };
  seg("a", 0.0, 0.0, 20.0, 0.0);
  seg("b", 20.0, 0.0, 20.0, 20.0);
  seg("c", 20.0, 20.0, 0.0, 20.0);
  seg("d", 0.0, 20.0, 0.0, 0.0);
  roadmaker::derive_surfaces(net);
  const auto xml = roadmaker::write_xodr(net);
  ASSERT_TRUE(xml.has_value());
  const auto path = std::filesystem::path(dir.path().toStdString()) / "square.xodr";
  std::ofstream(path) << *xml;
  ASSERT_TRUE(h.document.load(path).has_value());
}

SurfaceId the_surface(const Document& document) {
  SurfaceId id{};
  document.network().for_each_surface([&](SurfaceId sid, const Surface&) { id = sid; });
  return id;
}

TEST(PropertiesPanel, DroppingOnTheMaterialSlotSetsSurfaceMaterialInOneCommand) {
  Harness h;
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  load_square_surface(h, dir);
  PropertiesPanel panel(h.document, h.selection);
  auto* slot = panel.findChild<SlotWidget*>(QStringLiteral("surface_material_slot"));
  ASSERT_NE(slot, nullptr);
  EXPECT_EQ(slot->category(), QStringLiteral("Materials"));
  EXPECT_FALSE(slot->isVisibleTo(&panel)); // nothing selected yet

  const SurfaceId surface = the_surface(h.document);
  ASSERT_TRUE(surface.is_valid());
  h.selection.select({.surface = surface});
  ASSERT_TRUE(slot->isVisibleTo(&panel)) << "a selected ground surface shows the Materials slot";

  const int base = h.document.undo_stack()->count();
  emit slot->item_dropped(QStringLiteral("material.asphalt"));
  EXPECT_EQ(h.document.undo_stack()->count(), base + 1);
  EXPECT_EQ(h.document.network().surface(surface)->material, "asphalt");

  h.document.undo_stack()->undo();
  EXPECT_EQ(h.document.network().surface(surface)->material, "");
}

TEST(PropertiesPanel, AnUnknownDroppedMaterialIsRefusedWithoutAnUndoEntry) {
  Harness h;
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  load_square_surface(h, dir);
  PropertiesPanel panel(h.document, h.selection);
  auto* slot = panel.findChild<SlotWidget*>(QStringLiteral("surface_material_slot"));
  ASSERT_NE(slot, nullptr);

  const SurfaceId surface = the_surface(h.document);
  h.selection.select({.surface = surface});

  const int base = h.document.undo_stack()->count();
  emit slot->item_dropped(QStringLiteral("tree_pine")); // a prop model, not a material
  EXPECT_EQ(h.document.undo_stack()->count(), base) << "a refusal must not reach the undo stack";
  EXPECT_EQ(h.document.network().surface(surface)->material, "");
}

TEST(PropertiesPanel, EngagingTheMaterialSlotRequestsTheMaterialsCategory) {
  Harness h;
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  load_square_surface(h, dir);
  PropertiesPanel panel(h.document, h.selection);
  auto* slot = panel.findChild<SlotWidget*>(QStringLiteral("surface_material_slot"));
  ASSERT_NE(slot, nullptr);

  QSignalSpy spy(&panel, &PropertiesPanel::library_category_requested);
  emit slot->engage_requested(slot->category());
  ASSERT_EQ(spy.count(), 1);
  EXPECT_EQ(spy.front().front().toString(), QStringLiteral("Materials"));
}

// --- the lane Marking slot (p3-s1) ------------------------------------------

// The LaneId with OpenDRIVE `odr_id` in the first section of `road`.
LaneId lane_on_road(const Document& document, RoadId road, int odr_id) {
  const Road* r = document.network().road(road);
  if (r == nullptr || r->sections.empty()) {
    return {};
  }
  const LaneSection* section = document.network().lane_section(r->sections.front());
  if (section == nullptr) {
    return {};
  }
  for (const LaneId lid : section->lanes) {
    const Lane* lane = document.network().lane(lid);
    if (lane != nullptr && lane->odr_id == odr_id) {
      return lid;
    }
  }
  return {};
}

TEST(PropertiesPanel, DroppingOnTheMarkingSlotSetsLaneRoadMarkInOneCommand) {
  Harness h;
  ASSERT_TRUE(h.document.load(kSample).has_value());
  PropertiesPanel panel(h.document, h.selection);
  auto* slot = panel.findChild<SlotWidget*>(QStringLiteral("lane_marking_slot"));
  ASSERT_NE(slot, nullptr);
  EXPECT_EQ(slot->category(), QStringLiteral("Markings"));

  const RoadId road = first_plain_road(h.document);
  const LaneId lane = lane_on_road(h.document, road, -1);
  ASSERT_TRUE(lane.is_valid());
  h.selection.select({.road = road, .lane = lane});

  // Capture the lane's mark before the drop so undo can be checked against it.
  const std::vector<RoadMark> before = h.document.network().lane(lane)->road_marks;
  const int base = h.document.undo_stack()->index();
  emit slot->item_dropped(QStringLiteral("marking.double_dashed_yellow"));
  EXPECT_EQ(h.document.undo_stack()->index(), base + 1);
  ASSERT_FALSE(h.document.network().lane(lane)->road_marks.empty());
  EXPECT_EQ(h.document.network().lane(lane)->road_marks.front().type, RoadMarkType::BrokenBroken);
  EXPECT_EQ(h.document.network().lane(lane)->road_marks.front().color, RoadMarkColor::Yellow);

  h.document.undo_stack()->undo();
  EXPECT_EQ(h.document.undo_stack()->index(), base);
  EXPECT_EQ(h.document.network().lane(lane)->road_marks.size(), before.size());
}

TEST(PropertiesPanel, AnUnknownDroppedMarkingIsRefusedWithoutAnUndoEntry) {
  Harness h;
  ASSERT_TRUE(h.document.load(kSample).has_value());
  PropertiesPanel panel(h.document, h.selection);
  auto* slot = panel.findChild<SlotWidget*>(QStringLiteral("lane_marking_slot"));
  ASSERT_NE(slot, nullptr);

  const RoadId road = first_plain_road(h.document);
  const LaneId lane = lane_on_road(h.document, road, -1);
  ASSERT_TRUE(lane.is_valid());
  h.selection.select({.road = road, .lane = lane});

  const int base = h.document.undo_stack()->count();
  emit slot->item_dropped(QStringLiteral("material.asphalt")); // a material, not a marking
  EXPECT_EQ(h.document.undo_stack()->count(), base) << "a refusal must not reach the undo stack";
}

TEST(PropertiesPanel, EngagingTheMarkingSlotRequestsTheMarkingsCategory) {
  Harness h;
  ASSERT_TRUE(h.document.load(kSample).has_value());
  PropertiesPanel panel(h.document, h.selection);
  auto* slot = panel.findChild<SlotWidget*>(QStringLiteral("lane_marking_slot"));
  ASSERT_NE(slot, nullptr);

  QSignalSpy spy(&panel, &PropertiesPanel::library_category_requested);
  emit slot->engage_requested(slot->category());
  ASSERT_EQ(spy.count(), 1);
  EXPECT_EQ(spy.front().front().toString(), QStringLiteral("Markings"));
}

// --- crosswalk asset editor (p3-s2) -----------------------------------------

TEST(PropertiesPanel, EditAssetPopulatesAndCommitsCrosswalkParams) {
  qRegisterMetaType<LibraryItem>();
  Harness harness;
  LibraryListModel model;
  const auto manifest = LibraryManifest::parse(QByteArrayLiteral(R"({
    "manifest_version": 1,
    "items": [{"key": "crosswalk.zebra", "label": "Zebra", "category": "Crosswalks",
               "create": {"kind": "crosswalk", "width": 3.0, "dash_length": 0.5,
                          "dash_gap": 0.5, "material": "material.paint_white"}}]
  })"));
  ASSERT_TRUE(manifest.has_value());
  model.set_manifest(*manifest);

  PropertiesPanel panel(harness.document, harness.selection);
  panel.set_library_model(&model);
  QSignalSpy spy(&panel, &PropertiesPanel::crosswalk_asset_committed);

  panel.edit_asset(QStringLiteral("crosswalk.zebra"), /*editable=*/true);
  auto* width = panel.findChild<QDoubleSpinBox*>(QStringLiteral("asset_width_spin"));
  ASSERT_NE(width, nullptr);
  EXPECT_DOUBLE_EQ(width->value(), 3.0); // populated from the manifest

  width->setValue(4.5);
  emit width->editingFinished();
  ASSERT_EQ(spy.count(), 1);
  const auto committed = qvariant_cast<LibraryItem>(spy.front().front());
  EXPECT_DOUBLE_EQ(committed.crosswalk_width, 4.5);
  EXPECT_EQ(committed.key, QStringLiteral("crosswalk.zebra"));
  EXPECT_EQ(committed.kind, LibraryItem::Kind::Crosswalk);

  // A read-only (built-in) asset disables editing and does not commit.
  panel.edit_asset(QStringLiteral("crosswalk.zebra"), /*editable=*/false);
  EXPECT_FALSE(width->isEnabled());
}

TEST(PropertiesPanel, PropSetEditorEmitsCommittedItem) {
  qRegisterMetaType<LibraryItem>();
  Harness harness;
  LibraryListModel model;
  const auto manifest = LibraryManifest::parse(QByteArrayLiteral(R"({
    "manifest_version": 1,
    "items": [{"key": "prop_set.mixed", "label": "Mixed", "category": "Prop sets",
               "create": {"kind": "prop_set",
                          "entries": [{"model": "tree_pine", "portion": 3},
                                      {"model": "tree_birch", "portion": 1}]}}]
  })"));
  ASSERT_TRUE(manifest.has_value());
  model.set_manifest(*manifest);

  PropertiesPanel panel(harness.document, harness.selection);
  panel.set_library_model(&model);
  QSignalSpy spy(&panel, &PropertiesPanel::prop_set_asset_committed);

  panel.edit_asset(QStringLiteral("prop_set.mixed"), /*editable=*/true);
  // Two rows were built from the manifest's two entries.
  const auto portions = panel.findChildren<QDoubleSpinBox*>(QStringLiteral("prop_set_portion"));
  ASSERT_EQ(portions.size(), 2);
  EXPECT_DOUBLE_EQ(portions[0]->value(), 3.0);
  EXPECT_DOUBLE_EQ(portions[1]->value(), 1.0);

  // Edit a weight and Save.
  portions[1]->setValue(2.5);
  auto* save = panel.findChild<QPushButton*>(QStringLiteral("prop_set_save"));
  ASSERT_NE(save, nullptr);
  save->click();

  ASSERT_EQ(spy.count(), 1);
  const auto committed = qvariant_cast<LibraryItem>(spy.front().front());
  EXPECT_EQ(committed.kind, LibraryItem::Kind::PropSet);
  EXPECT_EQ(committed.key, QStringLiteral("prop_set.mixed"));
  ASSERT_EQ(committed.prop_entries.size(), 2U);
  EXPECT_EQ(committed.prop_entries[0].model, QStringLiteral("tree_pine"));
  EXPECT_DOUBLE_EQ(committed.prop_entries[0].portion, 3.0);
  EXPECT_DOUBLE_EQ(committed.prop_entries[1].portion, 2.5);

  // A read-only asset disables the entry widgets and the Save button.
  panel.edit_asset(QStringLiteral("prop_set.mixed"), /*editable=*/false);
  EXPECT_FALSE(save->isEnabled());
}

// --- Corner section (p4-s1, issue #225; GW-2 s9 / GW-3 s5) --------------------
// The Corner tool's active corner is tool-local sub-selection, so what matters
// here is the binding: the section appears only for the junction the pane is
// showing, and both edit paths (typed and scrubbed) are ONE undo entry each.

namespace {

/// A ROOMY cross junction: four arms stopping 20 m short of the origin, so
/// every corner has real headroom (max_radius well above the derived radius).
/// On a tight junction the derived radius already sits AT max_radius and an
/// authored value would be clamped on the way in — assertions would then fail
/// for reasons that have nothing to do with the panel.
struct CornerScene {
  Document document;
  SelectionModel selection{document};
  JunctionId junction;

  /// `reach` is how far each arm's face stops from the crossing [m]; the
  /// default leaves every corner metres of headroom. A small reach makes the
  /// arm faces crowd the crossing (see TightCornerScene).
  explicit CornerScene(double reach = 20.0) {
    const std::array<RoadEnd, 4> ends{
        RoadEnd{make(-80.0, 0.0, -reach, 0.0, "1"), ContactPoint::End},
        RoadEnd{make(80.0, 0.0, reach, 0.0, "2"), ContactPoint::End},
        RoadEnd{make(0.0, -80.0, 0.0, -reach, "3"), ContactPoint::End},
        RoadEnd{make(0.0, 80.0, 0.0, reach, "4"), ContactPoint::End}};
    if (!document.push_command(edit::create_junction(document.network(), ends))) {
      throw std::runtime_error("junction create failed");
    }
    document.network().for_each_junction([&](JunctionId id, const Junction&) { junction = id; });
  }

  RoadId make(double x0, double y0, double x1, double y1, const char* odr) {
    if (!document.push_command(
            edit::create_road({Waypoint{.x = x0, .y = y0}, Waypoint{.x = x1, .y = y1}},
                              LaneProfile::two_lane_default(),
                              odr))) {
      throw std::runtime_error("road create failed");
    }
    return document.network().find_road(odr);
  }

  [[nodiscard]] JunctionCornerInfo corner() const {
    const std::vector<JunctionCornerInfo> all = junction_corners(document.network(), junction);
    if (all.empty()) {
      throw std::runtime_error("the cross junction solved no corners");
    }
    return all.front();
  }

  /// The authored override for the first corner's pair, or nullptr when the
  /// corner is still derived.
  [[nodiscard]] const JunctionCorner* authored() const {
    const JunctionCornerInfo info = corner();
    const Junction* junc = document.network().junction(junction);
    for (const JunctionCorner& stored : junc->corners) {
      if (stored.arm_a == info.arm_a && stored.arm_b == info.arm_b) {
        return &stored;
      }
    }
    return nullptr;
  }
};

/// A cross junction whose arm faces crowd the crossing: no fillet fits, so the
/// panel's radius row goes away (max_radius below its 0.5 m floor) while the
/// corner is still a real, paintable corner.
/// At this reach the four corners still solve, but each leaves only ~0.3 m of
/// free edge — below the panel's 0.5 m floor.
struct TightCornerScene : CornerScene {
  TightCornerScene() : CornerScene(4.8) {}
};

/// Click-selects `info`'s corner the way the viewport reports a click on the
/// junction floor, which also mirrors the junction into the SelectionModel.
void activate_corner(CornerTool& tool, const CornerScene& scene, const JunctionCornerInfo& info) {
  const std::array<double, 2> apex = info.apex();
  ToolEvent event;
  event.world_x = apex[0];
  event.world_y = apex[1];
  event.buttons = Qt::LeftButton;
  PickHit hit;
  hit.junction = scene.junction;
  event.pick = hit;
  if (!tool.mouse_press(event)) {
    throw std::runtime_error("the corner click was not consumed");
  }
  event.buttons = Qt::NoButton;
  static_cast<void>(tool.mouse_release(event));
}

/// The panel's Corner Radius spin box (the row the section is really about).
QDoubleSpinBox* corner_spin(PropertiesPanel& panel) {
  return panel.findChild<QDoubleSpinBox*>(QStringLiteral("corner_radius_spin"));
}

} // namespace

TEST(PropertiesPanel, CornerRadiusRowAppearsOnlyForTheActiveCornersJunction) {
  CornerScene scene;
  PropertiesPanel panel(scene.document, scene.selection);
  CornerTool tool(scene.document, scene.selection);
  panel.set_corner_tool(&tool);
  tool.activate();

  QDoubleSpinBox* spin = corner_spin(panel);
  ASSERT_NE(spin, nullptr);
  EXPECT_FALSE(spin->isVisibleTo(&panel)) << "no active corner yet";

  // Selecting the junction alone is not enough — the pane needs a corner.
  scene.selection.select({.junction = scene.junction});
  EXPECT_FALSE(spin->isVisibleTo(&panel));

  const JunctionCornerInfo info = scene.corner();
  activate_corner(tool, scene, info);
  ASSERT_TRUE(spin->isVisibleTo(&panel)) << "an active corner shows the Corner section";
  EXPECT_DOUBLE_EQ(spin->value(), info.radius);
  EXPECT_DOUBLE_EQ(spin->maximum(), info.max_radius);
  auto* arms = panel.findChild<QLabel*>(QStringLiteral("corner_arms_label"));
  ASSERT_NE(arms, nullptr);
  EXPECT_TRUE(arms->text().contains(QStringLiteral("road"))) << arms->text().toStdString();

  // A non-junction primary selection takes the section away again, even though
  // the tool's sub-selection is untouched.
  scene.selection.select({.road = scene.document.network().find_road("1")});
  EXPECT_FALSE(spin->isVisibleTo(&panel));
  EXPECT_TRUE(tool.active_corner().has_value());
}

TEST(PropertiesPanel, TypingACornerRadiusIsOneUndoEntryEachAndUndoRestores) {
  CornerScene scene;
  PropertiesPanel panel(scene.document, scene.selection);
  CornerTool tool(scene.document, scene.selection);
  panel.set_corner_tool(&tool);
  tool.activate();
  const JunctionCornerInfo info = scene.corner();
  ASSERT_GT(info.max_radius, 10.0) << "the fixture must leave room for a 10 m fillet";
  activate_corner(tool, scene, info);

  QDoubleSpinBox* spin = corner_spin(panel);
  ASSERT_NE(spin, nullptr);
  const int base = scene.document.undo_stack()->index();

  spin->setValue(5.0);
  emit spin->editingFinished();
  ASSERT_EQ(scene.document.undo_stack()->index(), base + 1);
  ASSERT_NE(scene.authored(), nullptr);
  ASSERT_TRUE(scene.authored()->radius.has_value());
  EXPECT_DOUBLE_EQ(*scene.authored()->radius, 5.0);

  spin->setValue(10.0);
  emit spin->editingFinished();
  EXPECT_EQ(scene.document.undo_stack()->index(), base + 2)
      << "one typed edit is one command, never one per keystroke";
  EXPECT_DOUBLE_EQ(*scene.authored()->radius, 10.0);

  // Undo restores the previous radius (count() does NOT move on undo — index does).
  scene.document.undo_stack()->undo();
  EXPECT_EQ(scene.document.undo_stack()->index(), base + 1);
  ASSERT_TRUE(scene.authored()->radius.has_value());
  EXPECT_DOUBLE_EQ(*scene.authored()->radius, 5.0);
}

TEST(PropertiesPanel, ARefreshedCornerRadiusDoesNotEchoACommand) {
  CornerScene scene;
  PropertiesPanel panel(scene.document, scene.selection);
  CornerTool tool(scene.document, scene.selection);
  panel.set_corner_tool(&tool);
  tool.activate();
  activate_corner(tool, scene, scene.corner());

  QDoubleSpinBox* spin = corner_spin(panel);
  ASSERT_NE(spin, nullptr);
  const int base = scene.document.undo_stack()->index();
  emit spin->editingFinished(); // focus-out with the value the pane just seeded
  EXPECT_EQ(scene.document.undo_stack()->index(), base);
}

TEST(PropertiesPanel, ScrubbingTheCornerRadiusCommitsExactlyOneUndoEntry) {
  CornerScene scene;
  PropertiesPanel panel(scene.document, scene.selection);
  CornerTool tool(scene.document, scene.selection);
  panel.set_corner_tool(&tool);
  tool.activate();
  const JunctionCornerInfo info = scene.corner();
  activate_corner(tool, scene, info);

  ScrubLabel* label = nullptr;
  for (ScrubLabel* candidate : panel.findChildren<ScrubLabel*>()) {
    if (candidate->objectName() == QStringLiteral("corner_radius_scrub")) {
      label = candidate;
    }
  }
  ASSERT_NE(label, nullptr);

  const int base = scene.document.undo_stack()->index();
  scrub(label, -40); // ~-2 m at 0.05 m/px, away from the geometric ceiling

  EXPECT_EQ(scene.document.undo_stack()->index(), base + 1)
      << "a drag is one gesture and must be one undo entry, not one per frame";
  ASSERT_NE(scene.authored(), nullptr);
  ASSERT_TRUE(scene.authored()->radius.has_value());
  EXPECT_LT(*scene.authored()->radius, info.radius);

  scene.document.undo_stack()->undo();
  EXPECT_EQ(scene.document.undo_stack()->index(), base);
  const JunctionCorner* after_undo = scene.authored();
  EXPECT_TRUE(after_undo == nullptr || !after_undo->radius.has_value())
      << "undo returns the corner to its derived radius";
}

// --- per-corner + junction-wide materials (p4-s2, issue #226) ----------------

namespace {

SlotWidget* slot_named(PropertiesPanel& panel, const char* name) {
  return panel.findChild<SlotWidget*>(QString::fromLatin1(name));
}

/// Whether a slot's caption shows `text` — the slot renders its reference as a
/// plain label, so this is what "the slot reflects the stored value" means.
[[nodiscard]] bool slot_shows(const SlotWidget* slot, const QString& text) {
  const QList<QLabel*> labels = slot->findChildren<QLabel*>();
  return std::any_of(
      labels.begin(), labels.end(), [&text](const QLabel* label) { return label->text() == text; });
}

/// The junction-wide corner-radius spin box.
QDoubleSpinBox* junction_spin(PropertiesPanel& panel) {
  return panel.findChild<QDoubleSpinBox*>(QStringLiteral("junction_corner_radius_spin"));
}

/// A panel + Corner tool with the fixture's first corner already active.
struct CornerPanel {
  PropertiesPanel panel;
  CornerTool tool;
  JunctionCornerInfo info;

  explicit CornerPanel(CornerScene& scene)
      : panel(scene.document, scene.selection), tool(scene.document, scene.selection),
        info(scene.corner()) {
    panel.set_corner_tool(&tool);
    tool.activate();
    activate_corner(tool, scene, info);
  }
};

} // namespace

TEST(PropertiesPanel, CornerSlotsShowAuthoredMaterials) {
  CornerScene scene;
  const JunctionCornerInfo info = scene.corner();
  ASSERT_TRUE(scene.document.push_command(edit::set_corner_sidewalk_material(
      scene.document.network(), scene.junction, info.arm_a, info.arm_b, "concrete")));
  ASSERT_TRUE(scene.document.push_command(edit::set_corner_median_material(
      scene.document.network(), scene.junction, info.arm_a, info.arm_b, "asphalt")));

  CornerPanel ui(scene);
  SlotWidget* sidewalk = slot_named(ui.panel, "corner_sidewalk_slot");
  SlotWidget* median = slot_named(ui.panel, "corner_median_slot");
  ASSERT_NE(sidewalk, nullptr);
  ASSERT_NE(median, nullptr);
  EXPECT_EQ(sidewalk->category(), QStringLiteral("Materials"));
  EXPECT_TRUE(sidewalk->isVisibleTo(&ui.panel));
  EXPECT_TRUE(slot_shows(sidewalk, QStringLiteral("concrete")));
  EXPECT_TRUE(slot_shows(median, QStringLiteral("asphalt")));
}

TEST(PropertiesPanel, DropOnCornerSidewalkSlotPushesOneCommand) {
  CornerScene scene;
  CornerPanel ui(scene);
  SlotWidget* sidewalk = slot_named(ui.panel, "corner_sidewalk_slot");
  ASSERT_NE(sidewalk, nullptr);

  const int base = scene.document.undo_stack()->index();
  emit sidewalk->item_dropped(QStringLiteral("material.concrete"));
  ASSERT_EQ(scene.document.undo_stack()->index(), base + 1);
  ASSERT_NE(scene.authored(), nullptr);
  ASSERT_TRUE(scene.authored()->sidewalk_material.has_value());
  // The BARE catalog name is stored, not the library key spelling.
  EXPECT_EQ(*scene.authored()->sidewalk_material, "concrete");

  // An identical second drop changes nothing, so it must push nothing.
  emit sidewalk->item_dropped(QStringLiteral("material.concrete"));
  EXPECT_EQ(scene.document.undo_stack()->index(), base + 1);

  scene.document.undo_stack()->undo();
  EXPECT_EQ(scene.document.undo_stack()->index(), base);
  const JunctionCorner* after_undo = scene.authored();
  EXPECT_TRUE(after_undo == nullptr || !after_undo->sidewalk_material.has_value());
}

TEST(PropertiesPanel, DropOnCornerMedianSlotPushesOneCommand) {
  CornerScene scene;
  CornerPanel ui(scene);
  SlotWidget* median = slot_named(ui.panel, "corner_median_slot");
  ASSERT_NE(median, nullptr);

  const int base = scene.document.undo_stack()->index();
  emit median->item_dropped(QStringLiteral("material.asphalt"));
  ASSERT_EQ(scene.document.undo_stack()->index(), base + 1);
  ASSERT_NE(scene.authored(), nullptr);
  ASSERT_TRUE(scene.authored()->median_material.has_value());
  EXPECT_EQ(*scene.authored()->median_material, "asphalt");
  // The sidewalk slot is independent — painting one leaves the other unset.
  EXPECT_FALSE(scene.authored()->sidewalk_material.has_value());
}

TEST(PropertiesPanel, DropUnknownKeyOnCornerSlotToastsWithoutCommand) {
  CornerScene scene;
  CornerPanel ui(scene);
  SlotWidget* sidewalk = slot_named(ui.panel, "corner_sidewalk_slot");
  ASSERT_NE(sidewalk, nullptr);

  QSignalSpy spy(&ui.panel, &PropertiesPanel::status_message);
  const int base = scene.document.undo_stack()->index();
  emit sidewalk->item_dropped(QStringLiteral("tree_pine")); // a prop model, not a material
  EXPECT_EQ(scene.document.undo_stack()->index(), base) << "a refusal never reaches the undo stack";
  EXPECT_EQ(spy.count(), 1) << "an unknown key is reported, not silently defaulted";
  EXPECT_EQ(scene.authored(), nullptr);
}

TEST(PropertiesPanel, CornerSlotsVisibleWhenRadiusRowHidden) {
  // A corner whose arm faces leave no room hides the radius row — but it can
  // still be painted, so the material slots must survive that early return.
  TightCornerScene scene;
  PropertiesPanel panel(scene.document, scene.selection);
  CornerTool tool(scene.document, scene.selection);
  panel.set_corner_tool(&tool);
  tool.activate();
  const JunctionCornerInfo info = scene.corner();
  ASSERT_LT(info.max_radius, 0.5) << "the fixture must be too tight for a radius row";
  activate_corner(tool, scene, info);

  QDoubleSpinBox* spin = corner_spin(panel);
  ASSERT_NE(spin, nullptr);
  EXPECT_FALSE(spin->isVisibleTo(&panel)) << "no room for a fillet — no radius row";
  SlotWidget* sidewalk = slot_named(panel, "corner_sidewalk_slot");
  SlotWidget* median = slot_named(panel, "corner_median_slot");
  ASSERT_NE(sidewalk, nullptr);
  ASSERT_NE(median, nullptr);
  EXPECT_TRUE(sidewalk->isVisibleTo(&panel));
  EXPECT_TRUE(median->isVisibleTo(&panel));

  // And the slots still commit from here.
  const int base = scene.document.undo_stack()->index();
  emit sidewalk->item_dropped(QStringLiteral("material.concrete"));
  EXPECT_EQ(scene.document.undo_stack()->index(), base + 1);
}

TEST(PropertiesPanel, JunctionGroupVisibleForJunctionSelection) {
  CornerScene scene;
  PropertiesPanel panel(scene.document, scene.selection);
  QDoubleSpinBox* spin = junction_spin(panel);
  SlotWidget* material = slot_named(panel, "junction_material_slot");
  ASSERT_NE(spin, nullptr);
  ASSERT_NE(material, nullptr);
  EXPECT_FALSE(spin->isVisibleTo(&panel)) << "nothing selected yet";

  scene.selection.select({.junction = scene.junction});
  EXPECT_TRUE(spin->isVisibleTo(&panel));
  EXPECT_TRUE(material->isVisibleTo(&panel));
  EXPECT_EQ(material->category(), QStringLiteral("Materials"));

  // A road selection takes the whole section away again.
  scene.selection.select({.road = scene.document.network().find_road("1")});
  EXPECT_FALSE(spin->isVisibleTo(&panel));
}

TEST(PropertiesPanel, JunctionRadiusSpinSeedsWithoutCommand) {
  CornerScene scene;
  ASSERT_TRUE(scene.document.push_command(
      edit::set_junction_default_corner_radius(scene.document.network(), scene.junction, 6.0)));
  PropertiesPanel panel(scene.document, scene.selection);
  QDoubleSpinBox* spin = junction_spin(panel);
  ASSERT_NE(spin, nullptr);

  const int base = scene.document.undo_stack()->index();
  scene.selection.select({.junction = scene.junction});
  EXPECT_DOUBLE_EQ(spin->value(), 6.0);
  EXPECT_EQ(spin->minimum(), 0.0) << "specialValueText only renders at exactly the minimum";
  EXPECT_DOUBLE_EQ(spin->maximum(), 50.0);
  EXPECT_EQ(scene.document.undo_stack()->index(), base) << "seeding must not echo a command";

  emit spin->editingFinished(); // focus-out with the value the pane just seeded
  EXPECT_EQ(scene.document.undo_stack()->index(), base);
}

TEST(PropertiesPanel, JunctionRadiusEditPushesSingleCommand) {
  CornerScene scene;
  PropertiesPanel panel(scene.document, scene.selection);
  scene.selection.select({.junction = scene.junction});
  QDoubleSpinBox* spin = junction_spin(panel);
  ASSERT_NE(spin, nullptr);

  const int base = scene.document.undo_stack()->index();
  spin->setValue(7.5);
  emit spin->editingFinished();
  ASSERT_EQ(scene.document.undo_stack()->index(), base + 1);
  const Junction* junction = scene.document.network().junction(scene.junction);
  ASSERT_TRUE(junction->default_corner_radius.has_value());
  EXPECT_DOUBLE_EQ(*junction->default_corner_radius, 7.5);

  spin->setValue(9.0);
  emit spin->editingFinished();
  EXPECT_EQ(scene.document.undo_stack()->index(), base + 2)
      << "one typed edit is one command, never one per keystroke";

  scene.document.undo_stack()->undo();
  EXPECT_DOUBLE_EQ(*scene.document.network().junction(scene.junction)->default_corner_radius, 7.5);
}

TEST(PropertiesPanel, JunctionRadiusUnchangedCommitPushesNothing) {
  CornerScene scene;
  PropertiesPanel panel(scene.document, scene.selection);
  scene.selection.select({.junction = scene.junction});
  QDoubleSpinBox* spin = junction_spin(panel);
  ASSERT_NE(spin, nullptr);

  const int base = scene.document.undo_stack()->index();
  // Nothing is authored yet, so the spin sits at the clear sentinel: a commit
  // here would ask the kernel to clear a default that does not exist.
  EXPECT_DOUBLE_EQ(spin->value(), 0.0);
  emit spin->editingFinished();
  EXPECT_EQ(scene.document.undo_stack()->index(), base);

  spin->setValue(4.0);
  emit spin->editingFinished();
  ASSERT_EQ(scene.document.undo_stack()->index(), base + 1);
  emit spin->editingFinished(); // focus-out again, same value
  EXPECT_EQ(scene.document.undo_stack()->index(), base + 1);
}

TEST(PropertiesPanel, JunctionRadiusScrubIsOneUndoEntry) {
  CornerScene scene;
  PropertiesPanel panel(scene.document, scene.selection);
  scene.selection.select({.junction = scene.junction});

  ScrubLabel* label = panel.findChild<ScrubLabel*>(QStringLiteral("junction_corner_radius_scrub"));
  ASSERT_NE(label, nullptr);

  const int base = scene.document.undo_stack()->index();
  scrub(label, 40); // +2 m at 0.05 m/px, up from the unset baseline
  EXPECT_EQ(scene.document.undo_stack()->index(), base + 1)
      << "a drag is one gesture and must be one undo entry, not one per frame";
  const Junction* junction = scene.document.network().junction(scene.junction);
  ASSERT_TRUE(junction->default_corner_radius.has_value())
      << "a scrub never emits the clear sentinel";
  EXPECT_DOUBLE_EQ(*junction->default_corner_radius, 2.0);

  scene.document.undo_stack()->undo();
  EXPECT_EQ(scene.document.undo_stack()->index(), base);
  EXPECT_FALSE(
      scene.document.network().junction(scene.junction)->default_corner_radius.has_value());
}

TEST(PropertiesPanel, JunctionRadiusZeroClearsDefault) {
  CornerScene scene;
  PropertiesPanel panel(scene.document, scene.selection);
  scene.selection.select({.junction = scene.junction});
  QDoubleSpinBox* spin = junction_spin(panel);
  ASSERT_NE(spin, nullptr);

  spin->setValue(5.0);
  emit spin->editingFinished();
  const int base = scene.document.undo_stack()->index();
  ASSERT_TRUE(scene.document.network().junction(scene.junction)->default_corner_radius.has_value());

  spin->setValue(0.0); // the "Derived" position
  emit spin->editingFinished();
  EXPECT_EQ(scene.document.undo_stack()->index(), base + 1);
  EXPECT_FALSE(
      scene.document.network().junction(scene.junction)->default_corner_radius.has_value());

  // Clearing again would be a kernel error — the panel must not even try.
  emit spin->editingFinished();
  EXPECT_EQ(scene.document.undo_stack()->index(), base + 1);
}

TEST(PropertiesPanel, DropOnJunctionMaterialSlotPushesOneCommand) {
  CornerScene scene;
  PropertiesPanel panel(scene.document, scene.selection);
  scene.selection.select({.junction = scene.junction});
  SlotWidget* slot = slot_named(panel, "junction_material_slot");
  ASSERT_NE(slot, nullptr);

  const int base = scene.document.undo_stack()->index();
  emit slot->item_dropped(QStringLiteral("material.concrete"));
  ASSERT_EQ(scene.document.undo_stack()->index(), base + 1);
  EXPECT_EQ(scene.document.network().junction(scene.junction)->material, "concrete");
  EXPECT_TRUE(slot_shows(slot, QStringLiteral("concrete")));

  emit slot->item_dropped(QStringLiteral("material.concrete")); // unchanged
  EXPECT_EQ(scene.document.undo_stack()->index(), base + 1);

  QSignalSpy spy(&panel, &PropertiesPanel::status_message);
  emit slot->item_dropped(QStringLiteral("tree_pine")); // not a material
  EXPECT_EQ(scene.document.undo_stack()->index(), base + 1);
  EXPECT_EQ(spy.count(), 1);

  scene.document.undo_stack()->undo();
  EXPECT_EQ(scene.document.network().junction(scene.junction)->material, "");
}

TEST(PropertiesPanel, JunctionDefaultThenPerCornerOverrideWins) {
  CornerScene scene;
  ASSERT_TRUE(scene.document.push_command(
      edit::set_junction_default_corner_radius(scene.document.network(), scene.junction, 6.0)));

  CornerPanel ui(scene);
  QDoubleSpinBox* spin = corner_spin(ui.panel);
  ASSERT_NE(spin, nullptr);
  ASSERT_TRUE(spin->isVisibleTo(&ui.panel));
  // With no per-corner radius the corner shows the junction default …
  EXPECT_DOUBLE_EQ(spin->value(), 6.0);
  const JunctionCornerInfo defaulted = scene.corner();
  EXPECT_TRUE(defaulted.radius_from_junction_default);
  EXPECT_FALSE(defaulted.radius_authored);

  // … and authoring the corner itself takes precedence over that default.
  spin->setValue(9.0);
  emit spin->editingFinished();
  const JunctionCornerInfo overridden = scene.corner();
  EXPECT_DOUBLE_EQ(overridden.radius, 9.0);
  EXPECT_TRUE(overridden.radius_authored);
  EXPECT_FALSE(overridden.radius_from_junction_default);
  EXPECT_DOUBLE_EQ(*scene.document.network().junction(scene.junction)->default_corner_radius, 6.0)
      << "the junction default survives a per-corner override";
}

} // namespace

// --- Stop line section (p4-s3, #318) -----------------------------------------
//
// Every arm HAS a stop line, so unlike the Corner section there is nothing to
// create: the pane only ever edits, and "Reset to default" is the way back.

namespace {

/// Click-selects `info`'s stop line, which also mirrors the junction into the
/// SelectionModel.
/// The band is its own handle, so — unlike a corner — no junction-floor pick is
/// needed: the click lands directly on the line's centreline.
void activate_stopline(StopLineTool& tool, const JunctionStopLineInfo& info) {
  ToolEvent event;
  event.world_x = (info.left[0] + info.right[0]) / 2.0;
  event.world_y = (info.left[1] + info.right[1]) / 2.0;
  event.buttons = Qt::LeftButton;
  if (!tool.mouse_press(event)) {
    throw std::runtime_error("the stop line click was not consumed");
  }
  event.buttons = Qt::NoButton;
  static_cast<void>(tool.mouse_release(event));
}

JunctionStopLineInfo first_stopline(const CornerScene& scene) {
  const std::vector<JunctionStopLineInfo> all =
      junction_stoplines(scene.document.network(), scene.junction);
  if (all.empty()) {
    throw std::runtime_error("the cross junction solved no stop lines");
  }
  return all.front();
}

QDoubleSpinBox* stopline_spin(PropertiesPanel& panel) {
  return panel.findChild<QDoubleSpinBox*>(QStringLiteral("stopline_distance_spin"));
}

} // namespace

TEST(PropertiesPanel, StopLineRowAppearsOnlyForTheActiveLinesJunction) {
  CornerScene scene;
  PropertiesPanel panel(scene.document, scene.selection);
  StopLineTool tool(scene.document, scene.selection);
  panel.set_stopline_tool(&tool);
  tool.activate();

  QDoubleSpinBox* spin = stopline_spin(panel);
  ASSERT_NE(spin, nullptr);
  EXPECT_FALSE(spin->isVisibleTo(&panel)) << "no active stop line yet";

  const JunctionStopLineInfo info = first_stopline(scene);
  activate_stopline(tool, info);
  ASSERT_TRUE(spin->isVisibleTo(&panel));
  EXPECT_DOUBLE_EQ(spin->value(), info.distance);
  EXPECT_DOUBLE_EQ(spin->maximum(), info.max_distance);
  EXPECT_DOUBLE_EQ(spin->minimum(), 0.0)
      << "zero is a meaningful setback — no special-value sentinel here";

  auto* arm = panel.findChild<QLabel*>(QStringLiteral("stopline_arm_label"));
  ASSERT_NE(arm, nullptr);
  EXPECT_TRUE(arm->text().contains(QStringLiteral("road"))) << arm->text().toStdString();
  EXPECT_TRUE(arm->text().contains(QStringLiteral("approach")));

  // A non-junction primary selection takes the section away again.
  scene.selection.select({.road = scene.document.network().find_road("1")});
  EXPECT_FALSE(spin->isVisibleTo(&panel));
  EXPECT_TRUE(tool.active_stopline().has_value());
}

TEST(PropertiesPanel, StopLineDistanceEditPushesOneCommand) {
  CornerScene scene;
  PropertiesPanel panel(scene.document, scene.selection);
  StopLineTool tool(scene.document, scene.selection);
  panel.set_stopline_tool(&tool);
  tool.activate();
  activate_stopline(tool, first_stopline(scene));

  const int before = scene.document.undo_stack()->index();
  QDoubleSpinBox* spin = stopline_spin(panel);
  spin->setValue(7.5);
  emit spin->editingFinished();

  EXPECT_EQ(scene.document.undo_stack()->index(), before + 1);
  EXPECT_DOUBLE_EQ(first_stopline(scene).distance, 7.5);
  EXPECT_TRUE(first_stopline(scene).distance_authored);
}

TEST(PropertiesPanel, StopLineFlipButtonFlipsAndResetEnablesOnlyWhenAuthored) {
  CornerScene scene;
  PropertiesPanel panel(scene.document, scene.selection);
  StopLineTool tool(scene.document, scene.selection);
  panel.set_stopline_tool(&tool);
  tool.activate();
  activate_stopline(tool, first_stopline(scene));

  auto* reset = panel.findChild<QPushButton*>(QStringLiteral("stopline_reset_button"));
  auto* flip = panel.findChild<QPushButton*>(QStringLiteral("stopline_flip_button"));
  ASSERT_NE(reset, nullptr);
  ASSERT_NE(flip, nullptr);
  EXPECT_FALSE(reset->isEnabled()) << "a derived line has nothing to reset";

  flip->click();
  EXPECT_TRUE(first_stopline(scene).flipped);
  EXPECT_TRUE(reset->isEnabled()) << "the flip authored a record";

  reset->click();
  EXPECT_FALSE(first_stopline(scene).authored);
  EXPECT_FALSE(first_stopline(scene).flipped);
  EXPECT_FALSE(reset->isEnabled());
}

// --- junction type + lock (p4-s4, issue #319) --------------------------------
//
// The Type row is DERIVED state, so the smoke test walks all four values; the
// Locked checkbox is the one thing the user can change here, and it is enabled
// exactly for the two states edit::set_junction_locked accepts.

namespace {

QLabel* junction_type(PropertiesPanel& panel) {
  return panel.findChild<QLabel*>(QStringLiteral("junction_type_label"));
}

QCheckBox* junction_lock(PropertiesPanel& panel) {
  return panel.findChild<QCheckBox*>(QStringLiteral("junction_locked_check"));
}

} // namespace

TEST(PropertiesPanel, JunctionTypeRowFollowsTheDerivedState) {
  CornerScene scene;
  PropertiesPanel panel(scene.document, scene.selection);
  QLabel* type = junction_type(panel);
  QCheckBox* lock = junction_lock(panel);
  ASSERT_NE(type, nullptr);
  ASSERT_NE(lock, nullptr);

  scene.selection.select({.junction = scene.junction});
  EXPECT_EQ(type->text(), QStringLiteral("Automatic"));
  EXPECT_FALSE(lock->isChecked());
  EXPECT_TRUE(lock->isEnabled());

  ASSERT_TRUE(scene.document.push_command(
      edit::set_junction_locked(scene.document.network(), scene.junction, true)));
  scene.selection.select({.junction = scene.junction});
  EXPECT_EQ(type->text(), QStringLiteral("Locked"));
  EXPECT_TRUE(lock->isChecked());
  EXPECT_TRUE(lock->isEnabled());
}

TEST(PropertiesPanel, JunctionLockCheckboxPushesOneCommandAndNeverEchoes) {
  CornerScene scene;
  PropertiesPanel panel(scene.document, scene.selection);
  QCheckBox* lock = junction_lock(panel);
  ASSERT_NE(lock, nullptr);
  scene.selection.select({.junction = scene.junction});

  const int base = scene.document.undo_stack()->index();
  lock->setChecked(true);
  EXPECT_EQ(scene.document.undo_stack()->index(), base + 1);
  EXPECT_TRUE(scene.document.network().junction(scene.junction)->locked);

  // Re-seeding from the record must not push a second (no-op) command — the
  // kernel refuses those, so an echo would show up as a rejected command.
  scene.selection.select({.junction = scene.junction});
  EXPECT_EQ(scene.document.undo_stack()->index(), base + 1);

  lock->setChecked(false);
  EXPECT_EQ(scene.document.undo_stack()->index(), base + 2);
  EXPECT_FALSE(scene.document.network().junction(scene.junction)->locked);
}

TEST(PropertiesPanel, SpanJunctionShowsSpanAndCannotBeUnlocked) {
  Document document;
  SelectionModel selection{document};
  ASSERT_TRUE(document.push_command(
      edit::create_road({Waypoint{.x = 0.0, .y = 0.0}, Waypoint{.x = 200.0, .y = 0.0}},
                        LaneProfile::two_lane_default(),
                        "main")));
  ASSERT_FALSE(document.last_dirty().roads.empty());
  const std::array<SpanArm, 1> spans{
      SpanArm{.road = document.last_dirty().roads.front(), .s_start = 40.0, .s_end = 60.0}};
  ASSERT_TRUE(document.push_command(edit::create_span_junction(document.network(), spans)));
  JunctionId span_junction;
  document.network().for_each_junction([&](JunctionId id, const Junction& junction) {
    if (!junction.spans.empty()) {
      span_junction = id;
    }
  });
  ASSERT_TRUE(span_junction.is_valid());

  PropertiesPanel panel(document, selection);
  selection.select({.junction = span_junction});
  ASSERT_NE(junction_type(panel), nullptr);
  EXPECT_EQ(junction_type(panel)->text(), QStringLiteral("Span (virtual)"));
  EXPECT_TRUE(junction_lock(panel)->isChecked());
  EXPECT_FALSE(junction_lock(panel)->isEnabled())
      << "a 12.7 virtual junction is locked structurally";
}

TEST(PropertiesPanel, ForeignJunctionShowsForeignAndCannotBeLocked) {
  // rm:arms is what makes a junction ours; strip it and the reload produces
  // exactly what someone else's file gives us.
  CornerScene source;
  auto written = write_xodr(source.document.network());
  ASSERT_TRUE(written.has_value());
  std::string text = *written;
  const std::string arms = R"(<userData code="rm:arms")";
  for (std::size_t at = text.find(arms); at != std::string::npos; at = text.find(arms, at)) {
    const std::size_t end = text.find("/>", at);
    ASSERT_NE(end, std::string::npos);
    text.erase(at, end + 2 - at);
  }
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  const std::filesystem::path path =
      std::filesystem::path(dir.path().toStdString()) / "foreign.xodr";
  {
    std::ofstream out(path);
    out << text;
  }

  Document document;
  SelectionModel selection{document};
  ASSERT_TRUE(document.load(path));
  JunctionId foreign;
  document.network().for_each_junction([&](JunctionId id, const Junction&) { foreign = id; });
  ASSERT_TRUE(foreign.is_valid());
  ASSERT_TRUE(document.network().junction(foreign)->arms.empty());

  PropertiesPanel panel(document, selection);
  selection.select({.junction = foreign});
  EXPECT_EQ(junction_type(panel)->text(), QStringLiteral("Foreign"));
  EXPECT_FALSE(junction_lock(panel)->isChecked());
  EXPECT_FALSE(junction_lock(panel)->isEnabled()) << "there is no automatic derivation to guard";
}

// --- per-instance prop size (p6-s10, #335) -----------------------------------
// The Height row is the Attributes pane's first MULTI-SELECT batch edit: one
// gesture over N selected props must be one update_objects command, hence one
// undo entry, with every prop scaled by the same factor.

namespace {

/// Adds a tree_pine prop at `s` with an optional declared @height, and returns
/// its id. A nullopt height leaves the object relying on the model's size.
ObjectId add_prop(
    Document& document, RoadId road, const char* odr_id, double s, std::optional<double> height) {
  Object tree;
  tree.odr_id = odr_id;
  tree.name = "tree_pine";
  tree.type = ObjectType::Tree;
  tree.s = s;
  tree.t = 6.0;
  tree.radius = 1.2;
  tree.height = height;
  if (!document.push_command(edit::add_object(document.network(), road, tree)).has_value()) {
    throw std::runtime_error("add_prop failed");
  }
  ObjectId id;
  document.network().for_each_object([&](ObjectId oid, const Object& object) {
    if (object.odr_id == odr_id) {
      id = oid;
    }
  });
  return id;
}

ScrubLabel* height_scrub(PropertiesPanel& panel) {
  return panel.findChild<ScrubLabel*>(QStringLiteral("object_height_scrub"));
}

double model_height() {
  const props::PropModel* model = props::model("tree_pine");
  if (model == nullptr) {
    throw std::runtime_error("tree_pine missing from the prop library");
  }
  return model->height;
}

// --- Surface spans (p4-s5, issue #320) ---------------------------------------

/// The panel's per-span rows, in display order.
std::vector<QWidget*> span_rows(const PropertiesPanel& panel) {
  std::vector<QWidget*> rows;
  auto* group = panel.findChild<QGroupBox*>(QStringLiteral("surface_spans_group"));
  if (group == nullptr) {
    return rows;
  }
  for (QWidget* row : group->findChildren<QWidget*>(QStringLiteral("surface_span_row"))) {
    rows.push_back(row);
  }
  return rows;
}

} // namespace

TEST(PropertiesPanel, PropHeightScrubCommitsOneUndoEntry) {
  Harness h;
  ASSERT_TRUE(h.document.load(kSample).has_value());
  PropertiesPanel panel(h.document, h.selection);
  const RoadId road = all_roads(h.document).front();
  const ObjectId tree = add_prop(h.document, road, "t1", 20.0, 4.0);
  h.selection.select({.road = road, .object = tree});

  ScrubLabel* label = height_scrub(panel);
  ASSERT_NE(label, nullptr);
  const std::string xodr_before = xodr(h.document);
  const int base = h.document.undo_stack()->count();

  scrub(label, 100); // +5 m at 0.05 m/px

  EXPECT_EQ(h.document.undo_stack()->count(), base + 1) << "one gesture, one undo entry";
  const Object* after = h.document.network().object(tree);
  ASSERT_NE(after, nullptr);
  ASSERT_TRUE(after->height.has_value());
  EXPECT_GT(*after->height, 4.0);
  ASSERT_TRUE(after->radius.has_value());
  EXPECT_GT(*after->radius, 1.2) << "a present sibling dimension scales with the height";

  h.document.undo_stack()->undo();
  EXPECT_EQ(xodr(h.document), xodr_before);
}

TEST(PropertiesPanel, PropHeightBatchScrubResizesAllSelected) {
  Harness h;
  ASSERT_TRUE(h.document.load(kSample).has_value());
  PropertiesPanel panel(h.document, h.selection);
  const RoadId road = all_roads(h.document).front();
  const ObjectId small = add_prop(h.document, road, "t1", 10.0, 2.0);
  const ObjectId bare = add_prop(h.document, road, "t2", 20.0, std::nullopt);
  const ObjectId tall = add_prop(h.document, road, "t3", 30.0, 8.0);
  // The LAST entry is the primary, and its height sets the batch factor.
  h.selection.select_many(std::vector<SelectionEntry>{{.road = road, .object = small},
                                                      {.road = road, .object = bare},
                                                      {.road = road, .object = tall}});
  ASSERT_EQ(h.selection.primary().object, tall);

  ScrubLabel* label = height_scrub(panel);
  ASSERT_NE(label, nullptr);
  const std::string xodr_before = xodr(h.document);
  const int base = h.document.undo_stack()->count();

  scrub(label, 80); // +4 m on the primary: 8 -> 12, a factor of 1.5

  EXPECT_EQ(h.document.undo_stack()->count(), base + 1)
      << "a batch resize is ONE command, not one per prop";
  const double factor = *h.document.network().object(tall)->height / 8.0;
  EXPECT_GT(factor, 1.0);
  EXPECT_NEAR(*h.document.network().object(small)->height, 2.0 * factor, 1e-9);
  // A prop that declared no height is MATERIALIZED from its model height.
  EXPECT_NEAR(*h.document.network().object(bare)->height, model_height() * factor, 1e-9);

  h.document.undo_stack()->undo();
  EXPECT_EQ(xodr(h.document), xodr_before) << "one undo reverses the whole batch";
}

TEST(PropertiesPanel, PropHeightTypedAppliesAbsoluteToAll) {
  Harness h;
  ASSERT_TRUE(h.document.load(kSample).has_value());
  PropertiesPanel panel(h.document, h.selection);
  const RoadId road = all_roads(h.document).front();
  const ObjectId small = add_prop(h.document, road, "t1", 10.0, 2.0);
  const ObjectId tall = add_prop(h.document, road, "t2", 20.0, 8.0);
  h.selection.select_many(
      std::vector<SelectionEntry>{{.road = road, .object = small}, {.road = road, .object = tall}});

  auto* spin = panel.findChild<QDoubleSpinBox*>(QStringLiteral("object_height_spin"));
  ASSERT_NE(spin, nullptr);
  ASSERT_TRUE(spin->isVisibleTo(&panel));
  EXPECT_DOUBLE_EQ(spin->value(), 8.0) << "seeded from the primary's effective height";

  const int base = h.document.undo_stack()->count();
  spin->setValue(5.0);
  emit spin->editingFinished();

  EXPECT_EQ(h.document.undo_stack()->count(), base + 1);
  // Typing is ABSOLUTE: every selected prop becomes 5 m tall.
  EXPECT_DOUBLE_EQ(*h.document.network().object(small)->height, 5.0);
  EXPECT_DOUBLE_EQ(*h.document.network().object(tall)->height, 5.0);
  // Each scaled by its OWN factor, so the small tree's radius grew 2.5x and the
  // tall one's shrank.
  EXPECT_NEAR(*h.document.network().object(small)->radius, 1.2 * (5.0 / 2.0), 1e-9);
  EXPECT_NEAR(*h.document.network().object(tall)->radius, 1.2 * (5.0 / 8.0), 1e-9);
}

TEST(PropertiesPanel, PropHeightScrubCancelRestoresByteIdentical) {
  Harness h;
  ASSERT_TRUE(h.document.load(kSample).has_value());
  PropertiesPanel panel(h.document, h.selection);
  const RoadId road = all_roads(h.document).front();
  const ObjectId tree = add_prop(h.document, road, "t1", 20.0, 4.0);
  h.selection.select({.road = road, .object = tree});

  ScrubLabel* label = height_scrub(panel);
  ASSERT_NE(label, nullptr);
  const std::string xodr_before = xodr(h.document);
  const int base = h.document.undo_stack()->count();

  scrub(label, 120, /*hold=*/true);
  ASSERT_NE(xodr(h.document), xodr_before) << "preview is live";
  QTest::keyClick(label, Qt::Key_Escape);

  EXPECT_EQ(h.document.undo_stack()->count(), base);
  EXPECT_EQ(xodr(h.document), xodr_before);
  EXPECT_FALSE(h.document.preview_active());
}

TEST(PropertiesPanel, PropHeightRowVisibility) {
  Harness h;
  ASSERT_TRUE(h.document.load(kSample).has_value());
  PropertiesPanel panel(h.document, h.selection);
  const RoadId road = all_roads(h.document).front();
  auto* spin = panel.findChild<QDoubleSpinBox*>(QStringLiteral("object_height_spin"));
  ASSERT_NE(spin, nullptr);

  // A prop with no declared height shows its MODEL height.
  const ObjectId tree = add_prop(h.document, road, "t1", 20.0, std::nullopt);
  h.selection.select({.road = road, .object = tree});
  EXPECT_TRUE(spin->isVisibleTo(&panel));
  EXPECT_DOUBLE_EQ(spin->value(), model_height());

  // A marking has no model to scale — the row is hidden.
  const ObjectId crosswalk = add_crosswalk(h.document, road, "cw", "material.paint_white");
  h.selection.select({.road = road, .object = crosswalk});
  EXPECT_FALSE(spin->isVisibleTo(&panel));
}

TEST(PropertiesPanel, PropHeightRefreshAndEqualTypedValueCommitNothing) {
  Harness h;
  ASSERT_TRUE(h.document.load(kSample).has_value());
  PropertiesPanel panel(h.document, h.selection);
  const RoadId road = all_roads(h.document).front();
  const ObjectId tree = add_prop(h.document, road, "t1", 20.0, 4.0);
  h.selection.select({.road = road, .object = tree});

  auto* spin = panel.findChild<QDoubleSpinBox*>(QStringLiteral("object_height_spin"));
  ASSERT_NE(spin, nullptr);
  const std::string xodr_before = xodr(h.document);
  const int base = h.document.undo_stack()->count();

  // Re-seeding the spin box must not echo a command back, and typing the value
  // it already holds is not an edit.
  emit spin->editingFinished();
  EXPECT_EQ(h.document.undo_stack()->count(), base);
  spin->setValue(4.0);
  emit spin->editingFinished();
  EXPECT_EQ(h.document.undo_stack()->count(), base);
  EXPECT_EQ(xodr(h.document), xodr_before);
}

TEST(PropertiesPanel, SurfaceSpanRowsAppearForAJunctionWithAFloor) {
  CornerScene scene;
  PropertiesPanel panel(scene.document, scene.selection);
  auto* group = panel.findChild<QGroupBox*>(QStringLiteral("surface_spans_group"));
  ASSERT_NE(group, nullptr);
  EXPECT_TRUE(group->isHidden()) << "nothing selected — nothing to show";

  scene.selection.select({.junction = scene.junction});
  EXPECT_FALSE(group->isHidden());
  const std::size_t expected =
      junction_surface_spans(scene.document.network(), scene.junction).size();
  ASSERT_GT(expected, 0U);
  EXPECT_EQ(span_rows(panel).size(), expected) << "one row per connecting road";

  // A road selection is not a junction selection: the section goes away.
  scene.selection.select({.road = scene.document.network().find_road("1")});
  EXPECT_TRUE(group->isHidden());
}

TEST(PropertiesPanel, SurfaceSpanSamplesCheckboxPushesOneCommandAndNeverEchoes) {
  CornerScene scene;
  PropertiesPanel panel(scene.document, scene.selection);
  scene.selection.select({.junction = scene.junction});
  const std::vector<QWidget*> rows = span_rows(panel);
  ASSERT_FALSE(rows.empty());
  auto* samples = rows.front()->findChild<QCheckBox*>(QStringLiteral("surface_span_samples_check"));
  ASSERT_NE(samples, nullptr);
  EXPECT_TRUE(samples->isChecked());

  const int base = scene.document.undo_stack()->index();
  samples->setChecked(false);
  EXPECT_EQ(scene.document.undo_stack()->index(), base + 1);
  ASSERT_EQ(scene.document.network().junction(scene.junction)->surface_spans.size(), 1U);
  EXPECT_FALSE(scene.document.network().junction(scene.junction)->surface_spans.front().included);

  // Re-seeding from the record must not push a second (no-op) command — the
  // kernel refuses those, so an echo would show up as a rejected command.
  scene.selection.select({.junction = scene.junction});
  EXPECT_EQ(scene.document.undo_stack()->index(), base + 1);
}

TEST(PropertiesPanel, SurfaceSpanRaiseAndLowerMoveTheSortIndexByOne) {
  CornerScene scene;
  PropertiesPanel panel(scene.document, scene.selection);
  scene.selection.select({.junction = scene.junction});
  ASSERT_FALSE(span_rows(panel).empty());

  const auto sort_text = [&panel] {
    return span_rows(panel)
        .front()
        ->findChild<QLabel*>(QStringLiteral("surface_span_sort_label"))
        ->text();
  };
  EXPECT_EQ(sort_text(), QStringLiteral("0"));

  const int base = scene.document.undo_stack()->index();
  span_rows(panel)
      .front()
      ->findChild<QToolButton*>(QStringLiteral("surface_span_raise_button"))
      ->click();
  EXPECT_EQ(scene.document.undo_stack()->index(), base + 1);
  EXPECT_EQ(sort_text(), QStringLiteral("1")) << "the row label follows the authored value";

  span_rows(panel)
      .front()
      ->findChild<QToolButton*>(QStringLiteral("surface_span_lower_button"))
      ->click();
  EXPECT_EQ(scene.document.undo_stack()->index(), base + 2);
  EXPECT_EQ(sort_text(), QStringLiteral("0"));
  // Back at its default, the record is erased rather than kept.
  EXPECT_TRUE(scene.document.network().junction(scene.junction)->surface_spans.empty());
}

} // namespace roadmaker::editor
