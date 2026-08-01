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

#include "lane_border.hpp"

#include "roadmaker/tol.hpp"

#include <algorithm>
#include <vector>

namespace roadmaker::xodr {

namespace {

/// The record covering `s` — the last one starting at or before it, and the
/// first record for a query ahead of the profile. Deliberately the same rule
/// `eval_profile` uses, so a converted width evaluates to the border value at
/// every station rather than only inside the records' own spans.
const Poly3* covering_record(std::span<const Poly3> profile, double s) {
  const Poly3* found = &profile.front();
  for (const Poly3& poly : profile) {
    if (poly.s <= s + tol::kLength) {
      found = &poly;
    }
  }
  return found;
}

} // namespace

std::vector<Poly3>
widths_from_borders(std::span<const Poly3> outer, std::span<const Poly3> inner, int lane_odr_id) {
  if (outer.empty()) {
    return {};
  }

  // Break where EITHER profile breaks: a segment is only exactly a cubic while
  // both operands stay on one record. Station 0 is always a break so the width
  // covers the section from its start.
  std::vector<double> stations{0.0};
  for (const Poly3& poly : outer) {
    stations.push_back(poly.s);
  }
  for (const Poly3& poly : inner) {
    stations.push_back(poly.s);
  }
  std::ranges::sort(stations);
  const auto duplicates = std::ranges::unique(
      stations, [](double lhs, double rhs) { return std::abs(lhs - rhs) <= tol::kLength; });
  stations.erase(duplicates.begin(), duplicates.end());

  // t grows to the LEFT, so a left lane's width is outer − inner and a right
  // lane's is the reverse; both borders are negative on the right.
  const double sign = lane_odr_id > 0 ? 1.0 : -1.0;

  std::vector<Poly3> widths;
  widths.reserve(stations.size());
  for (const double station : stations) {
    if (station < -tol::kLength) {
      continue; // a negative @sOffset is not a legal t_grEqZero; ignore it
    }
    const Poly3 far = rebase_poly3(*covering_record(outer, station), station);
    const Poly3 near = inner.empty() ? Poly3{.s = station}
                                     : rebase_poly3(*covering_record(inner, station), station);
    widths.push_back(Poly3{
        .s = station,
        .a = sign * (far.a - near.a),
        .b = sign * (far.b - near.b),
        .c = sign * (far.c - near.c),
        .d = sign * (far.d - near.d),
    });
  }
  return widths;
}

} // namespace roadmaker::xodr
