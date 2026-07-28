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

// Imported GIS reference data: what a vector or raster file becomes once read
// (p7-s2, #242).
//
// THESE ARE PLAIN DATA, NOT ARENA ENTITIES, and deliberately so. Imported
// imagery and vectors are authoring reference — a backdrop to trace over — and
// per ADR-0008 that is Layer-2 state which never enters the `.xodr`. The kernel
// reads the files and reprojects them; the editor owns which layers a scene
// has. The ONE exception is an elevation raster, which becomes a `HeightField`
// on the network through the existing `edit::set_terrain_field` and is
// therefore real, undoable, persisted scene content.
//
// SUPPORTED FORMATS are a closed list (ADR-0010). Anything else is refused by
// name, with the issue that would lift the limitation cited in the message.

#include "roadmaker/error.hpp"
#include "roadmaker/export.hpp"
#include "roadmaker/road/terrain.hpp"
#include "roadmaker/xodr/diagnostic.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace roadmaker::gis {

/// Upper bounds, in the spirit of `kMaxFieldSamples`. A GIS file can legally
/// describe a continent at centimetre resolution; refusing that with a stated
/// limit is honest, where attempting it is an out-of-memory crash blamed on the
/// user's file.
inline constexpr std::size_t kMaxRasterTexels = 64ULL * 1024ULL * 1024ULL; ///< 8192² RGBA ≈ 256 MB
inline constexpr std::size_t kMaxVectorVertices = 8ULL * 1024ULL * 1024ULL;

/// One imported vector feature, already in the scene's frame once it has been
/// through `reproject`.
struct GisFeature {
  enum class Geometry : std::uint8_t {
    Point,
    Line,
    Polygon,
  };

  Geometry geometry = Geometry::Line;

  /// Vertex ring(s), x/y pairs. A Point has one vertex; a Line one open run; a
  /// Polygon one or more closed rings (outer first, then holes).
  std::vector<std::array<double, 2>> vertices;

  /// Start index of each ring in `vertices`. Always begins with 0. A single-run
  /// geometry has exactly one entry, so callers never special-case.
  std::vector<std::size_t> ring_starts;

  /// A label if the source carried an obvious one (shapefile `.dbf` NAME field,
  /// GeoJSON `properties.name`); empty otherwise. Never load-bearing.
  std::string name;
};

/// A vector layer as read. `crs` is the source's own description, verbatim, so
/// a refusal can name it and the editor can show what the file claimed.
struct GisVectorLayer {
  std::string crs;
  std::vector<GisFeature> features;
  std::array<double, 4> bounds{}; ///< {min_x, min_y, max_x, max_y}

  [[nodiscard]] bool empty() const { return features.empty(); }
};

/// A raster as read, before reprojection.
///
/// `rgba` and `band` are mutually exclusive: imagery fills `rgba` (4 bytes per
/// texel, row-major, TOP row first, which is TIFF's and PNG's own order), while
/// a single-band elevation raster fills `band` with one float per sample and
/// sets `elevation`. Keeping them apart rather than packing height into a
/// colour channel is what lets an elevation import stay lossless.
struct GisRaster {
  int width = 0;
  int height = 0;
  std::vector<std::uint8_t> rgba;
  std::vector<float> band;
  bool elevation = false;

  /// Value meaning "no data" in `band`, when the source declared one.
  /// Samples equal to it are holes, not heights.
  std::optional<double> nodata;

  /// Affine map from pixel (column, row) centres to source-CRS coordinates, in
  /// world-file order: {A, D, B, E, C, F}, i.e.
  ///   x = A*col + B*row + C
  ///   y = D*col + E*row + F
  /// `E` is normally negative because raster rows run north to south while the
  /// CRS's y axis runs south to north.
  std::array<double, 6> transform{1.0, 0.0, 0.0, -1.0, 0.0, 0.0};

  std::string crs;

  [[nodiscard]] bool empty() const { return width <= 0 || height <= 0; }

  [[nodiscard]] std::size_t texel_count() const {
    return static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
  }
};

/// Parse results carry their diagnostics, mirroring `TerrainParseResult`: a
/// reader that skipped a feature, coerced a value, or ignored a tag says so.
/// Nothing is ever dropped silently.
struct GisVectorParseResult {
  GisVectorLayer layer;
  std::vector<Diagnostic> diagnostics;
};

struct GisRasterParseResult {
  GisRaster raster;
  std::vector<Diagnostic> diagnostics;
};

/// Reads a vector file, dispatching on extension: `.geojson`/`.json` (RFC 7946)
/// and `.shp` (ESRI Shapefile, reading the sibling `.dbf` and `.prj` when
/// present). Fails outright only when the file is unreadable or structurally
/// not the format its extension claims; a file with some unusable features
/// imports the rest and warns about each.
[[nodiscard]] RM_API Expected<GisVectorParseResult>
load_gis_vector(const std::filesystem::path& path);

/// Reads a raster file: `.tif`/`.tiff` (GeoTIFF), or `.png`/`.jpg`/`.jpeg`
/// georeferenced by a sibling world file (`.pgw`/`.jgw`/`.wld`) and `.prj`.
///
/// An image with no georeferencing at all is an error, not a warning: placing
/// it would mean inventing a position, and this whole module exists to not do
/// that.
[[nodiscard]] RM_API Expected<GisRasterParseResult>
load_gis_raster(const std::filesystem::path& path);

/// True when the extension is one `load_gis_vector`/`load_gis_raster` handles.
/// Used by the editor's file dialogs so the filter and the reader cannot
/// disagree about what is importable.
[[nodiscard]] RM_API bool is_vector_extension(const std::filesystem::path& path);
[[nodiscard]] RM_API bool is_raster_extension(const std::filesystem::path& path);

} // namespace roadmaker::gis
