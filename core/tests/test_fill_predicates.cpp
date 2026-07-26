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

// Point-in-polygon soundness for the shared surface-fill backend (issue #442).
//
// These reach into core/src because `fill_backend.hpp` is an internal header
// and the defect is invisible through the public mesh API: it lives entirely in
// which grid points a fill decides are interior, and the harmonic solve absorbs
// a ragged interior point set without ever failing a watertightness or
// byte-identity assertion. Testing the predicate directly is the only way to
// pin the mechanism. The header is fully `inline`, so including it from a test
// translation unit is ODR-safe.

#include <clipper2/clipper.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <numbers>
#include <vector>

#include "mesh/fill_backend.hpp"

namespace {

using Clipper2Lib::PathD;
using Clipper2Lib::PathsD;
using Clipper2Lib::PointD;
using roadmaker::fill_backend::inside_path;
using roadmaker::fill_backend::inside_region;
using roadmaker::fill_backend::kSteinerStep;

// A CCW axis-aligned rectangle. Placed far from the origin on purpose: it is
// the coordinate DELTAS the broken predicate truncates, never the coordinates.
PathD rect_ccw(double x0, double y0, double x1, double y1) {
  return PathD{PointD{x0, y0}, PointD{x1, y0}, PointD{x1, y1}, PointD{x0, y1}};
}

/// A circle of `radius` about `cx, cy`, tessellated to roughly `chord`-long
/// segments and wound CCW. `chord` defaults to the scale a `JoinType::Round`
/// erosion produces at the corners of a junction floor: decimeters.
PathD circle_ccw(double cx, double cy, double radius, double chord = 0.2) {
  const int steps = static_cast<int>(std::ceil((2.0 * std::numbers::pi * radius) / chord));
  PathD path;
  path.reserve(static_cast<std::size_t>(steps));
  for (int i = 0; i < steps; ++i) {
    const double theta = (2.0 * std::numbers::pi * static_cast<double>(i)) / steps;
    path.emplace_back(cx + (radius * std::cos(theta)), cy + (radius * std::sin(theta)));
  }
  return path;
}

// The upstream defect this suite exists for, asserted directly so a future
// reader does not have to take the comment in fill_backend.hpp on faith.
// Clipper2's CrossProductSign casts the edge deltas to __int128_t; on a PathD
// every difference below 1.0 truncates to zero, both products come out 0, and
// the predicate answers "on the edge" for a point at the dead center.
TEST(FillPredicates, ClipperPointInPolygonIsUnsoundOnSubMeterPaths) {
  const PathD half_meter = rect_ccw(100.0, 50.0, 100.5, 50.5);
  const PointD center{100.25, 50.25};

  EXPECT_EQ(Clipper2Lib::PointInPolygon(center, half_meter),
            Clipper2Lib::PointInPolygonResult::IsOn)
      << "upstream behavior changed — re-check whether #442's workaround is still needed";

  // The same ring scaled up past the truncation threshold answers correctly,
  // which is why the bug survived: every region anyone looked at was big.
  const PathD fifty_meter = rect_ccw(100.0, 50.0, 150.0, 100.0);
  EXPECT_EQ(Clipper2Lib::PointInPolygon(PointD{125.0, 75.0}, fifty_meter),
            Clipper2Lib::PointInPolygonResult::IsInside);
}

TEST(FillPredicates, InsideRegionAcceptsTheInteriorOfASubMeterRing) {
  const PathsD region{rect_ccw(100.0, 50.0, 100.5, 50.5)};

  EXPECT_TRUE(inside_region(region, PointD{100.25, 50.25}));
  EXPECT_TRUE(inside_region(region, PointD{100.05, 50.45}));
  EXPECT_FALSE(inside_region(region, PointD{100.75, 50.25}));
  EXPECT_FALSE(inside_region(region, PointD{100.25, 49.9}));
}

// Scale invariance: the same query against the same shape must not change
// answer because the ring was tessellated more finely.
TEST(FillPredicates, InsideRegionIsIndifferentToTessellationDensity) {
  const PointD probe{100.0, 100.0};
  for (const double chord : {0.05, 0.2, 1.0, 5.0}) {
    const PathsD coarse_or_fine{circle_ccw(100.0, 100.0, 20.0, chord)};
    EXPECT_TRUE(inside_region(coarse_or_fine, probe)) << "chord " << chord;
    EXPECT_FALSE(inside_region(coarse_or_fine, PointD{100.0, 130.0})) << "chord " << chord;
  }
}

// Hole semantics, unchanged from the Clipper-based implementation: CCW
// (positive area) is an outer contour, CW is a hole, and hole membership wins
// regardless of the order the paths arrive in.
TEST(FillPredicates, InsideRegionRejectsPointsInAHole) {
  PathD hole = rect_ccw(110.0, 60.0, 110.4, 60.4);
  std::ranges::reverse(hole); // CW == negative area == hole
  ASSERT_LT(Clipper2Lib::Area(hole), 0.0);

  const PathsD outer_first{rect_ccw(100.0, 50.0, 140.0, 90.0), hole};
  const PathsD hole_first{hole, rect_ccw(100.0, 50.0, 140.0, 90.0)};

  const PointD in_hole{110.2, 60.2};
  const PointD in_slab{130.0, 80.0};
  for (const PathsD& region : {outer_first, hole_first}) {
    EXPECT_FALSE(inside_region(region, in_hole));
    EXPECT_TRUE(inside_region(region, in_slab));
  }
}

// `InflatePaths` can hand back rings of fewer than three points when a region
// erodes to nothing. Clipper's predicate guarded that; the replacement must
// too, or the reverse iteration underflows on an empty path.
TEST(FillPredicates, InsideRegionRejectsDegenerateRings) {
  const PointD probe{100.0, 50.0};
  EXPECT_FALSE(inside_path(PathD{}, probe));
  EXPECT_FALSE(inside_path(PathD{PointD{100.0, 50.0}}, probe));
  EXPECT_FALSE(inside_path(PathD{PointD{100.0, 50.0}, PointD{101.0, 50.0}}, probe));
  EXPECT_FALSE(inside_region(PathsD{}, probe));
  EXPECT_FALSE(inside_region(PathsD{PathD{}}, probe));
}

// The consequence the issue is actually about, at the one call site.
//
// The region is a square with a deep ACUTE notch cut into one side — the shape
// a skew junction's footprint union takes between two arms meeting well off
// perpendicular. That matters: it is specifically the acute geometry that
// triggers the defect, and it took measuring to establish. Eroding by half a
// Steiner step drives the notch walls together, and the `JoinType::Round`
// offset resolves the apex into millimeter-long edges. Query points within a
// few decimeters of those edges are exactly the case where Clipper's truncated
// cross product loses its sign, so grid points meters inside the pavement were
// dropped and points outside it were accepted.
//
// What does NOT trigger it, verified while writing this: a convex region at any
// tessellation density (a 0.2 m-chord circle disagrees nowhere), and reflex
// corners on their own (a plus-shaped four-arm footprint disagrees nowhere once
// its walls are off the grid lines). Junction floors, ground surfaces and
// terrain therefore only ever differed at skew and tight-radius corners — which
// is precisely the set of quality-matrix cases whose meshes this fix moves.
TEST(FillPredicates, SteinerGridSeedsTheInteriorOfAnAcuteNotch) {
  // Square [80, 120]^2 minus the triangle (kLo, kHi) - (kApexX, kApexY) - (kHi, kHi).
  constexpr double kLo = 80.0;
  constexpr double kHi = 120.0;
  constexpr double kApexX = 100.0;
  constexpr double kApexY = 92.0;
  const PathsD region{PathD{PointD{kLo, kLo},
                            PointD{kHi, kLo},
                            PointD{kHi, kHi},
                            PointD{kApexX, kApexY},
                            PointD{kLo, kHi}}};
  ASSERT_GT(Clipper2Lib::Area(region.front()), 0.0) << "region must be CCW";

  std::vector<CDT::V2d<double>> vertices;
  roadmaker::fill_backend::steiner_grid_fill(region, vertices, kLo, kLo, kHi, kHi);

  ASSERT_FALSE(vertices.empty()) << "the interior grid was dropped wholesale";

  // Ground truth, independent of any point-in-polygon code: `steiner_grid_fill`
  // erodes by half a Steiner step, and eroding a polygon by a disk of radius r
  // keeps exactly the points that are inside it and at least r from its
  // boundary. Distance to the boundary is a plain point-to-segment minimum over
  // the twelve edges, and "inside the plus" is two rectangle tests. A margin
  // absorbs the arc tolerance of the round joins near the reflex corners.
  constexpr double kErosion = 0.5 * kSteinerStep;
  constexpr double kMargin = 0.05;
  const PathD& ring = region.front();
  const auto boundary_distance = [&ring](double x, double y) {
    double best = std::numeric_limits<double>::max();
    for (std::size_t i = 0, j = ring.size() - 1; i < ring.size(); j = i++) {
      const double ax = ring[j].x;
      const double ay = ring[j].y;
      const double bx = ring[i].x;
      const double by = ring[i].y;
      const double dx = bx - ax;
      const double dy = by - ay;
      const double t =
          std::clamp((((x - ax) * dx) + ((y - ay) * dy)) / ((dx * dx) + (dy * dy)), 0.0, 1.0);
      best = std::min(best, std::hypot(x - (ax + (t * dx)), y - (ay + (t * dy))));
    }
    return best;
  };

  std::size_t inside_checked = 0;
  std::size_t outside_checked = 0;
  const auto steps = static_cast<std::size_t>((kHi - kLo) / kSteinerStep);
  for (std::size_t iy = 1; iy < steps; ++iy) {
    for (std::size_t ix = 1; ix < steps; ++ix) {
      const double x = kLo + (static_cast<double>(ix) * kSteinerStep);
      const double y = kLo + (static_cast<double>(iy) * kSteinerStep);
      // Inside the square and outside the notch triangle. Both tests are plain
      // arithmetic, so the ground truth owes nothing to the code under test.
      const bool in_square = x >= kLo && x <= kHi && y >= kLo && y <= kHi;
      // Cross products against the two notch walls, both directed away from the
      // apex. The notch interior is right of the left wall and left of the
      // right wall; the region is everything else in the square.
      const double cross_left = ((kLo - kApexX) * (y - kApexY)) - ((kHi - kApexY) * (x - kApexX));
      const double cross_right = ((kHi - kApexX) * (y - kApexY)) - ((kHi - kApexY) * (x - kApexX));
      const bool in_notch = y > kApexY && cross_left < 0.0 && cross_right > 0.0;
      const bool in_region = in_square && !in_notch;
      const double distance = boundary_distance(x, y);
      const bool seeded = std::ranges::any_of(vertices, [x, y](const CDT::V2d<double>& v) {
        return std::abs(v.x - x) < 1e-9 && std::abs(v.y - y) < 1e-9;
      });

      if (in_region && distance > kErosion + kMargin) {
        ++inside_checked;
        EXPECT_TRUE(seeded) << "interior point (" << x << ", " << y << "), " << distance
                            << " m from the boundary, was dropped";
      } else if (!in_region || distance < kErosion - kMargin) {
        ++outside_checked;
        EXPECT_FALSE(seeded) << "point (" << x << ", " << y << ") should not have been seeded";
      }
    }
  }
  EXPECT_GT(inside_checked, 100U) << "the sweep itself covered nothing";
  EXPECT_GT(outside_checked, 100U) << "the sweep itself covered nothing";
}

} // namespace
