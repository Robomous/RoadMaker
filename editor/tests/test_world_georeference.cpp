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

// The editor half of the world georeference (p7-s5, #324): the Layer-2
// workspace block in the scene sidecar, the mismatch rule that discards a
// workspace framed in a frame the scene no longer has, and the tool window
// that authors both halves.
//
// The two halves live in DIFFERENT persistence layers on purpose, and the
// tests keep that distinction visible: the georeference is Layer 0 and goes
// through the undo stack, the workspace is Layer 2 and does not.

#include <gtest/gtest.h>

#include <QDoubleSpinBox>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPlainTextEdit>
#include <QRadioButton>
#include <QFile>
#include <QTemporaryDir>

#include "app/shortcut_registry.hpp"
#include "document/document.hpp"
#include "document/scene_sidecar.hpp"
#include "document/selection_model.hpp"
#include "panels/world_georeference_window.hpp"

#include "roadmaker/edit/operations.hpp"
#include "roadmaker/road/authoring.hpp"
#include "roadmaker/road/road.hpp"
#include "roadmaker/road/georeference.hpp"
#include "roadmaker/xodr/rules.hpp"
#include "roadmaker/xodr/writer.hpp"

namespace roadmaker::editor {
namespace {

std::filesystem::path fs_path(const QString& path) {
  return std::filesystem::path(path.toStdString());
}

/// A scene with one road, so the network has plan bounds to frame against.
/// Documents mutate only through the command layer, so this goes through it.
void seed(Document& document) {
  EXPECT_TRUE(document
                  .push_command(edit::create_road(
                      {Waypoint{.x = 0.0, .y = 0.0}, Waypoint{.x = 100.0, .y = 0.0}},
                      LaneProfile::two_lane_default(),
                      "main"))
                  .has_value());
}

QByteArray sidecar_with_workspace(const QString& crs) {
  QJsonObject workspace;
  workspace.insert("extents", QJsonArray{-10.0, -20.0, 250.0, 60.0});
  workspace.insert("crs", crs);
  QJsonObject root;
  root.insert("scene_version", 1);
  root.insert("workspace", workspace);
  return QJsonDocument(root).toJson();
}

bool has_rule(const std::vector<Diagnostic>& diagnostics, std::string_view rule) {
  return std::any_of(diagnostics.begin(), diagnostics.end(), [&](const Diagnostic& d) {
    return d.rule_id == rule;
  });
}

} // namespace

// --- the Layer-2 workspace block --------------------------------------------

TEST(SceneSidecarWorkspace, RoundTripsAtFullDoublePrecision) {
  // Metres in the kernel frame, at UTM scale. Through the float path the
  // camera uses, a northing like this would lose about a decimetre — which is
  // why the workspace has its own double-width formatter.
  const QByteArray json = sidecar_with_workspace("");
  auto state = scene_sidecar::parse(json);
  ASSERT_TRUE(state.has_value()) << state.error().message;
  ASSERT_TRUE(state->workspace.has_value());

  state->workspace->extents = {448000.125, 5411000.0625, 448500.5, 5411600.75};
  const QByteArray written = scene_sidecar::to_json(*state);
  const auto reparsed = scene_sidecar::parse(written);
  ASSERT_TRUE(reparsed.has_value());
  ASSERT_TRUE(reparsed->workspace.has_value());
  for (std::size_t index = 0; index < 4; ++index) {
    EXPECT_EQ(reparsed->workspace->extents[index], state->workspace->extents[index]);
  }
  // And the emitted text is a fixed point, which is what makes save → load →
  // save byte-identical.
  EXPECT_EQ(scene_sidecar::to_json(*reparsed), written);
}

TEST(SceneSidecarWorkspace, AMalformedBlockIsDroppedWhole) {
  // The house all-or-nothing rule: half a workspace is not a workspace.
  const auto missing_component = scene_sidecar::parse(
      R"({"scene_version": 1, "workspace": {"extents": [1, 2, 3], "crs": ""}})");
  ASSERT_TRUE(missing_component.has_value());
  EXPECT_FALSE(missing_component->workspace.has_value());

  const auto inverted = scene_sidecar::parse(
      R"({"scene_version": 1, "workspace": {"extents": [10, 0, -10, 20], "crs": ""}})");
  ASSERT_TRUE(inverted.has_value());
  EXPECT_FALSE(inverted->workspace.has_value());

  const auto bad_crs = scene_sidecar::parse(
      R"({"scene_version": 1, "workspace": {"extents": [0, 0, 1, 1], "crs": 7}})");
  ASSERT_TRUE(bad_crs.has_value());
  EXPECT_FALSE(bad_crs->workspace.has_value());
}

TEST(SceneSidecarWorkspace, UnknownKeysInsideTheBlockSurviveARewrite) {
  // Same forward-compat guarantee the `view` block carries: a future
  // `workspace.grid` must not be destroyed by this build saving.
  const auto state = scene_sidecar::parse(
      R"({"scene_version": 1,
          "workspace": {"extents": [0, 0, 100, 50], "crs": "+proj=tmerc", "grid": 25}})");
  ASSERT_TRUE(state.has_value());
  ASSERT_TRUE(state->workspace.has_value());

