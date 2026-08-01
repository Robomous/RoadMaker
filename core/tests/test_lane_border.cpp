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

// The <border> -> <width> conversion (§11.7.2, #538), pinned at the level the
// arithmetic actually lives at.
//
// The claim this fix makes is EXACTNESS: a border profile re-expressed as a
// width profile must agree at every station, not merely at the record starts.
// Going through the reader alone could not show that — a conversion that got
// the cubic re-basing wrong still produces a plausible-looking width record at
// each breakpoint. So these tests reach into core/src/xodr/lane_border.hpp
// (core/tests has core/src on its include path, as test_fill_predicates.cpp
// does) and compare against the borders evaluated directly.

#include "roadmaker/geometry/poly3.hpp"

#include <gtest/gtest.h>

#include <span>
#include <vector>

#include "xodr/lane_border.hpp"

namespace roadmaker::xodr {
namespace {

/// The width the borders IMPLY at a station, computed the long way round:
/// evaluate both profiles and take the gap. This is the oracle — deliberately
/// sharing no code with the conversion under test.
double
width_at(std::span<const Poly3> outer, std::span<const Poly3> inner, int lane_odr_id, double s) {
  const double sign = lane_odr_id > 0 ? 1.0 : -1.0;
  const double far = eval_profile(outer, s);
  const double near = inner.empty() ? 0.0 : eval_profile(inner, s);
  return sign * (far - near);
}

} // namespace

TEST(LaneBorder, RebasingACubicPreservesEveryValue) {
  // The one piece of arithmetic the whole conversion rests on. A cubic moved to
  // a new origin must describe the SAME curve; get the expansion wrong and the
  // error is zero at the new origin and grows from there, which is exactly the
  // shape of bug a record-start-only comparison misses.
  const Poly3 poly{.s = 10.0, .a = 2.0, .b = -0.5, .c = 0.03, .d = -0.001};
  for (const double origin : {0.0, 10.0, 25.0, 60.0}) {
    const Poly3 moved = rebase_poly3(poly, origin);
    EXPECT_DOUBLE_EQ(moved.s, origin);
    for (double s = 0.0; s <= 100.0; s += 0.5) {
      EXPECT_NEAR(moved.eval(s), poly.eval(s), 1e-9) << "origin " << origin << " at s " << s;
    }
  }
}

TEST(LaneBorder, AConstantBorderBecomesAConstantWidth) {
  const std::vector<Poly3> outer{Poly3{.s = 0.0, .a = -3.5}};
  const std::vector<Poly3> widths = widths_from_borders(outer, {}, -1);
  ASSERT_EQ(widths.size(), 1U);
  EXPECT_DOUBLE_EQ(widths[0].s, 0.0);
  EXPECT_DOUBLE_EQ(widths[0].a, 3.5) << "a right lane's width is the NEGATED border gap";
  EXPECT_DOUBLE_EQ(widths[0].b, 0.0);
}

TEST(LaneBorder, TheInnerNeighbourIsSubtractedOnBothSides) {
  // Left and right differ only in the sign of t, so the same border magnitudes
  // must give the same width. A sign error passes a right-only test.
  const std::vector<Poly3> inner_left{Poly3{.s = 0.0, .a = 3.5}};
  const std::vector<Poly3> outer_left{Poly3{.s = 0.0, .a = 7.0}};
  const std::vector<Poly3> inner_right{Poly3{.s = 0.0, .a = -3.5}};
  const std::vector<Poly3> outer_right{Poly3{.s = 0.0, .a = -7.0}};

  EXPECT_DOUBLE_EQ(widths_from_borders(outer_left, inner_left, 2).at(0).a, 3.5);
  EXPECT_DOUBLE_EQ(widths_from_borders(outer_right, inner_right, -2).at(0).a, 3.5);
}

TEST(LaneBorder, TheResultBreaksWhereTheInnerProfileBreaks) {
  // ★ The case a naive conversion gets wrong. This lane's own border never
  // breaks; its inner neighbour's does. If the conversion only walked the
  // lane's own records it would emit one constant width and be wrong over the
  // whole second half — the lane would not narrow as its neighbour widened.
  const std::vector<Poly3> outer{Poly3{.s = 0.0, .a = -9.0}};
  const std::vector<Poly3> inner{Poly3{.s = 0.0, .a = -7.0},
                                 Poly3{.s = 50.0, .a = -7.0, .b = -0.02}};

  const std::vector<Poly3> widths = widths_from_borders(outer, inner, -3);
  ASSERT_EQ(widths.size(), 2U) << "the inner profile's breakpoint must appear in the result";
  EXPECT_DOUBLE_EQ(widths[0].s, 0.0);
  EXPECT_DOUBLE_EQ(widths[1].s, 50.0);
  EXPECT_DOUBLE_EQ(widths[0].a, 2.0);
  EXPECT_DOUBLE_EQ(widths[0].b, 0.0);
  EXPECT_DOUBLE_EQ(widths[1].a, 2.0);
  EXPECT_DOUBLE_EQ(widths[1].b, -0.02) << "it must NARROW as the lane inside it widens";
}

TEST(LaneBorder, TheConversionAgreesWithTheBordersAtEveryStation) {
  // The exactness claim itself, swept rather than sampled at the record starts.
  // Cubic borders with breakpoints that do NOT line up, so every mechanism —
  // re-basing, the union, the sign — has to be right simultaneously.
  const std::vector<Poly3> inner{
      Poly3{.s = 0.0, .a = -3.0, .b = -0.01, .c = 0.0004, .d = -0.000002},
      Poly3{.s = 33.0, .a = -3.7, .b = 0.02, .c = -0.0001, .d = 0.0},
  };
  const std::vector<Poly3> outer{
      Poly3{.s = 0.0, .a = -6.5, .b = 0.005, .c = 0.0, .d = 0.0000015},
      Poly3{.s = 20.0, .a = -6.4, .b = -0.03, .c = 0.0002, .d = 0.0},
      Poly3{.s = 71.0, .a = -8.1, .b = 0.011, .c = 0.0, .d = 0.0},
  };

  const std::vector<Poly3> widths = widths_from_borders(outer, inner, -2);
  ASSERT_EQ(widths.size(), 4U)
      << "0, 20, 33 and 71 are all breakpoints of one profile or the other";

  for (double s = 0.0; s <= 100.0; s += 0.25) {
    EXPECT_NEAR(eval_profile(widths, s), width_at(outer, inner, -2, s), 1e-9)
        << "converted width disagrees with the borders at s = " << s;
  }
}

TEST(LaneBorder, TheWidthIsDefinedFromTheSectionStartEvenIfTheBordersAreNot) {
  // asam.net:xodr:1.4.0:road.lane.width.width_defined_whole_section wants a
  // record at s = 0. A profile evaluated ahead of its first record uses that
  // record, so emitting one at 0 changes no value — it just closes the gap.
  const std::vector<Poly3> outer{Poly3{.s = 12.0, .a = -4.0, .b = -0.1}};
  const std::vector<Poly3> widths = widths_from_borders(outer, {}, -1);
  ASSERT_FALSE(widths.empty());
  EXPECT_DOUBLE_EQ(widths.front().s, 0.0);
  for (double s = 0.0; s <= 60.0; s += 0.5) {
    EXPECT_NEAR(eval_profile(widths, s), width_at(outer, {}, -1, s), 1e-9);
  }
}

TEST(LaneBorder, ALaneWithNoBordersConvertsToNothing) {
  EXPECT_TRUE(widths_from_borders({}, {}, -1).empty());
}

} // namespace roadmaker::xodr
