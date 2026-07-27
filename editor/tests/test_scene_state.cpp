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

// ADR-0008's Layer-2 compatibility contract, at the Document level (fmt-s1,
// #325). The four bullets it pins:
//
//   1. open a pure .xodr  → full editing, no sidecar required;
//   2. save               → the sidecar is written atomically beside the file;
//   3. the ASAM layer     → the .xodr bytes are identical with and without a
//                           sidecar (there is no separate "export ASAM" path —
//                           Document::save IS the export, so Layer-0 purity is
//                           the testable form of that bullet);
//   4. missing or stale   → defaults, never a blocked load and never lost
//                           scene content.
//
// Plus the two paths where the sidecar could DESTROY state rather than just
// lose it: a provider that drops retained keys, and crash recovery re-pointing
// a document at a file whose sidecar it never read.

#include "roadmaker/edit/operations.hpp"
#include "roadmaker/xodr/writer.hpp"

#include <gtest/gtest.h>

#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <fstream>
#include <sstream>

#include "document/document.hpp"
#include "document/scene_sidecar.hpp"

namespace roadmaker::editor {
namespace {

const std::filesystem::path kSample = std::filesystem::path(RM_SAMPLES_DIR) / "t_junction.xodr";

std::unique_ptr<edit::Command> make_road(double y, const char* name) {
  return edit::create_road({Waypoint{.x = 0.0, .y = y}, Waypoint{.x = 100.0, .y = y}},
                           LaneProfile::two_lane_default(),
                           name);
}

std::string file_bytes(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  std::ostringstream out;
  out << in.rdbuf();
  return std::move(out).str();
}

void write_text(const std::filesystem::path& path, const char* text) {
  std::ofstream out(path);
  out << text;
}

SceneViewState sample_view() {
  return SceneViewState{
      .target = {12.5F, -3.25F, 1.5F},
      .yaw = 0.8F,
      .pitch = 0.9F,
      .distance = 80.0F,
      .projection = ProjectionMode::Orthographic,
  };
}

/// The shape of what MainWindow installs: stamps the live camera and render
/// mode onto the state Document seeds it with, touching nothing else.
std::function<void(SceneState&)> provider(SceneViewState view, bool textured) {
  return [view, textured](SceneState& state) {
    state.view = view;
    state.textured = textured;
  };
}

// ---------------------------------------------------------------------------
// Bullet 1 — a pure .xodr needs no sidecar
// ---------------------------------------------------------------------------

TEST(DocumentSceneState, LoadWithoutASidecarLeavesDefaultsAndStillEdits) {
  Document document;
  ASSERT_TRUE(document.load(kSample).has_value());
  EXPECT_FALSE(document.scene_state().view.has_value());
  EXPECT_FALSE(document.scene_state().textured.has_value());
  EXPECT_GT(document.network().road_count(), 0U);
  // Full editing, not just viewing.
  EXPECT_TRUE(document.push_command(make_road(50.0, "Added")).has_value());
}

TEST(DocumentSceneState, AMalformedSidecarNeverBlocksTheLoad) {
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  const std::filesystem::path scene =
      std::filesystem::path(dir.path().toStdString()) / "scene.xodr";
  std::filesystem::copy_file(kSample, scene);
  write_text(scene_sidecar::path_for(scene), "{ this is not json");

  Document document;
  ASSERT_TRUE(document.load(scene).has_value()) << "Layer 2 must never gate Layer 0";
  EXPECT_FALSE(document.scene_state().view.has_value());
  EXPECT_GT(document.network().road_count(), 0U);
}

TEST(DocumentSceneState, ASidecarWithoutAViewLoadsTheRestOfIt) {
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  const std::filesystem::path scene =
      std::filesystem::path(dir.path().toStdString()) / "scene.xodr";
  std::filesystem::copy_file(kSample, scene);
  write_text(scene_sidecar::path_for(scene), R"({"scene_version": 1, "textured": false})");

  Document document;
  ASSERT_TRUE(document.load(scene).has_value());
  EXPECT_FALSE(document.scene_state().view.has_value());
  ASSERT_TRUE(document.scene_state().textured.has_value());
  EXPECT_FALSE(*document.scene_state().textured);
}

// ---------------------------------------------------------------------------
// Bullets 2 + 3 — the sidecar is written beside the scene, and the .xodr is
// untouched by it
// ---------------------------------------------------------------------------

TEST(DocumentSceneState, SaveWritesTheSidecarNextToTheXodr) {
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  const std::filesystem::path scene = std::filesystem::path(dir.path().toStdString()) / "town.xodr";

  Document document;
  document.reset();
  document.set_scene_state_provider(provider(sample_view(), true));
  ASSERT_TRUE(document.push_command(make_road(0.0, "First")).has_value());
  ASSERT_TRUE(document.save(scene).has_value());

  const auto sidecar = scene_sidecar::path_for(scene);
  EXPECT_EQ(sidecar.filename().string(), "town.rmscene.json");
  ASSERT_TRUE(std::filesystem::exists(sidecar));
  const auto state = scene_sidecar::load(sidecar);
  ASSERT_TRUE(state.has_value()) << state.error().message;
  ASSERT_TRUE(state->view.has_value());
  EXPECT_EQ(state->view->yaw, sample_view().yaw);
  EXPECT_EQ(state->view->projection, ProjectionMode::Orthographic);
  ASSERT_TRUE(state->textured.has_value());
  EXPECT_TRUE(*state->textured);
}

TEST(DocumentSceneState, TheXodrBytesAreIdenticalWithAndWithoutASidecar) {
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  const auto root = std::filesystem::path(dir.path().toStdString());

  // Same authored network, saved twice: once with no view state at all, once
  // with a full one. Layer 2 must not leak a single byte into Layer 0.
  Document bare;
  bare.reset();
  ASSERT_TRUE(bare.push_command(make_road(0.0, "First")).has_value());
  ASSERT_TRUE(bare.save(root / "scene.xodr").has_value());

  Document rich;
  rich.reset();
  rich.set_scene_state_provider(provider(sample_view(), true));
  ASSERT_TRUE(rich.push_command(make_road(0.0, "First")).has_value());
  ASSERT_TRUE(rich.save(root / "scene.xodr").has_value());

  EXPECT_EQ(file_bytes(root / "scene.xodr"), *roadmaker::write_xodr(rich.network(), "scene"));
}

TEST(DocumentSceneState, ASidecarBesideAnUnsavedSceneDoesNotSurviveReset) {
  Document document;
  ASSERT_TRUE(document.load(kSample).has_value());
  document.set_scene_state_provider(provider(sample_view(), true));
  document.reset();
  EXPECT_FALSE(document.scene_state().view.has_value());
  EXPECT_FALSE(document.scene_state().textured.has_value());
}

// ---------------------------------------------------------------------------
// Round trip — the feature itself
// ---------------------------------------------------------------------------

TEST(DocumentSceneState, ProviderStatePersistsAndRestoresOnReload) {
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  const std::filesystem::path scene = std::filesystem::path(dir.path().toStdString()) / "town.xodr";

  Document document;
  document.reset();
  document.set_scene_state_provider(provider(sample_view(), false));
  ASSERT_TRUE(document.push_command(make_road(0.0, "First")).has_value());
  ASSERT_TRUE(document.save(scene).has_value());

  Document reloaded;
  ASSERT_TRUE(reloaded.load(scene).has_value());
  ASSERT_TRUE(reloaded.scene_state().view.has_value());
  const SceneViewState& view = *reloaded.scene_state().view;
  EXPECT_EQ(view.target, sample_view().target);
  EXPECT_EQ(view.yaw, sample_view().yaw);
  EXPECT_EQ(view.pitch, sample_view().pitch);
  EXPECT_EQ(view.distance, sample_view().distance);
  EXPECT_EQ(view.projection, sample_view().projection);
  ASSERT_TRUE(reloaded.scene_state().textured.has_value());
  EXPECT_FALSE(*reloaded.scene_state().textured);
}

TEST(DocumentSceneState, SaveWithoutAProviderPreservesTheLoadedState) {
  // Headless callers (the soak driver, a script) install no provider. Their
  // saves must carry the sidecar through unchanged rather than blanking it.
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  const std::filesystem::path scene = std::filesystem::path(dir.path().toStdString()) / "town.xodr";
  std::filesystem::copy_file(kSample, scene);
  write_text(scene_sidecar::path_for(scene), R"({
    "scene_version": 1,
    "view": {"target": [1, 2, 3], "yaw": 0.4, "pitch": 0.5, "distance": 60,
             "projection": "perspective"},
    "textured": true
  })");

