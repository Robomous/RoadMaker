// Copyright 2026 Robomous
// SPDX-License-Identifier: Apache-2.0

#include "surface_fill.hpp"

#include "roadmaker/road/surface.hpp"
#include "roadmaker/tol.hpp"

#include <CDT.h>
#include <clipper2/clipper.h>

#include <algorithm>
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

} // namespace

SubMesh build_surface_mesh(const RoadNetwork& network,
                           const Surface& surface,
                           const SamplingOptions& sampling) {
  // 1. Gather footprints, exact inner+outer border rings, and centerline
  //    samples for every road bounding the enclosed area.
  Clipper2Lib::PathsD footprints;
  std::vector<Vec3> border;
  std::vector<Vec3> centerline;
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
    footprints.push_back(std::move(contribution.footprint));
    for (const Vec3& p : contribution.border) {
      z_sum += p.z;
      ++z_count;
    }
    border.insert(border.end(), contribution.border.begin(), contribution.border.end());
    centerline.insert(
        centerline.end(), contribution.centerline.begin(), contribution.centerline.end());
  }
  if (footprints.empty()) {
    return {};
  }

  // 2. Union the full-width footprints. A ring of roads unions into a picture
  //    frame: the enclosed block appears as an inner HOLE (opposite winding).
  Clipper2Lib::PathsD merged =
      Clipper2Lib::Union(footprints, Clipper2Lib::FillRule::NonZero, kUnionPrecision);
  merged = Clipper2Lib::SimplifyPaths(merged, 5e-3);

  // 3. The enclosed region is that hole (largest negative-area contour). No hole
  //    → the roads do not actually enclose an area → no surface.
  Clipper2Lib::PathD region = largest_hole(merged);
  if (region.size() < 3) {
    return {};
  }
  const double area = std::abs(Clipper2Lib::Area(region));
  const double mean_z = z_count > 0 ? z_sum / static_cast<double>(z_count) : 0.0;
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
  //    enclosed area.
  assign_boundary_elevation_and_solve(mesh, flat_floor, mean_z, border, centerline);

  return emit(mesh, "surface");
}

} // namespace roadmaker
