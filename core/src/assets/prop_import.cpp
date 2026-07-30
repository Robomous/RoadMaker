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

#include "roadmaker/assets/prop_import.hpp"

#include <fmt/format.h>

#include "../io/mesh_export_common.hpp" // from_export_frame — the boundary rotation

// tinygltf WITHOUT the implementation macro: the one implementation TU is
// core/src/io/gltf_exporter.cpp, and a second would be an ODR violation. Its
// NO_STB_IMAGE / NO_EXTERNAL_IMAGE config comes from the rm_tinygltf target, so
// this TU and that one agree about which symbols exist.
#include <tiny_gltf.h>

// stb_image WITHOUT the implementation macro, for the same reason: the one
// implementation lives in core/src/gis/world_file.cpp. These STBI_NO_* macros
// MUST mirror that TU's — they gate which declarations this file can see, so
// keeping them in step is what stops a future edit from calling a symbol that
// was never compiled.
#define STBI_NO_STDIO
#define STBI_NO_GIF
#define STBI_NO_PSD
#define STBI_NO_PIC
#define STBI_NO_PNM
#define STBI_NO_HDR
#include <fmt/ranges.h> // fmt::join over extensionsUsed

#include <stb_image.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace roadmaker::props {

namespace {

/// A column-major 4x4, glTF's own layout, so `node.matrix` copies straight in.
using Mat4 = std::array<double, 16>;

constexpr Mat4 kIdentity4 = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};

[[nodiscard]] Mat4 multiply(const Mat4& a, const Mat4& b) {
  Mat4 out{};
  for (int col = 0; col < 4; ++col) {
    for (int row = 0; row < 4; ++row) {
      double sum = 0.0;
      for (int k = 0; k < 4; ++k) {
        sum += a[(static_cast<std::size_t>(k) * 4) + static_cast<std::size_t>(row)] *
               b[(static_cast<std::size_t>(col) * 4) + static_cast<std::size_t>(k)];
      }
      out[(static_cast<std::size_t>(col) * 4) + static_cast<std::size_t>(row)] = sum;
    }
  }
  return out;
}

[[nodiscard]] std::array<double, 3> transform_point(const Mat4& m, const std::array<double, 3>& p) {
  return {(m[0] * p[0]) + (m[4] * p[1]) + (m[8] * p[2]) + m[12],
          (m[1] * p[0]) + (m[5] * p[1]) + (m[9] * p[2]) + m[13],
          (m[2] * p[0]) + (m[6] * p[1]) + (m[10] * p[2]) + m[14]};
}

/// The normal matrix: inverse-transpose of the upper-left 3x3, which is what
/// keeps normals perpendicular under a non-uniform node scale. A singular block
/// (a node scaled flat on some axis) has no inverse, so the caller is told and
/// the plain 3x3 is used — wrong-but-stated beats a NaN normal reaching the
/// renderer.
[[nodiscard]] std::optional<std::array<double, 9>> normal_matrix(const Mat4& m) {
  const std::array<double, 9> b = {m[0], m[1], m[2], m[4], m[5], m[6], m[8], m[9], m[10]};
  const double det = (b[0] * ((b[4] * b[8]) - (b[5] * b[7]))) -
                     (b[3] * ((b[1] * b[8]) - (b[2] * b[7]))) +
                     (b[6] * ((b[1] * b[5]) - (b[2] * b[4])));
  if (!std::isfinite(det) || std::abs(det) < 1e-18) {
    return std::nullopt;
  }
  const double inv = 1.0 / det;
  // adjugate(b)^T * inv == (b^-1)^T, written out to avoid a matrix class.
  return std::array<double, 9>{((b[4] * b[8]) - (b[5] * b[7])) * inv,
                               ((b[6] * b[5]) - (b[3] * b[8])) * inv,
                               ((b[3] * b[7]) - (b[6] * b[4])) * inv,
                               ((b[7] * b[2]) - (b[1] * b[8])) * inv,
                               ((b[0] * b[8]) - (b[6] * b[2])) * inv,
                               ((b[6] * b[1]) - (b[0] * b[7])) * inv,
                               ((b[1] * b[5]) - (b[4] * b[2])) * inv,
                               ((b[3] * b[2]) - (b[0] * b[5])) * inv,
                               ((b[0] * b[4]) - (b[3] * b[1])) * inv};
}

[[nodiscard]] std::array<double, 3> transform_normal(const std::array<double, 9>& n,
                                                     const std::array<double, 3>& v) {
  return {(n[0] * v[0]) + (n[3] * v[1]) + (n[6] * v[2]),
          (n[1] * v[0]) + (n[4] * v[1]) + (n[7] * v[2]),
          (n[2] * v[0]) + (n[5] * v[1]) + (n[8] * v[2])};
}

