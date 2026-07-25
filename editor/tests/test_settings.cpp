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

// Settings as PRODUCTION DATA (#399). A user's whole recent list was destroyed
// by scripted --screenshot runs writing into the real store, and by relative
// paths that stop resolving the moment the working directory changes. The
// existing Welcome/Project suites cannot see either failure: they seed
// ABSOLUTE paths into an ISOLATED scope, which is precisely the shape that
// hides both bugs. These cases attack the two mechanisms directly.

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QListWidget>
#include <QMainWindow>
#include <QMap>
#include <QSettings>
#include <QTemporaryDir>
#include <QVariant>
#include <filesystem>

#include "app/main_window.hpp"
#include "app/settings.hpp"
#include "app/welcome_widget.hpp"
#include "document/units.hpp"

namespace roadmaker::editor {
namespace {

const std::filesystem::path kSample = std::filesystem::path(RM_SAMPLES_DIR) / "crossing.xodr";

/// Restores the process working directory on the way out. cwd is per-PROCESS,
/// and running the test binary directly executes every case in one process, so
/// a case that changes it must put it back itself — TearDown is too late once
/// a sibling has already read it.
class ScopedWorkingDir {
public:
  explicit ScopedWorkingDir(const QString& dir) : previous_(QDir::currentPath()) {
    QDir::setCurrent(dir);
  }

  ~ScopedWorkingDir() { QDir::setCurrent(previous_); }

  ScopedWorkingDir(const ScopedWorkingDir&) = delete;
  ScopedWorkingDir& operator=(const ScopedWorkingDir&) = delete;
  ScopedWorkingDir(ScopedWorkingDir&&) = delete;
  ScopedWorkingDir& operator=(ScopedWorkingDir&&) = delete;

private:
  QString previous_;
};

/// Every key/value in the current scope — the oracle for "nothing was written".
QMap<QString, QVariant> store_snapshot() {
  QSettings raw;
  QMap<QString, QVariant> out;
  for (const QString& key : raw.allKeys()) {
    out.insert(key, raw.value(key));
  }
  return out;
}

/// Copies the sample scene into `dir` under `name` so a test can open it by a
/// RELATIVE path without depending on where the repo sits.
QString stage_scene(const QTemporaryDir& dir, const QString& name) {
  const QString target = dir.filePath(name);
  QFile::remove(target);
  EXPECT_TRUE(QFile::copy(QString::fromStdString(kSample.string()), target));
  return target;
}

class SettingsTest : public ::testing::Test {
protected:
  void SetUp() override {
    // The suite-wide contract: a throwaway org, an application name carrying
    // this test's name (ctest -j runs siblings as concurrent processes of one
    // binary, and a shared settings domain races on clear()/setValue()), and
    // Settings constructed only AFTER the rename — QSettings resolves org/app
    // at construction.
    QCoreApplication::setOrganizationName(QStringLiteral("RobomousTests"));
    const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
    QCoreApplication::setApplicationName(QStringLiteral("RoadMakerSettingsTest_") +
                                         QString::fromUtf8(info->name()));
    QSettings().clear();
    settings_ = std::make_unique<Settings>();
  }

  void TearDown() override {
    settings_.reset();
    QSettings().clear();
    // Back to the suite-wide test scope (qt_gtest_main.cpp), never the shipped
    // Robomous/RoadMaker names.
    QCoreApplication::setOrganizationName(QStringLiteral("RobomousTests"));
    QCoreApplication::setApplicationName(QStringLiteral("RoadMakerEditorTests"));
    ASSERT_FALSE(Settings::read_only()) << "a leaked write guard would turn later cases green";
  }

