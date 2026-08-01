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

#include "roadmaker/geometry/poly3.hpp"
#include "roadmaker/tol.hpp"

#include <algorithm>
#include <cmath>
#include <span>
#include <vector>

/// `<border>` -> `<width>` conversion (ASAM OpenDRIVE 1.9.0 §11.7.2).
///
/// Internal to the reader: RoadMaker's model stores lane geometry as widths, so
/// borders are converted on read rather than carried as a second encoding.
///
/// The header lives under core/src and is entirely `inline`, so a test TU can
/// include it and pin the arithmetic directly (core/tests has core/src on its
/// include path). Being header-only is also what keeps it working under
/// RM_BUILD_SHARED: only RM_API symbols leave the shared kernel, and an
/// internal helper has no business being one — so it must not need to cross the
/// library boundary at all. Same shape, and the same reason, as
/// mesh/fill_backend.hpp.
namespace roadmaker::xodr {

/// Shift a cubic to a new origin, exactly.
///
/// A `Poly3` means `a + b·ds + c·ds² + d·ds³` with `ds = s − poly.s`. Moving the
/// origin to `origin` is a change of variable `ds = du + h` with `h = origin −
/// poly.s`, expanded and regrouped — all four coefficients change, and the
/// result describes the SAME curve. No approximation is involved, which is what
/// lets a border profile be re-expressed as a width profile without loss.
[[nodiscard]] constexpr Poly3 rebase_poly3(const Poly3& poly, double origin) {
  const double h = origin - poly.s;
  return Poly3{
      .s = origin,
      .a = poly.a + (h * (poly.b + (h * (poly.c + (h * poly.d))))),
      .b = poly.b + (h * ((2.0 * poly.c) + (3.0 * poly.d * h))),
      .c = poly.c + (3.0 * poly.d * h),
      .d = poly.d,
  };
}

namespace detail {

/// The record covering `s` — the last one starting at or before it, and the
/// first record for a query ahead of the profile. Deliberately the same rule
/// `eval_profile` uses, so a converted width evaluates to the border value at
/// every station rather than only inside the records' own spans.
[[nodiscard]] inline const Poly3* covering_record(std::span<const Poly3> profile, double s) {
  const Poly3* found = &profile.front();
  for (const Poly3& poly : profile) {
    if (poly.s <= s + tol::kLength) {
      found = &poly;
    }
  }
  return found;
}

} // namespace detail

/// The width profile equivalent to a lane's border profile.
///
/// §11.7.2: a `<border>` gives `t_border(ds)`, the lane's **outer** t-limit,
/// measured in the road's t coordinate. A lane's width is therefore the gap
/// between its own border and the border of the next lane inward — the lane
/// with the same sign of `@id` and an absolute value one smaller. For `|@id| ==
/// 1` there is no inner lane and the inner limit is the centre, so pass an empty
/// `inner`.
///
/// `outer` and `inner` are section-local and sorted ascending by `s`, exactly as
/// `<border>`'s `@sOffset` is defined ("relative to the position of the
/// preceding `<laneSection>` element") — the same frame `Lane::widths` uses, so
/// no frame conversion is involved.
///
/// The two profiles may break at different stations, so the result breaks at the
/// UNION of their stations, with both operands re-based onto each segment's own
/// origin first. A difference of two cubics is a cubic, so every segment is
/// represented exactly; nothing is sampled or fitted.
///
/// The first record is emitted at `s = 0` regardless of where the borders start.
/// A profile evaluated before its first record uses that record (`eval_profile`),
/// so this changes no value — it just makes the width defined across the whole
/// section, which `asam.net:xodr:1.4.0:road.lane.width.width_defined_whole_section`
/// requires and a gap at the section head would violate.
///
/// Returns empty for an empty `outer` (the lane declared no borders).
[[nodiscard]] inline std::vector<Poly3>
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
    const Poly3 far = rebase_poly3(*detail::covering_record(outer, station), station);
    const Poly3 near = inner.empty()
                           ? Poly3{.s = station}
                           : rebase_poly3(*detail::covering_record(inner, station), station);
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