/// A node's own transform: `matrix` when present, otherwise T * R * S, exactly as
/// the glTF spec composes them.
[[nodiscard]] Mat4 local_transform(const tinygltf::Node& node) {
  if (node.matrix.size() == 16) {
    Mat4 out{};
    std::copy(node.matrix.begin(), node.matrix.end(), out.begin());
    return out;
  }
  const std::array<double, 3> t =
      node.translation.size() == 3
          ? std::array<double, 3>{node.translation[0], node.translation[1], node.translation[2]}
          : std::array<double, 3>{0.0, 0.0, 0.0};
  const std::array<double, 3> s =
      node.scale.size() == 3 ? std::array<double, 3>{node.scale[0], node.scale[1], node.scale[2]}
                             : std::array<double, 3>{1.0, 1.0, 1.0};
  // glTF stores a quaternion as (x, y, z, w).
  const std::array<double, 4> q = node.rotation.size() == 4
                                      ? std::array<double, 4>{node.rotation[0],
                                                              node.rotation[1],
                                                              node.rotation[2],
                                                              node.rotation[3]}
                                      : std::array<double, 4>{0.0, 0.0, 0.0, 1.0};
  const double x = q[0];
  const double y = q[1];
  const double z = q[2];
  const double w = q[3];
  const std::array<double, 9> r = {1.0 - (2.0 * ((y * y) + (z * z))),
                                   2.0 * ((x * y) + (z * w)),
                                   2.0 * ((x * z) - (y * w)),
                                   2.0 * ((x * y) - (z * w)),
                                   1.0 - (2.0 * ((x * x) + (z * z))),
                                   2.0 * ((y * z) + (x * w)),
                                   2.0 * ((x * z) + (y * w)),
                                   2.0 * ((y * z) - (x * w)),
                                   1.0 - (2.0 * ((x * x) + (y * y)))};
  Mat4 out = kIdentity4;
  for (std::size_t col = 0; col < 3; ++col) {
    for (std::size_t row = 0; row < 3; ++row) {
      out[(col * 4) + row] = r[(col * 3) + row] * s[col];
    }
  }
  out[12] = t[0];
  out[13] = t[1];
  out[14] = t[2];
  return out;
}

/// sRGB -> linear, the electro-optical transfer function glTF specifies for a
/// baseColorTexture. Averaging the encoded bytes directly would bias every
/// flattened colour bright, because sRGB is not proportional to light.
[[nodiscard]] double srgb_to_linear(double channel) {
  return channel <= 0.04045 ? channel / 12.92 : std::pow((channel + 0.055) / 1.055, 2.4);
}

/// Reads a whole file. Small by construction here: a `.gltf`'s sibling image.
[[nodiscard]] std::optional<std::vector<unsigned char>>
read_bytes(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return std::nullopt;
  }
  return std::vector<unsigned char>{std::istreambuf_iterator<char>(in),
                                    std::istreambuf_iterator<char>()};
}

/// Decodes encoded image bytes to RGBA8, refusing anything past the texel budget.
struct DecodedImage {
  int width = 0;
  int height = 0;
  std::vector<unsigned char> rgba;
};

[[nodiscard]] Expected<DecodedImage> decode_image(const unsigned char* bytes, std::size_t size) {
  if (bytes == nullptr || size == 0) {
    return make_error(ErrorCode::InvalidDocument, "image has no data");
  }
  int width = 0;
  int height = 0;
  int channels = 0;
  if (stbi_info_from_memory(bytes, static_cast<int>(size), &width, &height, &channels) == 0) {
    const char* reason = stbi_failure_reason();
    return make_error(ErrorCode::InvalidDocument,
                      fmt::format("image is not a format this build decodes ({})",
                                  reason != nullptr ? reason : "unknown"));
  }
  if (width <= 0 || height <= 0) {
    return make_error(ErrorCode::InvalidDocument, "image reports a non-positive size");
  }
  const std::size_t texels = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
  if (texels > kMaxPropImageTexels) {
    return make_error(ErrorCode::InvalidDocument,
                      fmt::format("image is {}x{} = {} texels, past the {} limit",
                                  width,
                                  height,
                                  texels,
                                  kMaxPropImageTexels));
  }
  unsigned char* pixels =
      stbi_load_from_memory(bytes, static_cast<int>(size), &width, &height, &channels, 4);
  if (pixels == nullptr) {
    const char* reason = stbi_failure_reason();
    return make_error(
        ErrorCode::InvalidDocument,
        fmt::format("image could not be decoded ({})", reason != nullptr ? reason : "unknown"));
  }
  DecodedImage out;
  out.width = width;
  out.height = height;
  out.rgba.assign(pixels, pixels + (texels * 4));
  stbi_image_free(pixels);
  return out;
}

/// tinygltf's image callback. Installed because TINYGLTF_NO_STB_IMAGE leaves the
/// loader null, so without this an embedded texture is simply absent.
bool load_embedded_image(tinygltf::Image* image,
                         const int image_idx,
                         std::string* err,
                         std::string* /*warn*/,
                         int /*req_width*/,
                         int /*req_height*/,
                         const unsigned char* bytes,
                         int size,
                         void* /*user_data*/) {
  Expected<DecodedImage> decoded = decode_image(bytes, static_cast<std::size_t>(size));
  if (!decoded.has_value()) {
    // NOT an error for the load as a whole: a model whose texture we cannot read
    // still has geometry, and geometry is what a prop is. The caller notices the
    // empty image and warns.
    if (err != nullptr) {
      *err += fmt::format("image[{}]: {}\n", image_idx, decoded.error().message);
    }
    return true;
  }
  image->width = decoded->width;
  image->height = decoded->height;
  image->component = 4;
  image->bits = 8;
  image->pixel_type = TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE;
  image->image = std::move(decoded->rgba);
  return true;
}

/// The mean linear-RGB colour of a decoded image, alpha-weighted so a mostly
/// transparent texture is not averaged towards its invisible parts.
[[nodiscard]] std::optional<std::array<double, 3>> average_color(const tinygltf::Image& image) {
  if (image.image.empty() || image.width <= 0 || image.height <= 0 || image.component < 3) {
    return std::nullopt;
  }
  const std::size_t stride = static_cast<std::size_t>(image.component);
  const std::size_t texels = image.image.size() / stride;
  if (texels == 0) {
    return std::nullopt;
  }
  std::array<double, 3> sum{0.0, 0.0, 0.0};
  double weight_sum = 0.0;
  for (std::size_t i = 0; i < texels; ++i) {
    const std::size_t at = i * stride;
    const double alpha = stride >= 4 ? static_cast<double>(image.image[at + 3]) / 255.0 : 1.0;
    if (alpha <= 0.0) {
      continue;
    }
    for (std::size_t c = 0; c < 3; ++c) {
      sum[c] += srgb_to_linear(static_cast<double>(image.image[at + c]) / 255.0) * alpha;
    }
    weight_sum += alpha;
  }
  if (weight_sum <= 0.0) {
    return std::nullopt;
  }
  return std::array<double, 3>{sum[0] / weight_sum, sum[1] / weight_sum, sum[2] / weight_sum};
}

