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

// Reading a user's own 3D model into a prop (p6-s8, #322).
//
// WHY glTF AND NOTHING ELSE. tinygltf is already pinned and already linked on
// this library, and its read side already compiles — so glTF/GLB costs no new
// dependency at all, which is what makes it the format this sprint reads.
// ADR-0013 records the closed list and names the issue behind every refusal:
// `.obj` needs tinyobjloader pinned on its own terms (#511), USD read is out for
// v0.1.0, and `.fbx` is permanently excluded by the licensing rules.
//
// ★ A TEXTURED MODEL IMPORTS FLAT, AND SAYS SO. `props::PropPart` carries one
// flat colour and no UVs, and so does every consumer of it — the viewport draws
// all props with a single untextured material, and both exporters emit flat
// colour per part. So a primitive's baseColorTexture is DECODED, AVERAGED, and
// multiplied into baseColorFactor, with a warning naming the primitive and
// #507, which owns carrying real textures through. The obvious cheaper answer —
// use baseColorFactor and ignore the image — is not a compromise but a bug: a
// real-world GLB usually leaves the factor white and puts all its colour in the
// texture, so it would import a brown chair as a white one and report nothing.
//
// UNTRUSTED INPUT. These files come from somewhere else, so every limit below is
// stated rather than discovered, and an external image `uri` in a `.gltf` is
// resolved only inside that file's own directory (tinygltf is built with
// TINYGLTF_NO_EXTERNAL_IMAGE, so we do that read ourselves — which makes the
// traversal check ours to make). Malformed input produces an Expected error or a
// Diagnostic; it never crashes and never asserts.

#include "roadmaker/assets/prop_library.hpp"
#include "roadmaker/error.hpp"
#include "roadmaker/export.hpp"
#include "roadmaker/xodr/diagnostic.hpp"

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace roadmaker::props {

/// Upper bound on the triangles kept from one model.
///
/// In the spirit of `gis::kMaxRasterTexels` and `lidar::kMaxCloudPoints`:
/// refusing an oversized file with a stated limit is honest, where attempting it
/// is an out-of-memory crash the user reads as a defect in their file. A prop is
/// placed by the hundred and drawn in an instanced batch, so the budget is set by
/// what is sane to replicate, not by what a mesh viewer could open.
inline constexpr std::size_t kMaxPropTriangles = 200'000;

/// Upper bound on the vertices kept from one model. Positions and normals are
/// stored as doubles, so this is ~9.6 MB of geometry per model before indices.
inline constexpr std::size_t kMaxPropVertices = 200'000;

/// Upper bound on primitives-turned-parts. Each part is a separate draw call per
/// instance in the renderer and a separate glTF/USD material on export, so a
/// model split into thousands of primitives is a performance cliff, not detail.
inline constexpr std::size_t kMaxPropParts = 256;

/// Upper bound on the texels decoded from one embedded image (4096 x 4096). The
/// image is read only to average it, so nothing is gained by decoding more.
inline constexpr std::size_t kMaxPropImageTexels = 16'777'216;

/// How to read a model.
struct PropImportOptions {
  std::size_t max_triangles = kMaxPropTriangles;
  std::size_t max_vertices = kMaxPropVertices;
  std::size_t max_parts = kMaxPropParts;

  /// The OpenDRIVE class a placed instance of the imported model carries. The
  /// default is what every authoring path in the editor already uses for a point
  /// prop; `ObjectType::None` would make the model unmeshable, because
  /// `build_object_instances` filters on this.
  ObjectType type = ObjectType::Tree;
};

/// An imported model and everything that had to be said about reading it.
struct PropImportResult {
  PropModel model;
  std::vector<Diagnostic> diagnostics;
};

/// Reads a `.glb` or `.gltf` into a prop model with the given id.
///
/// The id becomes `PropModel::id`, and therefore the OpenDRIVE `<object @name>`
/// of every placed instance — so callers own making it unique and stable across
/// sessions, since that string is the only link between an object and its model.
///
/// The model is normalised to the `PropPart` contract on the way in: the frame is
/// rotated from glTF's Y-up to the kernel's Z-up, node transforms are flattened,
/// and the geometry is translated so its base sits at z = 0 with the horizontal
/// centre of its bounding box at the origin. `height` and `radius` are then
/// derived from that box, because the renderer frames from them and
/// `instance_scale` divides by `height`.
///
/// Fails (returns an error) when the file cannot be read or parsed, when it holds
/// no triangles at all, when the result would be degenerate, or when any budget
/// above is exceeded. Everything survivable is a diagnostic: a skipped
/// non-triangle primitive, a flattened texture, an unreadable image, a refused
/// `uri`, an ignored skin or animation.
[[nodiscard]] RM_API Expected<PropImportResult> import_prop_model(
    const std::filesystem::path& path, std::string id, const PropImportOptions& options = {});

/// True for the extensions `import_prop_model` accepts.
///
/// Exists so a file-dialog filter and the reader cannot disagree about what is
/// openable — the same reason `gis::is_vector_extension` and
/// `lidar::is_point_cloud_extension` do.
[[nodiscard]] RM_API bool is_prop_model_extension(const std::filesystem::path& path);

} // namespace roadmaker::props
