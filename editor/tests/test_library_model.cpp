#include <gtest/gtest.h>

#include <QAbstractItemModelTester>
#include <QByteArray>
#include <filesystem>

#include "document/library_list_model.hpp"
#include "document/library_manifest.hpp"

namespace roadmaker::editor {
namespace {

const std::filesystem::path kManifest =
    std::filesystem::path(RM_ASSETS_DIR) / "library" / "manifest.json";

QByteArray json(const char* text) {
  return QByteArray{text};
}

TEST(LibraryManifest, ParsesTheShippedManifest) {
  const auto manifest = LibraryManifest::load(kManifest);
  ASSERT_TRUE(manifest.has_value()) << (manifest ? "" : manifest.error().message);
  EXPECT_EQ(manifest->version(), 1);
  EXPECT_EQ(manifest->items().size(), 5U); // 3 road templates + 2 assemblies

  // The road templates resolve to a profile; the assemblies to a t/x kind.
  int templates = 0;
  int assemblies = 0;
  for (const LibraryItem& item : manifest->items()) {
    EXPECT_FALSE(item.key.isEmpty());
    EXPECT_FALSE(item.label.isEmpty());
    if (item.kind == LibraryItem::Kind::RoadTemplate) {
      ++templates;
      EXPECT_FALSE(item.profile.isEmpty());
    } else if (item.kind == LibraryItem::Kind::Assembly) {
      ++assemblies;
      EXPECT_TRUE(item.assembly == "t" || item.assembly == "x");
    }
  }
  EXPECT_EQ(templates, 3);
  EXPECT_EQ(assemblies, 2);
}

TEST(LibraryManifest, ParsesFieldsAndCreateKinds) {
  const auto manifest = LibraryManifest::parse(json(R"({
    "manifest_version": 1,
    "items": [
      {"key": "road.rural", "label": "Rural", "category": "Road templates",
       "thumbnail": "t/rural.png", "create": {"kind": "road_template", "profile": "two_lane_rural"}},
      {"key": "assembly.x", "label": "X", "category": "Assemblies",
       "create": {"kind": "assembly", "assembly": "x"}}
    ]
  })"));
  ASSERT_TRUE(manifest.has_value());
  ASSERT_EQ(manifest->items().size(), 2U);
  const LibraryItem& road = manifest->items()[0];
  EXPECT_EQ(road.key, "road.rural");
  EXPECT_EQ(road.category, "Road templates");
  EXPECT_EQ(road.thumbnail, "t/rural.png");
  EXPECT_EQ(road.kind, LibraryItem::Kind::RoadTemplate);
  EXPECT_EQ(road.profile, "two_lane_rural");
  EXPECT_EQ(manifest->items()[1].kind, LibraryItem::Kind::Assembly);
}

TEST(LibraryManifest, NewerVersionParsesBestEffort) {
  const auto manifest = LibraryManifest::parse(json(R"({
    "manifest_version": 99,
    "items": [{"key": "k", "label": "L", "create": {"kind": "prop_from_the_future"}}]
  })"));
  ASSERT_TRUE(manifest.has_value()); // forward-compat: a warning, not an error
  EXPECT_EQ(manifest->version(), 99);
  ASSERT_EQ(manifest->items().size(), 1U);
  EXPECT_EQ(manifest->items()[0].kind, LibraryItem::Kind::Unknown); // shown, not droppable
}

TEST(LibraryManifest, RejectsMalformedAndMissingVersion) {
  EXPECT_FALSE(LibraryManifest::parse(json("{ not json")).has_value());
  EXPECT_FALSE(LibraryManifest::parse(json(R"({"items": []})")).has_value());
  EXPECT_FALSE(LibraryManifest::parse(json(R"({"manifest_version": 1})")).has_value());
}

TEST(LibraryManifest, SkipsAnItemWithoutAKey) {
  const auto manifest = LibraryManifest::parse(json(R"({
    "manifest_version": 1,
    "items": [{"label": "no key"}, {"key": "ok", "label": "ok"}]
  })"));
  ASSERT_TRUE(manifest.has_value());
  ASSERT_EQ(manifest->items().size(), 1U);
  EXPECT_EQ(manifest->items()[0].key, "ok");
}

TEST(LibraryListModel, PassesQtModelSanityChecksEmptyAndPopulated) {
  LibraryListModel model;
  QAbstractItemModelTester tester(&model, QAbstractItemModelTester::FailureReportingMode::Fatal);
  EXPECT_EQ(model.rowCount(), 0);

  const auto manifest = LibraryManifest::load(kManifest);
  ASSERT_TRUE(manifest.has_value());
  model.set_manifest(*manifest);
  EXPECT_EQ(model.rowCount(), 5);
}

TEST(LibraryListModel, ExposesRolesAndItemLookup) {
  LibraryListModel model;
  const auto manifest = LibraryManifest::load(kManifest);
  ASSERT_TRUE(manifest.has_value());
  model.set_manifest(*manifest);

  const QModelIndex first = model.index(0, 0);
  EXPECT_FALSE(model.data(first, Qt::DisplayRole).toString().isEmpty());
  EXPECT_FALSE(model.data(first, LibraryListModel::KeyRole).toString().isEmpty());
  EXPECT_FALSE(model.data(first, LibraryListModel::CategoryRole).toString().isEmpty());

  const LibraryItem* item = model.item(0);
  ASSERT_NE(item, nullptr);
  EXPECT_EQ(model.data(first, LibraryListModel::KeyRole).toString(), item->key);
  EXPECT_EQ(model.item(-1), nullptr);
  EXPECT_EQ(model.item(5), nullptr);
}

} // namespace
} // namespace roadmaker::editor