/// Where an accessor's bytes are, validated against both its buffer view and its
/// buffer. Everything a malformed file can lie about is checked here, once.
struct AccessorSpan {
  const unsigned char* base = nullptr;
  std::size_t stride = 0;
  std::size_t count = 0;
};

[[nodiscard]] Expected<AccessorSpan>
accessor_span(const tinygltf::Model& gltf, int accessor_index, std::size_t element_bytes) {
  if (accessor_index < 0 || static_cast<std::size_t>(accessor_index) >= gltf.accessors.size()) {
    return make_error(ErrorCode::InvalidDocument, "accessor index is out of range");
  }
  const tinygltf::Accessor& accessor = gltf.accessors[static_cast<std::size_t>(accessor_index)];
  if (accessor.sparse.isSparse) {
    return make_error(ErrorCode::InvalidDocument, "accessor is sparse, which this reader declines");
  }
  if (accessor.bufferView < 0 ||
      static_cast<std::size_t>(accessor.bufferView) >= gltf.bufferViews.size()) {
    return make_error(ErrorCode::InvalidDocument, "accessor names no valid buffer view");
  }
  const tinygltf::BufferView& view =
      gltf.bufferViews[static_cast<std::size_t>(accessor.bufferView)];
  if (view.buffer < 0 || static_cast<std::size_t>(view.buffer) >= gltf.buffers.size()) {
    return make_error(ErrorCode::InvalidDocument, "buffer view names no valid buffer");
  }
  const std::vector<unsigned char>& data = gltf.buffers[static_cast<std::size_t>(view.buffer)].data;
  const int reported_stride = accessor.ByteStride(view);
  if (reported_stride <= 0) {
    return make_error(ErrorCode::InvalidDocument, "accessor has no usable byte stride");
  }
  const std::size_t stride = static_cast<std::size_t>(reported_stride);
  if (stride < element_bytes) {
    return make_error(ErrorCode::InvalidDocument, "accessor stride is smaller than its element");
  }
  const std::size_t start = view.byteOffset + accessor.byteOffset;
  if (accessor.count == 0) {
    return make_error(ErrorCode::InvalidDocument, "accessor is empty");
  }
  // The last element must end inside both the view and the buffer. Computed with
  // the stride so a truncated tail cannot be read past.
  const std::size_t span = ((accessor.count - 1) * stride) + element_bytes;
  if (span > view.byteLength || start > data.size() || span > data.size() - start) {
    return make_error(ErrorCode::InvalidDocument,
                      "accessor reaches past the end of its buffer view");
  }
  return AccessorSpan{.base = data.data() + start, .stride = stride, .count = accessor.count};
}

/// Reads a VEC3 of floats — the only encoding glTF permits for POSITION, and the
/// only one it permits unnormalised for NORMAL.
[[nodiscard]] Expected<std::vector<std::array<double, 3>>> read_vec3(const tinygltf::Model& gltf,
                                                                     int accessor_index) {
  if (accessor_index < 0 || static_cast<std::size_t>(accessor_index) >= gltf.accessors.size()) {
    return make_error(ErrorCode::InvalidDocument, "accessor index is out of range");
  }
  const tinygltf::Accessor& accessor = gltf.accessors[static_cast<std::size_t>(accessor_index)];
  if (accessor.type != TINYGLTF_TYPE_VEC3) {
    return make_error(ErrorCode::InvalidDocument, "attribute is not a VEC3");
  }
  if (accessor.componentType != TINYGLTF_COMPONENT_TYPE_FLOAT) {
    return make_error(ErrorCode::InvalidDocument, "attribute is not float-encoded");
  }
  Expected<AccessorSpan> span = accessor_span(gltf, accessor_index, sizeof(float) * 3);
  if (!span.has_value()) {
    return make_error(span.error().code, span.error().message);
  }
  std::vector<std::array<double, 3>> out;
  out.reserve(span->count);
  for (std::size_t i = 0; i < span->count; ++i) {
    std::array<float, 3> v{};
    std::memcpy(v.data(), span->base + (i * span->stride), sizeof(v));
    out.push_back(
        {static_cast<double>(v[0]), static_cast<double>(v[1]), static_cast<double>(v[2])});
  }
  return out;
}

/// Reads an index accessor in any of the three unsigned encodings glTF allows.
[[nodiscard]] Expected<std::vector<std::uint32_t>> read_indices(const tinygltf::Model& gltf,
                                                                int accessor_index) {
  if (accessor_index < 0 || static_cast<std::size_t>(accessor_index) >= gltf.accessors.size()) {
    return make_error(ErrorCode::InvalidDocument, "index accessor is out of range");
  }
  const tinygltf::Accessor& accessor = gltf.accessors[static_cast<std::size_t>(accessor_index)];
  std::size_t element = 0;
  switch (accessor.componentType) {
  case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE:
    element = 1;
    break;
  case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT:
    element = 2;
    break;
  case TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT:
    element = 4;
    break;
  default:
    return make_error(ErrorCode::InvalidDocument, "indices are not an unsigned integer type");
  }
  Expected<AccessorSpan> span = accessor_span(gltf, accessor_index, element);
  if (!span.has_value()) {
    return make_error(span.error().code, span.error().message);
  }
  std::vector<std::uint32_t> out;
  out.reserve(span->count);
  for (std::size_t i = 0; i < span->count; ++i) {
    const unsigned char* at = span->base + (i * span->stride);
    std::uint32_t value = 0;
    if (element == 1) {
      value = *at;
    } else if (element == 2) {
      std::uint16_t narrow = 0;
      std::memcpy(&narrow, at, sizeof(narrow));
      value = narrow;
    } else {
      std::memcpy(&value, at, sizeof(value));
    }
    out.push_back(value);
  }
  return out;
}

