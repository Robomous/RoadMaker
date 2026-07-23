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

#include <QAbstractItemModelTester>
#include <QByteArray>
#include <QFile>
#include <QIcon>
#include <QTemporaryDir>
#include <algorithm>
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
            44U); // 3 road templates + 1 road style + 2 assemblies + 10 tree props
                  // (5 trees/shrub + 2 streetlights + 3 buildings) + 5 signals
                  // + 9 markings + 5 materials + 1 crosswalk + 6 stencils + 2 prop sets

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
  int crosswalks = 0;
  int stencils = 0;
  int prop_sets = 0;
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
      // Point props (kind "tree") span the Props and Buildings categories.
      EXPECT_TRUE(item.category == "Props" || item.category == "Buildings");
    } else if (item.kind == LibraryItem::Kind::Signal) {
      ++signal_items;
      EXPECT_TRUE(item.signal == "light" || item.signal == "sign" || item.signal == "sign_stop" ||
                  item.signal == "sign_yield" || item.signal == "sign_text");
      EXPECT_EQ(item.category, "Signals");
    } else if (item.kind == LibraryItem::Kind::Marking) {
      ++markings;
      EXPECT_FALSE(item.mark_type.isEmpty());
      EXPECT_EQ(item.category, "Markings");
    } else if (item.kind == LibraryItem::Kind::Material) {
      ++materials;
      EXPECT_FALSE(item.material.isEmpty());
      EXPECT_EQ(item.category, "Materials");
    } else if (item.kind == LibraryItem::Kind::Crosswalk) {
      ++crosswalks;
      EXPECT_EQ(item.category, "Crosswalks");
      EXPECT_GT(item.crosswalk_width, 0.0);
    } else if (item.kind == LibraryItem::Kind::Stencil) {
      ++stencils;
      EXPECT_EQ(item.category, "Stencils");
      EXPECT_FALSE(item.stencil_subtype.isEmpty());
      EXPECT_GT(item.stencil_width_frac, 0.0);
    } else if (item.kind == LibraryItem::Kind::PropSet) {
      ++prop_sets;
      EXPECT_EQ(item.category, "Prop sets");
      EXPECT_FALSE(item.prop_entries.empty());
    }
  }
  EXPECT_EQ(templates, 3);
  EXPECT_EQ(styles, 1);
  EXPECT_EQ(assemblies, 2);
  EXPECT_EQ(trees, 10);
  EXPECT_EQ(signal_items, 5);
  EXPECT_EQ(markings, 9);
  EXPECT_EQ(materials, 5);
  EXPECT_EQ(crosswalks, 1);
  EXPECT_EQ(stencils, 6);
  EXPECT_EQ(prop_sets, 2);
}

TEST(LibraryManifest, ParsesCrosswalkCreateKind) {
  const auto manifest = LibraryManifest::parse(json(R"({
    "manifest_version": 1,
    "items": [
      {"key": "crosswalk.zebra", "label": "Zebra", "category": "Crosswalks",
       "create": {"kind": "crosswalk", "width": 3.5, "border_width": 0.2,
                  "dash_length": 0.4, "dash_gap": 0.6, "material": "material.paint_white",
                  "segmentation": "crosswalk"}}
    ]
  })"));
  ASSERT_TRUE(manifest.has_value());
  ASSERT_EQ(manifest->items().size(), 1U);
  const LibraryItem& cw = manifest->items()[0];
  EXPECT_EQ(cw.kind, LibraryItem::Kind::Crosswalk);
  EXPECT_DOUBLE_EQ(cw.crosswalk_width, 3.5);
  EXPECT_DOUBLE_EQ(cw.crosswalk_border, 0.2);
  EXPECT_DOUBLE_EQ(cw.crosswalk_dash, 0.4);
  EXPECT_DOUBLE_EQ(cw.crosswalk_gap, 0.6);
  EXPECT_EQ(cw.crosswalk_material, "material.paint_white");
  EXPECT_EQ(cw.crosswalk_segmentation, "crosswalk");
}

