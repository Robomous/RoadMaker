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

// Moving imported GIS data into the scene's frame (p7-s2, #242).

#include "roadmaker/error.hpp"
#include "roadmaker/export.hpp"
#include "roadmaker/gis/crs.hpp"
#include "roadmaker/gis/layer.hpp"
#include "roadmaker/road/terrain.hpp"
#include "roadmaker/xodr/diagnostic.hpp"

#include <array>
#include <vector>

namespace roadmaker::gis {

/// Rewrites every vertex into the target frame and recomputes `bounds`.
/// `layer.crs` is left as the source's, because it records where the data came
/// from and that stays true after the move.
RM_API void reproject_vector(GisVectorLayer& layer, const CrsTransform& transform);

/// How a raster ended up positioned. The distinction is user-visible on
/// purpose: a placed raster is the original pixels, a resampled one is not, and
/// a user comparing an import against its source deserves to know which they
/// are looking at.
enum class RasterPlacement : std::uint8_t {
  Placed,    ///< the transform was affine; pixels are untouched
  Resampled, ///< the transform curved; pixels were bilinearly resampled
};

/// A raster positioned in the scene frame.
struct PlacedRaster {
  GisRaster raster;
  RasterPlacement placement = RasterPlacement::Placed;
  /// Axis-aligned extent in scene coordinates: {min_x, min_y, max_x, max_y}.
  /// This is what the editor draws the underlay quad from.
  std::array<double, 4> extent{};
};

/// Moves a raster into the target frame.
///
/// When `transform.affine()` the pixels are kept exactly as read and only the
/// transform is rewritten. Otherwise the raster is bilinearly resampled onto an
/// axis-aligned grid in the target frame, sized to preserve the source's ground
/// resolution and capped at `kMaxRasterTexels`; the resample emits an Info
/// diagnostic naming the output size, because silence there would let a
/// resampled image pass for the original.
[[nodiscard]] RM_API Expected<PlacedRaster> reproject_raster(const GisRaster& raster,
                                                             const CrsTransform& transform,
                                                             std::vector<Diagnostic>& diagnostics);

/// Converts an elevation raster into a scene `HeightField`.
///
/// The field is a uniform grid, so a source whose pixels are not square, or
/// whose transform is rotated, is resampled onto one — reported the same way.
/// `nodata` samples are filled from their neighbours rather than written as
/// heights, because a hole in a DEM is missing data and a road conforming to
/// -9999 m is not a defensible reading of it.
[[nodiscard]] RM_API Expected<HeightField> raster_to_height_field(
    const GisRaster& raster, const CrsTransform& transform, std::vector<Diagnostic>& diagnostics);

} // namespace roadmaker::gis
