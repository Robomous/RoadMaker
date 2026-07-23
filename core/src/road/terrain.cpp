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

#include "roadmaker/road/terrain.hpp"

#include "roadmaker/geometry/poly3.hpp"
#include "roadmaker/geometry/reference_line.hpp"
#include "roadmaker/road/lane.hpp"
#include "roadmaker/road/lane_section.hpp"
#include "roadmaker/road/network.hpp"
#include "roadmaker/road/road.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace roadmaker {

namespace {

/// Widest half cross-section of a road, so the plan-view bounds taken from the
/// reference line still enclose the road SURFACE. Deliberately conservative:
/// it sums every lane's width on both sides and keeps the larger side, sampled
/// at each section start plus each width knot. Over-estimating only grows the
/// terrain grid a little, while under-estimating would clip terrain at a road
/// edge sitting on the field border.
double road_half_width(const RoadNetwork& network, const Road& road) {
  double widest = 0.0;
  for (const LaneSectionId section_id : road.sections) {
    const LaneSection* section = network.lane_section(section_id);
    if (section == nullptr) {
      continue;
    }
    // Stations to probe, section-local: the start plus every width knot of
    // every lane (a width poly is largest at one of its own knots or at the
    // section end, and the neighbouring section's start covers the latter).
    std::vector<double> probes{0.0};
    for (const LaneId lane_id : section->lanes) {
      const Lane* lane = network.lane(lane_id);
      if (lane == nullptr) {
        continue;
      }
      for (const Poly3& width : lane->widths) {
        probes.push_back(width.s);
      }
    }
    std::ranges::sort(probes);
    probes.erase(std::unique(probes.begin(), probes.end()), probes.end());

    for (const double local_s : probes) {
      double left = 0.0;
      double right = 0.0;
      for (const LaneId lane_id : section->lanes) {
        const Lane* lane = network.lane(lane_id);
        if (lane == nullptr || lane->odr_id == 0) {
          continue;
        }
        const double width = std::max(0.0, eval_profile(lane->widths, local_s));
        (lane->odr_id > 0 ? left : right) += width;
      }
      const double offset = std::abs(eval_profile(road.lane_offset, section->s0 + local_s));
      widest = std::max(widest, std::max(left, right) + offset);
    }
  }
  return widest;
}

} // namespace

double sample_height(const HeightField& field, double x, double y) {
  if (field.empty() || field.spacing <= 0.0 || field.heights.size() != field.cols * field.rows) {
    return 0.0;
  }
  // Continuous grid coordinates, then clamp so the field extends FLAT beyond
  // its extent instead of dropping to zero at the border.
  const double gx =
      std::clamp((x - field.origin_x) / field.spacing, 0.0, static_cast<double>(field.cols - 1));
  const double gy =
      std::clamp((y - field.origin_y) / field.spacing, 0.0, static_cast<double>(field.rows - 1));

  const auto col0 = static_cast<std::size_t>(std::floor(gx));
  const auto row0 = static_cast<std::size_t>(std::floor(gy));
  const std::size_t col1 = std::min(col0 + 1, field.cols - 1);
  const std::size_t row1 = std::min(row0 + 1, field.rows - 1);
  const double tx = gx - static_cast<double>(col0);
  const double ty = gy - static_cast<double>(row0);

  const double z00 = field.heights[(row0 * field.cols) + col0];
  const double z10 = field.heights[(row0 * field.cols) + col1];
  const double z01 = field.heights[(row1 * field.cols) + col0];
  const double z11 = field.heights[(row1 * field.cols) + col1];
  const double low = z00 + ((z10 - z00) * tx);
  const double high = z01 + ((z11 - z01) * tx);
  return low + ((high - low) * ty);
}

std::array<double, 4> field_extent(const HeightField& field) {
  if (field.empty()) {
    return {0.0, 0.0, 0.0, 0.0};
  }
  return {field.origin_x,
          field.origin_y,
          field.origin_x + (field.spacing * static_cast<double>(field.cols - 1)),
          field.origin_y + (field.spacing * static_cast<double>(field.rows - 1))};
}

HeightField make_flat_field(const RoadNetwork& network, double spacing, double margin) {
  const std::optional<std::array<double, 4>> bounds = network_plan_bounds(network);
  if (!bounds.has_value() || !(spacing > 0.0) || !std::isfinite(margin)) {
    return {};
  }
  const double pad = std::max(0.0, margin);
  double lo_x = (*bounds)[0] - pad;
  double lo_y = (*bounds)[1] - pad;
  const double hi_x = (*bounds)[2] + pad;
  const double hi_y = (*bounds)[3] + pad;

  // Snap the origin OUT to a whole multiple of the REQUESTED spacing so the
  // same network always produces the same grid regardless of where its roads
  // happen to sit. The origin is fixed here and never moves again — re-snapping
  // after the coarsening below would grow the span and put the post count back
  // over the cap.
  lo_x = std::floor(lo_x / spacing) * spacing;
  lo_y = std::floor(lo_y / spacing) * spacing;

  // Posts needed to COVER `length`: the last post sits at (n-1)*s >= length.
  const auto posts_at = [](double length, double s) {
    return static_cast<std::size_t>(std::floor(length / s)) + 2;
  };

  // Clamp each axis to kMaxFieldSamples posts by COARSENING, never by cropping:
  // a field that does not cover the network would clip terrain at its border.
  // kMaxFieldSamples - 2 (not - 1) is what makes posts_at land ON the cap
  // rather than one past it.
  double step = spacing;
  const double span = std::max(hi_x - lo_x, hi_y - lo_y);
  if (posts_at(span, step) > kMaxFieldSamples) {
    step = span / static_cast<double>(kMaxFieldSamples - 2);
  }

  HeightField field;
  field.origin_x = lo_x;
  field.origin_y = lo_y;
  field.spacing = step;
  field.cols = posts_at(hi_x - lo_x, step);
  field.rows = posts_at(hi_y - lo_y, step);
  field.heights.assign(field.cols * field.rows, 0.0);
  return field;
}

std::optional<std::array<double, 4>> network_plan_bounds(const RoadNetwork& network) {
  double lo_x = std::numeric_limits<double>::max();
  double lo_y = std::numeric_limits<double>::max();
  double hi_x = std::numeric_limits<double>::lowest();
  double hi_y = std::numeric_limits<double>::lowest();
  bool any = false;

  network.for_each_road([&](RoadId, const Road& road) {
    if (road.plan_view.empty()) {
      return;
    }
    // The reference line bounds the road's CENTER; grow by the widest half
    // cross-section so the returned box encloses the road surface itself.
    const double half = road_half_width(network, road);
    for (const double s : sample_stations(road.plan_view)) {
      const PathPoint point = road.plan_view.evaluate(s);
      lo_x = std::min(lo_x, point.x - half);
      lo_y = std::min(lo_y, point.y - half);
      hi_x = std::max(hi_x, point.x + half);
      hi_y = std::max(hi_y, point.y + half);
      any = true;
    }
  });

  if (!any) {
    return std::nullopt;
  }
  return std::array<double, 4>{lo_x, lo_y, hi_x, hi_y};
}

} // namespace roadmaker