TEST(LibraryManifest, ParsesPropSetEntries) {
  const auto manifest = LibraryManifest::parse(json(R"({
    "manifest_version": 1,
    "items": [
      {"key": "prop_set.mixed", "label": "Mixed", "category": "Prop sets",
       "create": {"kind": "prop_set",
                  "entries": [{"model": "tree_pine", "portion": 3},
                              {"model": "tree_birch", "portion": 1}]}}
    ]
  })"));
  ASSERT_TRUE(manifest.has_value());
  ASSERT_EQ(manifest->items().size(), 1U);
  const LibraryItem& set = manifest->items()[0];
  EXPECT_EQ(set.kind, LibraryItem::Kind::PropSet);
  ASSERT_EQ(set.prop_entries.size(), 2U);
  EXPECT_EQ(set.prop_entries[0].model, "tree_pine");
  EXPECT_DOUBLE_EQ(set.prop_entries[0].portion, 3.0);
  EXPECT_EQ(set.prop_entries[1].model, "tree_birch");
  EXPECT_DOUBLE_EQ(set.prop_entries[1].portion, 1.0);
}

TEST(LibraryManifest, DropsInvalidPropSetEntries) {
  const auto manifest = LibraryManifest::parse(json(R"({
    "manifest_version": 1,
    "items": [
      {"key": "prop_set.dodgy", "label": "Dodgy", "category": "Prop sets",
       "create": {"kind": "prop_set",
                  "entries": [{"model": "tree_pine", "portion": 2},
                              {"model": "not_a_model", "portion": 1},
                              {"model": "shrub", "portion": 0},
                              {"model": "tree_oak", "portion": -5}]}}
    ]
  })"));
  ASSERT_TRUE(manifest.has_value());
  ASSERT_EQ(manifest->items().size(), 1U);
  const LibraryItem& set = manifest->items()[0];
  // Only the resolvable, positive-weight entry survives.
  ASSERT_EQ(set.prop_entries.size(), 1U);
  EXPECT_EQ(set.prop_entries[0].model, "tree_pine");
}

TEST(LibraryManifest, PropSetRoundTripsVerbatim) {
  const auto manifest = LibraryManifest::parse(json(R"({
    "manifest_version": 1,
    "items": [
      {"key": "prop_set.mixed", "label": "Mixed", "category": "Prop sets",
       "create": {"kind": "prop_set",
                  "entries": [{"model": "tree_pine", "portion": 3},
                              {"model": "tree_birch", "portion": 1}]}}
    ]
  })"));
  ASSERT_TRUE(manifest.has_value());
  const auto reparsed = LibraryManifest::parse(manifest->to_json());
  ASSERT_TRUE(reparsed.has_value());
  ASSERT_EQ(reparsed->items().size(), 1U);
  EXPECT_EQ(reparsed->items()[0].kind, LibraryItem::Kind::PropSet);
  // A parsed item re-emits its verbatim create block.
  EXPECT_EQ(reparsed->items()[0].create_raw, manifest->items()[0].create_raw);
}

TEST(LibraryManifest, ProgrammaticPropSetSerializes) {
  // No create_raw (built in code): to_json serializes from prop_entries.
  LibraryItem set;
  set.key = "prop_set.custom1";
  set.label = "Custom";
  set.category = "Prop sets";
  set.kind = LibraryItem::Kind::PropSet;
  set.prop_entries.push_back({.model = "tree_oak", .portion = 2.0});
  set.prop_entries.push_back({.model = "shrub", .portion = 0.5});
  LibraryManifest manifest;
  manifest.upsert(set);

  const auto reparsed = LibraryManifest::parse(manifest.to_json());
  ASSERT_TRUE(reparsed.has_value());
  ASSERT_EQ(reparsed->items().size(), 1U);
  const LibraryItem& out = reparsed->items()[0];
  EXPECT_EQ(out.kind, LibraryItem::Kind::PropSet);
  ASSERT_EQ(out.prop_entries.size(), 2U);
  EXPECT_EQ(out.prop_entries[0].model, "tree_oak");
  EXPECT_DOUBLE_EQ(out.prop_entries[0].portion, 2.0);
  EXPECT_EQ(out.prop_entries[1].model, "shrub");
  EXPECT_DOUBLE_EQ(out.prop_entries[1].portion, 0.5);
}