  const QByteArray written = scene_sidecar::to_json(*state);
  const QJsonObject root = QJsonDocument::fromJson(written).object();
  EXPECT_EQ(root.value("workspace").toObject().value("grid").toInt(), 25);
}

TEST(SceneSidecarWorkspace, AnAbsentWorkspaceWritesNoKey) {
  auto state = scene_sidecar::parse(R"({"scene_version": 1})");
  ASSERT_TRUE(state.has_value());
  EXPECT_FALSE(state->workspace.has_value());
  const QJsonObject root = QJsonDocument::fromJson(scene_sidecar::to_json(*state)).object();
  EXPECT_FALSE(root.contains("workspace"));
}

// --- the mismatch rule ------------------------------------------------------

TEST(WorkspaceMismatch, AWorkspaceFramedInAnotherGeoreferenceIsDiscarded) {
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());

  // Save a georeferenced scene, then overwrite its sidecar with a workspace
  // that claims a DIFFERENT frame — which is what happens when someone
  // re-georeferences a scene, or a sidecar outlives the file beside it.
  Document document;
  seed(document);
  const auto proj = tmerc_projection(48.858844, 2.294351);
  ASSERT_TRUE(proj.has_value());
  ASSERT_TRUE(document.push_command(edit::set_georeference(document.network(),
                                                           GeoReference{.projection = *proj}))
                  .has_value());
  const std::filesystem::path scene = fs_path(dir.filePath("geo.xodr"));
  ASSERT_TRUE(document.save(scene).has_value());

  QFile sidecar(QString::fromStdString(scene_sidecar::path_for(scene).string()));
  ASSERT_TRUE(sidecar.open(QIODevice::WriteOnly | QIODevice::Truncate));
  sidecar.write(sidecar_with_workspace("+proj=utm +zone=31 +datum=WGS84"));
  sidecar.close();

  Document reopened;
  ASSERT_TRUE(reopened.load(scene).has_value());
  EXPECT_FALSE(reopened.scene_state().workspace.has_value());
  EXPECT_TRUE(has_rule(reopened.diagnostics(), rules::kGeoReferenceMismatch));
}

