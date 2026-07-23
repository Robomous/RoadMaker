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

/// True when the closed segments (a0,a1) and (b0,b1) properly cross. Endpoint
/// touching does not count — adjacent boundary segments always share one.
bool segments_cross(const std::array<double, 2>& a0,
                    const std::array<double, 2>& a1,
                    const std::array<double, 2>& b0,
                    const std::array<double, 2>& b1) {
  const auto cross = [](const std::array<double, 2>& o,
                        const std::array<double, 2>& p,
                        const std::array<double, 2>& q) {
    return ((p[0] - o[0]) * (q[1] - o[1])) - ((p[1] - o[1]) * (q[0] - o[0]));
  };
  const double d0 = cross(a0, a1, b0);
  const double d1 = cross(a0, a1, b1);
  const double d2 = cross(b0, b1, a0);
  const double d3 = cross(b0, b1, a1);
  // Strict sign opposition on both segments: collinear-overlap and
  // endpoint-touching cases fall through as "no crossing", which is what the
  // shared-endpoint case of a closed loop needs.
  return ((d0 > 0.0) != (d1 > 0.0)) && ((d2 > 0.0) != (d3 > 0.0));
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
