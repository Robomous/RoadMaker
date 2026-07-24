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

#include "roadmaker/road/grade_separation.hpp"

#include "roadmaker/edit/connection.hpp"
#include "roadmaker/geometry/poly3.hpp"
#include "roadmaker/geometry/reference_line.hpp"
#include "roadmaker/road/network.hpp"
#include "roadmaker/road/road.hpp"
#include "roadmaker/tol.hpp"

#include <array>
#include <cmath>
#include <cstddef>
#include <optional>
#include <vector>

namespace roadmaker {

namespace {

/// The reference line of one road, sampled to a plan-view polyline with each
/// vertex's arc-length station kept so a crossing maps back to an s value.
struct RoadPolyline {
  RoadId id;
  std::vector<double> s;                 ///< station at each vertex
  std::vector<std::array<double, 2>> xy; ///< plan-view point at each vertex
};

RoadPolyline sample_polyline(RoadId id, const Road& road) {
  RoadPolyline out;
  out.id = id;
  if (road.plan_view.empty()) {
    return out;
  }
  out.s = sample_stations(road.plan_view);
  out.xy.reserve(out.s.size());
  for (const double s : out.s) {
    const PathPoint p = road.plan_view.evaluate(s);
    out.xy.push_back({p.x, p.y});
  }
  return out;
}

double cross2(const std::array<double, 2>& a, const std::array<double, 2>& b) {
  return (a[0] * b[1]) - (a[1] * b[0]);
}

/// The (t, u) parameters where segments p1->p2 and p3->p4 intersect, each in
/// [0, 1], or nullopt for parallel/non-crossing segments.
std::optional<std::array<double, 2>> segment_intersection(const std::array<double, 2>& p1,
                                                          const std::array<double, 2>& p2,
                                                          const std::array<double, 2>& p3,
                                                          const std::array<double, 2>& p4) {
  const std::array<double, 2> d1{p2[0] - p1[0], p2[1] - p1[1]};
  const std::array<double, 2> d2{p4[0] - p3[0], p4[1] - p3[1]};
  const double denom = cross2(d1, d2);
  if (std::abs(denom) < 1e-12) {
    return std::nullopt; // parallel or degenerate
  }
  const std::array<double, 2> r{p3[0] - p1[0], p3[1] - p1[1]};
  const double t = cross2(r, d2) / denom;
  const double u = cross2(r, d1) / denom;
  if (t < 0.0 || t > 1.0 || u < 0.0 || u > 1.0) {
    return std::nullopt;
  }
  return std::array<double, 2>{t, u};
}

double lerp(double a, double b, double f) {
  return a + ((b - a) * f);
}

/// Junctions a road touches: the junction at either end plus, for a connecting
/// road, its owning junction. Two roads sharing any of these are "connected"
/// and can never form an overpass (design §4).
std::array<std::optional<JunctionId>, 3>
touched_junctions(const RoadNetwork& network, RoadId id, const Road& road) {
  std::array<std::optional<JunctionId>, 3> out{};
  out[0] = edit::junction_at_end(network, RoadEnd{.road = id, .contact = ContactPoint::Start});
  out[1] = edit::junction_at_end(network, RoadEnd{.road = id, .contact = ContactPoint::End});
  if (road.junction.is_valid()) {
    out[2] = road.junction;
  }
  return out;
}

bool junction_connected(const std::array<std::optional<JunctionId>, 3>& a,
                        const std::array<std::optional<JunctionId>, 3>& b) {
  for (const std::optional<JunctionId>& ja : a) {
    if (!ja.has_value()) {
      continue;
    }
    for (const std::optional<JunctionId>& jb : b) {
      if (jb.has_value() && *jb == *ja) {
        return true;
      }
    }
  }
  return false;
}

} // namespace

std::vector<GradeSeparation> find_grade_separations(const RoadNetwork& network, double clearance) {
  struct Entry {
    RoadId id;
    const Road* road;
    RoadPolyline poly;
    std::array<std::optional<JunctionId>, 3> junctions;
  };

  std::vector<Entry> roads;
  network.for_each_road([&](RoadId id, const Road& road) {
    RoadPolyline poly = sample_polyline(id, road);
    if (poly.xy.size() >= 2) {
      roads.push_back({id, &road, std::move(poly), touched_junctions(network, id, road)});
    }
  });

  std::vector<GradeSeparation> result;
  for (std::size_t i = 0; i < roads.size(); ++i) {
    for (std::size_t j = i + 1; j < roads.size(); ++j) {
      const Entry& a = roads[i];
      const Entry& b = roads[j];
      if (junction_connected(a.junctions, b.junctions)) {
        continue; // an intersection, not an overpass
      }
      // The best (max-clearance) qualifying crossing for this pair — a pair that
      // crosses twice (e.g. a loop over a road) reports the clearest crossing.
      std::optional<GradeSeparation> best;
      for (std::size_t sa = 0; sa + 1 < a.poly.xy.size(); ++sa) {
        for (std::size_t sb = 0; sb + 1 < b.poly.xy.size(); ++sb) {
          const std::optional<std::array<double, 2>> hit = segment_intersection(
              a.poly.xy[sa], a.poly.xy[sa + 1], b.poly.xy[sb], b.poly.xy[sb + 1]);
          if (!hit.has_value()) {
            continue;
          }
          const double s_a = lerp(a.poly.s[sa], a.poly.s[sa + 1], (*hit)[0]);
          const double s_b = lerp(b.poly.s[sb], b.poly.s[sb + 1], (*hit)[1]);
          const double z_a = eval_profile(a.road->elevation, s_a);
          const double z_b = eval_profile(b.road->elevation, s_b);
          const double gap = std::abs(z_a - z_b);
          if (gap + tol::kLength < clearance) {
            continue; // too close vertically — an at-grade crossing
          }
          const bool a_over = z_a >= z_b;
          GradeSeparation sep{.upper = a_over ? a.id : b.id,
                              .lower = a_over ? b.id : a.id,
                              .s_upper = a_over ? s_a : s_b,
                              .s_lower = a_over ? s_b : s_a,
                              .clearance = gap};
          if (!best.has_value() || sep.clearance > best->clearance) {
            best = sep;
          }
        }
      }
      if (best.has_value()) {
        result.push_back(*best);
      }
    }
  }
  return result;
}

} // namespace roadmaker