TEST(WorkspaceMismatch, AWorkspaceFramedInTheSameGeoreferenceIsKept) {
  // The control. Without it, a rule that discarded EVERY workspace would pass
  // the test above and never be noticed.
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());

  Document document;
  seed(document);
  const auto proj = tmerc_projection(48.858844, 2.294351);
  ASSERT_TRUE(proj.has_value());
  ASSERT_TRUE(document.push_command(edit::set_georeference(document.network(),
                                                           GeoReference{.projection = *proj}))
                  .has_value());
  const std::filesystem::path scene = fs_path(dir.filePath("geo.xodr"));
  ASSERT_TRUE(document.save(scene).has_value());

  QFile sidecar(QString::fromStdString(scene_sidecar::path_for(scene).string()));
  ASSERT_TRUE(sidecar.open(QIODevice::WriteOnly | QIODevice::Truncate));
  sidecar.write(sidecar_with_workspace(QString::fromStdString(*proj)));
  sidecar.close();

  Document reopened;
  ASSERT_TRUE(reopened.load(scene).has_value());
  ASSERT_TRUE(reopened.scene_state().workspace.has_value());
  EXPECT_EQ(reopened.scene_state().workspace->extents[2], 250.0);
  EXPECT_FALSE(has_rule(reopened.diagnostics(), rules::kGeoReferenceMismatch));
}

TEST(WorkspaceMismatch, AnUngeoreferencedSceneKeepsAnUngeoreferencedWorkspace) {
  // The empty CRS is a frame like any other — a plain local Cartesian one —
  // so it must match, not be treated as "unknown" and discarded.
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());

  Document document;
  seed(document);
  const std::filesystem::path scene = fs_path(dir.filePath("plain.xodr"));
  ASSERT_TRUE(document.save(scene).has_value());

  QFile sidecar(QString::fromStdString(scene_sidecar::path_for(scene).string()));
  ASSERT_TRUE(sidecar.open(QIODevice::WriteOnly | QIODevice::Truncate));
  sidecar.write(sidecar_with_workspace(""));
  sidecar.close();

  Document reopened;
  ASSERT_TRUE(reopened.load(scene).has_value());
  EXPECT_TRUE(reopened.scene_state().workspace.has_value());
}

// --- the tool window --------------------------------------------------------

TEST(WorldGeoreferenceWindow, AnOriginBecomesATransverseMercatorAndCommitsOnce) {
  Document document;
  SelectionModel selection(document);
  seed(document);
  WorldGeoreferenceWindow window(document, selection);

  window.origin_mode()->setChecked(true);
  window.latitude()->setValue(48.858844);
  window.longitude()->setValue(2.294351);
  const int before = document.undo_stack()->count();
  EXPECT_TRUE(window.apply());

  // ONE undo entry for the whole form, not one per control.
  EXPECT_EQ(document.undo_stack()->count(), before + 1);
  const auto origin = tmerc_origin(document.network().georeference().projection);
  ASSERT_TRUE(origin.has_value());
  EXPECT_EQ((*origin)[0], 48.858844);
  EXPECT_EQ((*origin)[1], 2.294351);
}

TEST(WorldGeoreferenceWindow, ACustomCrsIsStoredVerbatim) {
  Document document;
  SelectionModel selection(document);
  seed(document);
  WorldGeoreferenceWindow window(document, selection);

  window.custom_mode()->setChecked(true);
  window.projection_text()->setPlainText("  +proj=utm +zone=31 +datum=WGS84 +units=m  ");
  EXPECT_TRUE(window.apply());
  // Trimmed, but otherwise untouched — this build does not reformat a
  // projection it cannot read.
  EXPECT_EQ(document.network().georeference().projection,
            "+proj=utm +zone=31 +datum=WGS84 +units=m");
}

TEST(WorldGeoreferenceWindow, ReopeningShowsAnAuthoredOriginAsAnOrigin) {
  Document document;
  SelectionModel selection(document);
  seed(document);
  const auto proj = tmerc_projection(-33.8688, 151.2093);
  ASSERT_TRUE(proj.has_value());
  ASSERT_TRUE(document.push_command(edit::set_georeference(document.network(),
                                                           GeoReference{.projection = *proj}))
                  .has_value());

  WorldGeoreferenceWindow window(document, selection);
  EXPECT_TRUE(window.origin_mode()->isChecked());
  EXPECT_EQ(window.latitude()->value(), -33.8688);
  EXPECT_EQ(window.longitude()->value(), 151.2093);
}

