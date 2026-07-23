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

#include "junction_export.hpp"

#include "roadmaker/road/junction.hpp"
#include "roadmaker/tol.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "../mesh/junction_surface.hpp"
#include "../mesh/mesh_detail.hpp"

namespace roadmaker {

namespace {

// Elevation-grid spacing (03 §3): coarse enough to stay a "coarse square grid",
// fine enough that the spec's bicubic reconstruction (12.11.1) tracks the
// harmonic field within rm::tol. Never below 2 m; refines to extent/32 for
// large junctions.
constexpr double kMinGridSpacing = 2.0;
constexpr double kSpacingDivisor = 32.0;

struct Vec2 {
  double x, y;
};

/// A flat view of the surface mesh: 2D vertex positions, their z, and triangle
/// index triples — enough to point-locate and barycentrically sample the field.
struct SampledField {
  std::vector<Vec2> xy;
  std::vector<double> z;
  std::vector<std::array<std::uint32_t, 3>> tris;
};

SampledField flatten(const SubMesh& mesh) {
  SampledField field;
  const std::size_t n = mesh.positions.size() / 3;
  field.xy.reserve(n);
  field.z.reserve(n);
  for (std::size_t i = 0; i < n; ++i) {
    field.xy.push_back({mesh.positions[(3 * i)], mesh.positions[(3 * i) + 1]});
    field.z.push_back(mesh.positions[(3 * i) + 2]);
  }
  field.tris.reserve(mesh.indices.size() / 3);
  for (std::size_t t = 0; t + 2 < mesh.indices.size(); t += 3) {
    field.tris.push_back({mesh.indices[t], mesh.indices[t + 1], mesh.indices[t + 2]});
  }
  return field;
}

/// z of the field at (px, py): barycentric interpolation inside the containing
/// triangle, or the nearest vertex's z for points outside the footprint (the
/// grid's margin squares). The mesh boundary z equals road elevation, so the
/// nearest-vertex extension is continuous across the footprint edge.
double sample(const SampledField& field, double px, double py) {
  for (const auto& tri : field.tris) {
    const Vec2& a = field.xy[tri[0]];
    const Vec2& b = field.xy[tri[1]];
    const Vec2& c = field.xy[tri[2]];
    const double det = ((b.y - c.y) * (a.x - c.x)) + ((c.x - b.x) * (a.y - c.y));
    if (std::abs(det) < tol::kLength * tol::kLength) {
      continue; // degenerate triangle
    }
    const double l0 = (((b.y - c.y) * (px - c.x)) + ((c.x - b.x) * (py - c.y))) / det;
    const double l1 = (((c.y - a.y) * (px - c.x)) + ((a.x - c.x) * (py - c.y))) / det;
    const double l2 = 1.0 - l0 - l1;
    const double eps = 1e-9;
    if (l0 >= -eps && l1 >= -eps && l2 >= -eps) {
      return (l0 * field.z[tri[0]]) + (l1 * field.z[tri[1]]) + (l2 * field.z[tri[2]]);
    }
  }
  double best = std::numeric_limits<double>::max();
  double z = 0.0;
  for (std::size_t i = 0; i < field.xy.size(); ++i) {
    const double d = ((field.xy[i].x - px) * (field.xy[i].x - px)) +
                     ((field.xy[i].y - py) * (field.xy[i].y - py));
    if (d < best) {
      best = d;
      z = field.z[i];
    }
  }
  return z;
}

} // namespace

JunctionSurfaceExport build_junction_export(const RoadNetwork& network,
                                            const Junction& junction,
                                            const SamplingOptions& sampling) {
  JunctionSurfaceExport out;
  const SubMesh mesh = build_junction_surface(network, junction, sampling);
  if (mesh.positions.size() < 9 || mesh.indices.size() < 3) {
    return out; // no usable footprint — nothing to export
  }
  const SampledField field = flatten(mesh);

  // 1. Principal axis of the footprint (PCA on the surface vertices): the
  //    reference line is the eigenvector of the larger covariance eigenvalue
  //    through the centroid, so a perpendicular from it reaches every point
  //    (junctions.geometry.ref_line_definition).
  double cx = 0.0;
  double cy = 0.0;
  for (const Vec2& p : field.xy) {
    cx += p.x;
    cy += p.y;
  }
  cx /= static_cast<double>(field.xy.size());
  cy /= static_cast<double>(field.xy.size());

  double sxx = 0.0;
  double sxy = 0.0;
  double syy = 0.0;
  for (const Vec2& p : field.xy) {
    const double dx = p.x - cx;
    const double dy = p.y - cy;
    sxx += dx * dx;
    sxy += dx * dy;
    syy += dy * dy;
  }
  // Major-axis angle of the symmetric covariance [[sxx,sxy],[sxy,syy]]. atan2
  // is well defined even when sxy and (sxx-syy) both vanish (circular blob):
  // it returns 0, giving a deterministic axis along +x.
  const double theta = 0.5 * std::atan2(2.0 * sxy, sxx - syy);
  const Vec2 dir{std::cos(theta), std::sin(theta)};
  const Vec2 perp{-std::sin(theta), std::cos(theta)}; // +t is left of the line

  // 2. Extent of the footprint in the (dir, perp) frame.
  double s_min = std::numeric_limits<double>::max();
  double s_max = std::numeric_limits<double>::lowest();
  double t_min = s_min;
  double t_max = s_max;
  for (const Vec2& p : field.xy) {
    const double dx = p.x - cx;
    const double dy = p.y - cy;
    const double s = (dx * dir.x) + (dy * dir.y);
    const double t = (dx * perp.x) + (dy * perp.y);
    s_min = std::min(s_min, s);
    s_max = std::max(s_max, s);
    t_min = std::min(t_min, t);
    t_max = std::max(t_max, t);
  }
  const double extent = s_max - s_min;
  const double g = std::max(kMinGridSpacing, extent / kSpacingDivisor);

  // 3. A full rectangular grid covering the footprint plus one square of margin
  //    in every direction (elevation_grid requires complete squares outside the
  //    boundary so bicubic support points always exist). Columns run along the
  //    reference line; rows run perpendicular, symmetric about the line (center
  //    at t=0, `left` at +t, `right` at -t).
  const auto ceil_div = [g](double span) {
    return static_cast<std::size_t>(std::ceil(std::max(0.0, span) / g));
  };
  const std::size_t n_left = ceil_div(t_max) + 1;   // +t rows beyond center
  const std::size_t n_right = ceil_div(-t_min) + 1; // -t rows beyond center
  const double s_lo = s_min - g;
  const std::size_t n_cols = ceil_div((s_max + g) - s_lo) + 1;

  // 4. Reference line: starts at the first column, runs the full grid length.
  out.ref_line.x = cx + (s_lo * dir.x);
  out.ref_line.y = cy + (s_lo * dir.y);
  out.ref_line.hdg = theta;
  out.ref_line.length = static_cast<double>(n_cols - 1) * g;

  out.grid.s_start = 0.0;
  out.grid.grid_spacing = g;
  out.grid.columns.reserve(n_cols);
  for (std::size_t i = 0; i < n_cols; ++i) {
    const double s = s_lo + (static_cast<double>(i) * g);
    const double bx = cx + (s * dir.x);
    const double by = cy + (s * dir.y);
    JunctionGridColumn column;
    column.center = sample(field, bx, by);
    column.left.reserve(n_left);
    for (std::size_t k = 1; k <= n_left; ++k) {
      const double d = static_cast<double>(k) * g;
      column.left.push_back(sample(field, bx + (d * perp.x), by + (d * perp.y)));
    }
    column.right.reserve(n_right);
    for (std::size_t k = 1; k <= n_right; ++k) {
      const double d = static_cast<double>(k) * g;
      column.right.push_back(sample(field, bx - (d * perp.x), by - (d * perp.y)));
    }
    out.grid.columns.push_back(std::move(column));
  }

  out.has_surface = true;
  return out;
}

namespace {

/// An arm of a junction: the end of an incoming/outgoing road that a
/// connecting road attaches to.
struct Arm {
  RoadId road;
  ContactPoint contact = ContactPoint::Start;
  double x = 0.0;
  double y = 0.0;

