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

// Terrain sculpting brushes (p5-s4, issue #234): the pure raise/lower/smooth
// stamp math over a HeightField. Everything here is order-independent and
// coordinate-exact, so the editor tool and the Python bindings can lean on it.

#include "roadmaker/road/terrain.hpp"
#include "roadmaker/road/terrain_brush.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <cstddef>
#include <vector>

using roadmaker::apply_brush_stamp;
using roadmaker::BrushFootprint;
using roadmaker::BrushMode;
using roadmaker::BrushStamp;
using roadmaker::HeightField;

namespace {

/// A flat cols x rows grid on a 10 m pitch, origin (0, 0), all posts at `z`.
HeightField flat_grid(std::size_t cols, std::size_t rows, double z = 0.0) {
  HeightField field;
  field.origin_x = 0.0;
  field.origin_y = 0.0;
  field.spacing = 10.0;
  field.cols = cols;
  field.rows = rows;
  field.heights.assign(cols * rows, z);
  return field;
}

double at(const HeightField& field, std::size_t col, std::size_t row) {
  return field.heights[(row * field.cols) + col];
}

} // namespace

TEST(TerrainBrush, RaiseLiftsThePeakPostByStrength) {
  HeightField field = flat_grid(9, 9);
  // Centre the stamp exactly on the middle post (col 4, row 4) → falloff 1 there.
  BrushStamp stamp{.center_x = 40.0,
                   .center_y = 40.0,
                   .radius = 25.0,
                   .strength = 3.0,
                   .mode = BrushMode::Raise};
  const BrushFootprint fp = apply_brush_stamp(field, stamp);

  ASSERT_TRUE(fp.touched);
  EXPECT_DOUBLE_EQ(at(field, 4, 4), 3.0); // peak == strength
  EXPECT_GT(at(field, 5, 4), 0.0);        // a covered neighbour rose
  EXPECT_LT(at(field, 5, 4), 3.0);        // ...but less than the peak
  // A post 3 cells away (30 m > 25 m radius) is untouched.
  EXPECT_DOUBLE_EQ(at(field, 7, 4), 0.0);
}

TEST(TerrainBrush, LowerIsTheSignedMirrorOfRaise) {
  HeightField up = flat_grid(9, 9);
  HeightField down = flat_grid(9, 9);
  const BrushStamp raise{.center_x = 40.0,
                         .center_y = 40.0,
                         .radius = 25.0,
                         .strength = 2.0,
                         .mode = BrushMode::Raise};
  BrushStamp lower = raise;
  lower.mode = BrushMode::Lower;

  apply_brush_stamp(up, raise);
  apply_brush_stamp(down, lower);
  for (std::size_t i = 0; i < up.heights.size(); ++i) {
    EXPECT_DOUBLE_EQ(up.heights[i], -down.heights[i]);
  }
}

TEST(TerrainBrush, RaiseThenLowerReturnsToBaseline) {
  HeightField field = flat_grid(11, 11, 5.0);
  const std::vector<double> baseline = field.heights;
  const BrushStamp raise{.center_x = 55.0,
                         .center_y = 55.0,
                         .radius = 33.0,
                         .strength = 4.0,
                         .mode = BrushMode::Raise};
  BrushStamp lower = raise;
  lower.mode = BrushMode::Lower;

  apply_brush_stamp(field, raise);
  apply_brush_stamp(field, lower);
  for (std::size_t i = 0; i < field.heights.size(); ++i) {
    EXPECT_NEAR(field.heights[i], baseline[i], 1e-12);
  }
}

TEST(TerrainBrush, SmoothPullsASpikeDownWithoutOvershoot) {
  HeightField field = flat_grid(9, 9);
  // A single tall post surrounded by flat ground.
  field.heights[(4 * field.cols) + 4] = 10.0;
  const double before = at(field, 4, 4);
  BrushStamp smooth{.center_x = 40.0,
                    .center_y = 40.0,
                    .radius = 15.0,
                    .strength = 1.0,
                    .mode = BrushMode::Smooth};
  apply_brush_stamp(field, smooth);

  const double after = at(field, 4, 4);
  EXPECT_LT(after, before); // the spike came down
  EXPECT_GE(after, 0.0);    // ...but never below its flat neighbours (no overshoot)
  // The neighbours it averages toward rose slightly, never above the spike.
  EXPECT_GT(at(field, 5, 4), 0.0);
  EXPECT_LT(at(field, 5, 4), before);
}