  Document document;
  ASSERT_TRUE(document.load(scene).has_value());
  ASSERT_TRUE(document.save(scene).has_value());

  const auto state = scene_sidecar::load(scene_sidecar::path_for(scene));
  ASSERT_TRUE(state.has_value());
  ASSERT_TRUE(state->view.has_value());
  EXPECT_EQ(state->view->distance, 60.0F);
  ASSERT_TRUE(state->textured.has_value());
  EXPECT_TRUE(*state->textured);
}

TEST(DocumentSceneState, TheProviderDoesNotDropRetainedUnknownKeys) {
  // The regression this guards: a provider that RETURNED a fresh SceneState
  // would arrive with an empty `raw`, and the first save in the GUI would
  // destroy everything a newer build had written. It takes the state by
  // reference, seeded from disk, precisely so it cannot.
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  const std::filesystem::path scene = std::filesystem::path(dir.path().toStdString()) / "town.xodr";
  std::filesystem::copy_file(kSample, scene);
  write_text(scene_sidecar::path_for(scene),
             R"({"scene_version": 99, "snap": {"grid": 0.5}, "textured": false})");

  Document document;
  ASSERT_TRUE(document.load(scene).has_value());
  document.set_scene_state_provider(provider(sample_view(), true));
  ASSERT_TRUE(document.save(scene).has_value());

  QFile file(QString::fromStdString(scene_sidecar::path_for(scene).string()));
  ASSERT_TRUE(file.open(QIODevice::ReadOnly));
  const QJsonObject root = QJsonDocument::fromJson(file.readAll()).object();
  EXPECT_EQ(root.value(QStringLiteral("scene_version")).toInt(), 99);
  EXPECT_EQ(root.value(QStringLiteral("snap")).toObject().value(QStringLiteral("grid")).toDouble(),
            0.5);
  EXPECT_TRUE(root.value(QStringLiteral("textured")).toBool());
}

TEST(DocumentSceneState, SceneStateLoadedFiresLastOnLoadAndOnReset) {
  Document document;
  // Ordering, not just counts: the viewport ARMS its post-load auto-framing on
  // loaded() and DISARMS it on scene_state_loaded(). Emit them the other way
  // round and every restored camera is silently re-framed away.
  std::vector<QString> order;
  QObject::connect(&document, &Document::loaded, &document, [&order] {
    order.emplace_back(QStringLiteral("loaded"));
  });
  QObject::connect(&document, &Document::mesh_changed, &document, [&order] {
    order.emplace_back(QStringLiteral("mesh_changed"));
  });
  QObject::connect(&document, &Document::scene_state_loaded, &document, [&order] {
    order.emplace_back(QStringLiteral("scene_state_loaded"));
  });

  ASSERT_TRUE(document.load(kSample).has_value());
  ASSERT_FALSE(order.empty());
  EXPECT_EQ(order.front(), QStringLiteral("loaded"));
  EXPECT_EQ(order.back(), QStringLiteral("scene_state_loaded"));

  order.clear();
  document.reset();
  ASSERT_FALSE(order.empty());
  EXPECT_EQ(order.back(), QStringLiteral("scene_state_loaded"))
      << "File → New must re-publish the (empty) state";
}

TEST(DocumentSceneState, AnUnwritableSidecarDoesNotFailTheSave) {
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  const std::filesystem::path scene = std::filesystem::path(dir.path().toStdString()) / "town.xodr";
  // A directory where the sidecar belongs: QSaveFile cannot open it.
  ASSERT_TRUE(std::filesystem::create_directory(scene_sidecar::path_for(scene)));

  Document document;
  document.reset();
  document.set_scene_state_provider(provider(sample_view(), true));
  ASSERT_TRUE(document.push_command(make_road(0.0, "First")).has_value());
  EXPECT_TRUE(document.save(scene).has_value()) << "comfort state must never cost a save";
  EXPECT_TRUE(std::filesystem::exists(scene));
}

// ---------------------------------------------------------------------------
// Crash recovery — the path where an absent sidecar could destroy a real one
// ---------------------------------------------------------------------------

TEST(DocumentSceneState, MarkRecoveredReSeatsTheStateFromTheOriginalScene) {
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  const auto root = std::filesystem::path(dir.path().toStdString());
  const std::filesystem::path original = root / "town.xodr";
  const std::filesystem::path copy = root / "session.xodr";
  std::filesystem::copy_file(kSample, original);
  std::filesystem::copy_file(kSample, copy);
  // The user's real scene has a sidecar; the recovery copy (an older autosave)
  // does not.
  write_text(scene_sidecar::path_for(original),
             R"({"scene_version": 1, "snap": {"grid": 2.0},
                 "view": {"target": [7, 8, 9], "yaw": 0.4, "pitch": 0.5,
                          "distance": 60, "projection": "perspective"}})");