/// One primitive, resolved to kernel space and ready to become a `PropPart`.
struct FlatPrimitive {
  std::vector<double> positions;
  std::vector<double> normals;
  std::vector<std::uint32_t> indices;
  std::array<float, 3> color{0.7F, 0.7F, 0.7F};
  std::string name;
};

/// Per-face normals for a primitive that shipped without any, area-weighted at
/// the shared vertices. A prop with no normals would otherwise render black.
void compute_normals(const std::vector<double>& positions,
                     const std::vector<std::uint32_t>& indices,
                     std::vector<double>& out) {
  out.assign(positions.size(), 0.0);
  for (std::size_t i = 0; i + 2 < indices.size(); i += 3) {
    const std::array<std::size_t, 3> tri = {static_cast<std::size_t>(indices[i]) * 3,
                                            static_cast<std::size_t>(indices[i + 1]) * 3,
                                            static_cast<std::size_t>(indices[i + 2]) * 3};
    if (tri[0] + 2 >= positions.size() || tri[1] + 2 >= positions.size() ||
        tri[2] + 2 >= positions.size()) {
      continue;
    }
    const std::array<double, 3> u = {positions[tri[1]] - positions[tri[0]],
                                     positions[tri[1] + 1] - positions[tri[0] + 1],
                                     positions[tri[1] + 2] - positions[tri[0] + 2]};
    const std::array<double, 3> v = {positions[tri[2]] - positions[tri[0]],
                                     positions[tri[2] + 1] - positions[tri[0] + 1],
                                     positions[tri[2] + 2] - positions[tri[0] + 2]};
    const std::array<double, 3> n = {(u[1] * v[2]) - (u[2] * v[1]),
                                     (u[2] * v[0]) - (u[0] * v[2]),
                                     (u[0] * v[1]) - (u[1] * v[0])};
    for (const std::size_t base : tri) {
      out[base] += n[0];
      out[base + 1] += n[1];
      out[base + 2] += n[2];
    }
  }
  for (std::size_t i = 0; i + 2 < out.size(); i += 3) {
    const double len =
        std::sqrt((out[i] * out[i]) + (out[i + 1] * out[i + 1]) + (out[i + 2] * out[i + 2]));
    if (len > 0.0) {
      out[i] /= len;
      out[i + 1] /= len;
      out[i + 2] /= len;
    } else {
      out[i] = 0.0;
      out[i + 1] = 0.0;
      out[i + 2] = 1.0;
    }
  }
}

/// Everything accumulated while walking one file.
class ModelReader {
public:
  ModelReader(const tinygltf::Model& model,
              std::filesystem::path base_dir,
              const PropImportOptions& options)
      : model_(model), base_dir_(std::move(base_dir)), options_(options) {}

  [[nodiscard]] std::vector<Diagnostic>& diagnostics() { return diagnostics_; }

  [[nodiscard]] std::vector<FlatPrimitive>& parts() { return parts_; }

  void warn(std::string location, std::string message) {
    diagnostics_.push_back(Diagnostic{.severity = Severity::Warning,
                                      .location = std::move(location),
                                      .message = std::move(message)});
  }

  /// A primitive was dropped. Recorded separately from ordinary warnings so
  /// that when NOTHING survives, the error can name why rather than quoting
  /// whichever diagnostic happened to come first.
  void skip(const std::string& location, std::string message) {
    skipped_.push_back(message);
    warn(location, std::move(message));
  }

  [[nodiscard]] const std::vector<std::string>& skipped() const { return skipped_; }

  void note(std::string location, std::string message) {
    diagnostics_.push_back(Diagnostic{.severity = Severity::Info,
                                      .location = std::move(location),
                                      .message = std::move(message)});
  }

  /// Walks the scene graph, flattening transforms. Returns an error only for the
  /// things that make the file unusable; everything else is a diagnostic.
  [[nodiscard]] Expected<void> read() {
    report_ignored_features();
    const std::vector<int> roots = scene_roots();
    if (roots.empty() && model_.nodes.empty()) {
      // A file with meshes but no nodes is legal and some exporters emit it.
      note("scene", "the file declares no nodes; its meshes are read at the origin");
      for (std::size_t i = 0; i < model_.meshes.size(); ++i) {
        if (Expected<void> ok = read_mesh(static_cast<int>(i), kIdentity4); !ok.has_value()) {
          return ok;
        }
      }
      return {};
    }
    if (roots.empty()) {
      warn("scene", "no node is a root of the scene graph, so nothing was read");
      return {};
    }
    std::unordered_set<int> on_stack;
    for (const int root : roots) {
      if (Expected<void> ok = walk(root, kIdentity4, on_stack, 0); !ok.has_value()) {
        return ok;
      }
    }
    return {};
  }

private:
  /// The default scene's roots, or every node nothing else parents when the file
  /// declares no scene at all.
  [[nodiscard]] std::vector<int> scene_roots() const {
    if (!model_.scenes.empty()) {
      const std::size_t index =
          model_.defaultScene >= 0 &&
                  static_cast<std::size_t>(model_.defaultScene) < model_.scenes.size()
              ? static_cast<std::size_t>(model_.defaultScene)
              : 0;
      return model_.scenes[index].nodes;
    }
    std::unordered_set<int> parented;
    for (const tinygltf::Node& node : model_.nodes) {
      for (const int child : node.children) {
        parented.insert(child);
      }
    }
    std::vector<int> roots;
    for (std::size_t i = 0; i < model_.nodes.size(); ++i) {
      if (parented.find(static_cast<int>(i)) == parented.end()) {
        roots.push_back(static_cast<int>(i));
      }
    }
    return roots;
  }