TEST(LibraryManifest, ToJsonRoundTripsParsedManifest) {
  const auto manifest = LibraryManifest::load(kManifest);
  ASSERT_TRUE(manifest.has_value());
  // to_json -> parse yields an item set equal in count, keys and kinds (each
  // item re-emits its verbatim create block).
  const auto reparsed = LibraryManifest::parse(manifest->to_json());
  ASSERT_TRUE(reparsed.has_value());
  ASSERT_EQ(reparsed->items().size(), manifest->items().size());
  for (std::size_t i = 0; i < manifest->items().size(); ++i) {
    EXPECT_EQ(reparsed->items()[i].key, manifest->items()[i].key);
    EXPECT_EQ(reparsed->items()[i].kind, manifest->items()[i].kind);
    EXPECT_EQ(reparsed->items()[i].create_raw, manifest->items()[i].create_raw);
  }
}

TEST(LibraryManifest, UnknownKindCreateRoundTripsVerbatim) {
  const auto manifest = LibraryManifest::parse(json(R"({
    "manifest_version": 1,
    "items": [{"key": "future", "label": "L",
               "create": {"kind": "prop_from_the_future", "magic": 42, "nested": {"a": 1}}}]
  })"));
  ASSERT_TRUE(manifest.has_value());
  const auto reparsed = LibraryManifest::parse(manifest->to_json());
  ASSERT_TRUE(reparsed.has_value());
  ASSERT_EQ(reparsed->items().size(), 1U);
  EXPECT_EQ(reparsed->items()[0].kind, LibraryItem::Kind::Unknown);
  EXPECT_EQ(reparsed->items()[0].create_raw, manifest->items()[0].create_raw); // verbatim
}

TEST(LibraryManifest, UpsertRemoveAndSaveLoadRoundTrip) {
  const auto loaded = LibraryManifest::load(kManifest);
  ASSERT_TRUE(loaded.has_value());
  LibraryManifest manifest = *loaded;
  const std::size_t original = manifest.items().size();

  LibraryItem cw;
  cw.key = "crosswalk.custom";
  cw.label = "Custom crossing";
  cw.category = "Crosswalks";
  cw.kind = LibraryItem::Kind::Crosswalk;
  cw.crosswalk_width = 4.0;
  cw.crosswalk_dash = 0.0; // solid
  cw.crosswalk_material = "material.paint_white";
  manifest.upsert(cw);
  EXPECT_EQ(manifest.items().size(), original + 1);
  cw.label = "Renamed";
  manifest.upsert(cw); // replace in place, not append
  EXPECT_EQ(manifest.items().size(), original + 1);

  const std::filesystem::path out =
      std::filesystem::temp_directory_path() / "rm_library_manifest_test.json";
  ASSERT_TRUE(manifest.save(out).has_value());
  const auto reloaded = LibraryManifest::load(out);
  std::filesystem::remove(out);
  ASSERT_TRUE(reloaded.has_value());
  const auto it = std::ranges::find_if(
      reloaded->items(), [](const LibraryItem& item) { return item.key == "crosswalk.custom"; });
  ASSERT_NE(it, reloaded->items().end());
  EXPECT_EQ(it->label, "Renamed");
  EXPECT_DOUBLE_EQ(it->crosswalk_width, 4.0);
  EXPECT_DOUBLE_EQ(it->crosswalk_dash, 0.0);

  EXPECT_TRUE(manifest.remove("crosswalk.custom"));
  EXPECT_FALSE(manifest.remove("crosswalk.custom")); // already gone
  EXPECT_EQ(manifest.items().size(), original);
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
  EXPECT_DOUBLE_EQ(tree.default_scale, 1.0); // absent -> native size
}

