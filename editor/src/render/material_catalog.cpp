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