  Document document;
  ASSERT_TRUE(document.load(copy).has_value());
  EXPECT_FALSE(document.scene_state().view.has_value());
  document.mark_recovered(QString::fromStdString(original.string()));

  // Without the re-seat, the next Save would overwrite town.rmscene.json with
  // an empty document — losing the camera AND the retained `snap` block.
  ASSERT_TRUE(document.scene_state().view.has_value());
  EXPECT_EQ(document.scene_state().view->distance, 60.0F);
  ASSERT_TRUE(document.save(original).has_value());

  const auto state = scene_sidecar::load(scene_sidecar::path_for(original));
  ASSERT_TRUE(state.has_value());
  ASSERT_TRUE(state->view.has_value());
  EXPECT_EQ(state->view->distance, 60.0F);
  EXPECT_EQ(
      state->raw.value(QStringLiteral("snap")).toObject().value(QStringLiteral("grid")).toDouble(),
      2.0);
}

TEST(DocumentSceneState, TheRecoveryCopysOwnSidecarWinsOverTheOriginals) {
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  const auto root = std::filesystem::path(dir.path().toStdString());
  const std::filesystem::path original = root / "town.xodr";
  const std::filesystem::path copy = root / "session.xodr";
  std::filesystem::copy_file(kSample, original);
  std::filesystem::copy_file(kSample, copy);
  write_text(scene_sidecar::path_for(original),
             R"({"scene_version": 1, "view": {"target": [0, 0, 0], "yaw": 0.1, "pitch": 0.2,
                 "distance": 10, "projection": "perspective"}})");
  write_text(scene_sidecar::path_for(copy),
             R"({"scene_version": 1, "view": {"target": [0, 0, 0], "yaw": 0.1, "pitch": 0.2,
                 "distance": 999, "projection": "perspective"}})");

  Document document;
  ASSERT_TRUE(document.load(copy).has_value());
  document.mark_recovered(QString::fromStdString(original.string()));
  ASSERT_TRUE(document.scene_state().view.has_value());
  EXPECT_EQ(document.scene_state().view->distance, 999.0F) << "the autosaved state is the newest";
}

TEST(DocumentSceneState, MarkRecoveredOnANeverSavedDocumentKeepsDefaults) {
  Document document;
  ASSERT_TRUE(document.load(kSample).has_value());
  document.mark_recovered(QString());
  EXPECT_FALSE(document.scene_state().view.has_value());
}

} // namespace
} // namespace roadmaker::editor
