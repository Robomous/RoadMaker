// Copyright 2026 Robomous
// SPDX-License-Identifier: Apache-2.0

#include "roadmaker/road/surface_derivation.hpp"

#include "roadmaker/geometry/reference_line.hpp"
#include "roadmaker/road/network.hpp"
#include "roadmaker/road/road.hpp"
#include "roadmaker/road/surface.hpp"
#include "roadmaker/tol.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <optional>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

namespace roadmaker {
namespace {

/// Faces below this signed area are numerically not a region [m²].
constexpr double kMinArea = 1.0;

/// One end of an edge road, welded into a graph node by position/links.
struct Endpoint {
  double x = 0.0;
  double y = 0.0;
  double hdg = 0.0; ///< tangent LEAVING the node along the road [rad]
};

/// A directed half-edge: origin node, the road it rides, and the heading it
/// leaves its origin node at. Its twin is the opposite direction of the road.
struct HalfEdge {
  std::uint32_t origin = 0; ///< node id
  std::uint32_t twin = 0;   ///< index of the opposite half-edge
  double hdg = 0.0;         ///< heading leaving `origin` [rad]
  RoadId road;
};

/// Union-find over endpoint slots (slot 2e = road e start, 2e+1 = end).
class UnionFind {
public:
  explicit UnionFind(std::size_t n) : parent_(n) {
    for (std::size_t i = 0; i < n; ++i) {
      parent_[i] = static_cast<std::uint32_t>(i);
    }
  }

  std::uint32_t find(std::uint32_t a) {
    while (parent_[a] != a) {
      parent_[a] = parent_[parent_[a]];
      a = parent_[a];
    }
    return a;
  }

  void unite(std::uint32_t a, std::uint32_t b) { parent_[find(a)] = find(b); }

private:
  std::vector<std::uint32_t> parent_;
};

double normalize_angle(double a) {
  constexpr double kTwoPi = 2.0 * std::numbers::pi;
  a = std::fmod(a, kTwoPi);
  if (a < 0.0) {
    a += kTwoPi;
  }
  return a;
}

/// Lexicographic comparison of two rings by RoadId arena index.
bool less_by_index(const std::vector<RoadId>& lhs, const std::vector<RoadId>& rhs) {
  return std::ranges::lexicographical_compare(
      lhs, rhs, {}, [](RoadId id) { return id.index; }, [](RoadId id) { return id.index; });
}

/// Canonical rotation of a cyclic ring: the lexicographically smallest (by
/// arena index) sequence over all rotations of the ring AND its reverse. This
/// makes bounding_roads independent of which road the walk started at and of
/// the traversal direction, so the output is byte-stable across runs.
std::vector<RoadId> canonical_ring(const std::vector<RoadId>& ring) {
  const std::size_t n = ring.size();
  if (n == 0) {
    return {};
  }
  std::vector<RoadId> reversed(ring.rbegin(), ring.rend());
  std::vector<RoadId> best;
  const auto consider = [&](const std::vector<RoadId>& src, std::size_t start) {
    std::vector<RoadId> candidate;
    candidate.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
      candidate.push_back(src[(start + i) % n]);
    }
    if (best.empty() || less_by_index(candidate, best)) {
      best = std::move(candidate);
    }
  };
  for (std::size_t s = 0; s < n; ++s) {
    consider(ring, s);
  }
  for (std::size_t s = 0; s < n; ++s) {
    consider(reversed, s);
  }
  return best;
}

std::string ring_key(const std::vector<RoadId>& ring) {
  std::string key;
  for (const RoadId id : ring) {
    key += std::to_string(id.index);
    key += ',';
  }
  return key;
}

} // namespace

