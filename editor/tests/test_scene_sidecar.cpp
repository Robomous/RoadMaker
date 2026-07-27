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

// The per-scene Layer-2 sidecar (fmt-s1, #325; ADR-0008), at the byte level:
// what parse() rejects outright versus what it degrades, the all-or-nothing
// `view` rule, the forward-compat retention that makes a rewrite non-lossy,
// and the float policy that makes save → load → save byte-identical. The
// Document-level compatibility contract lives in test_scene_state.cpp.

#include <gtest/gtest.h>

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <cmath>
#include <limits>

#include "document/scene_sidecar.hpp"

namespace roadmaker::editor {
namespace {

std::filesystem::path fs_path(const QString& path) {
  return std::filesystem::path(path.toStdString());
}

QByteArray file_bytes(const std::filesystem::path& path) {
  QFile file(QString::fromStdString(path.string()));
  EXPECT_TRUE(file.open(QIODevice::ReadOnly)) << path.string();
  return file.readAll();
}

/// A fully populated, plausible camera — deliberately not round numbers, so a
/// rounding bug in the float policy shows up as an inequality.
SceneViewState sample_view() {
  return SceneViewState{
      .target = {12.5F, -3.25F, 1.5F},
      .yaw = 0.8F,
      .pitch = 0.9F,
      .distance = 80.0F,
      .projection = ProjectionMode::Perspective,
  };
}

QByteArray sample_json(const QString& extra_root = QString()) {
  QString json = QStringLiteral(R"({
    "scene_version": 1,
    "view": {"target": [12.5, -3.25, 1.5], "yaw": 0.8, "pitch": 0.9,
             "distance": 80, "projection": "perspective"},
    "textured": true%1
  })");
  return json.arg(extra_root).toUtf8();
}

TEST(SceneSidecar, ParsesViewAndRenderMode) {
  const auto state = scene_sidecar::parse(sample_json());
  ASSERT_TRUE(state.has_value()) << state.error().message;
  EXPECT_EQ(state->version, 1);
  ASSERT_TRUE(state->view.has_value());
  EXPECT_EQ(state->view->target[0], 12.5F);
  EXPECT_EQ(state->view->target[1], -3.25F);
  EXPECT_EQ(state->view->target[2], 1.5F);
  EXPECT_EQ(state->view->yaw, 0.8F);
  EXPECT_EQ(state->view->pitch, 0.9F);
  EXPECT_EQ(state->view->distance, 80.0F);
  EXPECT_EQ(state->view->projection, ProjectionMode::Perspective);
  ASSERT_TRUE(state->textured.has_value());
  EXPECT_TRUE(*state->textured);
}

TEST(SceneSidecar, MalformedJsonIsAnError) {
  const auto state = scene_sidecar::parse(QByteArrayLiteral("{ not json"));
  ASSERT_FALSE(state.has_value());
  EXPECT_EQ(state.error().code, ErrorCode::InvalidArgument);
}

TEST(SceneSidecar, NonObjectRootIsAnError) {
  const auto state = scene_sidecar::parse(QByteArrayLiteral("[1, 2, 3]"));
  ASSERT_FALSE(state.has_value());
  EXPECT_EQ(state.error().code, ErrorCode::InvalidArgument);
}

TEST(SceneSidecar, MissingIntegerVersionIsAnError) {
  EXPECT_FALSE(scene_sidecar::parse(QByteArrayLiteral(R"({"textured": true})")).has_value());
  EXPECT_FALSE(scene_sidecar::parse(QByteArrayLiteral(R"({"scene_version": "1"})")).has_value());
}

TEST(SceneSidecar, NewerVersionParsesBestEffortAndReEmitsItsOwnVersion) {
  const auto state = scene_sidecar::parse(
      QByteArrayLiteral(R"({"scene_version": 99, "textured": false, "wormhole": {"depth": 7}})"));
  ASSERT_TRUE(state.has_value()) << state.error().message;
  EXPECT_EQ(state->version, 99);
  ASSERT_TRUE(state->textured.has_value());
  EXPECT_FALSE(*state->textured);

  // Never silently downgraded: a file from a newer build keeps announcing
  // itself, so that build still recognizes its own schema after we rewrite.
  const auto rewritten = scene_sidecar::parse(scene_sidecar::to_json(*state));
  ASSERT_TRUE(rewritten.has_value());
  EXPECT_EQ(rewritten->version, 99);
}

TEST(SceneSidecar, UnknownRootKeysSurviveARewrite) {
  // The reserved-but-unimplemented keys of ADR-0008's Layer 2 (snapping, the
  // session block) must not be destroyed by a build that predates them.
  const auto state = scene_sidecar::parse(
      sample_json(QStringLiteral(R"(, "snap": {"grid": 0.5}, "session": {"tool": "select"})")));
  ASSERT_TRUE(state.has_value()) << state.error().message;

  const QJsonObject root = QJsonDocument::fromJson(scene_sidecar::to_json(*state)).object();
  EXPECT_EQ(root.value(QStringLiteral("snap")).toObject().value(QStringLiteral("grid")).toDouble(),
            0.5);
  EXPECT_EQ(
      root.value(QStringLiteral("session")).toObject().value(QStringLiteral("tool")).toString(),
      QStringLiteral("select"));
}

TEST(SceneSidecar, UnknownKeysInsideViewSurviveARewrite) {
  // The view block is MERGED, not rebuilt: a future `view.roll` outlives a
  // save by this build, even though the camera fields around it are rewritten.
  auto state = scene_sidecar::parse(QByteArrayLiteral(R"({
    "scene_version": 1,
    "view": {"target": [0, 0, 0], "yaw": 0.1, "pitch": 0.2, "distance": 30,
             "projection": "orthographic", "roll": 1.25}
  })"));
  ASSERT_TRUE(state.has_value()) << state.error().message;
  ASSERT_TRUE(state->view.has_value());
  state->view->yaw = 0.5F; // a live camera move, i.e. the reason we rewrite

  const QJsonObject view = QJsonDocument::fromJson(scene_sidecar::to_json(*state))
                               .object()
                               .value(QStringLiteral("view"))
                               .toObject();
  EXPECT_EQ(view.value(QStringLiteral("roll")).toDouble(), 1.25);
  EXPECT_EQ(view.value(QStringLiteral("yaw")).toDouble(), 0.5);
  EXPECT_EQ(view.value(QStringLiteral("projection")).toString(), QStringLiteral("orthographic"));
}

TEST(SceneSidecar, PartialOrMalformedViewIsDroppedWholesale) {
  // All-or-nothing on the known grammar: half a camera is worse than none, so
  // every one of these degrades to "no stored view" while the rest of the
  // document (here `textured`) still parses.
  const QList<QByteArray> broken{
      QByteArrayLiteral(R"({"scene_version": 1, "textured": true, "view": 7})"),
      QByteArrayLiteral(R"({"scene_version": 1, "textured": true,
        "view": {"yaw": 0.1, "pitch": 0.2, "distance": 30, "projection": "perspective"}})"),
      QByteArrayLiteral(R"({"scene_version": 1, "textured": true,
        "view": {"target": [0, 0], "yaw": 0.1, "pitch": 0.2, "distance": 30,
                 "projection": "perspective"}})"),
      QByteArrayLiteral(R"({"scene_version": 1, "textured": true,
        "view": {"target": [0, 0, "x"], "yaw": 0.1, "pitch": 0.2, "distance": 30,
                 "projection": "perspective"}})"),
      QByteArrayLiteral(R"({"scene_version": 1, "textured": true,
        "view": {"target": [0, 0, 0], "pitch": 0.2, "distance": 30,
                 "projection": "perspective"}})"),
      QByteArrayLiteral(R"({"scene_version": 1, "textured": true,
        "view": {"target": [0, 0, 0], "yaw": 0.1, "pitch": 0.2, "distance": null,
                 "projection": "perspective"}})"),
      QByteArrayLiteral(R"({"scene_version": 1, "textured": true,
        "view": {"target": [0, 0, 0], "yaw": 0.1, "pitch": 0.2, "distance": 30,
                 "projection": "isometric"}})"),
  };
  for (const QByteArray& json : broken) {
    const auto state = scene_sidecar::parse(json);
    ASSERT_TRUE(state.has_value()) << json.toStdString();
    EXPECT_FALSE(state->view.has_value()) << json.toStdString();
    ASSERT_TRUE(state->textured.has_value()) << json.toStdString();
    EXPECT_TRUE(*state->textured) << json.toStdString();
  }
}