  /// Named once each, not per node: a model with 200 animated nodes should not
  /// produce 200 identical warnings.
  void report_ignored_features() {
    if (!model_.animations.empty()) {
      note("animations",
           fmt::format("{} animation(s) ignored — a prop is static geometry",
                       model_.animations.size()));
    }
    if (!model_.skins.empty()) {
      note("skins",
           fmt::format("{} skin(s) ignored — the mesh is read in its bind pose",
                       model_.skins.size()));
    }
    if (!model_.cameras.empty()) {
      note("cameras", fmt::format("{} camera(s) ignored", model_.cameras.size()));
    }
    if (!model_.extensionsUsed.empty()) {
      note("extensions",
           fmt::format("glTF extension(s) not interpreted: {}",
                       fmt::join(model_.extensionsUsed, ", ")));
    }
  }

  /// Deepest node chain this reader will follow. `walk` is recursive, so an
  /// unbounded chain is a stack overflow — which a hostile file can arrange with
  /// a few hundred kilobytes of JSON. The cycle check below cannot catch it,
  /// because a long chain is not a cycle.
  static constexpr int kMaxNodeDepth = 128;

  [[nodiscard]] Expected<void>
  walk(int node_index, const Mat4& parent, std::unordered_set<int>& on_stack, int depth) {
    if (node_index < 0 || static_cast<std::size_t>(node_index) >= model_.nodes.size()) {
      warn("scene", fmt::format("node index {} is out of range and was skipped", node_index));
      return {};
    }
    if (depth > kMaxNodeDepth) {
      return make_error(ErrorCode::InvalidDocument,
                        fmt::format("the node hierarchy is deeper than {} levels, which is past "
                                    "the limit this reader states",
                                    kMaxNodeDepth));
    }
    if (!on_stack.insert(node_index).second) {
      // The spec says the node graph is a forest. A file that cycles would spin
      // here forever, so it is refused rather than truncated.
      return make_error(
          ErrorCode::InvalidDocument,
          fmt::format("node {} is its own ancestor — the node graph is cyclic", node_index));
    }
    const tinygltf::Node& node = model_.nodes[static_cast<std::size_t>(node_index)];
    const Mat4 world = multiply(parent, local_transform(node));
    if (node.mesh >= 0) {
      if (Expected<void> ok = read_mesh(node.mesh, world); !ok.has_value()) {
        return ok;
      }
    }
    for (const int child : node.children) {
      if (Expected<void> ok = walk(child, world, on_stack, depth + 1); !ok.has_value()) {
        return ok;
      }
    }
    on_stack.erase(node_index);
    return {};
  }

  [[nodiscard]] Expected<void> read_mesh(int mesh_index, const Mat4& world) {
    if (mesh_index < 0 || static_cast<std::size_t>(mesh_index) >= model_.meshes.size()) {
      warn("scene", fmt::format("mesh index {} is out of range and was skipped", mesh_index));
      return {};
    }
    const tinygltf::Mesh& mesh = model_.meshes[static_cast<std::size_t>(mesh_index)];
    for (std::size_t i = 0; i < mesh.primitives.size(); ++i) {
      const std::string where =
          fmt::format("mesh \"{}\" primitive {}", mesh.name.empty() ? "?" : mesh.name, i);
      if (parts_.size() >= options_.max_parts) {
        return make_error(ErrorCode::InvalidDocument,
                          fmt::format("model has more than {} primitives, which is past the "
                                      "limit this reader states",
                                      options_.max_parts));
      }
      if (Expected<void> ok = read_primitive(mesh.primitives[i], world, where); !ok.has_value()) {
        return ok;
      }
    }
    return {};
  }