void derive_surfaces(RoadNetwork& network) {
  // --- 1. Collect edge roads (skip junction-internal connecting roads). ----
  std::vector<RoadId> edges;
  std::vector<Endpoint> endpoints; // 2 per edge: [2e] start, [2e+1] end
  network.for_each_road([&](RoadId id, const Road& road) {
    if (road.junction.is_valid()) {
      return; // connecting road — not part of the planar graph
    }
    if (road.plan_view.empty()) {
      return;
    }
    const double length = road.plan_view.length();
    const PathPoint start = road.plan_view.evaluate(0.0);
    const PathPoint end = road.plan_view.evaluate(length);
    edges.push_back(id);
    endpoints.push_back(Endpoint{.x = start.x, .y = start.y, .hdg = start.hdg});
    // Leaving the end node means travelling back into the road: reverse the
    // forward tangent.
    endpoints.push_back(Endpoint{.x = end.x, .y = end.y, .hdg = end.hdg + std::numbers::pi});
  });

  const std::size_t edge_count = edges.size();
  if (edge_count == 0) {
    // No edges: every surface is stale.
    std::vector<SurfaceId> doomed;
    network.for_each_surface([&](SurfaceId id, const Surface&) { doomed.push_back(id); });
    for (const SurfaceId id : doomed) {
      network.erase_surface(id);
    }
    return;
  }

  // --- 2. Weld endpoints into nodes. ---------------------------------------
  UnionFind uf(endpoints.size());

  // (a) Positional coincidence.
  for (std::size_t i = 0; i < endpoints.size(); ++i) {
    for (std::size_t j = i + 1; j < endpoints.size(); ++j) {
      const double dx = endpoints[i].x - endpoints[j].x;
      const double dy = endpoints[i].y - endpoints[j].y;
      if (std::hypot(dx, dy) <= tol::kWeldPosition) {
        uf.unite(static_cast<std::uint32_t>(i), static_cast<std::uint32_t>(j));
      }
    }
  }

  // Map RoadId -> edge index for link welding.
  std::unordered_map<std::uint64_t, std::size_t> edge_of;
  const auto road_key = [](auto id) {
    return (static_cast<std::uint64_t>(id.gen) << 32U) | id.index;
  };
  for (std::size_t e = 0; e < edge_count; ++e) {
    edge_of.emplace(road_key(edges[e]), e);
  }

  // (b) predecessor/successor links, and (c) junction collapse.
  std::unordered_map<std::uint64_t, std::uint32_t> junction_node; // junction -> a slot
  for (std::size_t e = 0; e < edge_count; ++e) {
    const Road* road = network.road(edges[e]);
    if (road == nullptr) {
      continue;
    }
    const auto weld_link = [&](const std::optional<RoadLink>& link, std::size_t local_slot) {
      if (!link.has_value()) {
        return;
      }
      if (const auto* target_road = std::get_if<RoadId>(&link->target)) {
        const auto it = edge_of.find(road_key(*target_road));
        if (it != edge_of.end()) {
          const std::size_t other = 2 * it->second + (link->contact == ContactPoint::End ? 1U : 0U);
          uf.unite(static_cast<std::uint32_t>(local_slot), static_cast<std::uint32_t>(other));
        }
      } else if (const auto* target_junction = std::get_if<JunctionId>(&link->target)) {
        // All road ends meeting the same junction collapse to one node.
        const std::uint64_t key = road_key(*target_junction);
        const auto [it, inserted] =
            junction_node.emplace(key, static_cast<std::uint32_t>(local_slot));
        if (!inserted) {
          uf.unite(static_cast<std::uint32_t>(local_slot), it->second);
        }
      }
    };
    weld_link(road->predecessor, 2 * e);     // predecessor attaches to our start
    weld_link(road->successor, (2 * e) + 1); // successor attaches to our end
  }

  // --- 3. Compress union roots into contiguous node ids. -------------------
  std::unordered_map<std::uint32_t, std::uint32_t> root_to_node;
  std::vector<double> node_x;
  std::vector<double> node_y;
  std::vector<std::uint32_t> slot_node(endpoints.size());
  for (std::uint32_t slot = 0; slot < endpoints.size(); ++slot) {
    const std::uint32_t root = uf.find(slot);
    const auto [it, inserted] =
        root_to_node.emplace(root, static_cast<std::uint32_t>(node_x.size()));
    if (inserted) {
      node_x.push_back(endpoints[slot].x);
      node_y.push_back(endpoints[slot].y);
    }
    slot_node[slot] = it->second;
  }

  // --- 4. Build half-edges. ------------------------------------------------
  std::vector<HalfEdge> half; // 2*edge_count: [2e] forward, [2e+1] reverse
  half.reserve(endpoints.size());
  for (std::size_t e = 0; e < edge_count; ++e) {
    const std::uint32_t forward = static_cast<std::uint32_t>(half.size());
    half.push_back(HalfEdge{.origin = slot_node[2 * e],
                            .twin = forward + 1,
                            .hdg = normalize_angle(endpoints[2 * e].hdg),
                            .road = edges[e]});
    half.push_back(HalfEdge{.origin = slot_node[(2 * e) + 1],
                            .twin = forward,
                            .hdg = normalize_angle(endpoints[(2 * e) + 1].hdg),
                            .road = edges[e]});
  }

  // --- 5. Rotational order of outgoing half-edges at each node. ------------
  std::vector<std::vector<std::uint32_t>> adjacency(node_x.size());
  for (std::uint32_t h = 0; h < half.size(); ++h) {
    adjacency[half[h].origin].push_back(h);
  }
  for (auto& list : adjacency) {
    std::ranges::sort(list, [&](std::uint32_t lhs, std::uint32_t rhs) {
      if (half[lhs].hdg != half[rhs].hdg) {
        return half[lhs].hdg < half[rhs].hdg;
      }
      // Deterministic tie-break for numerically-equal headings.
      if (half[lhs].road.index != half[rhs].road.index) {
        return half[lhs].road.index < half[rhs].road.index;
      }
      return lhs < rhs;
    });
  }
  std::vector<std::uint32_t> pos_in_adj(half.size());
  for (const auto& list : adjacency) {
    for (std::uint32_t k = 0; k < list.size(); ++k) {
      pos_in_adj[list[k]] = k;
    }
  }

  // next(h): arrive at dest via twin(h) (an outgoing half-edge there), then
  // leave along the next half-edge CLOCKWISE — the predecessor in the
  // ascending-angle order (wrapping). This traces bounded faces CCW.
  const auto next_half = [&](std::uint32_t h) {
    const std::uint32_t twin = half[h].twin;
    const std::uint32_t node = half[twin].origin;
    const auto& list = adjacency[node];
    const std::uint32_t k = pos_in_adj[twin];
    const std::uint32_t pred =
        (k + static_cast<std::uint32_t>(list.size()) - 1) % static_cast<std::uint32_t>(list.size());
    return list[pred];
  };

  // --- 6. Trace faces; keep the bounded (positive-area) ones. --------------
  std::vector<bool> visited(half.size(), false);
  std::vector<std::vector<RoadId>> faces; // canonicalized bounding rings
  for (std::uint32_t start = 0; start < half.size(); ++start) {
    if (visited[start]) {
      continue;
    }
    std::vector<RoadId> ring;
    double area2 = 0.0; // twice the signed shoelace area
    std::uint32_t h = start;
    do {
      visited[h] = true;
      ring.push_back(half[h].road);
      const std::uint32_t a = half[h].origin;
      const std::uint32_t b = half[half[h].twin].origin; // dest node
      area2 += (node_x[a] * node_y[b]) - (node_x[b] * node_y[a]);
      h = next_half(h);
    } while (h != start);

    const double area = 0.5 * area2;
    if (area > kMinArea) { // bounded face (CCW); the outer face is negative
      faces.push_back(canonical_ring(ring));
    }
  }

  // Sort faces by canonical key so surfaces are created in a stable order.
  std::ranges::sort(faces, less_by_index);

  // --- 7. Reconcile the surface arena (id-stable survivors). ---------------
  std::unordered_map<std::string, std::vector<SurfaceId>> existing;
  network.for_each_surface([&](SurfaceId id, const Surface& surface) {
    existing[ring_key(canonical_ring(surface.bounding_roads))].push_back(id);
  });

  std::vector<std::vector<RoadId>> to_create;
  for (auto& face : faces) {
    const auto it = existing.find(ring_key(face));
    if (it != existing.end() && !it->second.empty()) {
      it->second.pop_back(); // a survivor keeps its SurfaceId — do nothing
    } else {
      to_create.push_back(std::move(face));
    }
  }

  // Erase surfaces with no matching face (deterministic order by index).
  std::vector<SurfaceId> doomed;
  for (auto& [key, ids] : existing) {
    for (const SurfaceId id : ids) {
      doomed.push_back(id);
    }
  }
  std::ranges::sort(doomed, {}, [](SurfaceId id) { return id.index; });
  for (const SurfaceId id : doomed) {
    network.erase_surface(id);
  }

  for (auto& ring : to_create) {
    network.create_surface(
        Surface{.source = BoundarySource::Derived, .bounding_roads = std::move(ring)});
  }
}

} // namespace roadmaker
