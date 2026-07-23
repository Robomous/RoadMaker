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

#include "surface_fill.hpp"

#include "roadmaker/mesh/surface_boundary.hpp"
#include "roadmaker/road/surface.hpp"
#include "roadmaker/tol.hpp"

#include <CDT.h>
#include <clipper2/clipper.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <optional>
#include <utility>
#include <vector>

#include "fill_backend.hpp"

// The enclosed-area ground surface and the junction floor share the union → CDT
// → harmonic-solve → watertight-stitch backend (fill_backend.hpp). The ONE
// geometric difference is the region selected from the footprint union: a
// junction keeps the union's INTERIOR and drops holes; an enclosed surface
// keeps the union's HOLE (the block the ring roads surround shows up as an
// inner, opposite-wound contour).

namespace roadmaker {

using namespace fill_backend;

namespace {

/// The enclosed region: the largest-|area| HOLE (negative-area contour) of the
/// footprint union, returned CCW so it can be triangulated as a filled polygon.
/// Empty when the union has no hole — the ring roads do not enclose real area.
Clipper2Lib::PathD largest_hole(const Clipper2Lib::PathsD& merged) {
  const Clipper2Lib::PathD* best = nullptr;
  double best_area = 0.0;
  for (const Clipper2Lib::PathD& path : merged) {
    const double area = Clipper2Lib::Area(path);
    if (area < 0.0 && -area > best_area) {
      best_area = -area;
      best = &path;
    }
  }
  if (best == nullptr) {
    return {};
  }
  Clipper2Lib::PathD hole = *best;
  std::ranges::reverse(hole); // CW hole -> CCW filled region
  return hole;
}

/// The ring roads' contributions: footprints for the union, exact border rings
/// (snap targets + Dirichlet z), centerline samples (soft constraints), and the
/// mean border elevation. Gathered for BOTH boundary sources — an authored
/// surface keeps `bounding_roads` as provenance precisely so its elevation
/// still comes from the roads it was detached from.
struct RingInputs {
  Clipper2Lib::PathsD footprints;
  std::vector<Vec3> border;
  std::vector<Vec3> centerline;
  double mean_z = 0.0;
};

RingInputs
gather_ring(const RoadNetwork& network, const Surface& surface, const SamplingOptions& sampling) {
  RingInputs out;
  double z_sum = 0.0;
  std::size_t z_count = 0;
  for (const RoadId road_id : surface.bounding_roads) {
    const Road* road = network.road(road_id);
    if (road == nullptr || road->plan_view.empty() || road->sections.empty()) {
      continue;
    }
    RoadContribution contribution = build_contribution(network, *road, sampling);
    // Normalize each footprint CCW so the NonZero union sees consistent winding
    // (the ring is built left-forward + right-reversed, which winds CW).
    if (Clipper2Lib::Area(contribution.footprint) < 0.0) {
      std::ranges::reverse(contribution.footprint);
    }
    out.footprints.push_back(std::move(contribution.footprint));
    for (const Vec3& p : contribution.border) {
      z_sum += p.z;
      ++z_count;
    }
    out.border.insert(out.border.end(), contribution.border.begin(), contribution.border.end());
    out.centerline.insert(
        out.centerline.end(), contribution.centerline.begin(), contribution.centerline.end());
  }
  out.mean_z = z_count > 0 ? z_sum / static_cast<double>(z_count) : 0.0;
  return out;
}

/// The enclosed region of a derived ring: union the full-width footprints (a
/// ring of roads unions into a picture frame) and keep the inner HOLE — the
/// block those roads surround. Empty when there is no hole, i.e. the roads do
/// not actually enclose an area.
Clipper2Lib::PathD derived_region(const Clipper2Lib::PathsD& footprints) {
  if (footprints.empty()) {
    return {};
  }
  Clipper2Lib::PathsD merged =
      Clipper2Lib::Union(footprints, Clipper2Lib::FillRule::NonZero, kUnionPrecision);
  merged = Clipper2Lib::SimplifyPaths(merged, 5e-3);
  return largest_hole(merged);
}

/// The region an AUTHORED surface fills: its Hermite loop, tessellated and
/// normalized CCW so it triangulates as a filled polygon like the derived one.
Clipper2Lib::PathD authored_region(const Surface& surface) {
  Clipper2Lib::PathD region;
  for (const std::array<double, 2>& p : sample_surface_boundary(surface.nodes)) {
    region.emplace_back(p[0], p[1]);
  }
  if (region.size() >= 3 && Clipper2Lib::Area(region) < 0.0) {
    std::ranges::reverse(region);
  }
  return region;
}

} // namespace

std::vector<std::array<double, 2>> derived_region_polygon(const RoadNetwork& network,
                                                          const Surface& surface,
                                                          const SamplingOptions& sampling) {
  const Clipper2Lib::PathD region =
      derived_region(gather_ring(network, surface, sampling).footprints);
  std::vector<std::array<double, 2>> out;
  if (region.size() < 3) {
    return out;
  }
  out.reserve(region.size());
  for (const Clipper2Lib::PointD& p : region) {
    out.push_back({p.x, p.y});
  }
  return out;
}

SubMesh build_surface_mesh(const RoadNetwork& network,
                           const Surface& surface,
                           const SamplingOptions& sampling) {
  // 1. Gather the ring roads' footprints, exact border rings and centerline
  //    samples. Authored surfaces keep the ring as provenance: it no longer
  //    shapes the boundary, but it is still where the elevation comes from.
  const RingInputs ring = gather_ring(network, surface, sampling);
  const std::vector<Vec3>& border = ring.border;
  const std::vector<Vec3>& centerline = ring.centerline;
  const bool authored = surface.source == BoundarySource::Authored;
  if (ring.footprints.empty() && !authored) {
    return {};
  }

  // 2-3. The region to fill. An authored boundary IS the region (its node loop
  //      replaces the union/largest-hole derivation entirely); a derived one is
  //      still the hole its ring roads surround.
  Clipper2Lib::PathD region = authored ? authored_region(surface) : derived_region(ring.footprints);
  if (region.size() < 3) {
    return {};
  }
  const double area = std::abs(Clipper2Lib::Area(region));
  const bool flat_floor = area < kFlatFloorMinArea;
  if (area < tol::kLength) {
    return {};
  }

  const Clipper2Lib::PathsD region_paths{region};

  // 4. Constrained Delaunay of the region boundary, subdivided to the Steiner
  //    step (long straight constraint edges otherwise fan sub-degree triangles),
  //    with interior Steiner refinement so the harmonic field can bend.
  std::vector<CDT::V2d<double>> vertices;
  std::vector<CDT::Edge> edges;
  double min_x = std::numeric_limits<double>::max(), min_y = min_x;
  double max_x = std::numeric_limits<double>::lowest(), max_y = max_x;
  subdivide_rings_to_cdt(region_paths, vertices, edges, min_x, min_y, max_x, max_y);
  CDT::RemoveDuplicatesAndRemapEdges(vertices, edges);

  if (!flat_floor) {
    steiner_grid_fill(region_paths, vertices, min_x, min_y, max_x, max_y);
  }

  std::optional<CompactMesh> mesh_opt = triangulate_region(vertices, edges);
  if (!mesh_opt) {
    return {};
  }
  CompactMesh mesh = std::move(*mesh_opt);

  // 5. Watertight stitch BEFORE elevation: snap each boundary vertex onto the
  //    exact road border vertex it approximates, then cluster-weld sub-feature
  //    debris so the seam carries no cracks.
  stitch_and_weld(mesh, border);
  if (mesh.triangles.empty()) {
    return {};
  }

  // 6. Elevation: Dirichlet boundary z from the nearest road border; harmonic
  //    interior, or a flat surface at the mean border elevation for a tiny
  //    enclosed area. An authored boundary dragged clear of the roads keeps its
  //    plan-view shape but still takes its z from the provenance ring — a
  //    ring-less authored surface has no border at all and settles at z = 0
  //    until the P5 height field (#232) gives the ground its own elevation.
  assign_boundary_elevation_and_solve(mesh, flat_floor, ring.mean_z, border, centerline);

  return emit(mesh, "surface");
}

} // namespace roadmaker
