#include <gtest/gtest.h>

#include <QIcon>
#include <QLineEdit>
#include <QListView>
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
            26); // 3 templates + 1 style + T/X + 5 props + 2 signals + 9 markings + 3 materials + 1
                 // crosswalk
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
  EXPECT_EQ(panel.view()->model()->rowCount(), 4); // pine/oak/birch/poplar

  search->setText(QStringLiteral("Traffic"));
  EXPECT_EQ(panel.view()->model()->rowCount(), 2); // traffic light + traffic sign

  search->clear();
  EXPECT_EQ(panel.view()->model()->rowCount(), 26);
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
  EXPECT_EQ(panel.view()->model()->rowCount(), 26);
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
