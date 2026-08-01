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

// Preparing an esmini preview (p8-s5, issue #249, GW-6 step 14).
//
// ★ WHAT IS TESTED IS THE POLICY, NEVER THE LAUNCH. `prepare_preview` is a pure
// function of the document and a directory, so these run without esmini
// installed — which is the whole reason the launch was split out of it. The
// subprocess itself is one QProcess call with nothing to assert that a test
// could reach.
//
// The property that matters most is the LOGIC FILE REWRITE: a preview that
// pointed at the scene's own `.xodr` would resolve the last SAVE rather than
// what is on screen, and would resolve nothing at all for a scene that has
// never been saved.

#include "roadmaker/edit/operations.hpp"
#include "roadmaker/osc/catalog.hpp"
#include "roadmaker/osc/edit.hpp"
#include "roadmaker/osc/reader.hpp"
#include "roadmaker/osc/writer.hpp"
#include "roadmaker/road/network.hpp"
#include "roadmaker/road/road.hpp"
#include "roadmaker/xodr/reader.hpp"

#include <gtest/gtest.h>

#include <QFile>
#include <QTemporaryDir>
#include <filesystem>
#include <string>

#include "document/document.hpp"
#include "document/esmini_preview.hpp"

namespace roadmaker::editor {
namespace {

/// The first road's OpenDRIVE `@id`.
///
/// NOT `find_road("main")`: `create_road`'s third argument is the road's NAME,
/// and `find_road` looks a road up by its `@id`, which the kernel assigns. The
/// two are different strings, and reading the wrong one hands back an invalid
/// handle whose `road()` is a null pointer.
std::string first_road_odr_id(const Document& document) {
  std::string odr_id;
  document.network().for_each_road([&odr_id](RoadId, const Road& road) {
    if (odr_id.empty()) {
      odr_id = road.odr_id;
    }
  });
  return odr_id;
}

/// A straight road with an actor on it — the smallest scene worth previewing.
void author_scene(Document& document) {
  ASSERT_TRUE(document
                  .push_command(edit::create_road(
                      {Waypoint{.x = 0.0, .y = 0.0}, Waypoint{.x = 100.0, .y = 0.0}},
                      LaneProfile::two_lane_default(),
                      "main"))
                  .has_value());

  osc::LanePosition position;
  position.road_id = first_road_odr_id(document);
  ASSERT_FALSE(position.road_id.empty());
  position.lane_id = "-1";
  position.s = 20.0;
  ASSERT_TRUE(document
                  .push_scenario_command(osc::edit::place_scenario_object(
                      document.scenario(), osc::make_actor(osc::ActorKind::Car, "Ego"), position))
                  .has_value());
}

} // namespace

TEST(EsminiPreview, APreviewWritesThePairAndPointsTheScenarioAtIt) {
  Document document;
  author_scene(document);

  QTemporaryDir directory;
  ASSERT_TRUE(directory.isValid());
  const auto preview =
      prepare_preview(document, std::filesystem::path(directory.path().toStdString()));
  ASSERT_TRUE(preview.has_value()) << (preview ? "" : preview.error().message);

  EXPECT_TRUE(std::filesystem::exists(preview->network_path));
  EXPECT_TRUE(std::filesystem::exists(preview->scenario_path));
  EXPECT_EQ(preview->network_path.parent_path(), preview->scenario_path.parent_path())
      << "<LogicFile> is relative — the pair must travel together";

  // ★ THE REWRITE. The exported scenario points at the exported network by
  // FILENAME, not at wherever the scene's own .xodr lives (which, for a scene
  // that was never saved, is nowhere).
  const auto reloaded = osc::load_xosc(preview->scenario_path);
  ASSERT_TRUE(reloaded.has_value());
  ASSERT_TRUE(reloaded->scenario.road_network.logic_file.has_value());
  EXPECT_EQ(reloaded->scenario.road_network.logic_file->filepath,
            preview->network_path.filename().string());

  // And the network really is the one on screen.
  const auto network = roadmaker::load_xodr(preview->network_path);
  ASSERT_TRUE(network.has_value());
  bool saw_road = false;
  network->network.for_each_road([&saw_road](RoadId, const Road&) { saw_road = true; });
  EXPECT_TRUE(saw_road);
}

TEST(EsminiPreview, ThePreviewIsOfWhatIsOnScreenNotOfTheLastSave) {
  Document document;
  author_scene(document);

  QTemporaryDir first;
  const auto before = prepare_preview(document, std::filesystem::path(first.path().toStdString()));
  ASSERT_TRUE(before.has_value());

  // An edit that is NOT saved anywhere.
  ASSERT_TRUE(
      document
          .push_scenario_command(osc::edit::set_entity_init_speed(document.scenario(), "Ego", 22.0))
          .has_value());

  QTemporaryDir second;
  const auto after = prepare_preview(document, std::filesystem::path(second.path().toStdString()));
  ASSERT_TRUE(after.has_value());

  const auto reloaded = osc::load_xosc(after->scenario_path);
  ASSERT_TRUE(reloaded.has_value());
  const auto written = osc::write_xosc(reloaded->scenario);
  ASSERT_TRUE(written.has_value());
  EXPECT_NE(written->find("value=\"22\""), std::string::npos)
      << "the unsaved speed edit did not reach the preview";
}

TEST(EsminiPreview, ASceneWithNoScenarioIsRefusedRatherThanPreviewedEmpty) {
  // An empty `.xosc` loads fine in esmini and shows nothing, which looks like a
  // broken preview rather than an empty one.
  Document document;
  ASSERT_TRUE(document
                  .push_command(edit::create_road(
                      {Waypoint{.x = 0.0, .y = 0.0}, Waypoint{.x = 100.0, .y = 0.0}},
                      LaneProfile::two_lane_default(),
                      "main"))
                  .has_value());

  QTemporaryDir directory;
  const auto preview =
      prepare_preview(document, std::filesystem::path(directory.path().toStdString()));
  EXPECT_FALSE(preview.has_value());
}

TEST(EsminiPreview, TheArgumentsAskForAWindowAndAFixedTimestep) {
  // NOT --headless: this is the user watching their scenario, unlike the CI
  // smoke gate, which is the one place --headless belongs.
  Document document;
  author_scene(document);

  QTemporaryDir directory;
  const auto preview =
      prepare_preview(document, std::filesystem::path(directory.path().toStdString()));
  ASSERT_TRUE(preview.has_value());

  EXPECT_TRUE(preview->arguments.contains(QStringLiteral("--osc")));
  EXPECT_TRUE(preview->arguments.contains(QString::fromStdString(preview->scenario_path.string())));
  EXPECT_TRUE(preview->arguments.contains(QStringLiteral("--fixed_timestep")));
  EXPECT_FALSE(preview->arguments.contains(QStringLiteral("--headless")));
}

TEST(EsminiPreview, ResolutionPrefersTheSettingThenTheEnvironmentThenPath) {
  QTemporaryDir directory;
  ASSERT_TRUE(directory.isValid());

  // A file that exists but is not executable must NOT resolve — otherwise the
  // launch fails later with a message about the process rather than about the
  // path the user chose.
  const QString plain = directory.filePath(QStringLiteral("not-executable"));
  {
    QFile file(plain);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    file.write("x");
  }
  EXPECT_NE(resolve_esmini(plain), plain);

  const QString runnable = directory.filePath(QStringLiteral("fake-esmini"));
  {
    QFile file(runnable);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    file.write("#!/bin/sh\n");
    file.close();
    ASSERT_TRUE(file.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
  }
  EXPECT_EQ(resolve_esmini(runnable), runnable);

  // With nothing configured, the environment wins over PATH.
  qputenv("ESMINI_PATH", runnable.toUtf8());
  EXPECT_EQ(resolve_esmini(QString()), runnable);
  qunsetenv("ESMINI_PATH");
}

TEST(EsminiPreview, LaunchingWithNoBinaryIsAnErrorRatherThanASilentNoOp) {
  Document document;
  author_scene(document);
  QTemporaryDir directory;
  const auto preview =
      prepare_preview(document, std::filesystem::path(directory.path().toStdString()));
  ASSERT_TRUE(preview.has_value());

  const auto started = launch_preview(QString(), *preview);
  ASSERT_FALSE(started.has_value());
  EXPECT_NE(started.error().message.find("esmini"), std::string::npos);
}

} // namespace roadmaker::editor
