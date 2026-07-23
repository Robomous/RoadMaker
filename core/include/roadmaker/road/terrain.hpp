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

#include "roadmaker/export.hpp"

#include <array>
#include <cstddef>
#include <string>
#include <vector>

namespace roadmaker {

class RoadNetwork;

/// Per-axis sample cap for a generated field. A grid is scene data, not a
/// texture: 2048 samples on the long axis of a 20 km network is a 10 m post
/// spacing, which is already finer than anything P5 authors by hand.
inline constexpr std::size_t kMaxFieldSamples = 2048;

/// Default post spacing (meters) and network-bounds margin for make_flat_field.
inline constexpr double kDefaultFieldSpacing = 10.0;
inline constexpr double kDefaultFieldMargin = 50.0;

/// The ONE scene height field (ADR-0006, p5-s2): a regular grid in the kernel
/// frame (right-handed, Z-up, meters). Sample (col, row) sits at
/// (origin_x + col*spacing, origin_y + row*spacing) and `heights` is row-major
/// with row 0 on the LOW y edge, so index = row*cols + col.
///
/// An EMPTY field (cols == 0 || rows == 0) is the ABSENT field, and that is the
/// load-bearing case: every sample reads 0.0, the terrain mesh channel stays
/// empty, and a scene without terrain renders, meshes and serializes exactly as
/// it did before this type existed — bit for bit. Building the field first is
/// therefore invisible until someone creates one.
struct HeightField {
  double origin_x = 0.0;                 ///< world x of sample column 0 (the grid's low-x edge)
  double origin_y = 0.0;                 ///< world y of sample row 0 (the grid's low-y edge)
  double spacing = kDefaultFieldSpacing; ///< uniform post spacing [m], > 0
  std::size_t cols = 0;
  std::size_t rows = 0;
  std::vector<double> heights; ///< rows*cols, row-major, LOW-y row first

  /// The sidecar file this field is read from and written to, RELATIVE to the
  /// .xodr's own directory (decision D2 — the field is Layer-2 scene data and
  /// does not belong inside <userData>, so the .xodr carries only a reference
  /// to it until fmt-s1/#325 gives us a container). Empty on a field that has
  /// never been saved; save_xodr fills in a default from the document stem.
  std::string sidecar;

  /// True when this field carries no samples — see the type comment: this is
  /// "no terrain in this scene", not "a flat terrain".
  [[nodiscard]] bool empty() const { return cols == 0 || rows == 0; }

  /// Sample count. Always rows*cols on a well-formed field (the command layer
  /// refuses any other shape).
  [[nodiscard]] std::size_t sample_count() const { return cols * rows; }

  friend bool operator==(const HeightField&, const HeightField&) = default;
};

/// Ground height at a plan-view point: bilinear interpolation of the four
/// surrounding posts, CLAMPED to the grid edge outside the extent (so the field
/// extends flat to infinity rather than falling off a cliff at its border), and
/// 0.0 everywhere on an empty field.
///
/// This is THE definition of ground height — the terrain mesher, p5-s3's bridge
/// detection and p5-s4's brushes all go through it, so there is exactly one
/// place the answer can drift.
[[nodiscard]] RM_API double sample_height(const HeightField& field, double x, double y);

/// Plan-view extent as {lo_x, lo_y, hi_x, hi_y}; all zeros on an empty field.
[[nodiscard]] RM_API std::array<double, 4> field_extent(const HeightField& field);

/// A zero-height field covering the network's plan-view bounds grown by
/// `margin`, with its origin snapped OUT to a whole multiple of `spacing` so
/// the same network always yields the same grid. Returns an EMPTY field when
/// the network has no road geometry to bound, and clamps each axis to
/// kMaxFieldSamples posts (raising the effective spacing) so a huge network
/// cannot allocate an unbounded grid.
[[nodiscard]] RM_API HeightField make_flat_field(const RoadNetwork& network,
                                                 double spacing = kDefaultFieldSpacing,
                                                 double margin = kDefaultFieldMargin);

} // namespace roadmaker
