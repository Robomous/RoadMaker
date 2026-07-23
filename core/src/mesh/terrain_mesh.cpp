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

#include "terrain_mesh.hpp"

#include "roadmaker/mesh/mesh_builder.hpp"
#include "roadmaker/mesh/surface_boundary.hpp"
#include "roadmaker/road/network.hpp"
#include "roadmaker/road/surface.hpp"
#include "roadmaker/road/terrain.hpp"

#include <CDT.h>
#include <clipper2/clipper.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <unordered_map>
#include <vector>

#include "fill_backend.hpp"
#include "surface_fill.hpp"

// The scene terrain (p5-s2, #232). Reuses the SAME Clipper2 → CDT → weld backend
// as the enclosed-area surfaces and junction floors (fill_backend.hpp); the one
// difference is the region — terrain fills the extent MINUS the road/surface
// footprints — and the z, which is not a harmonic solve (that would flatten the
// whole terrain to road height) but a per-vertex blend between the sampled
// height field and the nearest road-edge z across a skirt band.

namespace roadmaker {

using namespace fill_backend;

namespace {

/// A uniform spatial hash of the road-border samples so the per-vertex nearest-
/// border query is O(1) instead of scanning every border point (a terrain grid
/// against every road border is quadratic). Cell size is the skirt width: a
/// vertex farther than one skirt from every border needs no border z at all
/// (its weight is 0), so an empty 3x3 neighbourhood is the correct answer, not
/// an approximation.
class BorderIndex {
public:
  BorderIndex(const std::vector<Vec3>& border, double cell) : cell_(cell > 0.0 ? cell : 1.0) {
    for (const Vec3& p : border) {
      buckets_[key(p.x, p.y)].push_back(p);
    }
  }

  /// {distance, z} of the nearest border sample, or {infinity, 0} when none is
  /// within the searched neighbourhood (i.e. farther than one cell).
  [[nodiscard]] std::array<double, 2> nearest(double x, double y) const {
    const std::int64_t cx = cell_of(x);
    const std::int64_t cy = cell_of(y);
    double best = std::numeric_limits<double>::max();
    double z = 0.0;
    for (std::int64_t gy = cy - 1; gy <= cy + 1; ++gy) {
      for (std::int64_t gx = cx - 1; gx <= cx + 1; ++gx) {
        const auto it = buckets_.find(pack(gx, gy));
        if (it == buckets_.end()) {
          continue;
        }
        for (const Vec3& p : it->second) {
          const double d = ((p.x - x) * (p.x - x)) + ((p.y - y) * (p.y - y));
          if (d < best) {
            best = d;
            z = p.z;
          }
        }
      }
    }
    return {best == std::numeric_limits<double>::max() ? best : std::sqrt(best), z};
  }

private:
  [[nodiscard]] std::int64_t cell_of(double v) const {
    return static_cast<std::int64_t>(std::floor(v / cell_));
  }

  [[nodiscard]] static std::uint64_t pack(std::int64_t x, std::int64_t y) {
    return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(x)) << 32) |
           static_cast<std::uint32_t>(y);
  }

  [[nodiscard]] std::uint64_t key(double x, double y) const { return pack(cell_of(x), cell_of(y)); }

  double cell_;
  std::unordered_map<std::uint64_t, std::vector<Vec3>> buckets_;
};

/// The plan-view rectangle covering the whole height field, CCW.
Clipper2Lib::PathD extent_rect(const HeightField& field) {
  const std::array<double, 4> e = field_extent(field);
  return {{e[0], e[1]}, {e[2], e[1]}, {e[2], e[3]}, {e[0], e[3]}};
}

