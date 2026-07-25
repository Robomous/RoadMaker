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

#include "roadmaker/mesh/junction_sidewalk_bands.hpp"

#include "roadmaker/road/junction.hpp"
#include "roadmaker/tol.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <numbers>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "junction_corner_detail.hpp"
#include "junction_sidewalk_bands.hpp"
#include "mesh_detail.hpp"

namespace roadmaker {

namespace {

using junction_corner_detail::arm_face;
using junction_corner_detail::ArmFace;
using junction_corner_detail::corner_curve;
using junction_corner_detail::corner_faces;
using junction_corner_detail::CornerFace;
using junction_corner_detail::CornerSolution;
using junction_corner_detail::solve_corner;
using mesh_detail::lateral_point;

// Miter cap for the inner boundary at a sharp corner, in sidewalk widths. As
// the corner angle closes, the exact miter offset (w / sin(phi/2)) runs away;
// past this the band is allowed to be slightly narrow at the apex instead of
// spiking across the carriageway. The floor clip cleans up the remainder.
constexpr double kMiterCap = 3.0;

/// One side of an arm face: does it carry a sidewalk, and if so where that
/// sidewalk's outer (curb) and inner (carriageway-facing) edges sit.
struct SidewalkSide {
  bool present = false;
  std::array<double, 2> outer{}; // curb edge at the face
  std::array<double, 2> inner{}; // sidewalk/carriageway boundary at the face
  double width = 0.0;
  /// How far the sidewalk's curb sits INSIDE the arm's outermost pavement edge
  /// — the total width of whatever lies outboard of it (a shoulder, a border).
  /// Zero when the sidewalk is the outermost lane, which is the common case.
  /// The corner geometry is solved on the OUTERMOST edge, so this is the offset
  /// that puts the band back on the sidewalk where the two differ.
  double outboard = 0.0;
};

/// The sidewalk side of `face` that meets `corner_pt` (one of the two outer
/// face corners). `corner_pt` selects which extreme — offsets.front() (left)
/// or offsets.back() (right) — so the caller never has to know CornerFace's
/// left/right convention.
///
/// The sidewalk is the OUTERMOST LANE OF SIDEWALK TYPE on that side, not simply
/// the outermost lane (issue #402): a cross section that carries a shoulder, a
/// border or a curb outboard of its sidewalk used to report no sidewalk at all,
/// which silently disabled banding for the whole junction.
SidewalkSide sidewalk_side_at(const ArmFace& face, const std::array<double, 2>& corner_pt) {
  SidewalkSide side;
  if (face.types.empty() || face.offsets.size() < 2) {
    return side;
  }
  const std::array<double, 3> back = lateral_point(face.frame, face.offsets.back());
  const std::array<double, 3> front = lateral_point(face.frame, face.offsets.front());
  const double db = ((back[0] - corner_pt[0]) * (back[0] - corner_pt[0])) +
                    ((back[1] - corner_pt[1]) * (back[1] - corner_pt[1]));
  const double df = ((front[0] - corner_pt[0]) * (front[0] - corner_pt[0])) +
                    ((front[1] - corner_pt[1]) * (front[1] - corner_pt[1]));
  const bool use_back = db <= df;

  // Walk inward from this side's outermost lane to the first Sidewalk. Lanes
  // outboard of it (shoulder, border, curb) simply are not the band.
  const std::size_t count = face.types.size();
  std::size_t lane = count; // index into face.types, `count` = not found
  for (std::size_t step = 0; step < count; ++step) {
    const std::size_t candidate = use_back ? count - 1 - step : step;
    if (face.types[candidate] == LaneType::Sidewalk) {
      lane = candidate;
      break;
    }
  }
  if (lane == count) {
    return side;
  }
  // types[k] is the lane between offsets[k] and offsets[k + 1]; on the back
  // side the outer edge is the higher index, on the front side the lower one.
  const double outer_off = use_back ? face.offsets[lane + 1] : face.offsets[lane];
  const double inner_off = use_back ? face.offsets[lane] : face.offsets[lane + 1];
  const double edge_off = use_back ? face.offsets.back() : face.offsets.front();
  const std::array<double, 3> o = lateral_point(face.frame, outer_off);
  const std::array<double, 3> in = lateral_point(face.frame, inner_off);
  side.present = true;
  side.outer = {o[0], o[1]};
  side.inner = {in[0], in[1]};
  side.width = std::abs(outer_off - inner_off);
  side.outboard = std::abs(edge_off - outer_off);
  return side;
}

/// Unit 2D vector b - a, or {0,0} when the two coincide.
std::array<double, 2> unit_delta(const std::array<double, 2>& a, const std::array<double, 2>& b) {
  const double dx = b[0] - a[0];
  const double dy = b[1] - a[1];
  const double len = std::hypot(dx, dy);
  return len > tol::kLength ? std::array<double, 2>{dx / len, dy / len}
                            : std::array<double, 2>{0.0, 0.0};
}

std::array<double, 2>
along(const std::array<double, 2>& p, const std::array<double, 2>& dir, double dist) {
  return {p[0] + (dir[0] * dist), p[1] + (dir[1] * dist)};
}

/// Appends one (curb, seam) sample pair, keeping the two rings parallel.
///
/// A pair identical to the one before it is dropped. The shapes below run into
/// each other by construction — a fillet tangency clamped to the arm faces IS
/// the face corner, and the corner curve starts at that same tangency — and a
/// repeated pair is a zero-length band segment: it adds a zero-length edge to
/// the ring (which upsets polygon predicates) and a duplicate sample to anyone
/// walking the band.
void push_pair(JunctionSidewalkBand& band,
               const std::array<double, 2>& outer,
               const std::array<double, 2>& inner) {
  constexpr double kSamePoint = 1e-9;
  if (!band.outer.empty() &&
      std::hypot(outer[0] - band.outer.back()[0], outer[1] - band.outer.back()[1]) < kSamePoint &&
      std::hypot(inner[0] - band.inner.back()[0], inner[1] - band.inner.back()[1]) < kSamePoint) {
    return;
  }
  band.outer.push_back(outer);
  band.inner.push_back(inner);
}

/// The band of one corner, or nullopt when neither adjacent arm is sidewalked.
///
/// A corner with only ONE sidewalked arm is not a shape of its own: the
/// sidewalked side's width is carried across and the same geometry runs, it
/// just ENDS EARLY — at the crown of the corner rather than at the other arm.
/// The obvious alternative, stopping at the fillet tangency on the sidewalked
/// side, yields a ZERO-LENGTH band: a derived fillet is routinely clamped to
/// the arm faces, which puts the tangency exactly ON the face corner. Running
/// the full wrap instead is equally wrong in the other direction, since a
/// fillet is often tens of meters long.
///
/// Three shapes remain, and the last two used to produce nothing at all (#402):
///   1. filleted corner → wrap the corner curve;
///   2. sharp corner (the fillet solver rejected it, but the edges do meet) →
///      run both legs to the apex, mitering the seam across the turn;
///   3. no usable corner (parallel edges, near-tangent arms, or an apex behind
///      a face) → span straight from one arm's curb to the other's.
std::optional<JunctionSidewalkBand> build_band(const CornerFace& a,
                                               const CornerFace& b,
                                               const SidewalkSide& in_a,
                                               const SidewalkSide& in_b,
                                               const CornerSolution& solution) {
  if (!in_a.present && !in_b.present) {
    return std::nullopt;
  }
  // Carry the present side's width across a one-sided corner. The synthesized
  // side sits on the other arm's own face corner, offset toward that arm's
  // carriageway, so the band ends flush against the arm that has no sidewalk.
  const auto carried = [](const SidewalkSide& present,
                          const std::array<double, 2>& face_outer,
                          const std::array<double, 2>& face_inward) {
    SidewalkSide side;
    side.present = true;
    side.width = present.width;
    side.outboard = present.outboard;
    const std::array<double, 2> n = unit_delta(face_outer, face_inward);
    side.outer = along(face_outer, n, present.outboard);
    side.inner = along(side.outer, n, present.width);
    return side;
  };
  const SidewalkSide sa = in_a.present ? in_a : carried(in_b, a.right, a.left);
  const SidewalkSide sb = in_b.present ? in_b : carried(in_a, b.left, b.right);
  // Where an arm has no sidewalk of its own, the band stops at the CROWN of the
  // corner — the midpoint of the fillet — rather than running on to that arm.
  // The corner pavement up to the crown reads as the sidewalked road's; past it
  // the geometry belongs to the plain arm, and a fillet is often tens of meters
  // long (a derived radius runs to 15 m), so following it to the far tangency
  // would stamp sidewalk right down the plain arm's mouth — the case #385
  // pinned, and the reason a one-sided corner cannot simply reuse the wrap.
  const bool stop_before_a = !in_a.present;
  const bool stop_before_b = !in_b.present;

  JunctionSidewalkBand band;
  band.arm_a = a.arm;
  band.arm_b = b.arm;

  // Perpendiculars from curb toward the carriageway, one per side.
  const std::array<double, 2> na = unit_delta(sa.outer, sa.inner);
  const std::array<double, 2> nb = unit_delta(sb.outer, sb.inner);

  if (solution.valid && !solution.parallel_edges) {
    // 1. The wrap: curb leg → corner curve → curb leg, the seam following at
    //    one sidewalk width, blending the two widths across the curve.
    band.wraps_corner = true;
    if (!stop_before_a) {
      push_pair(band, sa.outer, sa.inner);
      const std::array<double, 2> curb_a = along(solution.tangent_a, na, sa.outboard);
      push_pair(band, curb_a, along(curb_a, na, sa.width));
    }
    const std::vector<std::array<double, 2>> curve = corner_curve(solution);
    const std::size_t crown = curve.size() / 2;
    const std::size_t first = stop_before_a ? crown : 0;
    const std::size_t past = stop_before_b ? crown + 1 : curve.size();
    for (std::size_t j = first; j < past; ++j) {
      const std::array<double, 2>& q = curve[j];
      const std::array<double, 2> prev = j > 0 ? curve[j - 1] : solution.tangent_a;
      const std::array<double, 2> next = j + 1 < curve.size() ? curve[j + 1] : solution.tangent_b;
      const std::array<double, 2> dir = unit_delta(prev, next);
      std::array<double, 2> nrm{-dir[1], dir[0]};
      // Orient the normal toward the pavement (the corner apex is interior).
      if ((nrm[0] * (solution.corner[0] - q[0])) + (nrm[1] * (solution.corner[1] - q[1])) < 0.0) {
        nrm = {-nrm[0], -nrm[1]};
      }
      const double f =
          curve.size() > 1 ? static_cast<double>(j) / static_cast<double>(curve.size() - 1) : 0.0;
      const double outboard = sa.outboard + (f * (sb.outboard - sa.outboard));
      const double width = sa.width + (f * (sb.width - sa.width));
      const std::array<double, 2> curb = along(q, nrm, outboard);
      push_pair(band, curb, along(curb, nrm, width));
    }
    if (!stop_before_b) {
      const std::array<double, 2> curb_b = along(solution.tangent_b, nb, sb.outboard);
      push_pair(band, curb_b, along(curb_b, nb, sb.width));
      push_pair(band, sb.outer, sb.inner);
    }
    return band;
  }

  // A sharp corner is usable whenever the edges meet ahead and the arms are not
  // near-tangent; past that the pair reads as a corridor and the apex would be
  // hundreds of meters away.
  const bool sharp =
      solution.corner_exists && solution.phi > 0.1 && solution.phi < std::numbers::pi - 0.1;
  if (sharp) {
    // 2. Sharp corner: both legs meet at the apex. The seam miters along the
    //    inward bisector so the band keeps its width through the turn.
    const double half = std::sin(solution.phi / 2.0);
    const double width = 0.5 * (sa.width + sb.width);
    const double outboard = 0.5 * (sa.outboard + sb.outboard);
    const auto miter_of = [&](double d) {
      return half > tol::kLength ? std::min(d / half, kMiterCap * std::max(d, width)) : d;
    };
    const std::array<double, 2> apex_curb =
        along(solution.corner, solution.bisector, miter_of(outboard));
    if (!stop_before_a) {
      push_pair(band, sa.outer, sa.inner);
    }
    push_pair(
        band, apex_curb, along(solution.corner, solution.bisector, miter_of(outboard + width)));
    if (!stop_before_b) {
      push_pair(band, sb.outer, sb.inner);
    }
    return band;
  }

  // 3. Corridor, near-tangent arms, or an apex behind a face: the two curb
  //    lines are effectively one line, so span straight across.
  push_pair(band, sa.outer, sa.inner);
  push_pair(band, sb.outer, sb.inner);
  return band;
}

} // namespace

std::vector<JunctionSidewalkBand> junction_sidewalk_bands_of(const RoadNetwork& network,
                                                             const Junction& junction) {
  std::vector<JunctionSidewalkBand> bands;
  const std::vector<CornerFace> faces = corner_faces(network, junction);
  if (faces.size() < 2) {
    return bands;
  }
  for (std::size_t i = 0; i < faces.size(); ++i) {
    const CornerFace& a = faces[i];
    const CornerFace& b = faces[(i + 1) % faces.size()];
    const std::optional<ArmFace> af_a = arm_face(network, a.arm);
    const std::optional<ArmFace> af_b = arm_face(network, b.arm);
    const SidewalkSide sa = af_a ? sidewalk_side_at(*af_a, a.right) : SidewalkSide{};
    const SidewalkSide sb = af_b ? sidewalk_side_at(*af_b, b.left) : SidewalkSide{};
    std::optional<JunctionSidewalkBand> band =
        build_band(a, b, sa, sb, solve_corner(network, junction, a, b));
    if (!band.has_value() || band->outer.size() < 2) {
      continue;
    }
    const JunctionCorner* entry = junction_corner_detail::corner_override(junction, a.arm, b.arm);
    if (entry != nullptr && entry->sidewalk_material.has_value()) {
      band->surface = *entry->sidewalk_material;
    }
    bands.push_back(std::move(*band));
  }
  return bands;
}

Clipper2Lib::PathD band_ring(const JunctionSidewalkBand& band) {
  Clipper2Lib::PathD ring;
  ring.reserve(band.outer.size() * 2);
  for (const std::array<double, 2>& p : band.outer) {
    ring.emplace_back(p[0], p[1]);
  }
  for (std::size_t j = band.inner.size(); j-- > 0;) {
    ring.emplace_back(band.inner[j][0], band.inner[j][1]);
  }
  return ring;
}

std::vector<JunctionSidewalkBand> junction_sidewalk_bands(const RoadNetwork& network,
                                                          JunctionId junction_id) {
  const Junction* junction = network.junction(junction_id);
  if (junction == nullptr) {
    return {};
  }
  return junction_sidewalk_bands_of(network, *junction);
}

} // namespace roadmaker
