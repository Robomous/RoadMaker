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

// WelcomeWidget: logic-free surface, so the tests pin the contract that
// matters — buttons emit, the recent grid drops dead paths, samples resolve
// from the repo, thumbnails have a writable home.

#include <gtest/gtest.h>

#include <QFile>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include "app/settings.hpp"
#include "app/welcome_widget.hpp"
#include "document/project.hpp"

namespace roadmaker::editor {
namespace {

class WelcomeWidgetTest : public ::testing::Test {
protected:
  void SetUp() override {
    // Scope QSettings writes to a throwaway org so the suite never touches
    // the developer's real RoadMaker settings. Settings must be constructed
    // AFTER the rename — QSettings resolves org/app at construction.
    // The app name carries the test name: ctest -j runs each case as its
    // own process of this binary, and concurrent cases sharing one settings
    // domain race on clear()/setValue() through the OS-level store.
    QCoreApplication::setOrganizationName(QStringLiteral("RobomousTests"));
    const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
    QCoreApplication::setApplicationName(QStringLiteral("RoadMakerWelcomeTest_") +
                                         QString::fromUtf8(info->name()));
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

TEST_F(WelcomeWidgetTest, PrimaryButtonsEmitTheirSignals) {
  WelcomeWidget widget(*settings_);
  QSignalSpy new_spy(&widget, &WelcomeWidget::new_scene_requested);
  QSignalSpy open_spy(&widget, &WelcomeWidget::open_requested);

  auto* new_button = widget.findChild<QPushButton*>(QStringLiteral("welcomePrimary"));
  ASSERT_NE(new_button, nullptr);
  new_button->click();
  EXPECT_EQ(new_spy.count(), 1);

  auto* open_button = widget.findChild<QPushButton*>(QStringLiteral("welcomeOpen"));
  ASSERT_NE(open_button, nullptr);
  open_button->click();
  EXPECT_EQ(open_spy.count(), 1);
}

TEST_F(WelcomeWidgetTest, RecentGridListsOnlyExistingFilesAndEmitsPath) {
  QTemporaryDir dir;
  const QString real = dir.filePath(QStringLiteral("scene.xodr"));
  {
    QFile file(real);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    file.write("<OpenDRIVE/>");
  }
  settings_->add_recent_file(dir.filePath(QStringLiteral("deleted.xodr")));
  settings_->add_recent_file(real);

  WelcomeWidget widget(*settings_);
  widget.refresh();
  auto* list = widget.findChild<QListWidget*>(QStringLiteral("welcomeRecentList"));
  ASSERT_NE(list, nullptr);
  ASSERT_EQ(list->count(), 1); // the deleted path is filtered out
  EXPECT_EQ(list->item(0)->data(Qt::UserRole).toString(), real);

  QSignalSpy spy(&widget, &WelcomeWidget::file_requested);
  emit list->itemClicked(list->item(0));
  ASSERT_EQ(spy.count(), 1);
  EXPECT_EQ(spy.first().first().toString(), real);
}

TEST_F(WelcomeWidgetTest, SamplesResolveFromTheRepoCheckout) {
  // find_samples_dir walks up from the binary — in the build tree that must
  // land on <repo>/assets/samples (RM_SAMPLES_DIR pins the expectation).
  const std::filesystem::path dir = find_samples_dir();
  ASSERT_FALSE(dir.empty());
  EXPECT_TRUE(std::filesystem::exists(dir / "crossing.xodr"));

  WelcomeWidget widget(*settings_);
  auto* list = widget.findChild<QListWidget*>(QStringLiteral("welcomeSamplesList"));
  ASSERT_NE(list, nullptr);
  EXPECT_GT(list->count(), 0);
}

TEST_F(WelcomeWidgetTest, RecentProjectTilesShowNameAndSceneCountAndEmitDir) {
  QTemporaryDir dir;
  const auto project =
      Project::create(std::filesystem::path(dir.path().toStdString()), QStringLiteral("Harbor"));
  ASSERT_TRUE(project.has_value());
  for (const char* scene : {"a.xodr", "b.xodr"}) {
    QFile file(dir.filePath(QString::fromUtf8(scene)));
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    file.write("<OpenDRIVE/>");
  }
  settings_->add_recent_project(dir.filePath(QStringLiteral("gone"))); // not a project
  settings_->add_recent_project(dir.path());

  WelcomeWidget widget(*settings_);
  widget.refresh();
  auto* list = widget.findChild<QListWidget*>(QStringLiteral("welcomeProjectsList"));
  ASSERT_NE(list, nullptr);
  ASSERT_EQ(list->count(), 1); // the unopenable directory is filtered out
  EXPECT_TRUE(list->item(0)->text().contains(QStringLiteral("Harbor")));
  EXPECT_TRUE(list->item(0)->text().contains(QStringLiteral("2")));
  EXPECT_EQ(list->item(0)->data(Qt::UserRole).toString(), dir.path());

  QSignalSpy spy(&widget, &WelcomeWidget::project_requested);
  emit list->itemClicked(list->item(0));
  ASSERT_EQ(spy.count(), 1);
  EXPECT_EQ(spy.first().first().toString(), dir.path());
}

TEST_F(WelcomeWidgetTest, ActiveProjectSectionListsScenesAndOffersNewScene) {
  QTemporaryDir dir;
  const auto project =
      Project::create(std::filesystem::path(dir.path().toStdString()), QStringLiteral("Harbor"));
  ASSERT_TRUE(project.has_value());
  {
    QFile file(dir.filePath(QStringLiteral("quay.xodr")));
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    file.write("<OpenDRIVE/>");
  }

  WelcomeWidget widget(*settings_);
  auto* section = widget.findChild<QListWidget*>(QStringLiteral("welcomeProjectScenesList"));
  ASSERT_NE(section, nullptr);
  EXPECT_FALSE(section->parentWidget()->isVisibleTo(&widget)); // hidden until a project opens

  widget.set_active_project(dir.path());
  EXPECT_TRUE(section->parentWidget()->isVisibleTo(&widget));
  ASSERT_EQ(section->count(), 1);
  EXPECT_EQ(section->item(0)->text(), QStringLiteral("quay.xodr"));

  QSignalSpy file_spy(&widget, &WelcomeWidget::file_requested);
  emit section->itemClicked(section->item(0));
  ASSERT_EQ(file_spy.count(), 1);
  EXPECT_TRUE(file_spy.first().first().toString().endsWith(QStringLiteral("quay.xodr")));

  QSignalSpy new_spy(&widget, &WelcomeWidget::new_scene_requested);
  auto* new_button = widget.findChild<QPushButton*>(QStringLiteral("welcomeProjectNewScene"));
  ASSERT_NE(new_button, nullptr);
  new_button->click();
  EXPECT_EQ(new_spy.count(), 1);

  widget.set_active_project(QString()); // project closed — the section hides
  EXPECT_FALSE(section->parentWidget()->isVisibleTo(&widget));
}

TEST_F(WelcomeWidgetTest, ThumbnailPathIsStableAndWritable) {
  const QString path = WelcomeWidget::thumbnail_path_for(QStringLiteral("/tmp/a.xodr"));
  ASSERT_FALSE(path.isEmpty());
  EXPECT_TRUE(path.endsWith(QStringLiteral(".png")));
  EXPECT_EQ(path, WelcomeWidget::thumbnail_path_for(QStringLiteral("/tmp/a.xodr")));
  EXPECT_NE(path, WelcomeWidget::thumbnail_path_for(QStringLiteral("/tmp/b.xodr")));
}

} // namespace
} // namespace roadmaker::editor