  [[nodiscard]] Expected<void> read_primitive(const tinygltf::Primitive& primitive,
                                              const Mat4& world,
                                              const std::string& where) {
    if (primitive.mode != TINYGLTF_MODE_TRIANGLES) {
      skip(where, "primitive is not TRIANGLES and was skipped — a prop is a surface mesh");
      return {};
    }
    const auto position_at = primitive.attributes.find("POSITION");
    if (position_at == primitive.attributes.end()) {
      skip(where, "primitive has no POSITION attribute and was skipped");
      return {};
    }
    Expected<std::vector<std::array<double, 3>>> positions = read_vec3(model_, position_at->second);
    if (!positions.has_value()) {
      skip(where,
           fmt::format("POSITION could not be read and the primitive was skipped: {}",
                       positions.error().message));
      return {};
    }

    std::vector<std::uint32_t> indices;
    if (primitive.indices >= 0) {
      Expected<std::vector<std::uint32_t>> read = read_indices(model_, primitive.indices);
      if (!read.has_value()) {
        skip(where,
             fmt::format("indices could not be read and the primitive was skipped: {}",
                         read.error().message));
        return {};
      }
      indices = std::move(*read);
    } else {
      // Non-indexed draw: the vertices are already in triangle order.
      indices.resize(positions->size());
      for (std::size_t i = 0; i < indices.size(); ++i) {
        indices[i] = static_cast<std::uint32_t>(i);
      }
    }
    if (indices.size() % 3 != 0) {
      warn(where,
           fmt::format("index count {} is not a multiple of 3; the trailing {} were "
                       "dropped",
                       indices.size(),
                       indices.size() % 3));
      indices.resize(indices.size() - (indices.size() % 3));
    }
    if (indices.empty()) {
      skip(where, "primitive has no complete triangle and was skipped");
      return {};
    }
    // An index past the vertex array is the classic malformed-file case, and it
    // must be caught HERE — everything downstream indexes without checking.
    const std::uint32_t vertex_count = static_cast<std::uint32_t>(positions->size());
    for (const std::uint32_t index : indices) {
      if (index >= vertex_count) {
        skip(where,
             fmt::format("an index ({}) points past the {} vertices of the primitive, "
                         "which was skipped",
                         index,
                         vertex_count));
        return {};
      }
    }

    total_vertices_ += positions->size();
    total_triangles_ += indices.size() / 3;
    if (total_vertices_ > options_.max_vertices) {
      return make_error(ErrorCode::InvalidDocument,
                        fmt::format("model has more than {} vertices, which is past the limit "
                                    "this reader states",
                                    options_.max_vertices));
    }
    if (total_triangles_ > options_.max_triangles) {
      return make_error(ErrorCode::InvalidDocument,
                        fmt::format("model has more than {} triangles, which is past the limit "
                                    "this reader states",
                                    options_.max_triangles));
    }

    std::vector<std::array<double, 3>> normals;
    const auto normal_at = primitive.attributes.find("NORMAL");
    if (normal_at != primitive.attributes.end()) {
      Expected<std::vector<std::array<double, 3>>> read = read_vec3(model_, normal_at->second);
      if (!read.has_value()) {
        warn(where,
             fmt::format("NORMAL could not be read and was recomputed from the faces: {}",
                         read.error().message));
      } else if (read->size() != positions->size()) {
        warn(where, "NORMAL count does not match POSITION; normals were recomputed");
      } else {
        normals = std::move(*read);
      }
    }

    FlatPrimitive part;
    part.name = primitive_name(primitive);
    part.color = resolve_color(primitive, where);

    // Frame conversion happens HERE, once per vertex, through the one shared
    // definition of the boundary rotation.
    const std::optional<std::array<double, 9>> nm = normal_matrix(world);
    if (!nm.has_value()) {
      warn(where,
           "a node transform is singular (scaled flat on an axis); normals are "
           "transformed without the inverse-transpose and may be skewed");
    }
    part.positions.reserve(positions->size() * 3);
    for (const std::array<double, 3>& p : *positions) {
      const std::array<double, 3> world_p = transform_point(world, p);
      if (!std::isfinite(world_p[0]) || !std::isfinite(world_p[1]) || !std::isfinite(world_p[2])) {
        skip(where, "primitive has a non-finite vertex position and was skipped");
        return {};
      }
      const std::array<double, 3> kernel =
          io_common::from_export_frame(world_p[0], world_p[1], world_p[2]);
      part.positions.insert(part.positions.end(), kernel.begin(), kernel.end());
    }
    part.indices = std::move(indices);
    if (normals.empty()) {
      compute_normals(part.positions, part.indices, part.normals);
    } else {
      part.normals.reserve(normals.size() * 3);
      const std::array<double, 9> basis = nm.value_or(std::array<double, 9>{world[0],
                                                                            world[1],
                                                                            world[2],
                                                                            world[4],
                                                                            world[5],
                                                                            world[6],
                                                                            world[8],
                                                                            world[9],
                                                                            world[10]});
      for (const std::array<double, 3>& n : normals) {
        const std::array<double, 3> in_gltf = transform_normal(basis, n);
        const std::array<double, 3> rotated =
            io_common::from_export_frame(in_gltf[0], in_gltf[1], in_gltf[2]);
        const double len = std::sqrt((rotated[0] * rotated[0]) + (rotated[1] * rotated[1]) +
                                     (rotated[2] * rotated[2]));
        // A zero or non-finite normal renders black rather than shaded, so it is
        // replaced with +Z rather than passed through.
        const std::array<double, 3> unit =
            len > 0.0 && std::isfinite(len)
                ? std::array<double, 3>{rotated[0] / len, rotated[1] / len, rotated[2] / len}
                : std::array<double, 3>{0.0, 0.0, 1.0};
        part.normals.insert(part.normals.end(), unit.begin(), unit.end());
      }
    }
    parts_.push_back(std::move(part));
    return {};
  }

  /// The part name, which becomes the exported glTF/USD material name via
  /// `io_common::gltf_prop_material_name` — so it must never be empty.
  [[nodiscard]] std::string primitive_name(const tinygltf::Primitive& primitive) const {
    if (primitive.material >= 0 &&
        static_cast<std::size_t>(primitive.material) < model_.materials.size()) {
      const std::string& name = model_.materials[static_cast<std::size_t>(primitive.material)].name;
      if (!name.empty()) {
        return name;
      }
    }
    return fmt::format("part_{}", parts_.size());
  }

  /// baseColorFactor, multiplied by the mean of baseColorTexture when there is
  /// one — the flattening ADR-0013 records, warned per primitive with #507 named.
  [[nodiscard]] std::array<float, 3> resolve_color(const tinygltf::Primitive& primitive,
                                                   const std::string& where) {
    std::array<double, 3> color = {0.7, 0.7, 0.7};
    if (primitive.material < 0 ||
        static_cast<std::size_t>(primitive.material) >= model_.materials.size()) {
      // Not a warning: a primitive without a material is perfectly ordinary
      // (a mesh exported without one), and warning about every clean file
      // says as little as warning about none.
      note(where, "primitive names no material; it is imported in the default grey");
      return {
          static_cast<float>(color[0]), static_cast<float>(color[1]), static_cast<float>(color[2])};
    }
    const tinygltf::Material& material =
        model_.materials[static_cast<std::size_t>(primitive.material)];
    if (material.pbrMetallicRoughness.baseColorFactor.size() >= 3) {
      for (std::size_t c = 0; c < 3; ++c) {
        color[c] = material.pbrMetallicRoughness.baseColorFactor[c];
      }
    }
    const int texture_index = material.pbrMetallicRoughness.baseColorTexture.index;
    if (texture_index >= 0) {
      const std::optional<std::array<double, 3>> mean = texture_mean(texture_index, where);
      if (mean.has_value()) {
        for (std::size_t c = 0; c < 3; ++c) {
          color[c] *= (*mean)[c];
        }
        warn(where,
             fmt::format("material \"{}\" has a base-colour texture, which was flattened to its "
                         "average linear colour ({:.3f}, {:.3f}, {:.3f}) — prop meshes carry no "
                         "UVs. See #507.",
                         material.name.empty() ? "?" : material.name,
                         color[0],
                         color[1],
                         color[2]));
      }
    }
    for (double& channel : color) {
      channel = std::clamp(channel, 0.0, 1.0);
    }
    return {
        static_cast<float>(color[0]), static_cast<float>(color[1]), static_cast<float>(color[2])};
  }

