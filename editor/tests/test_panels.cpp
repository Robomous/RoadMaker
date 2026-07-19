// Offscreen smoke tests: panels instantiate, mirror the models, and route
// selection bidirectionally. Rendering itself is not asserted (no GL in the
// offscreen platform) — the ViewportWidget is deliberately absent here.

#include "roadmaker/edit/operations.hpp"
#include "roadmaker/road/authoring.hpp"
#include "roadmaker/road/surface_derivation.hpp"
#include "roadmaker/xodr/writer.hpp"

#include <gtest/gtest.h>

#include <QDoubleSpinBox>
#include <QItemSelectionModel>
#include <QLineEdit>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <fstream>
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

} // namespace
} // namespace roadmaker::editor
