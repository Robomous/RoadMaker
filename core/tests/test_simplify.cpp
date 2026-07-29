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

// The polyline simplifier (p7-s4, #244).
//
// It gets its own suite rather than being exercised only through the OSM
// import, because an algorithm whose correctness gate is "an import test
// passed" is one nobody can debug. Every case below is hand-computed.

#include "roadmaker/geometry/simplify.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <vector>

using roadmaker::drop_near_duplicates;
using roadmaker::Point2;
using roadmaker::polyline_deviation;
using roadmaker::simplify_polyline;

TEST(Simplify, ShortPolylinesAreReturnedUnchanged) {
  const std::vector<Point2> two{{0.0, 0.0}, {10.0, 0.0}};
  EXPECT_EQ(simplify_polyline(two, 1.0), (std::vector<std::size_t>{0, 1}));

  const std::vector<Point2> one{{0.0, 0.0}};
  EXPECT_EQ(simplify_polyline(one, 1.0), (std::vector<std::size_t>{0}));

  EXPECT_TRUE(simplify_polyline({}, 1.0).empty());
}

TEST(Simplify, EndpointsAreAlwaysRetained) {
  // The property the whole import design rests on: because a road is split at
  // its topology nodes BEFORE it is simplified, a junction node is always an
  // endpoint, and an endpoint is never dropped. That is what makes "a junction
  // can never be simplified away" a theorem rather than a policy.
  const std::vector<Point2> flat{{0.0, 0.0}, {10.0, 0.0}, {20.0, 0.0}, {30.0, 0.0}, {40.0, 0.0}};
  const auto kept = simplify_polyline(flat, 1.0);
  ASSERT_GE(kept.size(), 2U);
  EXPECT_EQ(kept.front(), 0U);
  EXPECT_EQ(kept.back(), flat.size() - 1);
  // A straight line collapses to exactly its two ends.
  EXPECT_EQ(kept, (std::vector<std::size_t>{0, 4}));
}

TEST(Simplify, TheToleranceBoundaryIsExercisedOnBothSides) {
  // 0.4999 and 0.5001, never 0.5 — a fixture landing exactly on the threshold
  // proves nothing about whether the code compares with < or <=.
  const std::vector<Point2> under{{0.0, 0.0}, {10.0, 0.4999}, {20.0, 0.0}};
  EXPECT_EQ(simplify_polyline(under, 0.5), (std::vector<std::size_t>{0, 2}))
      << "a vertex inside the tolerance must be dropped";

  const std::vector<Point2> over{{0.0, 0.0}, {10.0, 0.5001}, {20.0, 0.0}};
  EXPECT_EQ(simplify_polyline(over, 0.5), (std::vector<std::size_t>{0, 1, 2}))
      << "a vertex outside the tolerance must be retained";
}

TEST(Simplify, TheGuaranteeIsThatDeviationNeverExceedsTolerance) {
  // RDP's own construction guarantee, asserted as such. This is NOT a
  // tolerance that can be widened when real data makes it uncomfortable:
  // loosening it would be loosening the algorithm's promise, which is a
  // visible lie in review rather than a quiet adjustment.
  std::vector<Point2> wiggly;
  for (int i = 0; i <= 40; ++i) {
    const double x = i * 5.0;
    // Amplitude 2 m, so plenty of vertices are outside a 0.5 m tolerance.
    wiggly.push_back({x, (i % 2 == 0) ? 0.0 : 2.0});
  }
  for (const double tolerance : {0.1, 0.5, 1.0, 3.0}) {
    const auto kept = simplify_polyline(wiggly, tolerance);
    EXPECT_LE(polyline_deviation(wiggly, kept), tolerance) << "tolerance " << tolerance;
  }
}

TEST(Simplify, ALargerToleranceNeverRetainsMore) {
  std::vector<Point2> wiggly;
  for (int i = 0; i <= 60; ++i) {
    wiggly.push_back({i * 4.0, (i % 3) * 0.9});
  }
  std::size_t previous = wiggly.size() + 1;
  for (const double tolerance : {0.1, 0.25, 0.5, 1.0, 2.0}) {
    const std::size_t kept = simplify_polyline(wiggly, tolerance).size();
    EXPECT_LE(kept, previous) << "tolerance " << tolerance;
    previous = kept;
  }
}

TEST(Simplify, DeviationIsMeasuredToTheSEGMENTNotTheInfiniteLine) {
  // A SPUR: the interior vertex projects far BEYOND the chord's end, which is
  // the only geometry that distinguishes the two formulations.
  //
  // This test was vacuous when first written. Its "hairpin" — (0,0), (50,1),
  // (0,2) — has a vertical chord onto which the apex projects at t = 0.5, i.e.
  // INSIDE the segment, so clamping changes nothing and removing the clamp
  // left it passing. The sabotage run is what found that; the shape below is
  // the repair.
  //
  // Chord (0,0)→(10,0); apex at (100,1) projects to t = 10, far outside.
  //   segment distance ≈ 90.006  → retained at a 5 m tolerance
  //   infinite-line distance = 1 → DROPPED, flattening the spur away
  const std::vector<Point2> spur{{0.0, 0.0}, {100.0, 1.0}, {10.0, 0.0}};

  EXPECT_NEAR(polyline_deviation(spur, std::vector<std::size_t>{0, 2}), 90.0056, 1e-3)
      << "the deviation itself must be the distance to the segment";

  const auto kept = simplify_polyline(spur, 5.0);
  EXPECT_EQ(kept, (std::vector<std::size_t>{0, 1, 2}))
      << "a vertex 90 m off the chord must survive a 5 m tolerance";
}

