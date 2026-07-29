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

// Lidar point clouds: ASPRS LAS and LAZ as authoring reference and as
// ground-fitting input (p7-s3, #243).
//
// WHY THIS IS RoadMaker'S OWN READER AND NOT PDAL. The issue names PDAL; ADR-0011
// records why it is not taken. PDAL's cmake reads `find_package(PROJ 9.0
// REQUIRED CONFIG)` and `find_package(GDAL CONFIG REQUIRED)` — PROJ is not an
// optional feature to switch off, because PDAL delegates all spatial-reference
// handling to GDAL. Taking it would take the ~9 MB proj.db delivery problem
// ADR-0010 declined, as a rider on a feature sprint. LAS is a fixed-width binary
// format with a published layout, and its CRS travels as GeoTIFF GeoKeys or an
// OGC WKT string — both of which `roadmaker::gis` already reads. So the reader
// lives here and the CRS work is reused rather than duplicated: the refusal
// wording is identical across every P7 importer because it comes from the same
// `gis::crs_transform` call.
//
// LAYERING (ADR-0008). A point cloud is READ-ONLY REFERENCE GEOMETRY — a
// backdrop to trace over — so it is Layer-2 editor state that never enters the
// `.xodr`, exactly like an imported orthophoto. The ONE thing that crosses into
// real scene content is the height field a ground fit produces, which is
// installed through the ordinary `edit::set_terrain_field` command and is
// therefore undoable and persisted. Same split the elevation raster makes.

#include "roadmaker/error.hpp"
#include "roadmaker/export.hpp"
#include "roadmaker/gis/crs.hpp"
#include "roadmaker/road/terrain.hpp"
#include "roadmaker/xodr/diagnostic.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace roadmaker::lidar {

/// Upper bound on the points kept, in the spirit of `kMaxRasterTexels`.
///
/// This is a RENDER budget, not a parse budget: the editor's vertex format is 32
/// bytes interleaved plus a 4-byte index, so 2 Mi points is roughly 72 MB of
/// buffer. A survey tile can legally hold half a billion returns; keeping a
/// stated fraction of them and SAYING SO is honest, where attempting all of them
/// is an out-of-memory crash the user reads as a defect in their file.
inline constexpr std::size_t kMaxCloudPoints = 2ULL * 1024ULL * 1024ULL;

/// ASPRS standard classification for bare ground. The one class this reader
/// gives meaning to — everything else is carried through untouched, because
/// interpreting it would be the feature extraction #243 puts out of scope.
inline constexpr std::uint8_t kGroundClass = 2;

/// An imported point cloud, already in the scene's frame once it has been
/// through `reproject_point_cloud`.
///
/// ★ COORDINATES ARE FLOATS RELATIVE TO A DOUBLE `origin`, AND THAT IS
/// LOAD-BEARING, not a memory micro-optimisation. A tile in UTM sits at a
/// northing near 5.8e6 m, where a float's quantum is about half a metre — a
/// cloud stored in absolute scene coordinates as floats would visibly terrace
/// itself on any scene whose georeference is a UTM zone rather than a
/// scene-centred Transverse Mercator. Storing offsets keeps the precision where
/// the detail is, and halves the memory on the way past. Callers that want
/// absolute coordinates ask `point()`.
struct PointCloud {
  /// The local frame's origin in scene coordinates: the centre of `bounds`.
  std::array<double, 3> origin{};

  /// 3 floats per point — x, y, z RELATIVE to `origin`, in metres.
  std::vector<float> xyz;

  /// ASPRS classification per point. Empty when the source states none, which
  /// is a legitimate state (a raw scan), not an error.
  std::vector<std::uint8_t> classification;

  /// The source's own CRS description, verbatim, as `gis::parse_crs` reads it.
  /// Empty means the file stated none — treated as "already in the scene's
  /// frame", the same rule a shapefile with no `.prj` gets.
  std::string crs;

  /// {min_x, min_y, min_z, max_x, max_y, max_z}, ABSOLUTE, in whatever frame
  /// the cloud is currently in. Zeroed on an empty cloud rather than inverted,
  /// so callers can compare without special-casing.
  std::array<double, 6> bounds{};

