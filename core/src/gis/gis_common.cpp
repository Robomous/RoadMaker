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

#include "gis_common.hpp"

#include "roadmaker/gis/layer.hpp"

#include <fmt/format.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>
#include <system_error>
#include <vector>

namespace roadmaker::gis {

namespace {

/// The extension spellings each loader claims. Kept beside the dispatch below
/// so `is_*_extension` and `load_gis_*` cannot drift apart — the editor's file
/// dialog filters are built from these, and a filter that offers a file the
/// reader then refuses is a bug the user experiences as randomness.
constexpr std::array<std::string_view, 3> kVectorExtensions{".geojson", ".json", ".shp"};
constexpr std::array<std::string_view, 5> kRasterExtensions{
    ".tif", ".tiff", ".png", ".jpg", ".jpeg"};

} // namespace

Expected<std::string> read_file_bytes(const std::filesystem::path& path) {
  std::error_code ec;
  if (!std::filesystem::exists(path, ec) || ec) {
    return make_error(ErrorCode::FileNotFound, "no such file", path.string());
  }
  std::ifstream stream(path, std::ios::binary);
  if (!stream) {
    return make_error(ErrorCode::IoFailure, "could not be opened for reading", path.string());
  }
  std::string bytes((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
  if (stream.bad()) {
    return make_error(ErrorCode::IoFailure, "could not be read", path.string());
  }
  return bytes;
}

std::optional<std::filesystem::path> sibling(const std::filesystem::path& path,
                                             std::string_view extension) {
  std::error_code ec;
  std::filesystem::path candidate = path;
  candidate.replace_extension(std::filesystem::path(extension));
  if (std::filesystem::exists(candidate, ec) && !ec) {
    return candidate;
  }
  // Shapefile sets in the wild routinely mix `.shp` with `.PRJ`. On a
  // case-sensitive filesystem the lower-case probe alone silently loses the
  // CRS, which then shows up as an import in the wrong place.
  std::string upper(extension);
  std::transform(upper.begin(), upper.end(), upper.begin(), [](unsigned char c) {
    return static_cast<char>(std::toupper(c));
  });
  candidate = path;
  candidate.replace_extension(std::filesystem::path(upper));
  if (std::filesystem::exists(candidate, ec) && !ec) {
    return candidate;
  }
  return std::nullopt;
}

std::string lower_extension(const std::filesystem::path& path) {
  std::string extension = path.extension().string();
  std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return extension;
}

void recompute_bounds(GisVectorLayer& layer) {
  if (layer.features.empty()) {
    layer.bounds = {0.0, 0.0, 0.0, 0.0};
    return;
  }
  double min_x = std::numeric_limits<double>::infinity();
  double min_y = std::numeric_limits<double>::infinity();
  double max_x = -std::numeric_limits<double>::infinity();
  double max_y = -std::numeric_limits<double>::infinity();
  bool any = false;
  for (const GisFeature& feature : layer.features) {
    for (const std::array<double, 2>& v : feature.vertices) {
      min_x = std::min(min_x, v[0]);
      min_y = std::min(min_y, v[1]);
      max_x = std::max(max_x, v[0]);
      max_y = std::max(max_y, v[1]);
      any = true;
    }
  }
  layer.bounds = any ? std::array<double, 4>{min_x, min_y, max_x, max_y}
                     : std::array<double, 4>{0.0, 0.0, 0.0, 0.0};
}

Expected<void> enforce_vertex_budget(const GisVectorLayer& layer, std::string_view source_name) {
  std::size_t total = 0;
  for (const GisFeature& feature : layer.features) {
    total += feature.vertices.size();
  }
  if (total > kMaxVectorVertices) {
    return make_error(
        ErrorCode::InvalidArgument,
        fmt::format("holds {} vertices, past the {} this build imports — clip or simplify the "
                    "layer before importing it",
                    total,
                    kMaxVectorVertices),
        std::string(source_name));
  }
  return {};
}

Expected<void>
enforce_texel_budget(std::size_t width, std::size_t height, std::string_view source_name) {
  // Multiply in a form that cannot overflow before it is compared: a header can
  // declare 4-billion-square, and computing the product first would wrap to a
  // small number that passes.
  if (width != 0 && height > kMaxRasterTexels / width) {
    return make_error(
        ErrorCode::InvalidArgument,
        fmt::format("is {}×{}, past the {} texels this build imports — downsample or crop it first",
                    width,
                    height,
                    kMaxRasterTexels),
        std::string(source_name));
  }
  return {};
}

std::string read_sibling_prj(const std::filesystem::path& path) {
  const std::optional<std::filesystem::path> prj = sibling(path, ".prj");
  if (!prj.has_value()) {
    return {};
  }
  const Expected<std::string> text = read_file_bytes(*prj);
  if (!text.has_value()) {
    return {};
  }
  std::string value = *text;
  while (!value.empty() &&
         (std::isspace(static_cast<unsigned char>(value.back())) != 0 || value.back() == '\0')) {
    value.pop_back();
  }
  return value;
}

bool is_vector_extension(const std::filesystem::path& path) {
  const std::string extension = lower_extension(path);
  return std::find(kVectorExtensions.begin(), kVectorExtensions.end(), extension) !=
         kVectorExtensions.end();
}

bool is_raster_extension(const std::filesystem::path& path) {
  const std::string extension = lower_extension(path);
  return std::find(kRasterExtensions.begin(), kRasterExtensions.end(), extension) !=
         kRasterExtensions.end();
}

Expected<GisVectorParseResult> load_gis_vector(const std::filesystem::path& path) {
  const std::string extension = lower_extension(path);
  if (extension == ".shp") {
    return load_shapefile(path);
  }
  if (extension == ".geojson" || extension == ".json") {
    const Expected<std::string> bytes = read_file_bytes(path);
    if (!bytes.has_value()) {
      return make_error(bytes.error().code, bytes.error().message, bytes.error().context);
    }
    return parse_geojson(*bytes, path.filename().string());
  }
  return make_error(
      ErrorCode::InvalidArgument,
      fmt::format("\"{}\" is not a vector format this build reads — supported: GeoJSON "
                  "(.geojson, .json) and ESRI Shapefile (.shp); see issue #486",
                  extension),
      path.filename().string());
}

Expected<GisRasterParseResult> load_gis_raster(const std::filesystem::path& path) {
  const std::string extension = lower_extension(path);
  if (extension == ".tif" || extension == ".tiff") {
    return load_geotiff(path);
  }
  if (extension == ".png" || extension == ".jpg" || extension == ".jpeg") {
    return load_world_filed_image(path);
  }
  return make_error(
      ErrorCode::InvalidArgument,
      fmt::format("\"{}\" is not a raster format this build reads — supported: GeoTIFF (.tif, "
                  ".tiff) and world-filed PNG/JPEG; see issue #484",
                  extension),
      path.filename().string());
}

} // namespace roadmaker::gis
