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

// The in-app help viewer: resource resolution through the engine, slug
// navigation, and the unavailable-collection path.

#include <gtest/gtest.h>

#include <QFile>
#include <QHelpEngineCore>
#include <QTemporaryDir>
#include <QTextDocument>
#include <QUrl>
#include <QVariant>
#include <filesystem>

#include "help/help_browser.hpp"
#include "help/help_viewer.hpp"

namespace roadmaker::editor {
namespace {

QString stage_collection(QTemporaryDir& dir) {
  const std::filesystem::path stage = RM_HELP_STAGE_DIR;
  const QString dst_qhc = dir.filePath(QStringLiteral("roadmaker.qhc"));
  const QString dst_qch = dir.filePath(QStringLiteral("roadmaker.qch"));
  if (!QFile::copy(QString::fromStdString((stage / "roadmaker.qhc").string()), dst_qhc) ||
      !QFile::copy(QString::fromStdString((stage / "roadmaker.qch").string()), dst_qch)) {
    return {};
  }
  return dst_qhc;
}

TEST(HelpViewer, LoadResourceResolvesQthelpUrl) {
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  const QString collection = stage_collection(dir);
  ASSERT_FALSE(collection.isEmpty());

  QHelpEngineCore engine(collection);
  ASSERT_TRUE(engine.setupData()) << engine.error().toStdString();

  help::HelpBrowser browser(engine);
  const QVariant data =
      browser.resource(static_cast<int>(QTextDocument::HtmlResource),
                       QUrl(QStringLiteral("qthelp://ai.robomous.roadmaker/doc/index.html")));
  ASSERT_TRUE(data.isValid());
  EXPECT_FALSE(data.toByteArray().isEmpty());
}

TEST(HelpViewer, OpenPageNavigatesToSlug) {
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  const QString collection = stage_collection(dir);
  ASSERT_FALSE(collection.isEmpty());

  help::HelpViewer viewer(collection, nullptr);
  ASSERT_TRUE(viewer.available());

  viewer.open_page(QStringLiteral("create-road"));
  EXPECT_EQ(viewer.current_page(), help::HelpViewer::page_url(QStringLiteral("create-road")));
}

TEST(HelpViewer, MissingCollectionReportsUnavailable) {
  help::HelpViewer viewer(QStringLiteral("/no/such/dir/roadmaker.qhc"), nullptr);
  EXPECT_FALSE(viewer.available());
}

} // namespace
} // namespace roadmaker::editor
