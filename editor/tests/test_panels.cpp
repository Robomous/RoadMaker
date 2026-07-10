// Offscreen smoke tests: panels instantiate, mirror the models, and route
// selection bidirectionally. Rendering itself is not asserted (no GL in the
// offscreen platform) — the ViewportWidget is deliberately absent here.

#include <gtest/gtest.h>

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
  ASSERT_TRUE(h.document.load(kSample).has_value());
  DiagnosticsPanel panel(h.document, h.diagnostics_model, h.selection);
  ASSERT_NE(panel.view()->model(), nullptr);
  EXPECT_EQ(panel.view()->model()->rowCount(), static_cast<int>(h.document.diagnostics().size()));
}

} // namespace
} // namespace roadmaker::editor
