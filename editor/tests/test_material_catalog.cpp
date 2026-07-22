// Copyright 2026 Robomous
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>

#include "render/material_catalog.hpp"

namespace roadmaker::editor {
namespace {

TEST(MaterialCatalog, ResolvesTheThreeSpellings) {
  const MaterialCatalog catalog;
  const MaterialDef* rm = catalog.find_material("rm:asphalt");
  const MaterialDef* bare = catalog.find_material("asphalt");
  const MaterialDef* keyed = catalog.find_material("material.asphalt");
  ASSERT_NE(rm, nullptr);
  ASSERT_NE(bare, nullptr);
  ASSERT_NE(keyed, nullptr);
  // All three spellings resolve to the same definition.
  EXPECT_EQ(rm, bare);
  EXPECT_EQ(rm, keyed);
  EXPECT_EQ(rm->name, "asphalt");
}

TEST(MaterialCatalog, CarriesTexturePathsAndParams) {
  const MaterialCatalog catalog;
  const MaterialDef* worn = catalog.find_material("rm:asphalt_worn");
  ASSERT_NE(worn, nullptr);
  EXPECT_EQ(worn->name, "asphalt_worn");
  // Every catalog material bundles albedo + normal (PNG) + roughness maps.
  EXPECT_FALSE(worn->albedo.empty());
  EXPECT_FALSE(worn->normal.empty());
  EXPECT_FALSE(worn->roughness.empty());
  EXPECT_TRUE(worn->normal.ends_with(".png")); // normals lossless
  EXPECT_GT(worn->friction, 0.0);
  EXPECT_GT(worn->roughness_value, 0.0F);
}

TEST(MaterialCatalog, UnknownCodeIsNull) {
  const MaterialCatalog catalog;
  EXPECT_EQ(catalog.find_material("rm:granite"), nullptr);
  EXPECT_EQ(catalog.find_material(""), nullptr);
  EXPECT_EQ(catalog.find_material("material.gold"), nullptr);
}

TEST(MaterialCatalog, ShipsAsphaltWornConcreteAndPaints) {
  const MaterialCatalog catalog;
  EXPECT_NE(catalog.find_material("asphalt"), nullptr);
  EXPECT_NE(catalog.find_material("asphalt_worn"), nullptr);
  EXPECT_NE(catalog.find_material("concrete"), nullptr);
  // Texture-less road paints (p6-s6): flat-colour, no maps.
  EXPECT_NE(catalog.find_material("paint_white"), nullptr);
  EXPECT_NE(catalog.find_material("paint_yellow"), nullptr);
  EXPECT_EQ(catalog.materials().size(), 5U);
}

} // namespace
} // namespace roadmaker::editor