TEST(SceneSidecar, ANonBooleanTexturedFallsBackToTheAppDefault) {
  const auto state =
      scene_sidecar::parse(QByteArrayLiteral(R"({"scene_version": 1, "textured": "yes"})"));
  ASSERT_TRUE(state.has_value());
  EXPECT_FALSE(state->textured.has_value());
}

TEST(SceneSidecar, AbsentKeysStayAbsentAndAreNotDefaulted) {
  // nullopt must survive the write as an ABSENT key, not as `false`/an
  // origin camera: absent means "use the app default", which is a different
  // instruction from "this scene wants Sober".
  const auto state = scene_sidecar::parse(QByteArrayLiteral(R"({"scene_version": 1})"));
  ASSERT_TRUE(state.has_value());
  EXPECT_FALSE(state->view.has_value());
  EXPECT_FALSE(state->textured.has_value());

  const QJsonObject root = QJsonDocument::fromJson(scene_sidecar::to_json(*state)).object();
  EXPECT_FALSE(root.contains(QStringLiteral("view")));
  EXPECT_FALSE(root.contains(QStringLiteral("textured")));
}

TEST(SceneSidecar, ClearingAFieldRemovesItsKey) {
  auto state = scene_sidecar::parse(sample_json());
  ASSERT_TRUE(state.has_value());
  state->view.reset();
  state->textured.reset();

  const QJsonObject root = QJsonDocument::fromJson(scene_sidecar::to_json(*state)).object();
  EXPECT_FALSE(root.contains(QStringLiteral("view")));
  EXPECT_FALSE(root.contains(QStringLiteral("textured")));
}

