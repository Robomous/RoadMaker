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

// Project (p6-s1, #235): a directory with a project.json beside ordinary
// *.xodr scenes. These tests pin the manifest contract (create refuses an
// existing project, forward-compat parses a newer version, bad JSON errors),
// the top-level scene glob, the immediate-directory find_project_for rule,
// and the files/recent_projects settings list.

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QSettings>
#include <QTemporaryDir>

#include "app/settings.hpp"
#include "document/project.hpp"

namespace roadmaker::editor {
namespace {

void write_file(const QString& path, const QByteArray& bytes) {
  QFile file(path);
  ASSERT_TRUE(file.open(QIODevice::WriteOnly)) << path.toStdString();
  file.write(bytes);
}

std::filesystem::path fs_path(const QString& path) {
  return std::filesystem::path(path.toStdString());
}

TEST(Project, CreateWritesManifestAndOpens) {
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  const auto created = Project::create(fs_path(dir.path()), QStringLiteral("Alpha Town"));
  ASSERT_TRUE(created.has_value()) << created.error().message;
  EXPECT_EQ(created->name(), QStringLiteral("Alpha Town"));
  EXPECT_EQ(created->version(), Project::kSupportedVersion);
  EXPECT_TRUE(QFile::exists(QDir(dir.path()).filePath(QStringLiteral("project.json"))));

  const auto opened = Project::open(fs_path(dir.path()));
  ASSERT_TRUE(opened.has_value());
  EXPECT_EQ(opened->name(), QStringLiteral("Alpha Town"));
}

TEST(Project, CreateRefusesADirectoryThatIsAlreadyAProject) {
  QTemporaryDir dir;
  ASSERT_TRUE(Project::create(fs_path(dir.path()), QStringLiteral("First")).has_value());
  const auto second = Project::create(fs_path(dir.path()), QStringLiteral("Second"));
  ASSERT_FALSE(second.has_value()); // never clobber an existing project.json
}

TEST(Project, CreateMakesMissingDirectories) {
  QTemporaryDir dir;
  const auto created =
      Project::create(fs_path(dir.path()) / "nested" / "town", QStringLiteral("Nested"));
  ASSERT_TRUE(created.has_value()) << created.error().message;
  const auto scenes = created->scenes();
  EXPECT_TRUE(scenes.isEmpty());
}

TEST(Project, OpenErrorsOnMissingManifestAndBadJson) {
  QTemporaryDir dir;
  EXPECT_FALSE(Project::open(fs_path(dir.path())).has_value()); // no project.json

  write_file(QDir(dir.path()).filePath(QStringLiteral("project.json")), "{ not json");
  EXPECT_FALSE(Project::open(fs_path(dir.path())).has_value());

  write_file(QDir(dir.path()).filePath(QStringLiteral("project.json")),
             R"({"name": "no version"})");
  EXPECT_FALSE(Project::open(fs_path(dir.path())).has_value()); // version is mandatory

  write_file(QDir(dir.path()).filePath(QStringLiteral("project.json")), R"([1, 2, 3])");
  EXPECT_FALSE(Project::open(fs_path(dir.path())).has_value()); // root must be an object
}

TEST(Project, NewerVersionAndUnknownKeysParseBestEffort) {
  QTemporaryDir dir;
  write_file(QDir(dir.path()).filePath(QStringLiteral("project.json")),
             R"({"project_version": 99, "name": "From The Future", "wormhole": true})");
  const auto project = Project::open(fs_path(dir.path()));
  ASSERT_TRUE(project.has_value()); // forward-compat: a warning, not an error
  EXPECT_EQ(project->version(), 99);
  EXPECT_EQ(project->name(), QStringLiteral("From The Future"));
}

TEST(Project, MissingNameFallsBackToTheDirectoryName) {
  QTemporaryDir dir;
  const std::filesystem::path project_dir = fs_path(dir.path()) / "riverside";
  ASSERT_TRUE(QDir(dir.path()).mkpath(QStringLiteral("riverside")));
  write_file(QString::fromStdString((project_dir / "project.json").string()),
             R"({"project_version": 1})");
  const auto project = Project::open(project_dir);
  ASSERT_TRUE(project.has_value());
  EXPECT_EQ(project->name(), QStringLiteral("riverside"));
}

TEST(Project, ScenesGlobsTopLevelXodrSortedByName) {
  QTemporaryDir dir;
  const auto project = Project::create(fs_path(dir.path()), QStringLiteral("Glob"));
  ASSERT_TRUE(project.has_value());
  QDir root(dir.path());
  write_file(root.filePath(QStringLiteral("beta.xodr")), "<OpenDRIVE/>");
  write_file(root.filePath(QStringLiteral("alpha.xodr")), "<OpenDRIVE/>");
  write_file(root.filePath(QStringLiteral("notes.txt")), "not a scene");
  ASSERT_TRUE(root.mkpath(QStringLiteral("sub")));
  write_file(root.filePath(QStringLiteral("sub/deep.xodr")), "<OpenDRIVE/>"); // non-recursive

  const QStringList scenes = project->scenes();
  ASSERT_EQ(scenes.size(), 2);
  EXPECT_TRUE(scenes[0].endsWith(QStringLiteral("alpha.xodr")));
  EXPECT_TRUE(scenes[1].endsWith(QStringLiteral("beta.xodr")));
  EXPECT_TRUE(QDir::isAbsolutePath(scenes[0]));
}

TEST(Project, LibraryManifestPathIsOptional) {
  QTemporaryDir dir;
  const auto project = Project::create(fs_path(dir.path()), QStringLiteral("Overlay"));
  ASSERT_TRUE(project.has_value());
  EXPECT_FALSE(project->library_manifest_path().has_value());

  ASSERT_TRUE(QDir(dir.path()).mkpath(QStringLiteral("assets/library")));
  write_file(QDir(dir.path()).filePath(QStringLiteral("assets/library/manifest.json")),
             R"({"manifest_version": 1, "items": []})");
  const auto manifest = project->library_manifest_path();
  ASSERT_TRUE(manifest.has_value());
  EXPECT_EQ(manifest->filename().string(), "manifest.json");
}

TEST(Project, FindProjectForChecksTheImmediateDirectoryOnly) {
  QTemporaryDir dir;
  ASSERT_TRUE(Project::create(fs_path(dir.path()), QStringLiteral("Find")).has_value());
  QDir root(dir.path());
  write_file(root.filePath(QStringLiteral("scene.xodr")), "<OpenDRIVE/>");
  ASSERT_TRUE(root.mkpath(QStringLiteral("sub")));
  write_file(root.filePath(QStringLiteral("sub/nested.xodr")), "<OpenDRIVE/>");

  const auto found =
      Project::find_project_for(fs_path(root.filePath(QStringLiteral("scene.xodr"))));
  ASSERT_TRUE(found.has_value());
  const auto opened = Project::open(*found);
  ASSERT_TRUE(opened.has_value());
  EXPECT_EQ(opened->name(), QStringLiteral("Find"));
  // The normalized forms compare equal — the editor's "same project?" check.
  EXPECT_EQ(*found, opened->dir());

  // Deliberately no upward walk: a scene one level down is not a top-level
  // scene of this project.
  EXPECT_FALSE(Project::find_project_for(fs_path(root.filePath(QStringLiteral("sub/nested.xodr"))))
                   .has_value());
}

TEST(Project, FindProjectForOutsideAnyProjectIsEmpty) {
  QTemporaryDir dir;
  EXPECT_FALSE(
      Project::find_project_for(fs_path(QDir(dir.path()).filePath(QStringLiteral("a.xodr"))))
          .has_value());
}

// Settings recents: isolated QSettings scope (the WelcomeWidget suite's
// pattern) so the suite never touches the developer's real RoadMaker settings.
class RecentProjectsSettingsTest : public ::testing::Test {
protected:
  void SetUp() override {
    QCoreApplication::setOrganizationName(QStringLiteral("RobomousTests"));
    QCoreApplication::setApplicationName(QStringLiteral("RoadMakerProjectTest"));
    QSettings().clear();
    settings_ = std::make_unique<Settings>();
  }

