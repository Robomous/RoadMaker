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

#include "roadmaker/gis/reproject.hpp"

#include "roadmaker/gis/crs.hpp"
#include "roadmaker/road/terrain.hpp"

#include <fmt/format.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <tuple>
#include <utility>
#include <vector>

#include "gis_common.hpp"

namespace roadmaker::gis {

namespace {

/// Pixel (column, row) -> source CRS, using the world-file affine.
std::array<double, 2> pixel_to_source(const std::array<double, 6>& t, double col, double row) {
  return {(t[0] * col) + (t[2] * row) + t[4], (t[1] * col) + (t[3] * row) + t[5]};
}

/// The raster's four corners after `to_target`. Corners rather than the
/// axis-aligned source box, because a rotated or reprojected raster's extent is
/// the hull of its corners and not the transform of its box.
///
/// Takes the mapping as a callable so the already-in-frame case passes identity
/// explicitly, rather than leaning on an Opaque `CrsTransform` happening to
/// behave as one.
template <class Map>
std::array<double, 4> extent_of(const GisRaster& raster, Map&& to_target) {
  double min_x = std::numeric_limits<double>::infinity();
  double min_y = std::numeric_limits<double>::infinity();
  double max_x = -std::numeric_limits<double>::infinity();
  double max_y = -std::numeric_limits<double>::infinity();
  const double w = raster.width;
  const double h = raster.height;
  // Corners of the pixel GRID, not of pixel centres: the transform's C/F name
  // the centre of the top-left pixel, so the image's edge is half a pixel out.
  for (const auto& [col, row] : {std::pair{-0.5, -0.5},
                                 std::pair{w - 0.5, -0.5},
                                 std::pair{w - 0.5, h - 0.5},
                                 std::pair{-0.5, h - 0.5}}) {
    const std::array<double, 2> src = pixel_to_source(raster.transform, col, row);
    const std::array<double, 2> dst = to_target(src);
    min_x = std::min(min_x, dst[0]);
    min_y = std::min(min_y, dst[1]);
    max_x = std::max(max_x, dst[0]);
    max_y = std::max(max_y, dst[1]);
  }
  return {min_x, min_y, max_x, max_y};
}

std::array<double, 4> target_extent(const GisRaster& raster, const CrsTransform& transform) {
  return extent_of(
      raster, [&transform](const std::array<double, 2>& p) { return transform.apply(p[0], p[1]); });
}

std::array<double, 4> extent_in_place(const GisRaster& raster) {
  return extent_of(raster, [](const std::array<double, 2>& p) { return p; });
}

/// Bilinear sample of an RGBA image at a fractional pixel position. Out-of-range
/// reads return transparent rather than clamping: an edge that repeats its last
/// row for a whole reprojected margin looks like real imagery and is not.
std::array<double, 4> sample_rgba(const GisRaster& raster, double col, double row) {
  if (col < -0.5 || row < -0.5 || col > raster.width - 0.5 || row > raster.height - 0.5) {
    return {0.0, 0.0, 0.0, 0.0};
  }
  const double cx = std::clamp(col, 0.0, static_cast<double>(raster.width - 1));
  const double cy = std::clamp(row, 0.0, static_cast<double>(raster.height - 1));
  const auto x0 = static_cast<std::size_t>(std::floor(cx));
  const auto y0 = static_cast<std::size_t>(std::floor(cy));
  const std::size_t x1 = std::min<std::size_t>(x0 + 1, static_cast<std::size_t>(raster.width - 1));
  const std::size_t y1 = std::min<std::size_t>(y0 + 1, static_cast<std::size_t>(raster.height - 1));
  const double fx = cx - static_cast<double>(x0);
  const double fy = cy - static_cast<double>(y0);

  const auto texel = [&](std::size_t x, std::size_t y, std::size_t channel) {
    const std::size_t index = (((y * static_cast<std::size_t>(raster.width)) + x) * 4) + channel;
    return static_cast<double>(raster.rgba[index]);
  };

  std::array<double, 4> out{};
  for (std::size_t c = 0; c < 4; ++c) {
    const double top = (texel(x0, y0, c) * (1.0 - fx)) + (texel(x1, y0, c) * fx);
    const double bottom = (texel(x0, y1, c) * (1.0 - fx)) + (texel(x1, y1, c) * fx);
    out[c] = (top * (1.0 - fy)) + (bottom * fy);
  }
  return out;
}

/// Bilinear sample of a single band, returning nullopt only inside a declared
/// no-data hole. A hole must not be interpolated across: averaging -9999 with a
/// real height produces a plausible number that is wrong, which is worse than
/// an admitted gap.
///
/// OUTSIDE the image it clamps to the nearest edge post rather than reporting a
/// hole — the opposite of `sample_rgba`, and deliberately so. A resampled grid
/// covers a whole number of square pixels and therefore always overhangs the
/// source's corner hull by up to a pixel. For imagery that margin must stay
/// transparent, because a smeared border reads as real imagery. For elevation
/// it must clamp, because the alternative is a ring of no-data that becomes a
/// cliff at height 0 around every reprojected DEM — which is exactly what the
/// worked example produced before this distinction existed. Clamping outside
/// the extent is also what `sample_height` already does to the finished field.
std::optional<double> sample_band(const GisRaster& raster, double col, double row) {
  const double cx = std::clamp(col, 0.0, static_cast<double>(raster.width - 1));
  const double cy = std::clamp(row, 0.0, static_cast<double>(raster.height - 1));
  const auto x0 = static_cast<std::size_t>(std::floor(cx));
  const auto y0 = static_cast<std::size_t>(std::floor(cy));
  const std::size_t x1 = std::min<std::size_t>(x0 + 1, static_cast<std::size_t>(raster.width - 1));
  const std::size_t y1 = std::min<std::size_t>(y0 + 1, static_cast<std::size_t>(raster.height - 1));
  const double fx = cx - static_cast<double>(x0);
  const double fy = cy - static_cast<double>(y0);

  const auto value = [&](std::size_t x, std::size_t y) -> std::optional<double> {
    const double v = raster.band[(y * static_cast<std::size_t>(raster.width)) + x];
    if (raster.nodata.has_value() && std::abs(v - *raster.nodata) < 1e-6) {
      return std::nullopt;
    }
    return v;
  };

  const std::optional<double> v00 = value(x0, y0);
  const std::optional<double> v10 = value(x1, y0);
  const std::optional<double> v01 = value(x0, y1);
  const std::optional<double> v11 = value(x1, y1);
  if (!v00.has_value() || !v10.has_value() || !v01.has_value() || !v11.has_value()) {
    // Any corner missing: fall back to nearest, and only if THAT one is real.
    const std::optional<double> nearest = value(fx < 0.5 ? x0 : x1, fy < 0.5 ? y0 : y1);
    return nearest;
  }
  const double top = (*v00 * (1.0 - fx)) + (*v10 * fx);
  const double bottom = (*v01 * (1.0 - fx)) + (*v11 * fx);
  return (top * (1.0 - fy)) + (bottom * fy);
}

/// The source raster's ground resolution measured in the TARGET frame — the
/// spacing a resample must preserve. Measured across the middle of the image
/// rather than at a corner, where a projection's distortion is largest.
double target_ground_resolution(const GisRaster& raster, const CrsTransform& transform) {
  const double mid_col = raster.width / 2.0;
  const double mid_row = raster.height / 2.0;
  const std::array<double, 2> a =
      transform.apply(pixel_to_source(raster.transform, mid_col, mid_row)[0],
                      pixel_to_source(raster.transform, mid_col, mid_row)[1]);
  const std::array<double, 2> src_b = pixel_to_source(raster.transform, mid_col + 1.0, mid_row);
  const std::array<double, 2> b = transform.apply(src_b[0], src_b[1]);
  const std::array<double, 2> src_c = pixel_to_source(raster.transform, mid_col, mid_row + 1.0);
  const std::array<double, 2> c = transform.apply(src_c[0], src_c[1]);
  const double dx = std::hypot(b[0] - a[0], b[1] - a[1]);
  const double dy = std::hypot(c[0] - a[0], c[1] - a[1]);
  const double resolution = std::min(dx, dy);
  return resolution > 0.0 && std::isfinite(resolution) ? resolution : 1.0;
}

} // namespace

void reproject_vector(GisVectorLayer& layer, const CrsTransform& transform) {
  for (GisFeature& feature : layer.features) {
    for (std::array<double, 2>& v : feature.vertices) {
      v = transform.apply(v[0], v[1]);
    }
  }
  recompute_bounds(layer);
}

Expected<PlacedRaster> reproject_raster(const GisRaster& raster,
                                        const CrsTransform& transform,
                                        std::vector<Diagnostic>& diagnostics) {
  if (raster.empty()) {
    return make_error(ErrorCode::InvalidArgument, "the raster has no pixels", "gis");
  }

  PlacedRaster placed;

  if (transform.affine()) {
    // Nothing to resample: rewrite the affine so the pixels sit where the
    // target frame says, and keep every original texel.
    placed.raster = raster;
    const std::array<double, 2> origin = transform.apply(raster.transform[4], raster.transform[5]);
    const std::array<double, 2> along_col = transform.apply(
        raster.transform[4] + raster.transform[0], raster.transform[5] + raster.transform[1]);
    const std::array<double, 2> along_row = transform.apply(
        raster.transform[4] + raster.transform[2], raster.transform[5] + raster.transform[3]);
    placed.raster.transform = {along_col[0] - origin[0],
                               along_col[1] - origin[1],
                               along_row[0] - origin[0],
                               along_row[1] - origin[1],
                               origin[0],
                               origin[1]};
    placed.placement = RasterPlacement::Placed;
    placed.extent = extent_in_place(placed.raster);
    return placed;
  }

  // Non-affine: resample onto an axis-aligned grid in the target frame.
  const std::array<double, 4> extent = target_extent(raster, transform);
  const double span_x = extent[2] - extent[0];
  const double span_y = extent[3] - extent[1];
  if (!(span_x > 0.0) || !(span_y > 0.0) || !std::isfinite(span_x) || !std::isfinite(span_y)) {
    return make_error(ErrorCode::InvalidArgument,
                      "the raster's corners do not reproject to a finite extent — its coordinate "
                      "reference system and the scene's do not overlap",
                      "gis");
  }

  // ONE resolution for both axes, deliberately. Sizing each axis from its own
  // span (span_x/out_w, span_y/out_h) gives NEARLY square pixels — the two
  // ceil()s round differently — and "nearly" is not a shape a uniform grid can
  // be. `raster_to_height_field` needs square posts and would refuse the
  // result, which is exactly how this was found: the worked example failed on
  // an 8×6 source that resampled to 9×7.
  double resolution = target_ground_resolution(raster, transform);
  auto grid_size = [&](double step) {
    return std::pair{std::max<std::size_t>(static_cast<std::size_t>(std::ceil(span_x / step)), 1),
                     std::max<std::size_t>(static_cast<std::size_t>(std::ceil(span_y / step)), 1)};
  };
  auto [out_w, out_h] = grid_size(resolution);

  // Preserving the source resolution can ask for more texels than the budget
  // allows. Coarsen the RESOLUTION rather than shrink the axes, so the pixels
  // stay square and the aspect ratio survives.
  if (out_w > kMaxRasterTexels / out_h) {
    resolution *= std::sqrt(static_cast<double>(out_w) * static_cast<double>(out_h) /
                            static_cast<double>(kMaxRasterTexels));
    std::tie(out_w, out_h) = grid_size(resolution);
    diagnostics.push_back(Diagnostic{
        .severity = Severity::Warning,
        .location = "gis",
        .message = fmt::format("reprojecting at the source resolution would need more than {} "
                               "texels, so it was coarsened to {:.3f} m per pixel ({}×{})",
                               kMaxRasterTexels,
                               resolution,
                               out_w,
                               out_h)});
  }

  placed.placement = RasterPlacement::Resampled;
  placed.raster.width = static_cast<int>(out_w);
  placed.raster.height = static_cast<int>(out_h);
  placed.raster.crs = raster.crs;
  placed.raster.elevation = raster.elevation;
  placed.raster.nodata = raster.nodata;

  // A whole number of square pixels covers a hair MORE than the corner hull.
  // That margin samples outside the source and comes back transparent (or as
  // no-data), which is the honest result — stretching the image to fit an exact
  // hull would misplace every pixel in it.
  const double covered_x = static_cast<double>(out_w) * resolution;
  const double covered_y = static_cast<double>(out_h) * resolution;
  placed.extent = {extent[0], extent[1], extent[0] + covered_x, extent[1] + covered_y};

  // Rows run north to south, hence the negative y scale — the same convention
  // the source's own world transform uses.
  placed.raster.transform = {resolution,
                             0.0,
                             0.0,
                             -resolution,
                             placed.extent[0] + (resolution / 2.0),
                             placed.extent[3] - (resolution / 2.0)};

  // The inverse of the source's own affine, to turn a source coordinate back
  // into a pixel. Computed once: it is constant across the whole loop.
  const std::array<double, 6>& s = raster.transform;
  const double det = (s[0] * s[3]) - (s[2] * s[1]);
  if (std::abs(det) < std::numeric_limits<double>::epsilon()) {
    return make_error(ErrorCode::InvalidArgument,
                      "the raster's positioning is degenerate (its pixel axes are parallel)",
                      "gis");
  }

  if (raster.elevation) {
    placed.raster.band.assign(out_w * out_h, 0.0F);
  } else {
    placed.raster.rgba.assign(out_w * out_h * 4, 0);
  }

  for (std::size_t row = 0; row < out_h; ++row) {
    for (std::size_t col = 0; col < out_w; ++col) {
      // Walk the TARGET grid backwards into source pixels. The other direction
      // scatters and leaves holes wherever the mapping stretches.
      const std::array<double, 2> world = pixel_to_source(
          placed.raster.transform, static_cast<double>(col), static_cast<double>(row));
      const std::array<double, 2> src = transform.invert(world[0], world[1]);
      const double dx = src[0] - s[4];
      const double dy = src[1] - s[5];
      const double pixel_col = ((dx * s[3]) - (dy * s[2])) / det;
      const double pixel_row = ((dy * s[0]) - (dx * s[1])) / det;

      const std::size_t index = (row * out_w) + col;
      if (raster.elevation) {
        const std::optional<double> value = sample_band(raster, pixel_col, pixel_row);
        placed.raster.band[index] = static_cast<float>(value.value_or(raster.nodata.value_or(0.0)));
      } else {
        const std::array<double, 4> rgba = sample_rgba(raster, pixel_col, pixel_row);
        for (std::size_t c = 0; c < 4; ++c) {
          placed.raster.rgba[(index * 4) + c] =
              static_cast<std::uint8_t>(std::clamp(rgba[c], 0.0, 255.0));
        }
      }
    }
  }

  diagnostics.push_back(Diagnostic{
      .severity = Severity::Info,
      .location = "gis",
      .message = fmt::format(
          "{} and the scene use different projections, so the image was resampled from {}×{} to "
          "{}×{} — it is no longer pixel-for-pixel the source file",
          describe_crs(transform.source()),
          raster.width,
          raster.height,
          out_w,
          out_h)});

  return placed;
}

Expected<HeightField> raster_to_height_field(const GisRaster& raster,
                                             const CrsTransform& transform,
                                             std::vector<Diagnostic>& diagnostics) {
  if (!raster.elevation || raster.band.empty()) {
    return make_error(
        ErrorCode::InvalidArgument,
        "this raster is imagery, not elevation — a terrain import needs a single-band raster of "
        "heights (a 16/32-bit or floating-point GeoTIFF)",
        "gis");
  }

  const Expected<PlacedRaster> placed = reproject_raster(raster, transform, diagnostics);
  if (!placed.has_value()) {
    return make_error(placed.error().code, placed.error().message, placed.error().context);
  }

  const GisRaster& grid = placed->raster;
  const std::array<double, 6>& t = grid.transform;

  // A HeightField is a UNIFORM grid with no rotation, so anything else has to
  // be resampled onto one. `reproject_raster` already produces exactly that
  // shape for the non-affine case; the affine case can still arrive rotated or
  // with unequal x/y spacing, and that is what this guards.
  const double spacing_x = std::hypot(t[0], t[1]);
  const double spacing_y = std::hypot(t[2], t[3]);

  // RELATIVE tolerances, not absolute ones. Even an affine placement derives
  // this transform by mapping three points through the CRS hub, and at a UTM
  // northing of ~5.8e6 m a double carries about 6e-10 of round-trip residue —
  // which lands right on top of an absolute 1e-9 epsilon. macOS's libm happened
  // to fall under it and Linux's did not, so an absolute test passed locally and
  // failed in CI. A rotation is only meaningful as a FRACTION of the pixel size:
  // 1e-6 of a 10 m pixel is 10 µm of skew, orders of magnitude below any
  // rotation a real file expresses and orders above the noise.
  const bool rotated = std::abs(t[1]) > 1e-6 * spacing_x || std::abs(t[2]) > 1e-6 * spacing_y;
  const bool anisotropic = std::abs(spacing_x - spacing_y) > 1e-6 * std::max(spacing_x, spacing_y);
  if (rotated || anisotropic) {
    return make_error(
        ErrorCode::InvalidArgument,
        fmt::format("the elevation raster is {} — the scene height field is a square uniform grid, "
                    "so resample the source to square, unrotated pixels before importing",
                    rotated ? "rotated" : "non-square in its pixel spacing"),
        "gis");
  }
  if (!(spacing_x > 0.0) || !std::isfinite(spacing_x)) {
    return make_error(
        ErrorCode::InvalidArgument, "the elevation raster has no usable pixel size", "gis");
  }

  if (static_cast<std::size_t>(grid.width) > kMaxFieldSamples ||
      static_cast<std::size_t>(grid.height) > kMaxFieldSamples) {
    return make_error(
        ErrorCode::InvalidArgument,
        fmt::format("the elevation raster is {}×{}, past the {} posts per axis a scene height "
                    "field holds — crop or downsample it first",
                    grid.width,
                    grid.height,
                    kMaxFieldSamples),
        "gis");
  }

  HeightField field;
  field.spacing = spacing_x;
  field.cols = static_cast<std::size_t>(grid.width);
  field.rows = static_cast<std::size_t>(grid.height);
  // HeightField rows run LOW y first; a raster's run high y first. The flip
  // happens here and nowhere else, which is the same contract the .asc sidecar
  // reader keeps with the same comment.
  field.origin_x = t[4];
  field.origin_y = t[5] + (t[3] * static_cast<double>(grid.height - 1));
  field.heights.assign(field.cols * field.rows, 0.0);

  std::size_t holes = 0;
  for (std::size_t row = 0; row < field.rows; ++row) {
    const std::size_t source_row = field.rows - 1 - row;
    for (std::size_t col = 0; col < field.cols; ++col) {
      const double value = grid.band[(source_row * field.cols) + col];
      const bool hole = grid.nodata.has_value() && std::abs(value - *grid.nodata) < 1e-6;
      if (hole) {
        ++holes;
      }
      field.heights[(row * field.cols) + col] = hole ? 0.0 : value;
    }
  }

  if (holes > 0) {
    diagnostics.push_back(Diagnostic{
        .severity = Severity::Warning,
        .location = "gis",
        .message = fmt::format("{} of {} samples carried the raster's no-data value and were read "
                               "as height 0 — a gap in a DEM is missing data, not ground at sea "
                               "level, so check those areas",
                               holes,
                               field.heights.size())});
  }

  return field;
}

} // namespace roadmaker::gis
