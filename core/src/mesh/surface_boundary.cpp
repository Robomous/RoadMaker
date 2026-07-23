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

#include "roadmaker/mesh/surface_boundary.hpp"

#include "roadmaker/road/surface.hpp"
#include "roadmaker/tol.hpp"

#include <clipper2/clipper.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <vector>

#include "surface_fill.hpp"

namespace roadmaker {
namespace {

/// Catmull-Rom tangent at ring position `i`: half the chord between its
/// neighbours. The resulting Hermite loop passes through every seed point and
/// reproduces the derived ring's shape closely enough that detaching a surface
/// (decision D3) does not visibly move it.
SurfaceNode catmull_rom_node(const std::vector<std::array<double, 2>>& ring, std::size_t i) {
  const std::size_t n = ring.size();
  const std::array<double, 2>& prev = ring[(i + n - 1) % n];
  const std::array<double, 2>& next = ring[(i + 1) % n];
  const double tx = 0.5 * (next[0] - prev[0]);
  const double ty = 0.5 * (next[1] - prev[1]);
  return SurfaceNode{.x = ring[i][0],
                     .y = ring[i][1],
                     .tangent_in_x = tx,
                     .tangent_in_y = ty,
                     .tangent_out_x = tx,
                     .tangent_out_y = ty};
}

/// Decimates a dense ring to at most `kMaxSeedBoundaryNodes` points by
/// Ramer-Douglas-Peucker with a doubling tolerance. Starting from the union's
/// own simplification tolerance keeps the first passes cheap; the loop is
/// bounded (the tolerance grows past the ring's own extent, at which point RDP
/// cannot return more than the closing triangle) and falls back to uniform
/// stride if the ring somehow resists.
std::vector<std::array<double, 2>> decimate_ring(std::vector<std::array<double, 2>> ring) {
  if (ring.size() <= kMaxSeedBoundaryNodes) {
    return ring;
  }
  Clipper2Lib::PathD path;
  path.reserve(ring.size());
  for (const std::array<double, 2>& p : ring) {
    path.emplace_back(p[0], p[1]);
  }
  for (double epsilon = 0.05; epsilon < 1e4; epsilon *= 2.0) {
    const Clipper2Lib::PathD simplified =
        Clipper2Lib::SimplifyPath(path, epsilon, /*isClosedPath=*/true);
    if (simplified.size() < 3) {
      break; // over-simplified — the previous stride fallback is safer
    }
    if (simplified.size() <= kMaxSeedBoundaryNodes) {
      std::vector<std::array<double, 2>> out;
      out.reserve(simplified.size());
      for (const Clipper2Lib::PointD& p : simplified) {
        out.push_back({p.x, p.y});
      }
      return out;
    }
  }
  // Uniform stride fallback: never returns more than the cap, and keeps the
  // ring's traversal order (so the loop stays simple).
  const std::size_t stride = (ring.size() + kMaxSeedBoundaryNodes - 1) / kMaxSeedBoundaryNodes;
  std::vector<std::array<double, 2>> out;
  for (std::size_t i = 0; i < ring.size(); i += stride) {
    out.push_back(ring[i]);
  }
  return out;
}

/// How far off a segment's line a point must lie before it counts as being to
/// one side of it [m]. A nanometre: far below any boundary a user could draw,
/// and far above the rounding noise below.
///
/// That noise is not hypothetical. The Hermite basis weights do not sum to
/// exactly 1 in floating point, so samples along a STRAIGHT edge land ~1e-14 m
/// off the chord in an arbitrary direction. Without this tolerance, two
/// non-adjacent samples of the same straight edge produce tiny cross products
/// whose signs happen to oppose, and a perfectly simple boundary reads as
/// self-intersecting — differently on different platforms, because the rounding
/// does (macOS CI caught exactly this).
constexpr double kOnLineTolerance = 1e-9;

/// True when the closed segments (a0,a1) and (b0,b1) cross — including the case
/// where one segment's endpoint lands ON the other, which is how a boundary
/// that pinches through a sample vertex presents itself.
///
/// The one case deliberately excluded is two segments lying along the SAME
/// line, because that is what the sampling noise above produces: both endpoints
/// of `b` sit within tolerance of `a`'s line. Requiring each segment to have at
/// least one endpoint genuinely off the other's line separates the two — a real
/// crossing is transverse, rounding noise is collinear.
bool segments_cross(const std::array<double, 2>& a0,
                    const std::array<double, 2>& a1,
                    const std::array<double, 2>& b0,
                    const std::array<double, 2>& b1) {
  const auto cross = [](const std::array<double, 2>& o,
                        const std::array<double, 2>& p,
                        const std::array<double, 2>& q) {
    return ((p[0] - o[0]) * (q[1] - o[1])) - ((p[1] - o[1]) * (q[0] - o[0]));
  };
  const double len_a = std::hypot(a1[0] - a0[0], a1[1] - a0[1]);
  const double len_b = std::hypot(b1[0] - b0[0], b1[1] - b0[1]);
  if (len_a < kOnLineTolerance || len_b < kOnLineTolerance) {
    return false; // a degenerate segment has no side
  }
  // |cross| / |segment| IS the point's perpendicular distance from the line, so
  // the tolerance is applied in metres rather than in cross-product units,
  // which would otherwise scale with the segment length.
  const auto side = [](double numerator, double length) {
    const double distance = numerator / length;
    if (std::abs(distance) <= kOnLineTolerance) {
      return 0; // on the line, within the feature size
    }
    return distance > 0.0 ? 1 : -1;
  };
  const int s0 = side(cross(a0, a1, b0), len_a);
  const int s1 = side(cross(a0, a1, b1), len_a);
  const int s2 = side(cross(b0, b1, a0), len_b);
  const int s3 = side(cross(b0, b1, a1), len_b);
  if ((s0 == 0 && s1 == 0) || (s2 == 0 && s3 == 0)) {
    return false; // collinear within tolerance — the sampling-noise case
  }
  return s0 != s1 && s2 != s3;
}

} // namespace

std::vector<std::array<double, 2>> sample_surface_boundary(const std::vector<SurfaceNode>& nodes,
                                                           double step) {
  std::vector<std::array<double, 2>> out;
  const std::size_t n = nodes.size();
  if (n < 3) {
    return out;
  }
  const double safe_step = std::max(step, tol::kLength);
  for (std::size_t i = 0; i < n; ++i) {
    const SurfaceNode& a = nodes[i];
    const SurfaceNode& b = nodes[(i + 1) % n];
    out.push_back({a.x, a.y});
    // Interior samples of the segment leaving `a`. The count comes from the
    // CHORD, so a straight segment gets no samples it does not need and a long
    // one is tessellated evenly; the node itself is always emitted exactly.
    const double chord = std::hypot(b.x - a.x, b.y - a.y);
    const auto pieces = static_cast<std::size_t>(chord / safe_step);
    for (std::size_t k = 1; k < pieces; ++k) {
      const double t = static_cast<double>(k) / static_cast<double>(pieces);
      const double t2 = t * t;
      const double t3 = t2 * t;
      // Cubic Hermite basis.
      const double h00 = (2.0 * t3) - (3.0 * t2) + 1.0;
      const double h10 = t3 - (2.0 * t2) + t;
      const double h01 = (-2.0 * t3) + (3.0 * t2);
      const double h11 = t3 - t2;
      out.push_back({(h00 * a.x) + (h10 * a.tangent_out_x) + (h01 * b.x) + (h11 * b.tangent_in_x),
                     (h00 * a.y) + (h10 * a.tangent_out_y) + (h01 * b.y) + (h11 * b.tangent_in_y)});
    }
  }
  return out;
}

bool surface_boundary_self_intersects(const std::vector<SurfaceNode>& nodes) {
  const std::vector<std::array<double, 2>> ring = sample_surface_boundary(nodes);
  const std::size_t n = ring.size();
  if (n < 4) {
    return false;
  }
  for (std::size_t i = 0; i < n; ++i) {
    const std::size_t i_next = (i + 1) % n;
    for (std::size_t j = i + 1; j < n; ++j) {
      const std::size_t j_next = (j + 1) % n;
      if (i == j_next || j == i_next) {
        continue; // adjacent segments share an endpoint by construction
      }
      if (segments_cross(ring[i], ring[i_next], ring[j], ring[j_next])) {
        return true;
      }
    }
  }
  return false;
}

std::vector<SurfaceNode> surface_boundary_nodes(const RoadNetwork& network,
                                                SurfaceId surface_id,
                                                const SamplingOptions& sampling) {
  const Surface* surface = network.surface(surface_id);
  if (surface == nullptr) {
    return {};
  }
  if (surface->source == BoundarySource::Authored) {
    return surface->nodes;
  }
  const std::vector<std::array<double, 2>> ring =
      decimate_ring(derived_region_polygon(network, *surface, sampling));
  if (ring.size() < 3) {
    return {};
  }
  std::vector<SurfaceNode> nodes;
  nodes.reserve(ring.size());
  for (std::size_t i = 0; i < ring.size(); ++i) {
    nodes.push_back(catmull_rom_node(ring, i));
  }
  return nodes;
}

} // namespace roadmaker