  void TearDown() override {
    settings_.reset();
    QSettings().clear();
    QCoreApplication::setOrganizationName(QStringLiteral("Robomous"));
    QCoreApplication::setApplicationName(QStringLiteral("RoadMaker"));
  }

  std::unique_ptr<Settings> settings_;
};

TEST_F(RecentProjectsSettingsTest, MostRecentFirstDedupAndCap) {
  EXPECT_TRUE(settings_->recent_projects().isEmpty());
  settings_->add_recent_project(QStringLiteral("/p/one"));
  settings_->add_recent_project(QStringLiteral("/p/two"));
  settings_->add_recent_project(QStringLiteral("/p/one")); // re-open moves to front
  QStringList recent = settings_->recent_projects();
  ASSERT_EQ(recent.size(), 2);
  EXPECT_EQ(recent[0], QStringLiteral("/p/one"));
  EXPECT_EQ(recent[1], QStringLiteral("/p/two"));

  for (int i = 0; i < Settings::kMaxRecentFiles + 3; ++i) {
    settings_->add_recent_project(QStringLiteral("/p/cap%1").arg(i));
  }
  recent = settings_->recent_projects();
  EXPECT_EQ(recent.size(), Settings::kMaxRecentFiles);
  EXPECT_EQ(recent.front(),
            QStringLiteral("/p/cap%1").arg(Settings::kMaxRecentFiles + 2)); // newest kept
}

TEST_F(RecentProjectsSettingsTest, PersistsAcrossSettingsInstancesAndIsSeparateFromScenes) {
  settings_->add_recent_project(QStringLiteral("/p/persisted"));
  settings_->add_recent_file(QStringLiteral("/s/scene.xodr"));

  const Settings reread; // a fresh wrapper over the same QSettings store
  EXPECT_EQ(reread.recent_projects(), QStringList{QStringLiteral("/p/persisted")});
  EXPECT_EQ(reread.recent_files(), QStringList{QStringLiteral("/s/scene.xodr")});
}

// #333: the viewport-hint toggle is a persisted view setting. On (the pre-#333
// behavior) is the default, so an upgrade never silently removes the hint.
TEST_F(RecentProjectsSettingsTest, ViewportHintsDefaultOnAndRoundTrip) {
  EXPECT_TRUE(settings_->viewport_hints());

  settings_->set_viewport_hints(false);
  EXPECT_FALSE(settings_->viewport_hints());
  EXPECT_FALSE(Settings().viewport_hints()); // survives a fresh wrapper

  settings_->set_viewport_hints(true);
  EXPECT_TRUE(Settings().viewport_hints());
}

} // namespace
} // namespace roadmaker::editor