TEST(LibraryManifest, ParsesDefaultScale) {
  // Absent -> 1.0; an explicit positive value is taken; a non-positive value
  // falls back to 1.0 with a warning (the parser never silently drops input).
  const auto manifest = LibraryManifest::parse(json(R"({
    "manifest_version": 1,
    "items": [
      {"key": "prop.a", "label": "A", "category": "Props",
       "create": {"kind": "tree", "model": "tree_pine", "default_scale": 2.5}},
      {"key": "prop.b", "label": "B", "category": "Props",
       "create": {"kind": "tree", "model": "tree_oak", "default_scale": 0}},
      {"key": "prop.c", "label": "C", "category": "Props",
       "create": {"kind": "tree", "model": "shrub", "default_scale": -3}}
    ]
  })"));
  ASSERT_TRUE(manifest.has_value());
  ASSERT_EQ(manifest->items().size(), 3U);
  EXPECT_DOUBLE_EQ(manifest->items()[0].default_scale, 2.5);
  EXPECT_DOUBLE_EQ(manifest->items()[1].default_scale, 1.0); // zero -> 1.0
  EXPECT_DOUBLE_EQ(manifest->items()[2].default_scale, 1.0); // negative -> 1.0
}

TEST(LibraryManifest, TreeRoundTripsWithDefaultScaleVerbatim) {
  const auto manifest = LibraryManifest::parse(json(R"({
    "manifest_version": 1,
    "items": [
      {"key": "prop.tree.pine", "label": "Pine", "category": "Props",
       "create": {"kind": "tree", "model": "tree_pine", "default_scale": 2}}
    ]
  })"));
  ASSERT_TRUE(manifest.has_value());
  const auto reparsed = LibraryManifest::parse(manifest->to_json());
  ASSERT_TRUE(reparsed.has_value());
  ASSERT_EQ(reparsed->items().size(), 1U);
  EXPECT_DOUBLE_EQ(reparsed->items()[0].default_scale, 2.0);
  // A parsed item re-emits its verbatim create block byte-for-byte.
  EXPECT_EQ(reparsed->items()[0].create_raw, manifest->items()[0].create_raw);
}

TEST(LibraryManifest, ProgrammaticTreeSerializesDefaultScale) {
  // No create_raw: to_json serializes kind/model, and default_scale only when
  // it differs from 1.0 (so a native-size prop stays byte-identical).
  LibraryItem scaled;
  scaled.key = "prop.custom.big";
  scaled.kind = LibraryItem::Kind::Tree;
  scaled.model = "tree_oak";
  scaled.default_scale = 3.0;
  LibraryItem native;
  native.key = "prop.custom.native";
  native.kind = LibraryItem::Kind::Tree;
  native.model = "shrub";
  LibraryManifest manifest;
  manifest.upsert(scaled);
  manifest.upsert(native);

  const auto reparsed = LibraryManifest::parse(manifest.to_json());
  ASSERT_TRUE(reparsed.has_value());
  ASSERT_EQ(reparsed->items().size(), 2U);
  const LibraryItem& out_scaled = reparsed->items()[0];
  EXPECT_EQ(out_scaled.kind, LibraryItem::Kind::Tree);
  EXPECT_EQ(out_scaled.model, "tree_oak");
  EXPECT_DOUBLE_EQ(out_scaled.default_scale, 3.0);
  // The native item omitted the key on serialize; parsing it defaults to 1.0.
  EXPECT_FALSE(reparsed->items()[1].create_raw.contains(QStringLiteral("default_scale")));
  EXPECT_DOUBLE_EQ(reparsed->items()[1].default_scale, 1.0);
}

