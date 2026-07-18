#include <gtest/gtest.h>

#include <QAbstractItemModelTester>
#include <QByteArray>
#include <QFile>
#include <QIcon>
#include <QTemporaryDir>
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
  EXPECT_EQ(manifest->items().size(),
            18U); // 3 road templates + 1 road style + 2 assemblies + 5 tree props
                  // + 2 signals + 2 markings + 3 materials (asphalt/asphalt_worn/concrete)

  // Road templates resolve to a profile, road styles to a style name, assemblies
  // to a t/x kind, trees to a bundled prop model id, signals to a light/sign tag,
  // markings to a mark type/color, materials to a material name.
  int templates = 0;
  int styles = 0;
  int assemblies = 0;
  int trees = 0;
  int signal_items = 0;
  int markings = 0;
  int materials = 0;
  for (const LibraryItem& item : manifest->items()) {
    EXPECT_FALSE(item.key.isEmpty());
    EXPECT_FALSE(item.label.isEmpty());
    if (item.kind == LibraryItem::Kind::RoadTemplate) {
      ++templates;
      EXPECT_FALSE(item.profile.isEmpty());
    } else if (item.kind == LibraryItem::Kind::RoadStyle) {
      ++styles;
      EXPECT_FALSE(item.style.isEmpty());
      EXPECT_EQ(item.category, "Road styles");
    } else if (item.kind == LibraryItem::Kind::Assembly) {
      ++assemblies;
      EXPECT_TRUE(item.assembly == "t" || item.assembly == "x");
    } else if (item.kind == LibraryItem::Kind::Tree) {
      ++trees;
      EXPECT_FALSE(item.model.isEmpty());
      EXPECT_EQ(item.category, "Props");
    } else if (item.kind == LibraryItem::Kind::Signal) {
      ++signal_items;
      EXPECT_TRUE(item.signal == "light" || item.signal == "sign");
      EXPECT_EQ(item.category, "Signals");
    } else if (item.kind == LibraryItem::Kind::Marking) {
      ++markings;
      EXPECT_FALSE(item.mark_type.isEmpty());
      EXPECT_EQ(item.category, "Markings");
    } else if (item.kind == LibraryItem::Kind::Material) {
      ++materials;
      EXPECT_FALSE(item.material.isEmpty());
      EXPECT_EQ(item.category, "Materials");
    }
  }
  EXPECT_EQ(templates, 3);
  EXPECT_EQ(styles, 1);
  EXPECT_EQ(assemblies, 2);
  EXPECT_EQ(trees, 5);
  EXPECT_EQ(signal_items, 2);
  EXPECT_EQ(markings, 2);
  EXPECT_EQ(materials, 3);
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

TEST(LibraryManifest, ParsesTreeCreateKindWithModel) {
  const auto manifest = LibraryManifest::parse(json(R"({
    "manifest_version": 1,
    "items": [
      {"key": "prop.tree.pine", "label": "Pine", "category": "Props",
       "create": {"kind": "tree", "model": "tree_pine"}}
    ]
  })"));
  ASSERT_TRUE(manifest.has_value());
  ASSERT_EQ(manifest->items().size(), 1U);
  const LibraryItem& tree = manifest->items()[0];
  EXPECT_EQ(tree.kind, LibraryItem::Kind::Tree);
  EXPECT_EQ(tree.model, "tree_pine");
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
  EXPECT_EQ(model.rowCount(), 18);
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
  EXPECT_EQ(model.item(18), nullptr);
}

