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

// The export-preview tools (p7-s1, #241) — GW-2 steps 21 and 22.

#include "roadmaker/edit/operations.hpp"
#include "roadmaker/road/authoring.hpp"
#include "roadmaker/xodr/writer.hpp"

#include <gtest/gtest.h>

#include <QAbstractItemModelTester>
#include <QLabel>
#include <QPlainTextEdit>
#include <QSignalSpy>
#include <QTabWidget>
#include <QTableView>
#include <filesystem>

#include "document/document.hpp"
#include "document/export_preview_models.hpp"
#include "document/export_preview_state.hpp"
#include "document/units.hpp"
#include "panels/export_preview_window.hpp"

namespace roadmaker::editor {
namespace {

/// The bytes a save would write — the byte-identity oracle used throughout
/// this repo.
std::string xodr_of(const Document& document) {
  auto written = write_xodr(document.network(), "probe");
  return written ? *written : std::string{"<refused>"};
}

struct Scene {
  Document document;

  Scene() {
    const auto path = std::filesystem::path(RM_SAMPLES_DIR) / "t_junction.xodr";
    EXPECT_TRUE(document.load(path).has_value());
  }
};

} // namespace

// ------------------------------------------------------------- the models

TEST(ExportPreviewModels, EmptyModelsSatisfyTheModelTester) {
  ExportChannelModel channels;
  QAbstractItemModelTester channel_tester(&channels,
                                          QAbstractItemModelTester::FailureReportingMode::Fatal);
  ExportMaterialModel materials;
  QAbstractItemModelTester material_tester(&materials,
                                           QAbstractItemModelTester::FailureReportingMode::Fatal);
  XodrRecordModel records;
  QAbstractItemModelTester record_tester(&records,
                                         QAbstractItemModelTester::FailureReportingMode::Fatal);

  EXPECT_EQ(channels.rowCount(), 0);
  EXPECT_EQ(materials.rowCount(), 0);
  EXPECT_EQ(records.rowCount(), 0);
}

TEST(ExportPreviewModels, PopulatedModelsSatisfyTheModelTester) {
  Scene scene;
  ExportPreviewState state;
  recompute_export_preview(scene.document, state);

  ExportChannelModel channels;
  QAbstractItemModelTester channel_tester(&channels,
                                          QAbstractItemModelTester::FailureReportingMode::Fatal);
  ExportMaterialModel materials;
  QAbstractItemModelTester material_tester(&materials,
                                           QAbstractItemModelTester::FailureReportingMode::Fatal);
  XodrRecordModel records;
  QAbstractItemModelTester record_tester(&records,
                                         QAbstractItemModelTester::FailureReportingMode::Fatal);

  channels.set_preview(&state.gltf);
  materials.set_preview(&state.gltf);
  records.set_preview(&state.xodr);

  EXPECT_EQ(channels.rowCount(), static_cast<int>(kMeshChannelCount));
  EXPECT_GT(materials.rowCount(), 0);
  // Clearing must also be model-tester-clean.
  channels.set_preview(nullptr);
  EXPECT_EQ(channels.rowCount(), 0);
}

TEST(ExportPreviewModels, CountsAreGroupedNotFormattedAsLengths) {
  const QString rendered = format_count(1234567);
  // A count is not a length: it must never pick up " m" / " ft".
  EXPECT_FALSE(rendered.contains(QStringLiteral("m")));
  EXPECT_FALSE(rendered.contains(QStringLiteral("ft")));
  EXPECT_TRUE(rendered.contains(QStringLiteral("1")));
}

// -------------------------------------------------------------- the state

TEST(ExportPreviewState, OpeningThePreviewNeverDirtiesTheDocument) {
  Scene scene;
  const std::string before = xodr_of(scene.document);
  const bool dirty_before = scene.document.is_dirty();
  const int undo_before = scene.document.undo_stack()->count();

  ExportPreviewState state;
  recompute_export_preview(scene.document, state);

  EXPECT_EQ(scene.document.is_dirty(), dirty_before);
  EXPECT_EQ(scene.document.undo_stack()->count(), undo_before);
  // The strongest form: what a save would write is byte-identical.
  EXPECT_EQ(xodr_of(scene.document), before);
  EXPECT_TRUE(state.computed);
}

TEST(ExportPreviewState, TheXmlIsWhatTheWriterWouldProduce) {
  Scene scene;
  ExportPreviewState state;
  recompute_export_preview(scene.document, state);

  ASSERT_TRUE(state.xodr.would_write);
  const auto written = write_xodr(scene.document.network(), "t_junction");
  ASSERT_TRUE(written.has_value());
  EXPECT_EQ(state.xodr.xml, *written);
}

TEST(ExportPreviewState, BothMeshFormatsArePreviewedRegardlessOfTheBuild) {
  Scene scene;
  ExportPreviewState state;
  recompute_export_preview(scene.document, state);

  EXPECT_EQ(state.gltf.format, MeshExportFormat::Gltf);
  EXPECT_EQ(state.usd.format, MeshExportFormat::Usd);
  // The USD manifest is computed even where .usda cannot be written — the
  // build's capability is reported, not used to hide information.
  EXPECT_GT(state.usd.total_triangles, 0U);
#ifndef RM_HAVE_USD
  EXPECT_FALSE(state.usd.available);
#endif
}

// --------------------------------------------------------- the diagnostics

TEST(ExportPreviewState, RefreshingDiagnosticsPublishesTheValidatorsViewWithoutSaving) {
  Scene scene;
  QSignalSpy spy(&scene.document, &Document::diagnostics_changed);

  const std::string before = xodr_of(scene.document);
  scene.document.refresh_diagnostics();

  EXPECT_EQ(spy.count(), 1);
  // Before #241 the only way to get here was to save the file.
  EXPECT_FALSE(scene.document.is_dirty());
  EXPECT_EQ(xodr_of(scene.document), before);
  // The published list is the validator's, not the reader's.
  EXPECT_EQ(scene.document.diagnostics().size(), validate_network(scene.document.network()).size());
}

// ------------------------------------------------------------- the window

TEST(ExportPreviewWindow, ShowingAPageComputesItAndSelectsIt) {
  Scene scene;
  ExportPreviewWindow window(scene.document);

  EXPECT_FALSE(window.state().computed);
  window.show_page(ExportPreviewWindow::Page::OpenDrive);
  EXPECT_TRUE(window.state().computed);
  EXPECT_EQ(window.tabs()->currentIndex(), 1);
  EXPECT_FALSE(window.xml_view()->toPlainText().isEmpty());

  window.show_page(ExportPreviewWindow::Page::Scene);
  EXPECT_EQ(window.tabs()->currentIndex(), 0);
  EXPECT_GT(window.channel_model().rowCount(), 0);
}

TEST(ExportPreviewWindow, GoesStaleOnEveryChannelThatChangesTheExport) {
  // Three separate DirtySet channels reach the manifest. A window wired only
  // to mesh_changed would silently under-report the third.
  struct Case {
    const char* what;
    bool geometry;
  };

  {
    Scene scene;
    ExportPreviewWindow window(scene.document);
    window.refresh();
    ASSERT_FALSE(window.state().stale);
    emit scene.document.mesh_changed({});
    EXPECT_TRUE(window.state().stale) << "a geometry edit must invalidate the manifest";
  }
  {
    Scene scene;
    ExportPreviewWindow window(scene.document);
    window.refresh();
    ASSERT_FALSE(window.state().stale);
    emit scene.document.topology_changed();
    EXPECT_TRUE(window.state().stale) << "adding or removing a road must invalidate the manifest";
  }
  {
    Scene scene;
    ExportPreviewWindow window(scene.document);
    window.refresh();
    ASSERT_FALSE(window.state().stale);
    // A prop placement moves no road, so it arrives on its own channel.
    emit scene.document.objects_changed({});
    EXPECT_TRUE(window.state().stale) << "a prop edit must invalidate the manifest";
  }
}

TEST(ExportPreviewWindow, TheUsdPageIsPresentAndHonestWithoutAUsdBuild) {
  Scene scene;
  ExportPreviewWindow window(scene.document);
  window.refresh();
  window.set_scene_format(MeshExportFormat::Usd);

  // The page is never #ifdef'd away: a manifest for a format this build cannot
  // write is still information, and hiding it would leave the code untested on
  // every developer machine.
  EXPECT_GT(window.channel_model().rowCount(), 0);
#ifdef RM_HAVE_USD
  EXPECT_FALSE(window.availability_note()->isVisibleTo(&window));
#else
  EXPECT_TRUE(window.availability_note()->isVisibleTo(&window));
  EXPECT_FALSE(window.availability_note()->text().isEmpty());
#endif
}

TEST(ExportPreviewWindow, LengthsReRenderWhenTheUnitSystemFlips) {
  const units::UnitSystem restore = units::active();
  units::set_active(units::UnitSystem::Metric);

  Scene scene;
  ExportPreviewWindow window(scene.document);
  window.refresh();
  const QString metric = window.scene_summary()->text();
  ASSERT_FALSE(metric.isEmpty());

  units::set_active(units::UnitSystem::Imperial);
  const QString imperial = window.scene_summary()->text();
  EXPECT_NE(metric, imperial) << "the summary caches formatted lengths and must re-render";
  EXPECT_TRUE(imperial.contains(QStringLiteral("ft")));

  units::set_active(restore);
}

TEST(ExportPreviewWindow, AGroundlessExportIsReportedRatherThanHidden) {
  Scene scene;
  // Give the scene a height field, which no exporter writes (#390).
  ASSERT_TRUE(scene.document.push_command(edit::create_terrain_field(scene.document.network()))
                  .has_value());

  ExportPreviewWindow window(scene.document);
  window.refresh();

  const auto& terrain =
      window.state().gltf.channels[static_cast<std::size_t>(MeshChannel::Terrain)];
  ASSERT_GT(terrain.elements, 0U) << "no terrain in the scene — the test is vacuous";
  EXPECT_EQ(terrain.reason, OmissionReason::ChannelNotWalked);
  EXPECT_TRUE(window.scene_summary()->text().contains(QStringLiteral("#390")));
}

} // namespace roadmaker::editor