TEST(WorldGeoreferenceWindow, ReopeningShowsAForeignCrsAsCustom) {
  Document document;
  SelectionModel selection(document);
  seed(document);
  ASSERT_TRUE(document
                  .push_command(edit::set_georeference(
                      document.network(), GeoReference{.projection = "+proj=utm +zone=31"}))
                  .has_value());

  WorldGeoreferenceWindow window(document, selection);
  EXPECT_TRUE(window.custom_mode()->isChecked());
  EXPECT_EQ(window.projection_text()->toPlainText().toStdString(), "+proj=utm +zone=31");
}

TEST(WorldGeoreferenceWindow, AnAllZeroOffsetFormMeansNoOffset) {
  // The form and the writer have to agree about what "no offset" is, or
  // clearing the spins would leave a value the writer then declines to emit
  // and the two would disagree forever.
  Document document;
  SelectionModel selection(document);
  seed(document);
  WorldGeoreferenceWindow window(document, selection);

  window.latitude()->setValue(10.0);
  window.offset_x()->setValue(0.0);
  window.offset_y()->setValue(0.0);
  window.offset_z()->setValue(0.0);
  window.offset_hdg()->setValue(0.0);
  EXPECT_TRUE(window.apply());
  EXPECT_FALSE(document.network().georeference().offset.has_value());

  window.offset_x()->setValue(1000.0);
  EXPECT_TRUE(window.apply());
  ASSERT_TRUE(document.network().georeference().offset.has_value());
  EXPECT_EQ(document.network().georeference().offset->x, 1000.0);
}

TEST(WorldGeoreferenceWindow, FittingTheWorkspaceRecordsTheFrameItWasFramedIn) {
  Document document;
  SelectionModel selection(document);
  seed(document);
  const auto proj = tmerc_projection(1.0, 2.0);
  ASSERT_TRUE(proj.has_value());
  ASSERT_TRUE(document.push_command(edit::set_georeference(document.network(),
                                                           GeoReference{.projection = *proj}))
                  .has_value());

  WorldGeoreferenceWindow window(document, selection);
  const int before = document.undo_stack()->count();
  window.fit_workspace_to_selection();

  ASSERT_TRUE(document.scene_state().workspace.has_value());
  // The stamped CRS is what makes the mismatch rule possible at all — without
  // it a later re-georeferencing leaves these numbers describing somewhere
  // else, silently.
  EXPECT_EQ(document.scene_state().workspace->crs, *proj);
  EXPECT_GT(document.scene_state().workspace->extents[2], 100.0);
  // Layer 2 is not undoable: framing is not an edit.
  EXPECT_EQ(document.undo_stack()->count(), before);
}

TEST(WorldGeoreferenceWindow, ClearingRemovesTheGeoreferenceFromTheFile) {
  Document document;
  SelectionModel selection(document);
  seed(document);
  WorldGeoreferenceWindow window(document, selection);
  window.latitude()->setValue(10.0);
  ASSERT_TRUE(window.apply());
  ASSERT_FALSE(document.network().georeference().empty());

  ASSERT_TRUE(document
                  .push_command(edit::set_georeference(document.network(), GeoReference{}))
                  .has_value());
  const auto xml = write_xodr(document.network(), "geo");
  ASSERT_TRUE(xml.has_value());
  EXPECT_EQ(xml->find("<geoReference"), std::string::npos);
}

// --- the actions ------------------------------------------------------------

TEST(WorldGeoreferenceActions, BothIdsAreInTheRegistryAndUnbound) {
  // Menu-only by design — a scene is georeferenced once, not repeatedly — so
  // neither may claim a key, and both must still appear in the table (the
  // Id→QAction switch has no default:, so a missing row fails the build).
  using shortcuts::Id;
  EXPECT_TRUE(shortcuts::sequences(Id::WorldGeoreference).isEmpty());
  EXPECT_TRUE(shortcuts::sequences(Id::CenterWorldOrigin).isEmpty());
}

} // namespace roadmaker::editor
