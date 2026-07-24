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

#include "roadmaker/road/terrain_brush.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

namespace roadmaker {

namespace {

/// Clamps a real post index range to the grid. Returns [lo, hi] inclusive post
/// indices that could fall within the stamp, or an empty range (lo > hi) when
/// the stamp does not overlap the axis at all.
struct IndexRange {
  std::size_t lo = 1;
  std::size_t hi = 0; // hi < lo ⇒ empty

  [[nodiscard]] bool empty() const { return hi < lo; }
};

IndexRange
axis_range(double center, double radius, double origin, double spacing, std::size_t count) {
  if (count == 0) {
    return {};
  }
  const double lo_world = center - radius;
  const double hi_world = center + radius;
  const double lo_idx = std::floor((lo_world - origin) / spacing);
  const double hi_idx = std::ceil((hi_world - origin) / spacing);
  const double max_idx = static_cast<double>(count - 1);
  if (hi_idx < 0.0 || lo_idx > max_idx) {
    return {}; // wholly off this axis
  }
  IndexRange range;
  range.lo = static_cast<std::size_t>(std::max(0.0, lo_idx));
  range.hi = static_cast<std::size_t>(std::min(max_idx, hi_idx));
  return range;
}

} // namespace

BrushFootprint apply_brush_stamp(HeightField& field, const BrushStamp& stamp) {
  BrushFootprint footprint;
  if (field.empty() || !(stamp.radius > 0.0) || !(stamp.strength > 0.0)) {
    return footprint; // nothing to touch
  }

  const IndexRange cols =
      axis_range(stamp.center_x, stamp.radius, field.origin_x, field.spacing, field.cols);
  const IndexRange rows =
      axis_range(stamp.center_y, stamp.radius, field.origin_y, field.spacing, field.rows);
  if (cols.empty() || rows.empty()) {
    return footprint; // stamp lies entirely outside the grid
  }

  // Smooth reads the local mean; sourcing it from a snapshot keeps the result
  // independent of post iteration order (an in-place sweep would diffuse). Raise
  // and Lower are additive, so they need no snapshot.
  const std::vector<double> source =
      stamp.mode == BrushMode::Smooth ? field.heights : std::vector<double>{};
  const double sign = stamp.mode == BrushMode::Lower ? -1.0 : 1.0;
  const double inv_radius = 1.0 / stamp.radius;

  for (std::size_t row = rows.lo; row <= rows.hi; ++row) {
    const double py = field.origin_y + (static_cast<double>(row) * field.spacing);
    for (std::size_t col = cols.lo; col <= cols.hi; ++col) {
      const double px = field.origin_x + (static_cast<double>(col) * field.spacing);
      const double dx = px - stamp.center_x;
      const double dy = py - stamp.center_y;
      const double t = std::sqrt((dx * dx) + (dy * dy)) * inv_radius;
      if (t >= 1.0) {
        continue; // outside the disc
      }
      const double falloff = (1.0 - (t * t)) * (1.0 - (t * t)); // smooth bump, C1 at rim + centre
      const std::size_t index = (row * field.cols) + col;

      if (stamp.mode == BrushMode::Smooth) {
        double sum = 0.0;
        std::size_t n = 0;
        if (col > 0) {
          sum += source[index - 1];
          ++n;
        }
        if (col + 1 < field.cols) {
          sum += source[index + 1];
          ++n;
        }
        if (row > 0) {
          sum += source[index - field.cols];
          ++n;
        }
        if (row + 1 < field.rows) {
          sum += source[index + field.cols];
          ++n;
        }
        if (n > 0) {
          const double mean = sum / static_cast<double>(n);
          const double weight = std::min(1.0, stamp.strength) * falloff;
          field.heights[index] = source[index] + (weight * (mean - source[index]));
        }
      } else {
        field.heights[index] += sign * stamp.strength * falloff;
      }

      BrushFootprint post{.col0 = col, .row0 = row, .col1 = col, .row1 = row, .touched = true};
      footprint.merge(post);
    }
  }
  return footprint;
}

} // namespace roadmaker