  /// The mean colour of a texture, decoding an external image ourselves because
  /// TINYGLTF_NO_EXTERNAL_IMAGE means tinygltf left it alone.
  [[nodiscard]] std::optional<std::array<double, 3>> texture_mean(int texture_index,
                                                                  const std::string& where) {
    if (static_cast<std::size_t>(texture_index) >= model_.textures.size()) {
      warn(where, "base-colour texture index is out of range and was ignored");
      return std::nullopt;
    }
    const int source = model_.textures[static_cast<std::size_t>(texture_index)].source;
    if (source < 0 || static_cast<std::size_t>(source) >= model_.images.size()) {
      warn(where, "base-colour texture names no image and was ignored");
      return std::nullopt;
    }
    const std::size_t image_index = static_cast<std::size_t>(source);
    if (const auto cached = image_means_.find(image_index); cached != image_means_.end()) {
      return cached->second;
    }
    std::optional<std::array<double, 3>> mean = average_color(model_.images[image_index]);
    if (!mean.has_value()) {
      mean = external_image_mean(model_.images[image_index], where);
    }
    image_means_.emplace(image_index, mean);
    return mean;
  }

  /// Reads an image the `.gltf` references by relative path.
  ///
  /// ★ DIRECTORY-SCOPED, AND THAT CHECK IS OURS TO MAKE. tinygltf is built with
  /// TINYGLTF_NO_EXTERNAL_IMAGE, so it keeps the `uri` and loads nothing — which
  /// means nothing between a hostile file and the filesystem except this. An
  /// absolute path, a URL scheme, or any `..` that escapes the model's own
  /// directory is refused by name (ADR-0013).
  [[nodiscard]] std::optional<std::array<double, 3>>
  external_image_mean(const tinygltf::Image& image, const std::string& where) {
    if (image.uri.empty()) {
      warn(where, "base-colour texture has no image data and was ignored");
      return std::nullopt;
    }
    if (image.uri.find("://") != std::string::npos) {
      warn(where,
           fmt::format("image uri \"{}\" is a remote reference and was refused — an "
                       "import never fetches from the network",
                       image.uri));
      return std::nullopt;
    }
    const std::filesystem::path relative(image.uri);
    if (relative.is_absolute()) {
      warn(where, fmt::format("image uri \"{}\" is an absolute path and was refused", image.uri));
      return std::nullopt;
    }
    std::error_code ec;
    const std::filesystem::path root = std::filesystem::weakly_canonical(base_dir_, ec);
    const std::filesystem::path resolved =
        std::filesystem::weakly_canonical(base_dir_ / relative, ec);
    if (ec) {
      warn(where, fmt::format("image uri \"{}\" could not be resolved and was ignored", image.uri));
      return std::nullopt;
    }
    // lexically_relative starting with ".." is the escape; comparing resolved
    // paths rather than the raw uri is what makes a symlinked hop count too.
    const std::filesystem::path rel = resolved.lexically_relative(root);
    if (rel.empty() || *rel.begin() == "..") {
      warn(where,
           fmt::format("image uri \"{}\" resolves outside the model's own directory and "
                       "was refused",
                       image.uri));
      return std::nullopt;
    }
    const std::optional<std::vector<unsigned char>> bytes = read_bytes(resolved);
    if (!bytes.has_value()) {
      warn(where, fmt::format("image \"{}\" could not be opened and was ignored", image.uri));
      return std::nullopt;
    }
    Expected<DecodedImage> decoded = decode_image(bytes->data(), bytes->size());
    if (!decoded.has_value()) {
      warn(where, fmt::format("image \"{}\" was ignored: {}", image.uri, decoded.error().message));
      return std::nullopt;
    }
    tinygltf::Image staged;
    staged.width = decoded->width;
    staged.height = decoded->height;
    staged.component = 4;
    staged.image = std::move(decoded->rgba);
    return average_color(staged);
  }

  const tinygltf::Model& model_;
  std::filesystem::path base_dir_;
  PropImportOptions options_;
  std::vector<Diagnostic> diagnostics_;
  std::vector<FlatPrimitive> parts_;
  std::map<std::size_t, std::optional<std::array<double, 3>>> image_means_;
  std::vector<std::string> skipped_;
  std::size_t total_vertices_ = 0;
  std::size_t total_triangles_ = 0;
};

