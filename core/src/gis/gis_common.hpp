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

// Shared internals of the GIS readers. PRIVATE to core — the public surface is
// `gis/layer.hpp`.

#include "roadmaker/error.hpp"
#include "roadmaker/gis/layer.hpp"
#include "roadmaker/xodr/diagnostic.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace roadmaker::gis {

/// Reads a whole file as bytes. Binary mode everywhere, including for the text
/// formats: a `.prj` or `.geojson` written on Windows must read identically
/// here, and the parsers tolerate CR themselves.
[[nodiscard]] Expected<std::string> read_file_bytes(const std::filesystem::path& path);

/// The sibling file with the same stem and a different extension, if it exists.
/// Tries the given case and its upper-case twin, because shapefile sets in the
/// wild routinely mix `.shp` with `.PRJ` and a case-sensitive filesystem would
/// otherwise silently lose the CRS.
[[nodiscard]] std::optional<std::filesystem::path> sibling(const std::filesystem::path& path,
                                                           std::string_view extension);

/// Lower-cased extension including the dot, e.g. ".geojson".
[[nodiscard]] std::string lower_extension(const std::filesystem::path& path);

/// Recomputes `layer.bounds` from its features. An empty layer keeps a
/// zeroed box rather than an inverted one, so callers can compare without
/// special-casing.
void recompute_bounds(GisVectorLayer& layer);

/// Refuses a layer past `kMaxVectorVertices`, naming the limit. A stated
/// refusal is honest; attempting it is an out-of-memory crash the user reads as
/// a defect in their file.
[[nodiscard]] Expected<void> enforce_vertex_budget(const GisVectorLayer& layer,
                                                   std::string_view source_name);

/// Refuses a raster past `kMaxRasterTexels`, naming the limit.
[[nodiscard]] Expected<void>
enforce_texel_budget(std::size_t width, std::size_t height, std::string_view source_name);

/// Reads the CRS a sibling `.prj` declares, or an empty string when there is
/// none. Never fails: a missing `.prj` is a legitimate state that the caller
/// turns into a warning with format-specific wording.
[[nodiscard]] std::string read_sibling_prj(const std::filesystem::path& path);

/// The CRS a GeoTIFF GeoKey directory describes, as a string `parse_crs`
/// understands, or empty when it names none.
///
/// `keys` is the flat uint16 array itself: a 4-value header (version, revision,
/// minor, key count) then 4 values per key (id, tiff_tag_location, count,
/// value_or_offset). When tiff_tag_location is 0, `value_or_offset` IS the
/// value — the only case a supported CRS ever takes, since EPSG codes are plain
/// shorts.
///
/// Takes the array rather than a `TIFF*` because a GeoTIFF is not the only
/// place this directory appears: LAS carries the identical structure in its
/// `LASF_Projection` record 34735 (p7-s3, #243). One reader, so a GeoTIFF and a
/// lidar tile describing the same CRS can never disagree about it.
[[nodiscard]] std::string crs_from_geokey_directory(std::span<const std::uint16_t> keys,
                                                    std::vector<Diagnostic>& diagnostics,
                                                    std::string_view source_name);

// --- Format entry points, one per source file -----------------------------

[[nodiscard]] Expected<GisVectorParseResult> parse_geojson(std::string_view text,
                                                           std::string_view source_name);

[[nodiscard]] Expected<GisVectorParseResult> load_shapefile(const std::filesystem::path& path);

[[nodiscard]] Expected<GisRasterParseResult> load_geotiff(const std::filesystem::path& path);

/// PNG/JPEG pixels plus a world file and `.prj` for georeferencing.
[[nodiscard]] Expected<GisRasterParseResult>
load_world_filed_image(const std::filesystem::path& path);

} // namespace roadmaker::gis
