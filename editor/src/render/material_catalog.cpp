#include "render/material_catalog.hpp"

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
}

const MaterialDef* MaterialCatalog::find_material(std::string_view code) const {
  // Accept rm:<name> / <name> / material.<name>.
  if (code.rfind("rm:", 0) == 0) {
    code.remove_prefix(3);
  } else if (code.rfind("material.", 0) == 0) {
    code.remove_prefix(std::string_view("material.").size());
  }
  for (const MaterialDef& def : materials_) {
    if (def.name == code) {
      return &def;
    }
  }
  return nullptr;
}

} // namespace roadmaker::editor