TEST(Simplify, ANonPositiveToleranceRetainsEverything) {
  const std::vector<Point2> points{{0.0, 0.0}, {1.0, 0.0}, {2.0, 0.0}, {3.0, 0.0}};
  EXPECT_EQ(simplify_polyline(points, 0.0).size(), points.size());
  EXPECT_EQ(simplify_polyline(points, -1.0).size(), points.size());
}

TEST(Simplify, ADeeplySplittingPolylineDoesNotOverflowTheStack) {
  // A sawtooth retains every vertex AND splits maximally unbalanced — every
  // odd vertex is equidistant from the chord, so the first one always wins the
  // scan and the span shrinks by one. The textbook RDP therefore recurses once
  // per vertex, and this function is reachable from a file parser, where the
  // input size is not our choice.
  //
  // 20 000 rather than 200 000 deliberately. That same shape is also RDP's
  // O(n²) worst case — 200 000 vertices measured 165 SECONDS here — so a
  // bigger input would buy a slower gate rather than a stronger claim. 10 000
  // recursive frames is already far past what this proves, and the header now
  // records the cost bound the measurement revealed.
  std::vector<Point2> saw;
  saw.reserve(20'000);
  for (int i = 0; i < 20'000; ++i) {
    saw.push_back({static_cast<double>(i), (i % 2 == 0) ? 0.0 : 10.0});
  }
  const auto kept = simplify_polyline(saw, 0.5);
  EXPECT_GT(kept.size(), 10'000U);
  EXPECT_EQ(kept.front(), 0U);
  EXPECT_EQ(kept.back(), saw.size() - 1);
}

TEST(PolylineDeviation, ReportsTheActualErrorNotTheRequestedOne) {
  // The number a compromise diagnostic must quote. Two ways simplified at the
  // same tolerance can deviate by 0.02 m and 0.49 m; quoting the tolerance
  // reports the question rather than the answer.
  const std::vector<Point2> points{{0.0, 0.0}, {10.0, 0.3}, {20.0, 0.0}};
  const auto kept = simplify_polyline(points, 0.5);
  ASSERT_EQ(kept, (std::vector<std::size_t>{0, 2}));
  EXPECT_NEAR(polyline_deviation(points, kept), 0.3, 1e-12);
}

TEST(PolylineDeviation, IsZeroWhenNothingWasDropped) {
  const std::vector<Point2> points{{0.0, 0.0}, {10.0, 5.0}, {20.0, 0.0}};
  EXPECT_DOUBLE_EQ(polyline_deviation(points, std::vector<std::size_t>{0, 1, 2}), 0.0);
  EXPECT_DOUBLE_EQ(polyline_deviation(points, std::vector<std::size_t>{0}), 0.0);
  EXPECT_DOUBLE_EQ(polyline_deviation(points, {}), 0.0);
}

// --- near-duplicate collapse ------------------------------------------------

TEST(DropNearDuplicates, CollapsesInteriorPointsButNeverTheEnds) {
  const std::vector<Point2> points{{0.0, 0.0}, {0.1, 0.0}, {0.2, 0.0}, {50.0, 0.0}, {50.05, 0.0}};
  const auto kept = drop_near_duplicates(points, 1.0);

  // The last pair is 0.05 m apart and BOTH survive: the final point carries
  // the road's topology, and dropping it would move the road's end.
  EXPECT_EQ(kept, (std::vector<std::size_t>{0, 3, 4}));
  EXPECT_EQ(kept.front(), 0U);
  EXPECT_EQ(kept.back(), points.size() - 1);
}

TEST(DropNearDuplicates, MeasuresFromTheLastRETAINEDPoint) {
  // Measuring from the immediately preceding SOURCE point instead lets a run
  // of sub-epsilon steps accumulate without bound — a hundred 0.9 m steps
  // would all survive a 1.0 m epsilon while covering 90 m.
  std::vector<Point2> creep;
  creep.reserve(102);
  for (int i = 0; i <= 100; ++i) {
    creep.push_back({i * 0.9, 0.0});
  }
  const auto kept = drop_near_duplicates(creep, 1.0);
  // Every retained interior step must be at least epsilon from its
  // predecessor, so roughly half survive rather than all of them.
  EXPECT_LT(kept.size(), creep.size());
  for (std::size_t i = 1; i + 1 < kept.size(); ++i) {
    const double dx = creep[kept[i]][0] - creep[kept[i - 1]][0];
    EXPECT_GE(dx, 1.0) << "retained pair " << i << " is closer than epsilon";
  }
}

TEST(DropNearDuplicates, ExactlyCoincidentPointsAreRemoved) {
  // The case clothoid fitting refuses outright — duplicate nodes are common in
  // real extracts.
  const std::vector<Point2> points{{0.0, 0.0}, {0.0, 0.0}, {10.0, 0.0}, {20.0, 0.0}};
  const auto kept = drop_near_duplicates(points, 1.0);
  EXPECT_EQ(kept, (std::vector<std::size_t>{0, 2, 3}));
}

TEST(DropNearDuplicates, ShortInputsAndNonPositiveEpsilonAreUntouched) {
  const std::vector<Point2> two{{0.0, 0.0}, {0.001, 0.0}};
  EXPECT_EQ(drop_near_duplicates(two, 1.0), (std::vector<std::size_t>{0, 1}));

  const std::vector<Point2> four{{0.0, 0.0}, {0.1, 0.0}, {0.2, 0.0}, {0.3, 0.0}};
  EXPECT_EQ(drop_near_duplicates(four, 0.0).size(), four.size());
}