  friend bool operator==(const Arm& a, const Arm& b) {
    return a.road == b.road && a.contact == b.contact;
  }
};

/// World position of a road's contact end.
Vec2 road_end_point(const Road& road, ContactPoint contact) {
  const double s = contact == ContactPoint::Start ? 0.0 : road.plan_view.length();
  const PathPoint pose = road.plan_view.evaluate(s);
  return {pose.x, pose.y};
}

/// The arm a connecting-road link points at (target is a road end), or nullopt
/// when the link is absent or targets a junction.
std::optional<Arm> link_arm(const RoadNetwork& network, const std::optional<RoadLink>& link) {
  if (!link.has_value()) {
    return std::nullopt;
  }
  const auto* road_target = std::get_if<RoadId>(&link->target);
  if (road_target == nullptr) {
    return std::nullopt;
  }
  const Road* road = network.road(*road_target);
  if (road == nullptr || road->plan_view.empty()) {
    return std::nullopt;
  }
  const Vec2 p = road_end_point(*road, link->contact);
  return Arm{.road = *road_target, .contact = link->contact, .x = p.x, .y = p.y};
}

/// One connecting road with the two arms it bridges (from = predecessor,
/// to = successor — connecting roads run start→end from incoming to outgoing).
struct Bridge {
  RoadId road;
  Arm from;
  Arm to;
  Vec2 mid; // reference-line midpoint, for outer-arc selection
};

double dist2(const Vec2& p, const Vec2& q) {
  const double dx = p.x - q.x;
  const double dy = p.y - q.y;
  return (dx * dx) + (dy * dy);
}

/// The two outer corners (leftmost and rightmost lane edges) of an arm road's
/// junction-facing end cross-section, in world coordinates. Uses the same
/// frame/offset helpers as the junction surface mesher (mesh_detail) so an
/// auxiliary boundary road's endpoints land on the exact mouth corners the
/// neighbouring joint caps cross.
std::pair<Vec2, Vec2> arm_end_corners(const RoadNetwork& network, const Arm& arm) {
  const Road& road = *network.road(arm.road);
  const double s = arm.contact == ContactPoint::Start ? 0.0 : road.plan_view.length();
  const mesh_detail::StationFrame frame = mesh_detail::make_frame(road, s);
  const LaneSection& section = mesh_detail::section_at(network, road, s);
  const std::vector<double> offsets = mesh_detail::boundary_offsets(network, road, section, s);
  const std::array<double, 3> left = mesh_detail::lateral_point(frame, offsets.front());
  const std::array<double, 3> right = mesh_detail::lateral_point(frame, offsets.back());
  return {Vec2{left[0], left[1]}, Vec2{right[0], right[1]}};
}

/// A deterministic, collision-free @id for an auxiliary boundary road, namespaced
/// by the junction id and the CCW gap index (e.g. "1_b0"); a numeric suffix is
/// appended only if that string already names a real road.
std::string
unique_aux_id(const RoadNetwork& network, const Junction& junction, std::size_t gap_index) {
  const std::string base = junction.odr_id + "_b" + std::to_string(gap_index);
  std::string id = base;
  for (int suffix = 1; network.find_road(id).is_valid(); ++suffix) {
    id = base + "_" + std::to_string(suffix);
  }
  return id;
}

/// Synthesizes an auxiliary boundary road bridging the outer mouths of two
/// CCW-adjacent arms `a` and `b` that no connecting road links (spec Fig. 99).
/// The gap-facing corners are the nearest pair across the gap, so the closing
/// lane segment meets the neighbouring joint caps; a single straight reference
/// line runs corner→corner. Returns nullopt when the corners coincide
/// (degenerate — no road to add).
std::optional<AuxBoundaryRoad> make_aux_boundary_road(const RoadNetwork& network,
                                                      const Junction& junction,
                                                      const Arm& a,
                                                      const Arm& b,
                                                      std::size_t gap_index) {
  const auto [a_left, a_right] = arm_end_corners(network, a);
  const auto [b_left, b_right] = arm_end_corners(network, b);
  // Gap-facing corners = the nearest pair across the gap. Fixed probe order +
  // a tolerance-guarded strict improvement keep the choice deterministic.
  const std::array<std::pair<Vec2, Vec2>, 4> candidates = {
      {{a_left, b_left}, {a_left, b_right}, {a_right, b_left}, {a_right, b_right}}};
  const std::pair<Vec2, Vec2>* best = &candidates.front();
  double best_d = dist2(best->first, best->second);
  for (std::size_t i = 1; i < candidates.size(); ++i) {
    const double d = dist2(candidates[i].first, candidates[i].second);
    if (d < best_d - tol::kLength) {
      best = &candidates[i];
      best_d = d;
    }
  }
  const Vec2 start = best->first;
  const Vec2 end = best->second;
  const double length = std::hypot(end.x - start.x, end.y - start.y);
  if (length <= tol::kLength) {
    return std::nullopt;
  }

  AuxBoundaryRoad aux;
  aux.junction_odr_id = junction.odr_id;
  aux.odr_id = unique_aux_id(network, junction, gap_index);
  aux.x = start.x;
  aux.y = start.y;
  aux.hdg = std::atan2(end.y - start.y, end.x - start.x);
  aux.length = length;
  aux.pred_road = network.road(a.road)->odr_id;
  aux.pred_contact = a.contact;
  aux.succ_road = network.road(b.road)->odr_id;
  aux.succ_contact = b.contact;
  return aux;
}

} // namespace

JunctionBoundaryExport build_junction_boundary(const RoadNetwork& network,
                                               const Junction& junction) {
  JunctionBoundaryExport out;

  // 1. Every connecting road with the two arms it bridges. A connecting road
  //    missing either road link cannot be placed on the boundary — bail to the
  //    warning path rather than emit a partial (non-closing) boundary.
  std::vector<Bridge> bridges;
  for (const JunctionConnection& connection : junction.connections) {
    const Road* connecting = network.road(connection.connecting_road);
    if (connecting == nullptr || connecting->plan_view.empty()) {
      continue;
    }
    const std::optional<Arm> from = link_arm(network, connecting->predecessor);
    const std::optional<Arm> to = link_arm(network, connecting->successor);
    if (!from.has_value() || !to.has_value()) {
      return out; // no arm metadata (foreign junction) — keep the warning
    }
    const PathPoint mid = connecting->plan_view.evaluate(connecting->plan_view.length() / 2.0);
    bridges.push_back(Bridge{
        .road = connection.connecting_road, .from = *from, .to = *to, .mid = {mid.x, mid.y}});
  }
  if (bridges.empty()) {
    return out;
  }

  // 2. Distinct arms + footprint centroid.
  std::vector<Arm> arms;
  const auto note_arm = [&arms](const Arm& arm) {
    if (std::ranges::find(arms, arm) == arms.end()) {
      arms.push_back(arm);
    }
  };
  for (const Bridge& bridge : bridges) {
    note_arm(bridge.from);
    note_arm(bridge.to);
  }
  if (arms.size() < 2) {
    return out;
  }
  double cx = 0.0;
  double cy = 0.0;
  for (const Arm& arm : arms) {
    cx += arm.x;
    cy += arm.y;
  }
  cx /= static_cast<double>(arms.size());
  cy /= static_cast<double>(arms.size());

  // 3. Order arms counter-clockwise around the centroid.
  std::ranges::sort(arms, [cx, cy](const Arm& a, const Arm& b) {
    return std::atan2(a.y - cy, a.x - cx) < std::atan2(b.y - cy, b.x - cx);
  });

  // 4. Walk CCW-adjacent arm pairs: the outer connecting road between them is a
  //    lane segment, with a joint cap at each arm. A pair with no bridging
  //    connecting road is a gap the existing roads cannot close — defer to
  //    auxiliary boundary roads (not generated here), keeping the warning.
  const std::size_t n = arms.size();
  std::vector<JunctionBoundarySegment> segments;
  for (std::size_t i = 0; i < n; ++i) {
    const Arm& a = arms[i];
    const Arm& b = arms[(i + 1) % n];
    const Bridge* outer = nullptr;
    for (const Bridge& bridge : bridges) {
      const bool matches =
          (bridge.from == a && bridge.to == b) || (bridge.from == b && bridge.to == a);
      if (!matches) {
        continue;
      }
      const double d =
          ((bridge.mid.x - cx) * (bridge.mid.x - cx)) + ((bridge.mid.y - cy) * (bridge.mid.y - cy));
      if (outer == nullptr) {
        outer = &bridge;
        continue;
      }
      const double best =
          ((outer->mid.x - cx) * (outer->mid.x - cx)) + ((outer->mid.y - cy) * (outer->mid.y - cy));
      // Farthest-from-centre wins (the outer arc); ties break on road id for
      // determinism.
      if (d > best + tol::kLength ||
          (std::abs(d - best) <= tol::kLength &&
           network.road(bridge.road)->odr_id < network.road(outer->road)->odr_id)) {
        outer = &bridge;
      }
    }
    if (outer == nullptr) {
      // No connecting road bridges this adjacent arm pair: close the gap with a
      // synthesized auxiliary boundary road along the outer edge between the two
      // arm mouths (spec Fig. 99), whose lane 0 provides the missing segment.
      std::optional<AuxBoundaryRoad> aux =
          make_aux_boundary_road(network, junction, a, b, out.aux_roads.size());
      if (!aux.has_value()) {
        return {}; // degenerate mouths — cannot close; keep the warning
      }
      segments.push_back(JunctionBoundarySegment{
          .is_lane = true, .road_id = aux->odr_id, .boundary_lane = 0, .s_begin_to_end = true});
      out.aux_roads.push_back(std::move(*aux));
      // Joint cap at arm b, exactly as the bridged branch — the alternation and
      // CCW order are identical to a connected pair.
      segments.push_back(JunctionBoundarySegment{
          .is_lane = false, .road_id = network.road(b.road)->odr_id, .contact = b.contact});
      continue;
    }
    // Lane segment along the outer connecting road (its single driving lane
    // -1, whose outer edge forms the corner). Walk begin→end when the road
    // runs from arm a to arm b, else reversed.
    segments.push_back(JunctionBoundarySegment{.is_lane = true,
                                               .road_id = network.road(outer->road)->odr_id,
                                               .boundary_lane = -1,
                                               .s_begin_to_end = (outer->from == a)});
    // Joint cap at arm b (the shared end between this lane segment and the
    // next), perpendicular to the arm road across all its lanes.
    segments.push_back(JunctionBoundarySegment{
        .is_lane = false, .road_id = network.road(b.road)->odr_id, .contact = b.contact});
  }

  out.has_boundary = true;
  out.segments = std::move(segments);
  return out;
}

} // namespace roadmaker