/// Moves the assembled parts onto the `PropPart` contract: base at z = 0, the
/// bounding box's horizontal centre at the origin. Returns height and radius, or
/// nothing when the result is degenerate.
[[nodiscard]] std::optional<std::pair<double, double>>
normalise_to_base_centre(std::vector<PropPart>& parts) {
  std::array<double, 3> lo = {std::numeric_limits<double>::max(),
                              std::numeric_limits<double>::max(),
                              std::numeric_limits<double>::max()};
  std::array<double, 3> hi = {std::numeric_limits<double>::lowest(),
                              std::numeric_limits<double>::lowest(),
                              std::numeric_limits<double>::lowest()};
  for (const PropPart& part : parts) {
    for (std::size_t i = 0; i + 2 < part.positions.size(); i += 3) {
      for (std::size_t c = 0; c < 3; ++c) {
        lo[c] = std::min(lo[c], part.positions[i + c]);
        hi[c] = std::max(hi[c], part.positions[i + c]);
      }
    }
  }
  if (lo[0] > hi[0]) {
    return std::nullopt;
  }
  const std::array<double, 3> shift = {-0.5 * (lo[0] + hi[0]), -0.5 * (lo[1] + hi[1]), -lo[2]};
  for (PropPart& part : parts) {
    for (std::size_t i = 0; i + 2 < part.positions.size(); i += 3) {
      for (std::size_t c = 0; c < 3; ++c) {
        part.positions[i + c] += shift[c];
      }
    }
  }
  const double height = hi[2] - lo[2];
  // Radius is the horizontal half-diagonal of the box, measured from the origin
  // the shift just established — the renderer frames from it, so it has to cover
  // the model rather than approximate it.
  const double radius =
      0.5 * std::sqrt(((hi[0] - lo[0]) * (hi[0] - lo[0])) + ((hi[1] - lo[1]) * (hi[1] - lo[1])));
  if (!(height > 0.0) || !(radius > 0.0) || !std::isfinite(height) || !std::isfinite(radius)) {
    return std::nullopt;
  }
  return std::pair{height, radius};
}

} // namespace

namespace {

[[nodiscard]] std::string lower_extension(const std::filesystem::path& path) {
  std::string ext = path.extension().string();
  std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return ext;
}

} // namespace

bool is_prop_model_extension(const std::filesystem::path& path) {
  const std::string ext = lower_extension(path);
  return ext == ".glb" || ext == ".gltf";
}

Expected<PropImportResult> import_prop_model(const std::filesystem::path& path,
                                             std::string id,
                                             const PropImportOptions& options) {
  if (id.empty()) {
    return make_error(ErrorCode::InvalidArgument, "an imported prop needs a non-empty id");
  }
  if (!is_prop_model_extension(path)) {
    return make_error(ErrorCode::InvalidArgument,
                      fmt::format("\"{}\" is not a format RoadMaker reads as a prop — glTF "
                                  "(.gltf) and binary glTF (.glb) are. OBJ needs a new pinned "
                                  "dependency (#511); USD read and FBX are out (ADR-0013).",
                                  path.extension().string()),
                      path.string());
  }
  std::error_code ec;
  if (!std::filesystem::is_regular_file(path, ec)) {
    return make_error(ErrorCode::FileNotFound, "model file does not exist", path.string());
  }

  tinygltf::TinyGLTF loader;
  // TINYGLTF_NO_STB_IMAGE leaves the loader null, so without this an embedded
  // texture is simply invisible to us rather than reported.
  loader.SetImageLoader(load_embedded_image, nullptr);
  tinygltf::Model gltf;
  std::string err;
  std::string warn;
  const bool binary = lower_extension(path) == ".glb";
  const bool loaded = binary ? loader.LoadBinaryFromFile(&gltf, &err, &warn, path.string())
                             : loader.LoadASCIIFromFile(&gltf, &err, &warn, path.string());
  if (!loaded) {
    return make_error(ErrorCode::InvalidDocument,
                      err.empty() ? "the file is not readable as glTF" : err,
                      path.string());
  }

  const std::filesystem::path base_dir =
      path.has_parent_path() ? path.parent_path() : std::filesystem::path(".");
  ModelReader reader(gltf, base_dir, options);
  // tinygltf's own complaints are forwarded rather than swallowed: a file that
  // loaded with warnings is a file the user should hear about.
  if (!warn.empty()) {
    reader.warn(path.filename().string(), warn);
  }
  if (!err.empty()) {
    reader.warn(path.filename().string(), err);
  }
  if (Expected<void> ok = reader.read(); !ok.has_value()) {
    return make_error(ok.error().code, ok.error().message, path.string());
  }
  if (reader.parts().empty()) {
    // ★ THE REASON HAS TO TRAVEL WITH THE FAILURE. `import_prop_model` returns
    // either a result OR an error, so on failure the diagnostics explaining what
    // was skipped are discarded — and "no triangle geometry" on its own tells a
    // user nothing about the NaN vertex or the out-of-range index that actually
    // emptied the file. So the first warning is folded into the message.
    const std::string reason = reader.skipped().empty() ? std::string{} : reader.skipped().front();
    return make_error(ErrorCode::InvalidDocument,
                      reason.empty()
                          ? "the file holds no triangle geometry, so there is no prop to make "
                            "of it"
                          : fmt::format("the file holds no usable triangle geometry, so there "
                                        "is no prop to make of it — every primitive was "
                                        "skipped, the first because: {}",
                                        reason),
                      path.string());
  }

  PropModel out;
  out.id = std::move(id);
  out.type = options.type;
  out.parts.reserve(reader.parts().size());
  for (FlatPrimitive& flat : reader.parts()) {
    out.parts.push_back(PropPart{.positions = std::move(flat.positions),
                                 .normals = std::move(flat.normals),
                                 .indices = std::move(flat.indices),
                                 .color = flat.color,
                                 .name = std::move(flat.name)});
  }
  const std::optional<std::pair<double, double>> extent = normalise_to_base_centre(out.parts);
  if (!extent.has_value()) {
    return make_error(ErrorCode::InvalidDocument,
                      "the model has no measurable size — a prop needs a positive height and "
                      "radius, because the renderer frames from them and per-instance sizing "
                      "divides by the height",
                      path.string());
  }
  out.height = extent->first;
  out.radius = extent->second;

  PropImportResult result{.model = std::move(out), .diagnostics = std::move(reader.diagnostics())};
  result.diagnostics.push_back(
      Diagnostic{.severity = Severity::Info,
                 .location = path.filename().string(),
                 .message = fmt::format("imported {} part(s), {:.2f} m tall, {:.2f} m radius",
                                        result.model.parts.size(),
                                        result.model.height,
                                        result.model.radius)});
  return result;
}

} // namespace roadmaker::props
