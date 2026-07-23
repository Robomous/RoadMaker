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

#include <gtest/gtest.h>

#include <QComboBox>
#include <QIcon>
#include <QLineEdit>
#include <QListView>
#include <QSignalSpy>
#include <filesystem>

#include "document/library_list_model.hpp"
#include "document/library_manifest.hpp"
#include "panels/library_panel.hpp"

namespace roadmaker::editor {
namespace {

LibraryListModel& populated_model() {
  static LibraryListModel model;
  const auto manifest =
      LibraryManifest::load(std::filesystem::path(RM_ASSETS_DIR) / "library" / "manifest.json");
  if (manifest.has_value()) {
    model.set_manifest(*manifest);
  }
  return model;
}

TEST(LibraryPanel, ShowsEveryCatalogueItem) {
  LibraryPanel panel(populated_model());
  ASSERT_NE(panel.view()->model(), nullptr);
  EXPECT_EQ(panel.view()->model()->rowCount(),
            44); // 3 templates + 1 style + T/X + 10 props (5 trees/shrub + 2 streetlights + 3
                 // buildings) + 4 signals + 9 markings + 5 materials + 1 crosswalk + 6 stencils +
                 // 2 prop sets
  // The grid gives every item an icon (the proxy prefers the bundled thumbnail).
  const QModelIndex first = panel.view()->model()->index(0, 0);
  EXPECT_FALSE(panel.view()->model()->data(first, Qt::DecorationRole).isNull());
}

TEST(LibraryPanel, SearchFiltersByLabel) {
  LibraryPanel panel(populated_model());
  auto* search = panel.findChild<QLineEdit*>(QStringLiteral("library_search"));
  ASSERT_NE(search, nullptr);

  search->setText(QStringLiteral("rural"));
  EXPECT_EQ(panel.view()->model()->rowCount(), 1); // only "2-lane rural"

  search->setText(QStringLiteral("intersection"));
  EXPECT_EQ(panel.view()->model()->rowCount(), 2); // T + X

  search->setText(QStringLiteral("tree"));
  EXPECT_EQ(panel.view()->model()->rowCount(),
            7); // pine/oak/birch/poplar + "Mixed trees" prop set +
                // the two "sTREEtlight" props (substring match)

  search->setText(QStringLiteral("Traffic"));
  EXPECT_EQ(panel.view()->model()->rowCount(), 2); // traffic light + traffic sign

  search->clear();
  EXPECT_EQ(panel.view()->model()->rowCount(), 44);
}

TEST(LibraryPanel, CategoryComboFiltersGrid) {
  LibraryPanel panel(populated_model());
  auto* combo = panel.category_combo();
  ASSERT_NE(combo, nullptr);
  // Index 0 is "All categories" (empty userData); one entry per distinct
  // category follows.
  EXPECT_TRUE(combo->itemData(0).toString().isEmpty());
  EXPECT_GT(combo->count(), 1);

  const int props = combo->findData(QStringLiteral("Props"));
  ASSERT_GE(props, 0);
  combo->setCurrentIndex(props);
  const int shown = panel.view()->model()->rowCount();
  EXPECT_GT(shown, 0);
  EXPECT_LT(shown, 44); // a strict subset
  for (int row = 0; row < shown; ++row) {
    const QModelIndex index = panel.view()->model()->index(row, 0);
    EXPECT_EQ(panel.view()->model()->data(index, LibraryListModel::CategoryRole).toString(),
              QStringLiteral("Props"));
  }

  combo->setCurrentIndex(0); // back to All
  EXPECT_EQ(panel.view()->model()->rowCount(), 44);
}

TEST(LibraryPanel, CategoryFilterCombinesWithSearch) {
  LibraryPanel panel(populated_model());
  auto* combo = panel.category_combo();
  auto* search = panel.findChild<QLineEdit*>(QStringLiteral("library_search"));
  ASSERT_NE(combo, nullptr);
  ASSERT_NE(search, nullptr);

  combo->setCurrentIndex(combo->findData(QStringLiteral("Props")));
  search->setText(QStringLiteral("pine")); // a Props item
  EXPECT_EQ(panel.view()->model()->rowCount(), 1);
  search->setText(QStringLiteral("asphalt")); // a Materials item — filtered out by category
  EXPECT_EQ(panel.view()->model()->rowCount(), 0);
}

TEST(LibraryPanel, SelectingAnItemEmitsCurrentChanged) {
  LibraryPanel panel(populated_model());
  QSignalSpy spy(&panel, &LibraryPanel::asset_current_changed);
  ASSERT_TRUE(spy.isValid());

  const QModelIndex first = panel.view()->model()->index(0, 0);
  panel.view()->setCurrentIndex(first);
  ASSERT_EQ(spy.count(), 1);
  EXPECT_FALSE(spy.at(0).at(0).toString().isEmpty());
}

// An engaged Attributes-pane slot asks the Library to show its category
// (P1/GW-3): the first item of that category becomes current.
TEST(LibraryPanel, FocusCategorySelectsThatCategorysFirstItem) {
  LibraryPanel panel(populated_model());
  panel.focus_category(QStringLiteral("Props"));

  const QModelIndex current = panel.view()->currentIndex();
  ASSERT_TRUE(current.isValid());
  EXPECT_EQ(panel.view()->model()->data(current, LibraryListModel::CategoryRole).toString(),
            QStringLiteral("Props"));
}

// A stale search must not hide the category the slot is sending us to.
TEST(LibraryPanel, FocusCategoryClearsAFilterThatWouldHideIt) {
  LibraryPanel panel(populated_model());
  auto* search = panel.findChild<QLineEdit*>(QStringLiteral("library_search"));
  ASSERT_NE(search, nullptr);
  search->setText(QStringLiteral("rural")); // a road template — no props visible
  ASSERT_EQ(panel.view()->model()->rowCount(), 1);

  panel.focus_category(QStringLiteral("Props"));

  EXPECT_TRUE(search->text().isEmpty());
  EXPECT_EQ(panel.view()->model()->rowCount(), 44);
  ASSERT_TRUE(panel.view()->currentIndex().isValid());
  EXPECT_EQ(panel.view()
                ->model()
                ->data(panel.view()->currentIndex(), LibraryListModel::CategoryRole)
                .toString(),
            QStringLiteral("Props"));
}

TEST(LibraryPanel, FocusCategoryLeavesAnUnknownCategoryAlone) {
  LibraryPanel panel(populated_model());
  panel.focus_category(QStringLiteral("Nonexistent"));
  EXPECT_FALSE(panel.view()->currentIndex().isValid());
}

// An overlay item with no thumbnail has a null source decoration; the proxy
// falls back to a themed glyph so the grid still shows an icon (p6-s2).
TEST(LibraryPanel, FallsBackToAThemedGlyphWhenAnItemHasNoThumbnail) {
  LibraryListModel model;
  const auto overlay = LibraryManifest::parse(QByteArray(R"({
    "manifest_version": 1,
    "items": [{"key": "project.nothumb", "label": "No thumb", "category": "Project assets",
               "create": {"kind": "material", "material": "gravel"}}]
  })"));
  ASSERT_TRUE(overlay.has_value());
  model.set_overlay(*overlay, QString());
  // Source model: no decoration (null path).
  EXPECT_FALSE(model.data(model.index(0, 0), Qt::DecorationRole).isValid());

  LibraryPanel panel(model);
  const QModelIndex proxied = panel.view()->model()->index(0, 0);
  // Proxy: a glyph fills in — a valid, non-null decoration.
  const QVariant decoration = panel.view()->model()->data(proxied, Qt::DecorationRole);
  ASSERT_TRUE(decoration.isValid());
  EXPECT_FALSE(decoration.value<QIcon>().isNull());
}

} // namespace
} // namespace roadmaker::editor
