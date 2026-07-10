// Offscreen smoke tests: panels instantiate, mirror the models, and route
// selection bidirectionally. Rendering itself is not asserted (no GL in the
// offscreen platform) — the ViewportWidget is deliberately absent here.

#include <gtest/gtest.h>

#include <QTemporaryDir>
#include <fstream>

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

RoadId first_road(const Document& document) {
  RoadId first;
  document.network().for_each_road([&](RoadId id, const Road&) {
    if (!first.is_valid()) {
      first = id;
    }
  });
  return first;
}

TEST(SceneTreePanel, ViewClickDrivesSelectionModel) {
  Harness h;
  ASSERT_TRUE(h.document.load(kSample).has_value());
  SceneTreePanel panel(h.scene_tree_model, h.selection);

  const RoadId road = first_road(h.document);
  const QModelIndex road_index = h.scene_tree_model.index_for_road(road);
  ASSERT_TRUE(road_index.isValid());

  panel.view()->setCurrentIndex(road_index);
  EXPECT_EQ(h.selection.road(), road);
  EXPECT_FALSE(h.selection.lane().is_valid());
}

TEST(SceneTreePanel, SelectionModelDrivesViewCurrentIndex) {
  Harness h;
  ASSERT_TRUE(h.document.load(kSample).has_value());
  SceneTreePanel panel(h.scene_tree_model, h.selection);

  const RoadId road = first_road(h.document);
  h.selection.select_road(road);
  EXPECT_EQ(panel.view()->currentIndex(), h.scene_tree_model.index_for_road(road));

  h.selection.clear();
  EXPECT_FALSE(panel.view()->currentIndex().isValid());
}

TEST(PropertiesPanel, ConstructsAndFollowsSelection) {
  Harness h;
  ASSERT_TRUE(h.document.load(kSample).has_value());
  PropertiesPanel panel(h.document, h.selection);
  // No crash on select/clear cycles; content assertions live in the models.
  h.selection.select_road(first_road(h.document));
  h.selection.clear();
  ASSERT_TRUE(h.document.load(kSample).has_value()); // reload with panel alive
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
  EXPECT_EQ(h.selection.road(), lane_diag.road);
  EXPECT_EQ(h.selection.lane(), lane_diag.lane);

  // Rows without an attached entity leave the selection untouched.
  emit panel.view()->doubleClicked(h.diagnostics_model.index(no_entity_row, 0));
  EXPECT_EQ(h.selection.road(), lane_diag.road);
  EXPECT_EQ(h.selection.lane(), lane_diag.lane);
}

} // namespace
} // namespace roadmaker::editor
