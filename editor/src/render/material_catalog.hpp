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

#pragma once

// Compiled-in material catalog (p6-s3 / #237): maps a surface code to the
// texture qrc paths + PBR-lite params the renderer needs, and the nominal
// friction the editor authors into a <material> record.
//
// This is Qt-free and GL-free — it hands out qrc path STRINGS and plain floats;
// the viewport owns the actual texture uploads. Deviation from design doc §2:
// the definitions live in C++ rather than the manifest-v2 `materials[]` block
// (deferred). `assets/library/manifest.json` stays v1; a material there is just
// a create-intent whose `material` field is one of these catalog names.

#include <array>
#include <string>
#include <string_view>
#include <vector>

namespace roadmaker::editor {

/// One material definition. Absent map paths fall back the same way the shader
/// does: no albedo → flat mesh color, no normal → geometric normal, no
/// roughness map → the scalar `roughness_value`.
struct MaterialDef {
  std::string name;       ///< canonical id, e.g. "asphalt_worn"
  std::string albedo;     ///< qrc path or empty
  std::string normal;     ///< qrc path or empty
  std::string roughness;  ///< qrc path or empty
  float uv_scale = 0.25F; ///< texels per meter
  std::array<float, 4> tint{1.0F, 1.0F, 1.0F, 1.0F};
  float roughness_value = 0.8F; ///< scalar fallback / multiplier
  float normal_strength = 1.0F;
  double friction = 0.9; ///< nominal, written into <material>
};

/// The built-in material definitions and the resolver over them.
class MaterialCatalog {
public:
  MaterialCatalog();

  /// Resolves a surface code to its definition, accepting the three spellings
  /// `rm:<name>`, `<name>`, and `material.<name>` (the library key form).
  /// Returns nullptr for an unknown code.
  [[nodiscard]] const MaterialDef* find_material(std::string_view code) const;

  [[nodiscard]] const std::vector<MaterialDef>& materials() const { return materials_; }

private:
  std::vector<MaterialDef> materials_;
};

} // namespace roadmaker::editor