TEST(SceneSidecar, NonFiniteStateOmitsTheViewOnWrite) {
  // Qt writes NaN/±Inf as `null`, which the reader refuses — so the writer must
  // refuse it too, or a save would produce a file it cannot read back.
  SceneState state;
  state.view = sample_view();
  state.view->distance = std::numeric_limits<float>::infinity();
  const QByteArray json = scene_sidecar::to_json(state);
  EXPECT_FALSE(json.contains("null"));
  EXPECT_FALSE(QJsonDocument::fromJson(json).object().contains(QStringLiteral("view")));

  const auto reparsed = scene_sidecar::parse(json);
  ASSERT_TRUE(reparsed.has_value());
  EXPECT_FALSE(reparsed->view.has_value());
}

TEST(SceneSidecar, FloatRoundTripIsExactAndByteIdentical) {
  SceneState state;
  state.view = sample_view();
  // Values a real orbit produces: not representable in few digits, so a
  // decimal-places rounding scheme would shift them on the first write.
  state.view->yaw = 0.123456789F;
  state.view->pitch = 1.5607963F;
  state.view->distance = 12345.6789F;
  state.view->target = {-1234.5678F, 0.000123456F, 3.3333333F};
  state.textured = false;

  const QByteArray first = scene_sidecar::to_json(state);
  const auto reloaded = scene_sidecar::parse(first);
  ASSERT_TRUE(reloaded.has_value()) << reloaded.error().message;
  ASSERT_TRUE(reloaded->view.has_value());
  // Exact, not approximate: 9 significant digits round-trips a float bit for bit.
  EXPECT_EQ(reloaded->view->yaw, state.view->yaw);
  EXPECT_EQ(reloaded->view->pitch, state.view->pitch);
  EXPECT_EQ(reloaded->view->distance, state.view->distance);
  EXPECT_EQ(reloaded->view->target, state.view->target);
  // And the first write is already a fixed point, so save → load → save does
  // not churn the file.
  EXPECT_EQ(scene_sidecar::to_json(*reloaded), first);
}