/// Every road's plan-view footprint plus every surface's filled region — the
/// area the terrain must NOT cover, so it does not double up with the road or
/// surface channels. Connecting roads are roads, so junction interiors are
/// covered too. Border samples (with z) are accumulated for the skirt blend and
/// the watertight stitch.
Clipper2Lib::PathsD
occluders(const RoadNetwork& network, const SamplingOptions& sampling, std::vector<Vec3>& border) {
  Clipper2Lib::PathsD paths;
  network.for_each_road([&](RoadId, const Road& road) {
    if (road.plan_view.empty() || road.sections.empty()) {
      return;
    }
    RoadContribution contribution = build_contribution(network, road, sampling);
    if (Clipper2Lib::Area(contribution.footprint) < 0.0) {
      std::ranges::reverse(contribution.footprint);
    }
    paths.push_back(std::move(contribution.footprint));
    border.insert(border.end(), contribution.border.begin(), contribution.border.end());
  });

  network.for_each_surface([&](SurfaceId, const Surface& surface) {
    std::vector<std::array<double, 2>> polygon;
    if (surface.source == BoundarySource::Authored) {
      polygon = sample_surface_boundary(surface.nodes);
    } else {
      polygon = derived_region_polygon(network, surface, sampling);
    }
    if (polygon.size() < 3) {
      return;
    }
    Clipper2Lib::PathD path;
    path.reserve(polygon.size());
    for (const std::array<double, 2>& p : polygon) {
      path.emplace_back(p[0], p[1]);
    }
    if (Clipper2Lib::Area(path) < 0.0) {
      std::ranges::reverse(path);
    }
    paths.push_back(std::move(path));
  });
  return paths;
}

/// smoothstep(0,1,·): 0 at t<=0, 1 at t>=1, C1 in between.
double smoothstep(double t) {
  t = std::clamp(t, 0.0, 1.0);
  return t * t * (3.0 - (2.0 * t));
}

} // namespace

SubMesh
build_terrain_mesh(const RoadNetwork& network, const SamplingOptions& sampling, double skirt) {
  const HeightField& field = network.terrain();
  if (field.empty()) {
    return {};
  }

  std::vector<Vec3> border;
  const Clipper2Lib::PathsD blockers = occluders(network, sampling, border);

  // The ground the network does NOT already cover: the field extent minus every
  // road and surface footprint. NonZero so nested/overlapping footprints union
  // rather than punching holes back open.
  const Clipper2Lib::PathsD region =
      Clipper2Lib::Difference(Clipper2Lib::PathsD{extent_rect(field)},
                              blockers,
                              Clipper2Lib::FillRule::NonZero,
                              kUnionPrecision);
  if (region.empty()) {
    return {};
  }

  std::vector<CDT::V2d<double>> vertices;
  std::vector<CDT::Edge> edges;
  double min_x = std::numeric_limits<double>::max();
  double min_y = std::numeric_limits<double>::max();
  double max_x = std::numeric_limits<double>::lowest();
  double max_y = std::numeric_limits<double>::lowest();
  subdivide_rings_to_cdt(region, vertices, edges, min_x, min_y, max_x, max_y);
  steiner_grid_fill(region, vertices, min_x, min_y, max_x, max_y);

  std::optional<CompactMesh> mesh = triangulate_region(vertices, edges);
  if (!mesh.has_value()) {
    return {};
  }

  // z per vertex: blend the field sample toward the nearest road-edge z across
  // the skirt. w = 1 at the kerb (the ground meets the road), decaying to the
  // raw field at `skirt` — cut where the field is above the road, fill where
  // below (ADR-0006's "roads cut/conform" with no separate pass). Vertices
  // farther than one skirt from any road keep the bare field sample.
  const BorderIndex index(border, skirt);
  const double safe_skirt = skirt > 0.0 ? skirt : 1.0;
  for (Vec3& v : mesh->vertices) {
    const double sampled = sample_height(field, v.x, v.y);
    const std::array<double, 2> near = index.nearest(v.x, v.y);
    if (near[0] >= safe_skirt) {
      v.z = sampled;
      continue;
    }
    const double w = smoothstep(1.0 - (near[0] / safe_skirt));
    v.z = ((1.0 - w) * sampled) + (w * near[1]);
  }

  // Watertight stitch: snap terrain boundary vertices sitting within kSeamSnap
  // of a road-border vertex onto it exactly (bitwise-equal z). THIS is what
  // welds the ground to the road mesh with no cliff — the skirt blend gets the
  // z close, the stitch makes the seam identical. The outer extent boundary is
  // far from any border, so those vertices are left where they are.
  stitch_and_weld(*mesh, border);
  if (mesh->triangles.empty()) {
    return {};
  }
  return emit(*mesh, "terrain");
}

void remesh_terrain(const RoadNetwork& network, NetworkMesh& mesh, const MeshOptions& options) {
  if (!options.terrain || network.terrain().empty()) {
    mesh.terrain = {};
    return;
  }
  mesh.terrain = build_terrain_mesh(network, options.sampling, options.terrain_skirt);
}

} // namespace roadmaker
