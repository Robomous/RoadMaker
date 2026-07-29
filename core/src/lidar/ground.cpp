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

// Turning scattered returns into a HeightField.
//
// This is a BINNING FIT, not a classifier. #243 puts automatic feature
// extraction out of scope, and the two estimators here are the honest floor:
// trust the file's own ground classification when it has one, and otherwise take
// the lowest return in each cell. Both are named in a diagnostic, because they
// disagree under a bridge or a dense canopy and a user reading the terrain has
// to know which answer they are looking at.

#include <fmt/format.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>
#include <vector>

#include "lidar_common.hpp"

namespace roadmaker::lidar {

namespace {

constexpr double kUnfilled = std::numeric_limits<double>::max();

/// Fills cells no point landed in from their filled neighbours, spreading
/// inward one ring per pass until nothing is left.
///
/// Deterministic by construction: each pass reads the previous pass's buffer, so
/// the answer does not depend on traversal order. Returns how many cells it
/// had to invent.
std::size_t fill_gaps(std::vector<double>& heights, std::size_t cols, std::size_t rows) {
  const std::size_t total = heights.size();
  std::size_t empty =
      static_cast<std::size_t>(std::count(heights.begin(), heights.end(), kUnfilled));
  const std::size_t invented = empty;
  if (empty == 0 || empty == total) {
    return invented;
  }

  std::vector<double> next;
  while (empty > 0) {
    next = heights;
    std::size_t changed = 0;
    for (std::size_t row = 0; row < rows; ++row) {
      for (std::size_t col = 0; col < cols; ++col) {
        const std::size_t at = (row * cols) + col;
        if (heights[at] != kUnfilled) {
          continue;
        }
        double sum = 0.0;
        std::size_t seen = 0;
        for (int dr = -1; dr <= 1; ++dr) {
          for (int dc = -1; dc <= 1; ++dc) {
            if (dr == 0 && dc == 0) {
              continue;
            }
            const auto nr = static_cast<std::ptrdiff_t>(row) + dr;
            const auto nc = static_cast<std::ptrdiff_t>(col) + dc;
            if (nr < 0 || nc < 0 || static_cast<std::size_t>(nr) >= rows ||
                static_cast<std::size_t>(nc) >= cols) {
              continue;
            }
            const double neighbour =
                heights[(static_cast<std::size_t>(nr) * cols) + static_cast<std::size_t>(nc)];
            if (neighbour != kUnfilled) {
              sum += neighbour;
              ++seen;
            }
          }
        }
        if (seen > 0) {
          next[at] = sum / static_cast<double>(seen);
          ++changed;
        }
      }
    }
    if (changed == 0) {
      break; // no filled cell is reachable; the caller flattens the remainder
    }
    heights.swap(next);
    empty -= changed;
  }

  return invented;
}

} // namespace

Expected<HeightField> point_cloud_to_height_field(const PointCloud& cloud,
                                                  const GroundFitOptions& options,
                                                  std::vector<Diagnostic>& diagnostics) {
  if (cloud.empty()) {
    return make_error(
        ErrorCode::InvalidArgument, "this point cloud has no points to fit ground to", "lidar");
  }
  if (!(options.spacing > 0.0) || !std::isfinite(options.spacing)) {
    return make_error(
        ErrorCode::InvalidArgument, "the ground fit needs a positive post spacing", "lidar");
  }

  const double min_x = cloud.bounds[0];
  const double min_y = cloud.bounds[1];
  const double max_x = cloud.bounds[3];
  const double max_y = cloud.bounds[4];

  const double span_x = max_x - min_x;
  const double span_y = max_y - min_y;
  const auto cols = static_cast<std::size_t>(std::floor(span_x / options.spacing)) + 1;
  const auto rows = static_cast<std::size_t>(std::floor(span_y / options.spacing)) + 1;

  if (cols < 2 || rows < 2) {
    return make_error(
        ErrorCode::InvalidArgument,
        fmt::format("this cloud spans {:.1f} × {:.1f} m, which is under one {:.1f} m grid cell — "
                    "use a finer post spacing or a larger tile",
                    span_x,
                    span_y,
                    options.spacing),
        "lidar");
  }
  if (cols > kMaxFieldSamples || rows > kMaxFieldSamples) {
    return make_error(
        ErrorCode::InvalidArgument,
        fmt::format("a {:.1f} m grid over this cloud is {}×{} posts, past the {} per axis a scene "
                    "height field holds — use a coarser post spacing or a smaller tile",
                    options.spacing,
                    cols,
                    rows,
                    kMaxFieldSamples),
        "lidar");
  }

  const bool classified =
      options.use_classification && cloud.classification.size() == cloud.size() &&
      std::find(cloud.classification.begin(), cloud.classification.end(), kGroundClass) !=
          cloud.classification.end();

  std::vector<double> heights(cols * rows, kUnfilled);
  // ★ THE TWO ESTIMATORS ARE NOT THE SAME STATISTIC, AND THAT IS DELIBERATE.
  //
  // The lowest return exists to REJECT what is not ground — a roof, a canopy, a
  // parked lorry — in a file that never said which returns were which. It pays
  // for that robustness with a systematic bias: on a slope, the lowest point
  // inside a cell sits at the cell's uphill-facing edge rather than at its post,
  // so every post reads about half a cell of grade too low.
  //
  // When the file HAS classified its ground, that rejection is already done and
  // better than any heuristic here could manage, so there is nothing left for
  // the minimum to buy — it only keeps the bias. The mean of the classified
  // ground returns is the better estimate of the surface at the post, and it is
  // what the classified path uses.
  std::vector<double> sums;
  std::vector<std::size_t> counts;
  if (classified) {
    sums.assign(cols * rows, 0.0);
    counts.assign(cols * rows, 0);
  }

  std::size_t used = 0;
  for (std::size_t i = 0; i < cloud.size(); ++i) {
    if (classified && cloud.classification[i] != kGroundClass) {
      continue;
    }
    const std::array<double, 3> world = cloud.point(i);
    // Posts are SAMPLES, not cell centres, so a point belongs to the post it is
    // nearest to — not to the cell whose lower corner it sits past.
    const auto col =
        static_cast<std::ptrdiff_t>(std::llround((world[0] - min_x) / options.spacing));
    const auto row =
        static_cast<std::ptrdiff_t>(std::llround((world[1] - min_y) / options.spacing));
    if (col < 0 || row < 0 || static_cast<std::size_t>(col) >= cols ||
        static_cast<std::size_t>(row) >= rows) {
      continue;
    }
    const std::size_t at = (static_cast<std::size_t>(row) * cols) + static_cast<std::size_t>(col);
    if (classified) {
      sums[at] += world[2];
      ++counts[at];
    } else {
      heights[at] = heights[at] == kUnfilled ? world[2] : std::min(heights[at], world[2]);
    }
    ++used;
  }

  if (classified) {
    for (std::size_t at = 0; at < heights.size(); ++at) {
      if (counts[at] > 0) {
        heights[at] = sums[at] / static_cast<double>(counts[at]);
      }
    }
  }

  if (used == 0) {
    return make_error(ErrorCode::InvalidArgument,
                      "no point in this cloud landed inside its own bounding box, which means the "
                      "cloud's extent is not usable",
                      "lidar");
  }

  const std::size_t invented = fill_gaps(heights, cols, rows);

  // A cell the spread could not reach means no filled cell was connected to it
  // at all, which only happens when the grid is almost entirely empty. Flatten
  // the remainder to the mean of what IS known rather than to zero — zero is not
  // a missing value, it is a claim that the ground is at the vertical datum.
  double sum = 0.0;
  std::size_t known = 0;
  for (const double height : heights) {
    if (height != kUnfilled) {
      sum += height;
      ++known;
    }
  }
  const double mean = known > 0 ? sum / static_cast<double>(known) : 0.0;
  std::replace(heights.begin(), heights.end(), kUnfilled, mean);

  diagnostics.push_back(Diagnostic{
      .severity = Severity::Info,
      .location = "lidar",
      .message =
          classified
              ? fmt::format("ground fitted from the mean of the {} return(s) this tile classified "
                            "as bare ground, over a {}×{} grid at {:.1f} m",
                            used,
                            cols,
                            rows,
                            options.spacing)
              : fmt::format("this tile classifies no ground, so ground was fitted from the lowest "
                            "of the {} return(s) in each cell, over a {}×{} grid at {:.1f} m — a "
                            "bridge deck or a dense canopy will read low",
                            used,
                            cols,
                            rows,
                            options.spacing)});

  if (invented > 0) {
    diagnostics.push_back(Diagnostic{
        .severity = Severity::Warning,
        .location = "lidar",
        .message = fmt::format("{} of {} posts had no return in them and were interpolated from "
                               "their neighbours — water, occlusion and the ragged edge of a tile "
                               "all look like this, so check those areas",
                               invented,
                               heights.size())});
  }

  HeightField field;
  field.origin_x = min_x;
  field.origin_y = min_y;
  field.spacing = options.spacing;
  field.cols = cols;
  field.rows = rows;
  // Row 0 is the LOW-y row, which is the order the binning above already
  // produced — the flip a raster needs does not apply here, because a cloud has
  // no scanline order to disagree with.
  field.heights = std::move(heights);
  return field;
}

} // namespace roadmaker::lidar
