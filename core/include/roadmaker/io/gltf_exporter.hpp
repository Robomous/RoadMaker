// Copyright 2026 Robomous
// SPDX-License-Identifier: Apache-2.0

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