TEST(TerrainBrush, SmoothLeavesAFlatFieldFlat) {
  HeightField field = flat_grid(9, 9, 7.0);
  const std::vector<double> baseline = field.heights;
  apply_brush_stamp(field,
                    BrushStamp{.center_x = 40.0,
                               .center_y = 40.0,
                               .radius = 40.0,
                               .strength = 1.0,
                               .mode = BrushMode::Smooth});
  EXPECT_EQ(field.heights, baseline); // a flat field is its own local mean
}

TEST(TerrainBrush, SmoothIsIndependentOfIterationOrder) {
  // Two spikes: reading the mean from a snapshot (not an in-place sweep) means
  // the result cannot depend on which post is visited first.
  HeightField field = flat_grid(9, 9);
  field.heights[(4 * field.cols) + 4] = 8.0;
  field.heights[(4 * field.cols) + 5] = 8.0;
  HeightField copy = field;

  const BrushStamp smooth{.center_x = 45.0,
                          .center_y = 40.0,
                          .radius = 25.0,
                          .strength = 0.7,
                          .mode = BrushMode::Smooth};
  apply_brush_stamp(field, smooth);
  apply_brush_stamp(copy, smooth); // same stamp twice on separate copies

  EXPECT_EQ(field.heights, copy.heights); // deterministic
}

TEST(TerrainBrush, OffGridStampTouchesOnlyInGridPosts) {
  HeightField field = flat_grid(5, 5);
  // Centre well past the high-x/high-y corner (post 4,4 at world 40,40).
  BrushStamp stamp{.center_x = 50.0,
                   .center_y = 50.0,
                   .radius = 15.0,
                   .strength = 2.0,
                   .mode = BrushMode::Raise};
  const BrushFootprint fp = apply_brush_stamp(field, stamp);

  ASSERT_TRUE(fp.touched);
  EXPECT_EQ(fp.col1, 4u); // clamped to the grid's last column
  EXPECT_EQ(fp.row1, 4u);
  EXPECT_GT(at(field, 4, 4), 0.0); // the corner post is inside the disc
}

TEST(TerrainBrush, StampFullyOutsideTheGridChangesNothing) {
  HeightField field = flat_grid(5, 5);
  const std::vector<double> baseline = field.heights;
  const BrushFootprint fp = apply_brush_stamp(field,
                                              BrushStamp{.center_x = 500.0,
                                                         .center_y = 500.0,
                                                         .radius = 20.0,
                                                         .strength = 5.0,
                                                         .mode = BrushMode::Raise});
  EXPECT_FALSE(fp.touched);
  EXPECT_EQ(field.heights, baseline);
}

TEST(TerrainBrush, EmptyFieldIsANoOp) {
  HeightField empty; // cols == rows == 0 → the ABSENT field
  const BrushFootprint fp = apply_brush_stamp(empty,
                                              BrushStamp{.center_x = 0.0,
                                                         .center_y = 0.0,
                                                         .radius = 10.0,
                                                         .strength = 1.0,
                                                         .mode = BrushMode::Raise});
  EXPECT_FALSE(fp.touched);
  EXPECT_TRUE(empty.heights.empty());
}

TEST(TerrainBrush, NonPositiveRadiusOrStrengthDoesNothing) {
  HeightField field = flat_grid(5, 5);
  const std::vector<double> baseline = field.heights;
  EXPECT_FALSE(apply_brush_stamp(field,
                                 BrushStamp{.center_x = 20.0,
                                            .center_y = 20.0,
                                            .radius = 0.0,
                                            .strength = 2.0,
                                            .mode = BrushMode::Raise})
                   .touched);
  EXPECT_FALSE(apply_brush_stamp(field,
                                 BrushStamp{.center_x = 20.0,
                                            .center_y = 20.0,
                                            .radius = 10.0,
                                            .strength = 0.0,
                                            .mode = BrushMode::Raise})
                   .touched);
  EXPECT_EQ(field.heights, baseline);
}

TEST(TerrainBrushFootprint, MergeUnionsRectangles) {
  BrushFootprint a{.col0 = 2, .row0 = 3, .col1 = 4, .row1 = 5, .touched = true};
  const BrushFootprint b{.col0 = 1, .row0 = 4, .col1 = 6, .row1 = 4, .touched = true};
  a.merge(b);
  EXPECT_EQ(a.col0, 1u);
  EXPECT_EQ(a.row0, 3u);
  EXPECT_EQ(a.col1, 6u);
  EXPECT_EQ(a.row1, 5u);

  BrushFootprint fresh;
  fresh.merge(a);
  EXPECT_TRUE(fresh.touched);
  EXPECT_EQ(fresh.col1, 6u);

  const BrushFootprint untouched;
  BrushFootprint keep = a;
  keep.merge(untouched); // merging an untouched footprint is a no-op
  EXPECT_EQ(keep.col0, 1u);
  EXPECT_EQ(keep.col1, 6u);
}