  /// Points the file declared, BEFORE decimation. Kept so a read-out can say
  /// "1.9M of 23.4M" rather than implying the file was small.
  std::size_t source_count = 0;

  /// 1 means every point was kept; n means every nth. Deterministic, so two
  /// reads of the same file agree — a random sample would not.
  std::size_t stride = 1;

  [[nodiscard]] std::size_t size() const { return xyz.size() / 3; }

  [[nodiscard]] bool empty() const { return xyz.empty(); }

  /// Point `index` in absolute coordinates.
  [[nodiscard]] std::array<double, 3> point(std::size_t index) const {
    const std::size_t at = index * 3;
    return {origin[0] + static_cast<double>(xyz[at]),
            origin[1] + static_cast<double>(xyz[at + 1]),
            origin[2] + static_cast<double>(xyz[at + 2])};
  }
};

/// How much of a tile to keep.
struct LidarReadOptions {
  std::size_t max_points = kMaxCloudPoints;
};

struct PointCloudParseResult {
  PointCloud cloud;
  std::vector<Diagnostic> diagnostics;
};

/// Reads a `.las` or `.laz` tile in its own coordinate reference system.
///
/// Decimation is decided from the header BEFORE any point record is touched —
/// `stride = ceil(source_count / max_points)` — because a tile too large to hold
/// is also too large to read and then thin. The ratio is reported as a
/// diagnostic: a cloud silently reduced to a twelfth of itself looks like a
/// sparse survey rather than a decimated one.
///
/// Never guesses at a CRS. The `LASF_Projection` records this understands are
/// the GeoTIFF GeoKey directory (34735) and OGC WKT (2112); anything else leaves
/// `crs` empty and warns.
[[nodiscard]] RM_API Expected<PointCloudParseResult>
load_point_cloud(const std::filesystem::path& path, const LidarReadOptions& options = {});

/// Moves every point into the target frame and recomputes `origin`/`bounds`.
///
/// Reprojection happens in DOUBLES, on absolute coordinates, and the result is
/// re-offset — running the transform on the float offsets would reintroduce
/// exactly the precision the offset representation exists to avoid.
RM_API void reproject_point_cloud(PointCloud& cloud, const gis::CrsTransform& transform);

/// How to turn scattered returns into ground.
struct GroundFitOptions {
  /// Post spacing of the produced field, metres.
  double spacing = kDefaultFieldSpacing;

  /// Prefer points the file classified as bare ground (ASPRS class 2) when it
  /// classified any. A classified tile knows where its buildings and vegetation
  /// are, and no heuristic here beats that; an unclassified one falls back to
  /// the lowest return per cell.
  bool use_classification = true;
};

/// Bins a cloud into a `HeightField` ready for `edit::set_terrain_field`.
///
/// The estimator that ran is always named in a diagnostic, because "ground
/// classified" and "lowest return" disagree under a bridge or a canopy and the
/// user has to know which answer they are looking at.
///
/// ★ CELLS NO POINT LANDS IN ARE FILLED FROM THEIR NEIGHBOURS, NEVER WRITTEN AS
/// ZERO. A scattered cloud has genuine gaps — water, occlusion, the ragged edge
/// of any tile — and zero is not a missing value, it is a claim that the ground
/// is at the vertical datum. p7-s2 shipped that bug for reprojected DEMs, where
/// a no-data ring became a cliff at height 0; here the gaps are the normal case
/// rather than the edge case, so the fill is part of the contract.
[[nodiscard]] RM_API Expected<HeightField> point_cloud_to_height_field(
    const PointCloud& cloud, const GroundFitOptions& options, std::vector<Diagnostic>& diagnostics);

/// True for the extensions `load_point_cloud` accepts.
///
/// Exists so a file-dialog filter and the reader cannot disagree about what is
/// openable — the same reason `gis::is_vector_extension` does.
[[nodiscard]] RM_API bool is_point_cloud_extension(const std::filesystem::path& path);

} // namespace roadmaker::lidar