// The per-project overlay (p6-s1): project items merge into the built-in
// catalogue — a colliding key REPLACES the built-in item in place, a new key
// (and category) appends — and the overlay leaves with its project.
TEST(LibraryListModel, OverlayMergesProjectItemsOverBuiltIns) {
  LibraryListModel model;
  QAbstractItemModelTester tester(&model, QAbstractItemModelTester::FailureReportingMode::Fatal);

  const auto base = LibraryManifest::parse(json(R"({
    "manifest_version": 1,
    "items": [
      {"key": "road.rural", "label": "Rural", "category": "Road templates",
       "create": {"kind": "road_template", "profile": "two_lane_rural"}},
      {"key": "prop.tree.pine", "label": "Pine", "category": "Props",
       "create": {"kind": "tree", "model": "tree_pine"}}
    ]
  })"));
  ASSERT_TRUE(base.has_value());
  model.set_manifest(*base);
  ASSERT_EQ(model.rowCount(), 2);
  EXPECT_FALSE(model.has_overlay());

  const auto overlay = LibraryManifest::parse(json(R"({
    "manifest_version": 1,
    "items": [
      {"key": "prop.tree.pine", "label": "Project Pine", "category": "Props",
       "create": {"kind": "tree", "model": "tree_pine"}},
      {"key": "project.special", "label": "Special", "category": "Project assets",
       "create": {"kind": "tree", "model": "tree_oak"}}
    ]
  })"));
  ASSERT_TRUE(overlay.has_value());
  model.set_overlay(*overlay);
  EXPECT_TRUE(model.has_overlay());
  ASSERT_EQ(model.rowCount(), 3); // 2 base, 1 collided in place, 1 appended

  // The collision keeps the built-in item's row but shows the project's data.
  EXPECT_EQ(model.data(model.index(1, 0), Qt::DisplayRole).toString(),
            QStringLiteral("Project Pine"));
  const LibraryItem* collided = model.item_for_key(QStringLiteral("prop.tree.pine"));
  ASSERT_NE(collided, nullptr);
  EXPECT_EQ(collided->label, QStringLiteral("Project Pine"));
  // The new key (and its new category) appended after the built-ins.
  EXPECT_EQ(model.data(model.index(2, 0), LibraryListModel::KeyRole).toString(),
            QStringLiteral("project.special"));
  EXPECT_EQ(model.data(model.index(2, 0), LibraryListModel::CategoryRole).toString(),
            QStringLiteral("Project assets"));

  // Project close: the catalogue returns to the built-ins alone.
  model.clear_overlay();
  EXPECT_FALSE(model.has_overlay());
  ASSERT_EQ(model.rowCount(), 2);
  const LibraryItem* restored = model.item_for_key(QStringLiteral("prop.tree.pine"));
  ASSERT_NE(restored, nullptr);
  EXPECT_EQ(restored->label, QStringLiteral("Pine"));
  EXPECT_EQ(model.item_for_key(QStringLiteral("project.special")), nullptr);
  model.clear_overlay(); // idempotent
  EXPECT_EQ(model.rowCount(), 2);
}

TEST(LibraryListModel, SetManifestRemergesAnActiveOverlay) {
  LibraryListModel model;
  const auto overlay = LibraryManifest::parse(json(R"({
    "manifest_version": 1,
    "items": [{"key": "project.only", "label": "Only", "category": "Project assets",
               "create": {"kind": "tree", "model": "tree_oak"}}]
  })"));
  ASSERT_TRUE(overlay.has_value());
  model.set_overlay(*overlay);
  ASSERT_EQ(model.rowCount(), 1);

  const auto base = LibraryManifest::load(kManifest);
  ASSERT_TRUE(base.has_value());
  model.set_manifest(*base);       // the overlay survives a base reload
  EXPECT_EQ(model.rowCount(), 19); // 18 base items + 1 overlay
  EXPECT_NE(model.item_for_key(QStringLiteral("project.only")), nullptr);
}

TEST(LibraryManifest, ParsesMarkingCreateKind) {
  const auto manifest = LibraryManifest::parse(json(R"({
    "manifest_version": 1,
    "items": [
      {"key": "marking.double_yellow", "label": "Double yellow", "category": "Markings",
       "create": {"kind": "marking", "mark_type": "solid_solid", "mark_color": "yellow",
                  "mark_width": 0.15}}
    ]
  })"));
  ASSERT_TRUE(manifest.has_value());
  ASSERT_EQ(manifest->items().size(), 1U);
  const LibraryItem& mark = manifest->items()[0];
  EXPECT_EQ(mark.kind, LibraryItem::Kind::Marking);
  EXPECT_EQ(mark.mark_type, "solid_solid");
  EXPECT_EQ(mark.mark_color, "yellow");
  EXPECT_DOUBLE_EQ(mark.mark_width, 0.15);
}

