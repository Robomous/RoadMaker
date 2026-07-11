// Offscreen smoke tests: panels instantiate, mirror the models, and route
// selection bidirectionally. Rendering itself is not asserted (no GL in the
// offscreen platform) — the ViewportWidget is deliberately absent here.

#include "roadmaker/xodr/writer.hpp"

#include <gtest/gtest.h>

#include <QItemSelectionModel>
#include <QLineEdit>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "document/diagnostics_model.hpp"
#include "document/document.hpp"
#include "document/scene_tree_model.hpp"
#include "document/selection_model.hpp"
#include "panels/diagnostics_panel.hpp"
#include "panels/properties_panel.hpp"
#include "panels/scene_tree_panel.hpp"

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

} // namespace
} // namespace roadmaker::editor
