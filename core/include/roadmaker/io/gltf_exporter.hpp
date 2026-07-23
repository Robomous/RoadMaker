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

#include "roadmaker/error.hpp"
#include "roadmaker/export.hpp"
#include "roadmaker/mesh/mesh.hpp"

#include <filesystem>

namespace roadmaker {

/// Writes the network mesh as binary glTF 2.0 (.glb).
///
/// FRAME CONVERSION: the kernel is right-handed Z-up; glTF is right-handed
/// Y-up. This exporter is the single place where the conversion happens:
/// (x, y, z)_kernel → (x, z, −y)_glTF. Units stay meters. Winding stays CCW
/// (the mapping is a proper rotation).
[[nodiscard]] RM_API Expected<void> export_glb(const NetworkMesh& mesh,
                                               const std::filesystem::path& path);

} // namespace roadmaker