TEST(LibraryManifest, ParsesMaterialCreateKind) {
  const auto manifest = LibraryManifest::parse(json(R"({
    "manifest_version": 1,
    "items": [
      {"key": "material.asphalt", "label": "Asphalt", "category": "Materials",
       "create": {"kind": "material", "material": "asphalt"}}
    ]
  })"));
  ASSERT_TRUE(manifest.has_value());
  ASSERT_EQ(manifest->items().size(), 1U);
  const LibraryItem& material = manifest->items()[0];
  EXPECT_EQ(material.kind, LibraryItem::Kind::Material);
  EXPECT_EQ(material.material, "asphalt");
  EXPECT_DOUBLE_EQ(material.mark_width, 0.12); // default when absent
}

// Drift gate (p6-s2): every built-in item's manifest thumbnail resolves to a
// bundled qrc icon that actually loads. Guards the manifest name ↔ PNG ↔ qrc
// alias triple against a rename touching only one of the three.
TEST(LibraryListModel, ServesABundledThumbnailForEveryBuiltInItem) {
  LibraryListModel model;
  const auto manifest = LibraryManifest::load(kManifest);
  ASSERT_TRUE(manifest.has_value());
  model.set_manifest(*manifest);
  for (int row = 0; row < model.rowCount(); ++row) {
    const QModelIndex index = model.index(row, 0);
    const QString key = model.data(index, LibraryListModel::KeyRole).toString();
    const QString path = model.data(index, LibraryListModel::ThumbnailRole).toString();
    EXPECT_TRUE(path.startsWith(QStringLiteral(":/library/thumbnails/")))
        << key.toStdString() << " -> " << path.toStdString();
    const QVariant decoration = model.data(index, Qt::DecorationRole);
    ASSERT_TRUE(decoration.isValid()) << "no decoration for " << key.toStdString();
    EXPECT_FALSE(decoration.value<QIcon>().isNull())
        << "thumbnail failed to load for " << key.toStdString() << " at " << path.toStdString();
  }
}

// A project-overlay item's thumbnail is resolved on disk against the project
// directory; the loaded icon proves the resolution and read work end to end.
TEST(LibraryListModel, ResolvesOverlayThumbnailsAgainstTheProjectDir) {
  QTemporaryDir project_dir;
  ASSERT_TRUE(project_dir.isValid());
  // Reuse a bundled PNG as the overlay's on-disk thumbnail.
  QFile source(QStringLiteral(":/library/thumbnails/material_asphalt.png"));
  const QString on_disk = project_dir.filePath(QStringLiteral("custom.png"));
  ASSERT_TRUE(source.copy(on_disk));

  LibraryListModel model;
  const auto overlay = LibraryManifest::parse(json(R"({
    "manifest_version": 1,
    "items": [{"key": "project.custom", "label": "Custom", "category": "Project assets",
               "thumbnail": "custom.png", "create": {"kind": "material", "material": "gravel"}}]
  })"));
  ASSERT_TRUE(overlay.has_value());
  model.set_overlay(*overlay, project_dir.path());
  const QModelIndex index = model.index(0, 0);
  EXPECT_EQ(model.data(index, LibraryListModel::ThumbnailRole).toString(), on_disk);
  EXPECT_FALSE(model.data(index, Qt::DecorationRole).value<QIcon>().isNull());
}

// An item with no thumbnail (empty path, or a path that fails to load) yields an
// invalid decoration variant so the panel's proxy can fall back to a glyph.
TEST(LibraryListModel, MissingThumbnailYieldsNullDecoration) {
  LibraryListModel model;
  const auto overlay = LibraryManifest::parse(json(R"({
    "manifest_version": 1,
    "items": [{"key": "project.nothumb", "label": "No thumb", "category": "Project assets",
               "create": {"kind": "material", "material": "gravel"}}]
  })"));
  ASSERT_TRUE(overlay.has_value());
  model.set_overlay(*overlay, QStringLiteral("/nonexistent"));
  EXPECT_FALSE(model.data(model.index(0, 0), Qt::DecorationRole).isValid());
}

} // namespace
} // namespace roadmaker::editor