  std::unique_ptr<Settings> settings_;
};

// ---------------------------------------------------------------------------
// Paths are stored absolute
// ---------------------------------------------------------------------------

TEST_F(SettingsTest, RelativePathIsRecordedAbsolute) {
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  ASSERT_FALSE(stage_scene(dir, QStringLiteral("scene.xodr")).isEmpty());

  {
    const ScopedWorkingDir cwd(dir.path());
    settings_->add_recent_file(QStringLiteral("scene.xodr")); // as a CLI would give it
  }

  // Read from a DIFFERENT working directory: this is the Finder-launch case
  // that used to leave the welcome screen empty.
  ASSERT_EQ(settings_->recent_files().size(), 1);
  const QString stored = settings_->recent_files().front();
  EXPECT_TRUE(QFileInfo(stored).isAbsolute()) << stored.toStdString();
  EXPECT_TRUE(QFileInfo::exists(stored)) << stored.toStdString();
  EXPECT_EQ(QFileInfo(stored).fileName(), QStringLiteral("scene.xodr"));
  // Not EQ against dir.filePath(): on macOS QTemporaryDir hands out /var/…
  // while getcwd() reports the /private/var/… it is symlinked from.
  EXPECT_EQ(QFileInfo(stored).canonicalFilePath(),
            QFileInfo(dir.filePath(QStringLiteral("scene.xodr"))).canonicalFilePath());
}

TEST_F(SettingsTest, RelativeProjectDirIsRecordedAbsolute) {
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  ASSERT_TRUE(QDir(dir.path()).mkdir(QStringLiteral("harbor")));

  {
    const ScopedWorkingDir cwd(dir.path());
    settings_->add_recent_project(QStringLiteral("harbor"));
  }

  ASSERT_EQ(settings_->recent_projects().size(), 1);
  const QString stored = settings_->recent_projects().front();
  EXPECT_TRUE(QFileInfo(stored).isAbsolute()) << stored.toStdString();
  EXPECT_TRUE(QFileInfo(stored).isDir()) << stored.toStdString();
}

TEST_F(SettingsTest, ARelativeAndAnAbsoluteFormOfOnePathAreOneEntry) {
  // Before #399 these were two distinct strings and burned two of the ten
  // slots — half the reason a burst of runs flushed the list.
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  const QString absolute = stage_scene(dir, QStringLiteral("scene.xodr"));

  {
    const ScopedWorkingDir cwd(dir.path());
    settings_->add_recent_file(QStringLiteral("scene.xodr"));
    settings_->add_recent_file(QStringLiteral("./scene.xodr"));
    settings_->add_recent_file(absolute);
  }
  EXPECT_EQ(settings_->recent_files().size(), 1);
}

TEST_F(SettingsTest, LegacyRelativeEntriesAreHiddenAndPrunedOnTheNextWrite) {
  // What a pre-#399 build left behind: entries relative to a directory nobody
  // can name any more. They must not show, and they must not keep occupying
  // slots forever.
  const QString kept = QDir::rootPath() + QStringLiteral("scenes/kept.xodr");
  {
    QSettings raw;
    raw.setValue(QStringLiteral("files/recent"),
                 QStringList{QStringLiteral("assets/samples/crossing.xodr"),
                             kept,
                             QStringLiteral("../elsewhere/t.xodr")});
  }

  EXPECT_EQ(settings_->recent_files(), QStringList{kept});

  const QString fresh = QDir::rootPath() + QStringLiteral("scenes/fresh.xodr");
  settings_->add_recent_file(fresh);

  const QStringList raw_after = QSettings().value(QStringLiteral("files/recent")).toStringList();
  EXPECT_EQ(raw_after, (QStringList{fresh, kept})) << "the relative leftovers must be gone";
}

// ---------------------------------------------------------------------------
// The capture write guard
// ---------------------------------------------------------------------------

TEST_F(SettingsTest, ReadOnlySessionBlocksEveryWriteAndLeavesReadsIntact) {
  const QString seeded = QDir::rootPath() + QStringLiteral("scenes/seeded.xodr");
  settings_->add_recent_file(seeded);
  settings_->add_recent_project(QDir::rootPath() + QStringLiteral("projects/seeded"));
  settings_->set_theme_name(QStringLiteral("midnight"));
  settings_->set_autosave_enabled(false);
  settings_->set_tour_seen(true);
  settings_->set_textured_rendering(true);
  settings_->set_viewport_hints(false);
  settings_->set_display_units(units::UnitSystem::Imperial);
  QMainWindow window;
  settings_->save_window(window);
  const QMap<QString, QVariant> before = store_snapshot();
  ASSERT_FALSE(before.isEmpty());

  {
    const Settings::ReadOnlySession capture;
    EXPECT_TRUE(Settings::read_only());

    settings_->add_recent_file(QDir::rootPath() + QStringLiteral("scenes/automation.xodr"));
    settings_->add_recent_project(QDir::rootPath() + QStringLiteral("projects/automation"));
    settings_->set_theme_name(QStringLiteral("daylight"));
    settings_->set_autosave_enabled(true);
    settings_->set_tour_seen(false);
    settings_->set_textured_rendering(false);
    settings_->set_viewport_hints(true);
    settings_->set_display_units(units::UnitSystem::Metric);
    window.resize(640, 480);
    settings_->save_window(window);

    EXPECT_EQ(store_snapshot(), before) << "a capture session must not write anything";
    // Reads keep working — a capture renders with the user's theme and units.
    EXPECT_EQ(settings_->recent_files(), QStringList{seeded});
    EXPECT_EQ(settings_->theme_name(), QStringLiteral("midnight"));
    EXPECT_EQ(settings_->display_units(), units::UnitSystem::Imperial);
    EXPECT_FALSE(settings_->autosave_enabled());
  }

  EXPECT_FALSE(Settings::read_only()) << "the scope must restore the previous policy";
  settings_->set_theme_name(QStringLiteral("daylight"));
  EXPECT_EQ(settings_->theme_name(), QStringLiteral("daylight")) << "writes resume afterwards";
}

TEST_F(SettingsTest, ACaptureSessionLoadingASceneLeavesTheRecentListUntouched) {
  // The field repro, minus the subprocess: a full capture window opening a
  // scene the way --screenshot does. Ten of these emptied a real user's list.
  for (int i = 0; i < Settings::kMaxRecentFiles; ++i) {
    settings_->add_recent_file(QDir::rootPath() + QStringLiteral("scenes/genuine%1.xodr").arg(i));
  }
  const QStringList before = settings_->recent_files();
  ASSERT_EQ(before.size(), Settings::kMaxRecentFiles);

  {
    const Settings::ReadOnlySession capture; // what run_screenshot() installs
    MainWindow window(nullptr, /*restore_saved_layout=*/false);
    window.load_file(kSample);
  }

  EXPECT_EQ(settings_->recent_files(), before);
  EXPECT_TRUE(settings_->recent_projects().isEmpty())
      << "auto-associating a project must not write either";
}

TEST_F(SettingsTest, AnInteractiveSessionStillRecordsTheSceneItOpened) {
  // The control for the case above: without it, that one would pass even if
  // load_file had simply stopped recording for everybody.
  MainWindow window(nullptr, /*restore_saved_layout=*/false);
  window.load_file(kSample);

  ASSERT_FALSE(settings_->recent_files().isEmpty());
  EXPECT_EQ(QFileInfo(settings_->recent_files().front()).fileName(),
            QStringLiteral("crossing.xodr"));
}

// ---------------------------------------------------------------------------
// End to end: the issue's second repro
// ---------------------------------------------------------------------------

TEST_F(SettingsTest, ASceneOpenedByRelativePathStillListsAfterACwdChange) {
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  ASSERT_FALSE(stage_scene(dir, QStringLiteral("scene.xodr")).isEmpty());

  {
    const ScopedWorkingDir cwd(dir.path());
    MainWindow window(nullptr, /*restore_saved_layout=*/false);
    window.load_file(std::filesystem::path("scene.xodr")); // as `roadmaker-editor scene.xodr` does
    ASSERT_EQ(settings_->recent_files().size(), 1);
  }

  // Now we are somewhere else, exactly as a Finder/Explorer launch would be.
  WelcomeWidget welcome(*settings_);
  welcome.refresh();
  auto* list = welcome.findChild<QListWidget*>(QStringLiteral("welcomeRecentList"));
  ASSERT_NE(list, nullptr);
  ASSERT_EQ(list->count(), 1) << "the entry used to be filtered out as non-existent";
  EXPECT_EQ(QFileInfo(list->item(0)->data(Qt::UserRole).toString()).fileName(),
            QStringLiteral("scene.xodr"));
}

} // namespace
} // namespace roadmaker::editor