TEST(SceneSidecar, ShortValuesPrintShort) {
  // The point of the 9-significant-digit snap: a diffable file. Writing the
  // widened float straight through would print 0.800000011920929 here — which
  // a plain contains("\"yaw\": 0.8") would happily accept, since that IS a
  // prefix of it. So assert the absence of the long form as well, and cap the
  // digit run for every number in the document.
  SceneState state;
  state.view = sample_view();
  const QByteArray json = scene_sidecar::to_json(state);
  EXPECT_TRUE(json.contains("\"yaw\": 0.8")) << json.toStdString();
  EXPECT_FALSE(json.contains("0.80000")) << "widened float leaked: " << json.toStdString();

  int run = 0;
  for (const char character : json) {
    run = (character >= '0' && character <= '9') ? run + 1 : 0;
    ASSERT_LE(run, 9) << "a number carries more digits than the float policy allows: "
                      << json.toStdString();
  }
}

TEST(SceneSidecar, ToJsonIsStableUnderKeyInsertionOrder) {
  QJsonObject a;
  a.insert(QStringLiteral("scene_version"), 1);
  a.insert(QStringLiteral("zebra"), 1);
  a.insert(QStringLiteral("alpha"), 2);
  QJsonObject b;
  b.insert(QStringLiteral("alpha"), 2);
  b.insert(QStringLiteral("zebra"), 1);
  b.insert(QStringLiteral("scene_version"), 1);

  const auto first = scene_sidecar::parse(QJsonDocument(a).toJson());
  const auto second = scene_sidecar::parse(QJsonDocument(b).toJson());
  ASSERT_TRUE(first.has_value());
  ASSERT_TRUE(second.has_value());
  EXPECT_EQ(scene_sidecar::to_json(*first), scene_sidecar::to_json(*second));
}

TEST(SceneSidecar, PathForAppendsTheSuffixToTheStem) {
  EXPECT_EQ(scene_sidecar::path_for("/tmp/town/main.xodr"),
            std::filesystem::path("/tmp/town/main.rmscene.json"));
  // A dotted stem keeps everything before the LAST dot.
  EXPECT_EQ(scene_sidecar::path_for("/tmp/town/main.v2.xodr"),
            std::filesystem::path("/tmp/town/main.v2.rmscene.json"));
  // A bare relative name must not gain a leading separator.
  EXPECT_EQ(scene_sidecar::path_for("scene.xodr"), std::filesystem::path("scene.rmscene.json"));
}

TEST(SceneSidecar, SaveWritesAtomicallyAndLoadReadsItBack) {
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  const auto path = scene_sidecar::path_for(fs_path(QDir(dir.path()).filePath("scene.xodr")));

  SceneState state;
  state.view = sample_view();
  state.textured = true;
  ASSERT_TRUE(scene_sidecar::save(path, state).has_value());
  EXPECT_TRUE(std::filesystem::exists(path));
  // QSaveFile leaves no temporary behind on success.
  EXPECT_EQ(QDir(dir.path()).entryList(QDir::NoDotAndDotDot | QDir::AllEntries),
            QStringList({QStringLiteral("scene.rmscene.json")}));

  const auto loaded = scene_sidecar::load(path);
  ASSERT_TRUE(loaded.has_value()) << loaded.error().message;
  ASSERT_TRUE(loaded->view.has_value());
  EXPECT_EQ(loaded->view->target, state.view->target);
  EXPECT_EQ(file_bytes(path), scene_sidecar::to_json(state));
}

TEST(SceneSidecar, LoadOfAMissingFileIsFileNotFound) {
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  // Distinct from a PARSE failure on purpose: every plain .xodr open hits this
  // path, so Document must be able to stay quiet about it.
  const auto loaded =
      scene_sidecar::load(fs_path(QDir(dir.path()).filePath("absent.rmscene.json")));
  ASSERT_FALSE(loaded.has_value());
  EXPECT_EQ(loaded.error().code, ErrorCode::FileNotFound);
}

TEST(SceneSidecar, SaveIntoAnUnwritablePathFails) {
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  // A directory where the file should be: QSaveFile cannot open it, and the
  // caller (Document::save) must survive that.
  const auto path = fs_path(QDir(dir.path()).filePath("scene.rmscene.json"));
  ASSERT_TRUE(std::filesystem::create_directory(path));
  const auto saved = scene_sidecar::save(path, SceneState{});
  ASSERT_FALSE(saved.has_value());
  EXPECT_EQ(saved.error().code, ErrorCode::IoFailure);
}

} // namespace
} // namespace roadmaker::editor
