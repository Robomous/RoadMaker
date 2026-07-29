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

#include "roadmaker/geometry/simplify.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

namespace roadmaker {
namespace {

/// Perpendicular distance from `p` to the segment `a`–`b`, treating a
/// degenerate segment as the point `a`.
///
/// SEGMENT, not infinite line. RDP is usually written against the line, and on
/// a polyline that doubles back — a switchback, which real road data is full
/// of — the line form measures to a projection beyond the segment's end and
/// reports a distance smaller than the vertex's actual displacement, retaining
/// too little.
double point_segment_distance(const Point2& p, const Point2& a, const Point2& b) {
  const double dx = b[0] - a[0];
  const double dy = b[1] - a[1];
  const double len2 = (dx * dx) + (dy * dy);
  if (len2 <= 0.0) {
    return std::hypot(p[0] - a[0], p[1] - a[1]);
  }
  double t = (((p[0] - a[0]) * dx) + ((p[1] - a[1]) * dy)) / len2;
  t = std::clamp(t, 0.0, 1.0);
  return std::hypot(p[0] - (a[0] + (t * dx)), p[1] - (a[1] + (t * dy)));
}

} // namespace

std::vector<std::size_t> simplify_polyline(std::span<const Point2> points, double tolerance) {
  std::vector<std::size_t> kept;
  if (points.size() < 3 || !(tolerance > 0.0)) {
    kept.resize(points.size());
    for (std::size_t i = 0; i < points.size(); ++i) {
      kept[i] = i;
    }
    return kept;
  }

  // Iterative RDP. `keep[i]` marks a retained vertex; the stack holds the
  // half-open spans still to examine. Recursion here would be one frame per
  // retained vertex on an input a parser handed us.
  std::vector<bool> keep(points.size(), false);
  keep.front() = true;
  keep.back() = true;

  std::vector<std::pair<std::size_t, std::size_t>> stack;
  stack.reserve(32);
  stack.emplace_back(0, points.size() - 1);

  while (!stack.empty()) {
    const auto [first, last] = stack.back();
    stack.pop_back();
    if (last <= first + 1) {
      continue;
    }

    double worst = -1.0;
    std::size_t worst_at = first;
    for (std::size_t i = first + 1; i < last; ++i) {
      const double distance = point_segment_distance(points[i], points[first], points[last]);
      if (distance > worst) {
        worst = distance;
        worst_at = i;
      }
    }

    // Strictly greater: a vertex exactly AT the tolerance is within it, so it
    // goes. The fixtures deviate by 0.4999 and 0.5001 rather than by 0.5 so
    // this boundary is exercised on both sides rather than assumed.
    if (worst > tolerance) {
      keep[worst_at] = true;
      stack.emplace_back(first, worst_at);
      stack.emplace_back(worst_at, last);
    }
  }

  kept.reserve(points.size());
  for (std::size_t i = 0; i < points.size(); ++i) {
    if (keep[i]) {
      kept.push_back(i);
    }
  }
  return kept;
}

double polyline_deviation(std::span<const Point2> points, std::span<const std::size_t> kept) {
  if (kept.size() < 2 || points.empty()) {
    return 0.0;
  }
  double worst = 0.0;
  for (std::size_t segment = 0; segment + 1 < kept.size(); ++segment) {
    const std::size_t first = kept[segment];
    const std::size_t last = kept[segment + 1];
    if (first >= points.size() || last >= points.size()) {
      continue;
    }
    for (std::size_t i = first + 1; i < last; ++i) {
      worst = std::max(worst, point_segment_distance(points[i], points[first], points[last]));
    }
  }
  return worst;
}

std::vector<std::size_t> drop_near_duplicates(std::span<const Point2> points, double epsilon) {
  std::vector<std::size_t> kept;
  if (points.size() < 3 || !(epsilon > 0.0)) {
    kept.resize(points.size());
    for (std::size_t i = 0; i < points.size(); ++i) {
      kept[i] = i;
    }
    return kept;
  }

  kept.reserve(points.size());
  kept.push_back(0);
  const std::size_t last = points.size() - 1;
  for (std::size_t i = 1; i < last; ++i) {
    const Point2& previous = points[kept.back()];
    if (std::hypot(points[i][0] - previous[0], points[i][1] - previous[1]) >= epsilon) {
      kept.push_back(i);
    }
  }
  // The final point is retained unconditionally — it carries the topology.
  // Note this can leave the LAST retained pair closer than epsilon; that is
  // the correct trade, because dropping it would move the road's end.
  kept.push_back(last);
  return kept;
}

} // namespace roadmaker