TEST(LibraryListModel, DefaultScaleForModelReflectsTheMergedView) {
  LibraryListModel model;
  const auto base = LibraryManifest::parse(json(R"({
    "manifest_version": 1,
    "items": [
      {"key": "prop.tree.pine", "label": "Pine", "category": "Props",
       "create": {"kind": "tree", "model": "tree_pine", "default_scale": 2}},
      {"key": "prop.tree.oak", "label": "Oak", "category": "Props",
       "create": {"kind": "tree", "model": "tree_oak"}}
    ]
  })"));
  ASSERT_TRUE(base.has_value());
  model.set_manifest(*base);
  EXPECT_DOUBLE_EQ(model.default_scale_for_model(QStringLiteral("tree_pine")), 2.0);
  EXPECT_DOUBLE_EQ(model.default_scale_for_model(QStringLiteral("tree_oak")), 1.0);
  EXPECT_DOUBLE_EQ(model.default_scale_for_model(QStringLiteral("no_such_model")), 1.0);

  // A project overlay shadows the built-in: the first merged match wins.
  const auto overlay = LibraryManifest::parse(json(R"({
    "manifest_version": 1,
    "items": [
      {"key": "prop.tree.pine", "label": "Project Pine", "category": "Props",
       "create": {"kind": "tree", "model": "tree_pine", "default_scale": 5}}
    ]
  })"));
  ASSERT_TRUE(overlay.has_value());
  model.set_overlay(*overlay);
  EXPECT_DOUBLE_EQ(model.default_scale_for_model(QStringLiteral("tree_pine")), 5.0);
}

TEST(LibraryListModel, OverlayTreeKeepsAResolvedQrcThumbnail) {
  // Committing a copy of a built-in prop writes its already-resolved
  // ":/library/thumbnails/…" path into the overlay. QDir::filePath returns an
  // absolute path (a qrc ":/" counts) unchanged, so the shadowing overlay entry
  // keeps its icon — the thumbnail-preservation contract for prop asset edits.
  LibraryListModel model;
  const auto overlay = LibraryManifest::parse(json(R"({
    "manifest_version": 1,
    "items": [
      {"key": "prop.tree.pine", "label": "Project Pine", "category": "Props",
       "thumbnail": ":/library/thumbnails/prop_tree_pine.png",
       "create": {"kind": "tree", "model": "tree_pine", "default_scale": 2}}
    ]
  })"));
  ASSERT_TRUE(overlay.has_value());
  QTemporaryDir project_dir;
  ASSERT_TRUE(project_dir.isValid());
  model.set_overlay(*overlay, project_dir.path());
  const QModelIndex index = model.index(0, 0);
  const QVariant decoration = model.data(index, Qt::DecorationRole);
  ASSERT_TRUE(decoration.isValid());
  EXPECT_FALSE(decoration.value<QIcon>().isNull());
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
  EXPECT_EQ(model.rowCount(), 44);
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
  EXPECT_EQ(model.item(44), nullptr);
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
  EXPECT_EQ(model.rowCount(), 45); // 44 base items + 1 overlay
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
    const LibraryItem* entry = model.item(row);
    ASSERT_NE(entry, nullptr);
    // A PropSet carries neither a bundled PNG nor a model-level preview — its
    // grid icon is the LibraryPanel's glyph fallback (p6-s5), so it is exempt
    // from both the qrc-thumbnail drift gate and the decoration check.
    if (entry->kind == LibraryItem::Kind::PropSet) {
      EXPECT_TRUE(path.isEmpty()) << key.toStdString();
      continue;
    }
    // Crosswalk and Stencil assets carry no bundled PNG — their DecorationRole
    // is a runtime QPainter preview, so they are exempt from the qrc-thumbnail
    // drift gate but must still serve a non-null decoration.
    if (entry->kind == LibraryItem::Kind::Crosswalk || entry->kind == LibraryItem::Kind::Stencil) {
      EXPECT_TRUE(path.isEmpty()) << key.toStdString();
    } else {
      EXPECT_TRUE(path.startsWith(QStringLiteral(":/library/thumbnails/")))
          << key.toStdString() << " -> " << path.toStdString();
    }
    const QVariant decoration = model.data(index, Qt::DecorationRole);
    ASSERT_TRUE(decoration.isValid()) << "no decoration for " << key.toStdString();
    EXPECT_FALSE(decoration.value<QIcon>().isNull())
        << "decoration failed to load for " << key.toStdString() << " at " << path.toStdString();
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
