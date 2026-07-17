#include <gtest/gtest.h>

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
            13); // 3 templates + 1 style + T/X + 5 props + 2 signals
  // The grid gives every item a themed icon (the filter proxy injects it).
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
  EXPECT_EQ(panel.view()->model()->rowCount(), 13);
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
  EXPECT_EQ(panel.view()->model()->rowCount(), 13);
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

} // namespace
} // namespace roadmaker::editor
