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

#include <cstddef>
#include <string>
#include <utility>

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

// --------------------------------------------------------------------------- //
// The project overlay (p6-s8, #322 · ADR-0013)
// --------------------------------------------------------------------------- //

// The overlay is PROCESS-WIDE, because MaterialCatalog is constructed ad hoc in
// eight places and one of them is a stack local inside a free function. These
// tests are what pays that choice down: they are the reason it is safe to say a
// project switch replaces the overlay wholesale.

/// Restores the built-in catalog after every case, so an overlay left behind by
/// one test cannot alter another — the process-wide state's one real hazard.
class MaterialOverlay : public testing::Test {
protected:
  void TearDown() override { MaterialCatalog::clear_project_materials(); }

  static MaterialDef project_material(std::string name, float roughness = 0.5F) {
    MaterialDef def;
    def.name = std::move(name);
    // An ABSOLUTE filesystem path, not a qrc alias — which is exactly what an
    // imported material carries, and what ViewportWidget::texture_for already
    // QImage-decodes without any change for import.
    def.albedo = "/tmp/rm_project/assets/textures/brick.png";
    def.roughness_value = roughness;
    def.friction = 0.55;
    return def;
  }
};

TEST_F(MaterialOverlay, WithNoProjectTheCatalogIsExactlyTheBuiltInOne) {
  const MaterialCatalog catalog;
  EXPECT_EQ(catalog.materials().size(), catalog.builtin_materials().size());
  EXPECT_FALSE(MaterialCatalog::is_project_material("asphalt"));
}

TEST_F(MaterialOverlay, AProjectMaterialResolvesAndJoinsTheCatalog) {
  const MaterialCatalog catalog;
  const std::size_t builtin = catalog.builtin_materials().size();
  MaterialCatalog::set_project_materials({project_material("red_brick")});

  const MaterialDef* found = catalog.find_material("red_brick");
  ASSERT_NE(found, nullptr);
  EXPECT_EQ(found->name, "red_brick");
  EXPECT_TRUE(MaterialCatalog::is_project_material("red_brick"));
  EXPECT_EQ(catalog.materials().size(), builtin + 1);
  // The bundled five are still there.
  EXPECT_NE(catalog.find_material("asphalt"), nullptr);
}

TEST_F(MaterialOverlay, AllThreeSpellingsResolveAProjectMaterialToo) {
  const MaterialCatalog catalog;
  MaterialCatalog::set_project_materials({project_material("red_brick")});
  // An imported material's manifest id is `rm:<slug>`, the Library key is
  // `material.<slug>`, and a <material surface> attribute is `rm:<slug>` — so all
  // three have to land on the same definition or the drop path breaks.
  EXPECT_NE(catalog.find_material("rm:red_brick"), nullptr);
  EXPECT_NE(catalog.find_material("red_brick"), nullptr);
  EXPECT_NE(catalog.find_material("material.red_brick"), nullptr);
  EXPECT_EQ(catalog.find_material("rm:red_brick"), catalog.find_material("red_brick"));
}

TEST_F(MaterialOverlay, ClearingRestoresTheBuiltInCatalogExactly) {
  const MaterialCatalog catalog;
  const std::size_t builtin = catalog.builtin_materials().size();
  MaterialCatalog::set_project_materials({project_material("red_brick")});
  ASSERT_NE(catalog.find_material("red_brick"), nullptr);

  MaterialCatalog::clear_project_materials();

  // The invariant a project switch depends on.
  EXPECT_EQ(catalog.find_material("red_brick"), nullptr);
  EXPECT_FALSE(MaterialCatalog::is_project_material("red_brick"));
  EXPECT_EQ(catalog.materials().size(), builtin);
  EXPECT_NE(catalog.find_material("asphalt"), nullptr);
}

TEST_F(MaterialOverlay, SettingReplacesWholesaleRatherThanAccumulating) {
  const MaterialCatalog catalog;
  MaterialCatalog::set_project_materials({project_material("project_a_brick")});
  ASSERT_NE(catalog.find_material("project_a_brick"), nullptr);

  MaterialCatalog::set_project_materials({project_material("project_b_stone")});

  // Opening project B must not leave project A's materials resolvable.
  EXPECT_EQ(catalog.find_material("project_a_brick"), nullptr);
  EXPECT_NE(catalog.find_material("project_b_stone"), nullptr);
  EXPECT_EQ(catalog.materials().size(), catalog.builtin_materials().size() + 1);
}

TEST_F(MaterialOverlay, AProjectMaterialShadowsABundledOneAndIsListedOnce) {
  const MaterialCatalog catalog;
  const std::size_t builtin = catalog.builtin_materials().size();
  MaterialCatalog::set_project_materials({project_material("asphalt", 0.11F)});

  const MaterialDef* found = catalog.find_material("asphalt");
  ASSERT_NE(found, nullptr);
  // The project's definition wins — that is what lets a project override a
  // bundled material.
  EXPECT_FLOAT_EQ(found->roughness_value, 0.11F);
  EXPECT_TRUE(MaterialCatalog::is_project_material("asphalt"));
  // But the merged list does not show it twice, or the Materials category would
  // carry a duplicate row.
  EXPECT_EQ(catalog.materials().size(), builtin);
}

TEST_F(MaterialOverlay, EveryCatalogInstanceSeesTheSameOverlay) {
  // ★ THE WHOLE POINT of the process-wide choice. These stand in for the eight
  // real construction sites — including the stack local inside
  // library_drop.cpp's free function, which is why a per-instance overlay would
  // not have worked without threading an owner through all of them.
  const MaterialCatalog viewport_catalog;
  const MaterialCatalog panel_catalog;
  MaterialCatalog::set_project_materials({project_material("red_brick")});

  const MaterialCatalog constructed_after;
  EXPECT_NE(viewport_catalog.find_material("red_brick"), nullptr);
  EXPECT_NE(panel_catalog.find_material("red_brick"), nullptr);
  EXPECT_NE(constructed_after.find_material("red_brick"), nullptr);
  EXPECT_EQ(viewport_catalog.materials().size(), constructed_after.materials().size());
}

TEST_F(MaterialOverlay, TheMergedListRefreshesWhenTheOverlayChanges) {
  // The merged view is cached per instance against a generation counter, so a
  // stale cache would be invisible until exactly this sequence.
  const MaterialCatalog catalog;
  const std::size_t builtin = catalog.builtin_materials().size();
  MaterialCatalog::set_project_materials({project_material("one")});
  EXPECT_EQ(catalog.materials().size(), builtin + 1);
  MaterialCatalog::set_project_materials({project_material("one"), project_material("two")});
  EXPECT_EQ(catalog.materials().size(), builtin + 2);
  MaterialCatalog::clear_project_materials();
  EXPECT_EQ(catalog.materials().size(), builtin);
}

TEST_F(MaterialOverlay, AProjectMaterialCarriesItsOwnFrictionIntoTheDropPath) {
  // library_drop writes def->friction into the LaneMaterial record, so an
  // imported material has to supply one or a dropped lane would silently get the
  // bundled default.
  const MaterialCatalog catalog;
  MaterialCatalog::set_project_materials({project_material("red_brick")});
  const MaterialDef* found = catalog.find_material("rm:red_brick");
  ASSERT_NE(found, nullptr);
  EXPECT_DOUBLE_EQ(found->friction, 0.55);
  EXPECT_FALSE(found->albedo.empty());
}

} // namespace
} // namespace roadmaker::editor
