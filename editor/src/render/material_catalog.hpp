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
// This is Qt-free and GL-free — it hands out path STRINGS and plain floats; the
// viewport owns the actual texture uploads. The paths are qrc aliases for the
// bundled definitions and ordinary absolute filesystem paths for a project's
// imported ones — ViewportWidget::texture_for QImage-decodes either, so nothing
// in the renderer had to change for import (p6-s8, #322).
//
// The manifest-v2 `materials[]` block design doc §2 specified is no longer
// deferred: it ships in p6-s8 as the PROJECT overlay over these compiled-in
// definitions. The bundled five stay in C++ because they are keyed to qrc
// textures that ship inside the binary.

#include <array>
#include <cstdint>
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

  /// The merged list: the open project's definitions, then the built-in ones
  /// (minus any a project shadows). Rebuilt only when the overlay changes.
  [[nodiscard]] const std::vector<MaterialDef>& materials() const;

  /// The built-in definitions alone, without any project overlay.
  [[nodiscard]] const std::vector<MaterialDef>& builtin_materials() const { return materials_; }

  /// Installs the open project's material definitions, REPLACING any previous
  /// set; pair with `clear_project_materials()` on project close.
  ///
  /// ★ THIS IS PROCESS-WIDE STATE, ON PURPOSE (ADR-0013). `MaterialCatalog` is
  /// constructed ad hoc in eight places — the viewport, the Library model, the
  /// Attributes pane twice, MainWindow, two marking tools, and a STACK LOCAL
  /// deep inside `library_drop.cpp`'s free function. A per-instance overlay
  /// would be invisible to most of them, and threading one owner through all
  /// eight is a refactor this sprint deliberately did not take on. The invariant
  /// that makes it safe — a project switch replaces the overlay wholesale, and
  /// clearing restores the built-in catalog exactly — is tested.
  static void set_project_materials(std::vector<MaterialDef> materials);

  /// Drops the project overlay, restoring the built-in catalog exactly.
  static void clear_project_materials();

  /// True when `code` resolves to a project material rather than a built-in one.
  [[nodiscard]] static bool is_project_material(std::string_view code);

private:
  std::vector<MaterialDef> materials_;

  /// Merged view of the project overlay + `materials_`, rebuilt lazily whenever
  /// the process-wide overlay generation moves. Per instance rather than static
  /// because nothing here may assume every catalog was built the same way.
  mutable std::vector<MaterialDef> merged_;
  mutable std::uint64_t merged_generation_ = 0;
};

} // namespace roadmaker::editor
