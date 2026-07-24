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

// Terrain sculpting brushes (p5-s4, issue #234). A brush is a circular stamp
// that mutates the ONE scene HeightField (p5-s2) in place: raise/lower push the
// posts up or down through a smooth radial falloff, smooth relaxes each post
// toward the local mean. The editor's Terrain Brush tool replays a whole
// press-drag-release stroke through apply_brush_stamp onto a COPY of the field
// and commits ONE region-delta command on release (edit::stamp_terrain); the
// math lives in the kernel so it is testable headless and shared with Python.
//
// A brush never resizes the grid or moves its origin — it only rewrites heights
// — so an off-grid or partly-off-grid stamp simply touches the posts it covers
// and nothing else, and a stamp on the ABSENT field (cols==0||rows==0) is a
// no-op. Everything downstream (the sampler, the terrain mesh + skirt, bridge
// pier feet) reads the mutated heights unchanged.

#include "roadmaker/export.hpp"
#include "roadmaker/road/terrain.hpp"

#include <algorithm>
#include <cstddef>

namespace roadmaker {

/// What a stamp does to the posts under it.
enum class BrushMode {
  Raise,  ///< push posts up by `strength` * falloff
  Lower,  ///< push posts down by `strength` * falloff
  Smooth, ///< blend each post toward the mean of its in-grid 4-neighbours
};

/// One dab of the brush, in the kernel plan frame (meters). A drag is a
/// sequence of these; the tool spaces them along the cursor path.
struct BrushStamp {
  double center_x = 0.0;
  double center_y = 0.0;
  double radius = 20.0; ///< brush radius [m], must be > 0 to do anything
  double strength =
      0.5; ///< peak displacement [m] for Raise/Lower; blend weight in [0,1] for Smooth
  BrushMode mode = BrushMode::Raise;
};

/// The inclusive post rectangle a stamp (or a whole stroke) actually changed —
/// the region a delta command captures. `touched` is false when nothing moved
/// (empty field, zero radius/strength, or a stamp entirely off the grid), and
/// the col/row members are then meaningless.
struct BrushFootprint {
  std::size_t col0 = 0;
  std::size_t row0 = 0;
  std::size_t col1 = 0;
  std::size_t row1 = 0;
  bool touched = false;

  /// Grows this footprint to also cover `other` (used to union a stroke's
  /// per-stamp footprints). An untouched footprint adopts the other; an
  /// untouched other leaves this unchanged. Inline (not an exported symbol) so
  /// it links from a test even in the shared-kernel build, where core/src
  /// symbols are hidden.
  void merge(const BrushFootprint& other) {
    if (!other.touched) {
      return;
    }
    if (!touched) {
      *this = other;
      return;
    }
    col0 = std::min(col0, other.col0);
    row0 = std::min(row0, other.row0);
    col1 = std::max(col1, other.col1);
    row1 = std::max(row1, other.row1);
  }
};

/// Applies one stamp to `field` in place and returns the post rectangle it
/// changed. The radial weight is a smooth bump w = (1 - t*t)^2 with t = d/radius
/// (1 at the centre, 0 at the rim, C1 at both ends), so strokes leave rounded
/// hills rather than cones. Raise/Lower add +/- strength*w to each covered post;
/// Smooth moves each covered post a fraction strength*w of the way to the mean
/// of its existing in-grid orthogonal neighbours (a spike shrinks, a flat stays
/// flat, and the result never overshoots the neighbour mean). No-op — returning
/// an untouched footprint — on an empty field or a non-positive radius/strength.
///
/// The primary effect is the in-place mutation; the returned footprint is an
/// optimization for callers that capture a delta (edit::stamp_terrain), so it is
/// not [[nodiscard]] — applying and discarding the footprint is a valid use.
RM_API BrushFootprint apply_brush_stamp(HeightField& field, const BrushStamp& stamp);

} // namespace roadmaker
