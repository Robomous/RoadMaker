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

#include "render/material_catalog.hpp"

#include <algorithm>
#include <utility>

namespace roadmaker::editor {

MaterialCatalog::MaterialCatalog() {
  // Bundled Poly Haven CC0 textures (albedo + nor_gl PNG + roughness JPEG),
  // aliased in editor/resources/resources.qrc; ledgered in ASSETS_LICENSES.md.
  // roughness_value is the scalar the shader multiplies the map by (and the
  // sole roughness when no map loads); friction is the nominal coefficient
  // written into a <material> record when this material is assigned.
  materials_.push_back(MaterialDef{
      .name = "asphalt",
      .albedo = ":/textures/asphalt.jpg",
      .normal = ":/textures/asphalt_nor.png",
      .roughness = ":/textures/asphalt_rough.jpg",
      .uv_scale = 0.25F,
      .roughness_value = 0.72F,
      .normal_strength = 1.0F,
      .friction = 0.9,
  });
  materials_.push_back(MaterialDef{
      .name = "asphalt_worn",
      .albedo = ":/textures/asphalt_worn.jpg",
      .normal = ":/textures/asphalt_worn_nor.png",
      .roughness = ":/textures/asphalt_worn_rough.jpg",
      .uv_scale = 0.25F,
      .roughness_value = 0.88F,
      .normal_strength = 1.0F,
      .friction = 0.72,
  });
  materials_.push_back(MaterialDef{
      .name = "concrete",
      .albedo = ":/textures/concrete.jpg",
      .normal = ":/textures/concrete_nor.png",
      .roughness = ":/textures/concrete_rough.jpg",
      .uv_scale = 0.25F,
      .roughness_value = 0.75F,
      .normal_strength = 1.0F,
      .friction = 0.85,
  });
  // Road-paint materials: texture-less flat colours (empty map paths → the
  // shader's flat-colour path, u_has_texture = 0, tinted by `tint`). These are
  // the surfaces crosswalk/stencil library items already reference by
  // "material.paint_white", so those assets now resolve to a real definition.
  materials_.push_back(MaterialDef{
      .name = "paint_white",
      .uv_scale = 0.25F,
      .tint = {1.0F, 1.0F, 1.0F, 1.0F},
      .roughness_value = 0.6F,
      .normal_strength = 1.0F,
      .friction = 0.7,
  });
  materials_.push_back(MaterialDef{
      .name = "paint_yellow",
      .uv_scale = 0.25F,
      .tint = {0.95F, 0.78F, 0.12F, 1.0F},
      .roughness_value = 0.6F,
      .normal_strength = 1.0F,
      .friction = 0.7,
  });
}

namespace {

/// The project overlay, and a generation counter so every MaterialCatalog
/// instance can tell when its merged view went stale. See the header for why
/// this is process-wide rather than owned.
struct ProjectMaterials {
  std::vector<MaterialDef> definitions;
  std::uint64_t generation = 1; ///< never 0, so a fresh instance's cache is stale
};

ProjectMaterials& project_materials() {
  static ProjectMaterials state;
  return state;
}

/// Strips the three accepted spellings down to a bare catalog name, so
/// `rm:asphalt`, `asphalt` and `material.asphalt` all resolve to the same
/// definition — including for an imported material, whose manifest id is
/// `rm:<name>`.
std::string_view bare_name(std::string_view code) {
  if (code.rfind("rm:", 0) == 0) {
    code.remove_prefix(3);
  }
  if (code.rfind("material.", 0) == 0) {
    code.remove_prefix(std::string_view("material.").size());
  }
  return code;
}

} // namespace

void MaterialCatalog::set_project_materials(std::vector<MaterialDef> materials) {
  ProjectMaterials& state = project_materials();
  state.definitions = std::move(materials);
  ++state.generation;
}

void MaterialCatalog::clear_project_materials() {
  ProjectMaterials& state = project_materials();
  state.definitions.clear();
  ++state.generation;
}

bool MaterialCatalog::is_project_material(std::string_view code) {
  const std::string_view name = bare_name(code);
  const ProjectMaterials& state = project_materials();
  return std::any_of(state.definitions.begin(),
                     state.definitions.end(),
                     [name](const MaterialDef& def) { return def.name == name; });
}

const MaterialDef* MaterialCatalog::find_material(std::string_view code) const {
  const std::string_view name = bare_name(code);
  // The project's definitions are consulted FIRST, so a project may shadow a
  // bundled material — the same precedence props::model() gives imported models.
  const ProjectMaterials& state = project_materials();
  for (const MaterialDef& def : state.definitions) {
    if (def.name == name) {
      return &def;
    }
  }
  for (const MaterialDef& def : materials_) {
    if (def.name == name) {
      return &def;
    }
  }
  return nullptr;
}

const std::vector<MaterialDef>& MaterialCatalog::materials() const {
  const ProjectMaterials& state = project_materials();
  if (state.definitions.empty()) {
    // Nothing to merge: hand back the built-in list itself, so the common case
    // pays nothing for the overlay existing. This is also what makes
    // "clearing restores the built-in catalog exactly" hold by construction
    // rather than by remembering to clear a cache.
    return materials_;
  }
  if (merged_generation_ != state.generation) {
    merged_ = state.definitions;
    for (const MaterialDef& def : materials_) {
      const bool shadowed =
          std::any_of(state.definitions.begin(),
                      state.definitions.end(),
                      [&def](const MaterialDef& over) { return over.name == def.name; });
      if (!shadowed) {
        merged_.push_back(def);
      }
    }
    merged_generation_ = state.generation;
  }
  return merged_;
}

} // namespace roadmaker::editor
