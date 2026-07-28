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

// PNG/JPEG imagery georeferenced by a sibling world file.
//
// The world file is the oldest and simplest georeferencing convention there is:
// six numbers, one per line, beside the image. It earns its place here because
// it is what tile downloaders and screenshot-based workflows produce, and
// because it costs nothing — `stb_image` was ALREADY a pinned dependency (for
// stb_truetype's sign faces) with its image decoder simply unused.
//
// Extension convention: the world file takes the image's extension with the
// middle letters dropped and a `w` appended (`.png` -> `.pgw`, `.jpg` ->
// `.jgw`), or the universal `.wld`.

#include "roadmaker/gis/layer.hpp"

#include <fmt/format.h>

#include "../road/proj_string.hpp"
#include "gis_common.hpp"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO // we read the bytes ourselves, so paths stay std::filesystem
#define STBI_NO_GIF   // animation has no meaning as a georeferenced backdrop
#define STBI_NO_PSD
#define STBI_NO_PIC
#define STBI_NO_PNM
#define STBI_NO_HDR
#include <stb_image.h>

#include <array>
#include <cctype>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace roadmaker::gis {

namespace {

/// The world-file extensions to try for an image extension, in order.
std::vector<std::string> world_file_extensions(const std::string& image_extension) {
  std::vector<std::string> candidates;
  // ".png" -> ".pgw": first letter, third letter, 'w'.
  if (image_extension.size() >= 4) {
    candidates.push_back(std::string{'.', image_extension[1], image_extension[3], 'w'});
  }
  if (image_extension == ".jpeg") {
    candidates.emplace_back(".jgw");
  }
  if (image_extension == ".tiff" || image_extension == ".tif") {
    candidates.emplace_back(".tfw");
  }
  candidates.emplace_back(".wld");
  return candidates;
}

/// Six numbers, whitespace-separated, in the canonical order:
///   A  x-scale        D  y-skew
///   B  x-skew         E  y-scale (normally negative)
///   C  x of the CENTRE of the top-left pixel
///   F  y of the CENTRE of the top-left pixel
///
/// Note the file's own line order is A, D, B, E, C, F — the skews are
/// interleaved, which is a classic place to transpose two values and get an
/// image that is subtly sheared rather than obviously wrong.
std::optional<std::array<double, 6>> parse_world_file(std::string_view text) {
  std::array<double, 6> values{};
  std::size_t found = 0;
  std::size_t pos = 0;
  while (found < 6 && pos < text.size()) {
    while (pos < text.size() && (std::isspace(static_cast<unsigned char>(text[pos])) != 0)) {
      ++pos;
    }
    const std::size_t start = pos;
    while (pos < text.size() && (std::isspace(static_cast<unsigned char>(text[pos])) == 0)) {
      ++pos;
    }
    if (pos == start) {
      break;
    }
    const std::optional<double> value = proj_detail::parse_double(text.substr(start, pos - start));
    if (!value.has_value()) {
      return std::nullopt;
    }
    values[found] = *value;
    ++found;
  }
  if (found < 6) {
    return std::nullopt;
  }
  // File order A, D, B, E, C, F -> struct order A, D, B, E, C, F. Identical by
  // construction; spelled out so the mapping is auditable rather than implied.
  return std::array<double, 6>{values[0], values[1], values[2], values[3], values[4], values[5]};
}

} // namespace

Expected<GisRasterParseResult> load_world_filed_image(const std::filesystem::path& path) {
  const std::string source_name = path.filename().string();
  const std::string extension = lower_extension(path);

  std::optional<std::filesystem::path> world;
  for (const std::string& candidate : world_file_extensions(extension)) {
    world = sibling(path, candidate);
    if (world.has_value()) {
      break;
    }
  }
  if (!world.has_value()) {
    return make_error(
        ErrorCode::InvalidDocument,
        fmt::format("no world file beside the image, so it does not say where on the earth it is "
                    "— expected {} or a .wld file with the same name",
                    world_file_extensions(extension).front()),
        source_name);
  }

  const Expected<std::string> world_text = read_file_bytes(*world);
  if (!world_text.has_value()) {
    return make_error(
        world_text.error().code, world_text.error().message, world_text.error().context);
  }
  const std::optional<std::array<double, 6>> transform = parse_world_file(*world_text);
  if (!transform.has_value()) {
    return make_error(ErrorCode::InvalidDocument,
                      "the world file does not hold six numbers",
                      world->filename().string());
  }

  const Expected<std::string> bytes = read_file_bytes(path);
  if (!bytes.has_value()) {
    return make_error(bytes.error().code, bytes.error().message, bytes.error().context);
  }

  int width = 0;
  int height = 0;
  int channels = 0;
  // Ask for RGBA regardless of the source's channel count: the render boundary
  // takes RGBA8 and nothing else, and stb expands greyscale for free.
  unsigned char* pixels = stbi_load_from_memory(reinterpret_cast<const stbi_uc*>(bytes->data()),
                                                static_cast<int>(bytes->size()),
                                                &width,
                                                &height,
                                                &channels,
                                                4);
  if (pixels == nullptr) {
    const char* reason = stbi_failure_reason();
    return make_error(ErrorCode::InvalidDocument,
                      fmt::format("the image could not be decoded{}{}",
                                  reason == nullptr ? "" : ": ",
                                  reason == nullptr ? "" : reason),
                      source_name);
  }

  GisRasterParseResult result;
  GisRaster& raster = result.raster;

  if (const Expected<void> capped = enforce_texel_budget(
          static_cast<std::size_t>(width), static_cast<std::size_t>(height), source_name);
      !capped.has_value()) {
    stbi_image_free(pixels);
    return make_error(capped.error().code, capped.error().message, capped.error().context);
  }

  raster.width = width;
  raster.height = height;
  const std::size_t texels = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
  raster.rgba.assign(pixels, pixels + (texels * 4));
  stbi_image_free(pixels);

  raster.transform = *transform;
  raster.crs = read_sibling_prj(path);
  if (raster.crs.empty()) {
    result.diagnostics.push_back(Diagnostic{
        .severity = Severity::Warning,
        .location = source_name,
        .message = "no .prj file beside the image, so it does not state its coordinate reference "
                   "system; its world file is read as already being in the scene's frame"});
  }

  result.diagnostics.push_back(Diagnostic{
      .severity = Severity::Info,
      .location = source_name,
      .message = fmt::format(
          "read as a {}×{} image positioned by {}", width, height, world->filename().string())});
  return result;
}

} // namespace roadmaker::gis
