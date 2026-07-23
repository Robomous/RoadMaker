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

/// Writes the network mesh as OpenUSD ASCII (.usda).
///
/// Scene model (docs/design/m2/04_usd_export.md): a `World` Xform holding one
/// Xform per road, each wrapping a `lanesection0` Xform whose children are the
/// lane-surface and lane-marking `Mesh` prims; junction surfaces are `Mesh`
/// prims under `World`. Materials are `UsdPreviewSurface` prims under a `Looks`
/// scope, named identically to the glTF exporter's materials and bound per mesh
/// via `MaterialBindingAPI`. Stage metadata: `upAxis = "Y"`, `metersPerUnit =
/// 1`, `defaultPrim = "World"`.
///
/// FRAME CONVERSION: kernel is right-handed Z-up; USD here is right-handed
/// Y-up. Conversion happens through the shared exporter boundary
/// (io_common::to_export_frame): (x, y, z) → (x, z, −y). Units stay meters.
///
/// USDA (ASCII) only — `.usdc`/`.usdz` crate output is a documented M2
/// limitation (tinyusdz has no crate writer). The backend is isolated so it can
/// be swapped for OpenUSD later without touching callers.
///
/// Only built when the kernel is configured with `RM_BUILD_USD=ON`; consumers
/// gate on the `RM_HAVE_USD` compile definition.
[[nodiscard]] RM_API Expected<void> export_usda(const NetworkMesh& mesh,
                                                const std::filesystem::path& path);

} // namespace roadmaker
