#include "surface_fill.hpp"

#include "roadmaker/road/surface.hpp"
#include "roadmaker/tol.hpp"

#include <CDT.h>
#include <Eigen/SparseCholesky>
#include <Eigen/SparseCore>
#include <clipper2/clipper.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <utility>
#include <vector>

#include "mesh_detail.hpp"

// The enclosed-area ground surface reuses the proven junction-floor backend
// (docs/design/m2/03_junction_blending.md): the union → CDT → harmonic-solve →
// watertight-stitch stages are identical. The ONE geometric difference is the
// region selected from the footprint union: a junction keeps the union's
// INTERIOR and drops holes; an enclosed surface keeps the union's HOLE (the
// block the ring roads surround shows up as an inner, opposite-wound contour).
// The shared stages are duplicated here rather than shared out of
// junction_surface.cpp to keep that TU's regression surface untouched (a
// de-dup pass is a deliberate follow-up).

namespace roadmaker {

namespace {

using mesh_detail::boundary_offsets;
using mesh_detail::lateral_point;
using mesh_detail::make_frame;
using mesh_detail::mandatory_stations;
using mesh_detail::section_at;
using mesh_detail::StationFrame;

// Holes below this plan area collapse to a flat surface at the mean border
// elevation: the harmonic solve has no room to bend and the CDT would produce
// only slivers (mirrors junction_surface.cpp's tiny-footprint strategy).
constexpr double kFlatFloorMinArea = 4.0;

// Interior Steiner spacing for the elevation field. Grid points give the
// harmonic membrane room to bend between the enclosing road edges.
constexpr double kSteinerStep = 2.0;

// Soft-constraint weight pinning the field toward the ring roads' centerline
// elevations so a long enclosed span does not sag.
constexpr double kCenterlineWeight = 0.1;

// Cotangent weights are clamped positive so the interior Laplacian stays an SPD
// M-matrix (obtuse triangles otherwise yield negative weights).
constexpr double kMinCotWeight = 1e-4;

// Clipper2 union precision (decimal places): 6 = micrometer for meter data, so
// footprint vertices land on the road-mesh vertices the seam stitches to. The
// library default of 2 (1 cm) would round every vertex off its seam target.
constexpr int kUnionPrecision = 6;

// Snap radius for the watertight stitch: any surface boundary vertex this close
// to a road-mesh border vertex IS that vertex (bitwise copy of its exact z).
constexpr double kSeamSnap = 0.012;

struct Vec3 {
  double x, y, z;
};

/// One ring road's contribution: plan-view footprint (for the union), the exact
/// 3D border ring (snap targets + Dirichlet source), and centerline samples
/// (soft interior constraints). Identical construction to the junction backend
/// so the surface seam lands on the road mesh's exact vertices.
struct RoadContribution {
  Clipper2Lib::PathD footprint;
  std::vector<Vec3> border;
  std::vector<Vec3> centerline;
};

/// Stations for a road, identical to build_one_road so the border ring lands on
/// the road mesh's exact vertices (watertight stitch).
std::vector<double>
road_stations(const RoadNetwork& network, const Road& road, const SamplingOptions& sampling) {
  SamplingOptions s = sampling;
  const std::vector<double> extra = mandatory_stations(network, road);
  s.extra_stations.insert(s.extra_stations.end(), extra.begin(), extra.end());
  return sample_stations(road.plan_view, s);
}

RoadContribution
build_contribution(const RoadNetwork& network, const Road& road, const SamplingOptions& sampling) {
  RoadContribution out;
  const std::vector<double> stations = road_stations(network, road, sampling);
  out.border.reserve(stations.size() * 2);
  out.centerline.reserve(stations.size());
  out.footprint.reserve(stations.size() * 2);

  // Left border forward.
  for (const double s : stations) {
    const StationFrame frame = make_frame(road, s);
    const LaneSection& section = section_at(network, road, s);
    const std::vector<double> offsets = boundary_offsets(network, road, section, s);
    const auto left = lateral_point(frame, offsets.front());
    const auto center = lateral_point(frame, eval_profile(road.lane_offset, s));
    out.footprint.emplace_back(left[0], left[1]);
    out.border.push_back({left[0], left[1], left[2]});
    out.centerline.push_back({center[0], center[1], center[2]});
  }
  // Right border reversed — closes the footprint ring.
  for (auto it = stations.rbegin(); it != stations.rend(); ++it) {
    const StationFrame frame = make_frame(road, *it);
    const LaneSection& section = section_at(network, road, *it);
    const std::vector<double> offsets = boundary_offsets(network, road, section, *it);
    const auto right = lateral_point(frame, offsets.back());
    out.footprint.emplace_back(right[0], right[1]);
    out.border.push_back({right[0], right[1], right[2]});
  }
  return out;
}

/// True if `pt` lies within the filled (NonZero) region of `paths`: inside an
/// outer contour (positive area) and outside every hole (negative area).
bool inside_region(const Clipper2Lib::PathsD& paths, const Clipper2Lib::PointD& pt) {
  bool inside = false;
  for (const Clipper2Lib::PathD& path : paths) {
    if (Clipper2Lib::PointInPolygon(pt, path) == Clipper2Lib::PointInPolygonResult::IsInside) {
      if (Clipper2Lib::Area(path) > 0.0) {
        inside = true;
      } else {
        return false; // inside a hole
      }
    }
  }
  return inside;
}

/// z of the nearest border sample to (x, y) across all contributions.
double nearest_border_z(const std::vector<Vec3>& border, double x, double y) {
  double best = std::numeric_limits<double>::max();
  double z = 0.0;
  for (const Vec3& p : border) {
    const double d = ((p.x - x) * (p.x - x)) + ((p.y - y) * (p.y - y));
    if (d < best) {
      best = d;
      z = p.z;
    }
  }
  return z;
}

/// Compacted triangulation: only vertices referenced by a surviving triangle,
/// remapped to a dense index range.
struct CompactMesh {
  std::vector<Vec3> vertices; // z filled later
  std::vector<std::array<std::uint32_t, 3>> triangles;
};

CompactMesh compact(const CDT::Triangulation<double>& cdt) {
  CompactMesh out;
  std::vector<std::uint32_t> remap(cdt.vertices.size(), std::numeric_limits<std::uint32_t>::max());
  for (const auto& tri : cdt.triangles) {
    const auto& p0 = cdt.vertices[tri.vertices[0]];
    const auto& p1 = cdt.vertices[tri.vertices[1]];
    const auto& p2 = cdt.vertices[tri.vertices[2]];
    const double area2 = ((p1.x - p0.x) * (p2.y - p0.y)) - ((p2.x - p0.x) * (p1.y - p0.y));
    if (std::abs(area2) < 2e-8) {
      continue;
    }
    std::array<std::uint32_t, 3> mapped{};
    for (int k = 0; k < 3; ++k) {
      const std::uint32_t old = tri.vertices[static_cast<std::size_t>(k)];
      if (remap[old] == std::numeric_limits<std::uint32_t>::max()) {
        remap[old] = static_cast<std::uint32_t>(out.vertices.size());
        out.vertices.push_back({cdt.vertices[old].x, cdt.vertices[old].y, 0.0});
      }
      mapped[static_cast<std::size_t>(k)] = remap[old];
    }
    out.triangles.push_back(mapped);
  }
  return out;
}

/// Boundary vertices: endpoints of edges used by exactly one triangle.
std::vector<bool> boundary_flags(const CompactMesh& mesh) {
  std::map<std::pair<std::uint32_t, std::uint32_t>, int> edge_count;
  auto key = [](std::uint32_t a, std::uint32_t b) {
    return a < b ? std::pair{a, b} : std::pair{b, a};
  };
  for (const auto& t : mesh.triangles) {
    ++edge_count[key(t[0], t[1])];
    ++edge_count[key(t[1], t[2])];
    ++edge_count[key(t[2], t[0])];
  }
  std::vector<bool> on_boundary(mesh.vertices.size(), false);
  for (const auto& [edge, count] : edge_count) {
    if (count == 1) {
      on_boundary[edge.first] = true;
      on_boundary[edge.second] = true;
    }
  }
  return on_boundary;
}

/// Solves the harmonic elevation field in place: interior vertex z minimizes
/// membrane energy with the boundary z held fixed, plus soft centerline pulls.
void solve_elevation(CompactMesh& mesh,
                     const std::vector<bool>& on_boundary,
                     const std::vector<Vec3>& centerline) {
  const std::size_t n = mesh.vertices.size();

  std::vector<std::uint32_t> interior_index(n, std::numeric_limits<std::uint32_t>::max());
  std::vector<std::uint32_t> interior_of; // reduced -> global
  for (std::uint32_t i = 0; i < n; ++i) {
    if (!on_boundary[i]) {
      interior_index[i] = static_cast<std::uint32_t>(interior_of.size());
      interior_of.push_back(i);
    }
  }
  const std::size_t m = interior_of.size();
  if (m == 0) {
    return; // fully determined by the boundary; triangles interpolate it
  }

  std::map<std::pair<std::uint32_t, std::uint32_t>, double> weight;
  auto key = [](std::uint32_t a, std::uint32_t b) {
    return a < b ? std::pair{a, b} : std::pair{b, a};
  };
  auto cot = [&](std::uint32_t a, std::uint32_t o, std::uint32_t b) {
    const Vec3& pa = mesh.vertices[a];
    const Vec3& po = mesh.vertices[o];
    const Vec3& pb = mesh.vertices[b];
    const double ux = pa.x - po.x, uy = pa.y - po.y;
    const double vx = pb.x - po.x, vy = pb.y - po.y;
    const double dot = (ux * vx) + (uy * vy);
    const double crs = std::abs((ux * vy) - (uy * vx));
    return crs > tol::kLength ? dot / crs : 0.0;
  };
  for (const auto& t : mesh.triangles) {
    weight[key(t[1], t[2])] += std::max(kMinCotWeight, 0.5 * cot(t[1], t[0], t[2]));
    weight[key(t[2], t[0])] += std::max(kMinCotWeight, 0.5 * cot(t[2], t[1], t[0]));
    weight[key(t[0], t[1])] += std::max(kMinCotWeight, 0.5 * cot(t[0], t[2], t[1]));
  }

  std::vector<Eigen::Triplet<double>> triplets;
  Eigen::VectorXd rhs = Eigen::VectorXd::Zero(static_cast<Eigen::Index>(m));
  std::vector<double> diag(m, 0.0);
  for (const auto& [edge, w] : weight) {
    const std::uint32_t a = edge.first;
    const std::uint32_t b = edge.second;
    const bool ia = !on_boundary[a];
    const bool ib = !on_boundary[b];
    if (ia) {
      diag[interior_index[a]] += w;
    }
    if (ib) {
      diag[interior_index[b]] += w;
    }
    if (ia && ib) {
      triplets.emplace_back(
          static_cast<int>(interior_index[a]), static_cast<int>(interior_index[b]), -w);
      triplets.emplace_back(
          static_cast<int>(interior_index[b]), static_cast<int>(interior_index[a]), -w);
    } else if (ia) {
      rhs[interior_index[a]] += w * mesh.vertices[b].z;
    } else if (ib) {
      rhs[interior_index[b]] += w * mesh.vertices[a].z;
    }
  }

  for (const Vec3& c : centerline) {
    double best = std::numeric_limits<double>::max();
    std::uint32_t nearest = std::numeric_limits<std::uint32_t>::max();
    for (const std::uint32_t gi : interior_of) {
      const Vec3& p = mesh.vertices[gi];
      const double d = ((p.x - c.x) * (p.x - c.x)) + ((p.y - c.y) * (p.y - c.y));
      if (d < best) {
        best = d;
        nearest = interior_index[gi];
      }
    }
    if (nearest != std::numeric_limits<std::uint32_t>::max()) {
      diag[nearest] += kCenterlineWeight;
      rhs[nearest] += kCenterlineWeight * c.z;
    }
  }

  for (std::size_t i = 0; i < m; ++i) {
    triplets.emplace_back(static_cast<int>(i), static_cast<int>(i), diag[i]);
  }

  Eigen::SparseMatrix<double> a(static_cast<Eigen::Index>(m), static_cast<Eigen::Index>(m));
  a.setFromTriplets(triplets.begin(), triplets.end());
  Eigen::SimplicialLDLT<Eigen::SparseMatrix<double>> solver;
  solver.compute(a);
  if (solver.info() != Eigen::Success) {
    return;
  }
  const Eigen::VectorXd z = solver.solve(rhs);
  if (solver.info() != Eigen::Success) {
    return;
  }
  for (std::size_t r = 0; r < m; ++r) {
    mesh.vertices[interior_of[r]].z = z[static_cast<Eigen::Index>(r)];
  }
}

/// Emits the compacted mesh as a SubMesh with per-vertex normals (always in the
/// +Z hemisphere — the surface is a height field).
SubMesh emit(const CompactMesh& mesh, std::string name) {
  SubMesh out;
  // The ground surface is drivable/paved ground: the same material class as the
  // lanes bounding it, so renderers draw one continuous surface.
  out.material = LaneType::Driving;
  out.name = std::move(name);
  out.positions.reserve(mesh.vertices.size() * 3);
  for (const Vec3& v : mesh.vertices) {
    out.positions.insert(out.positions.end(), {v.x, v.y, v.z});
  }
  std::vector<std::array<double, 3>> normals(mesh.vertices.size(), {0.0, 0.0, 0.0});
  for (const auto& t : mesh.triangles) {
    const Vec3& p0 = mesh.vertices[t[0]];
    const Vec3& p1 = mesh.vertices[t[1]];
    const Vec3& p2 = mesh.vertices[t[2]];
    const double ux = p1.x - p0.x, uy = p1.y - p0.y, uz = p1.z - p0.z;
    const double vx = p2.x - p0.x, vy = p2.y - p0.y, vz = p2.z - p0.z;
    std::array<double, 3> nrm{(uy * vz) - (uz * vy), (uz * vx) - (ux * vz), (ux * vy) - (uy * vx)};
    if (nrm[2] < 0.0) { // never happens for CCW height-field triangles; guard
      nrm = {-nrm[0], -nrm[1], -nrm[2]};
    }
    for (int k = 0; k < 3; ++k) {
      for (int c = 0; c < 3; ++c) {
        normals[t[static_cast<std::size_t>(k)]][static_cast<std::size_t>(c)] +=
            nrm[static_cast<std::size_t>(c)];
      }
    }
    out.indices.insert(out.indices.end(), {t[0], t[1], t[2]});
  }
  for (auto& nrm : normals) {
    const double len = std::sqrt((nrm[0] * nrm[0]) + (nrm[1] * nrm[1]) + (nrm[2] * nrm[2]));
    if (len > 0.0) {
      out.normals.insert(out.normals.end(), {nrm[0] / len, nrm[1] / len, nrm[2] / len});
    } else {
      out.normals.insert(out.normals.end(), {0.0, 0.0, 1.0});
    }
  }
  return out;
}

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
  {
    std::vector<CDT::V2d<double>> ring;
    for (std::size_t p = 0; p < region.size(); ++p) {
      const Clipper2Lib::PointD& a = region[p];
      const Clipper2Lib::PointD& b = region[(p + 1) % region.size()];
      ring.push_back(CDT::V2d<double>{a.x, a.y});
      const double len = std::hypot(b.x - a.x, b.y - a.y);
      const int pieces = static_cast<int>(len / kSteinerStep);
      for (int k = 1; k <= pieces - 1; ++k) {
        const double f = static_cast<double>(k) / static_cast<double>(pieces);
        ring.push_back(CDT::V2d<double>{a.x + (f * (b.x - a.x)), a.y + (f * (b.y - a.y))});
      }
    }
    const std::size_t first = vertices.size();
    for (const CDT::V2d<double>& point : ring) {
      vertices.push_back(point);
      min_x = std::min(min_x, point.x);
      min_y = std::min(min_y, point.y);
      max_x = std::max(max_x, point.x);
      max_y = std::max(max_y, point.y);
    }
    for (std::size_t i = first; i < vertices.size(); ++i) {
      const std::size_t next = (i + 1 < vertices.size()) ? i + 1 : first;
      edges.emplace_back(static_cast<CDT::VertInd>(i), static_cast<CDT::VertInd>(next));
    }
  }
  CDT::RemoveDuplicatesAndRemapEdges(vertices, edges);

  if (!flat_floor) {
    // Steiner points only in the eroded interior: a grid point millimeters from
    // a boundary edge would pair with it into a needle triangle.
    const Clipper2Lib::PathsD interior = Clipper2Lib::InflatePaths(region_paths,
                                                                   -0.5 * kSteinerStep,
                                                                   Clipper2Lib::JoinType::Round,
                                                                   Clipper2Lib::EndType::Polygon,
                                                                   2.0,
                                                                   kUnionPrecision);
    const std::size_t nx = static_cast<std::size_t>((max_x - min_x) / kSteinerStep);
    const std::size_t ny = static_cast<std::size_t>((max_y - min_y) / kSteinerStep);
    for (std::size_t iy = 1; iy < ny; ++iy) {
      for (std::size_t ix = 1; ix < nx; ++ix) {
        const Clipper2Lib::PointD pt{min_x + (static_cast<double>(ix) * kSteinerStep),
                                     min_y + (static_cast<double>(iy) * kSteinerStep)};
        if (inside_region(interior, pt)) {
          vertices.push_back(CDT::V2d<double>{pt.x, pt.y});
        }
      }
    }
  }

  // TryResolve: Clipper2's float output is topologically a union but can carry
  // constraint edges CDT's exact predicates consider intersecting; the default
  // strategy THROWS on those. The catch is the kernel exception boundary: a
  // region that cannot triangulate degrades to no surface, never a crash.
  CDT::Triangulation<double> cdt(
      CDT::VertexInsertionOrder::Auto, CDT::IntersectingConstraintEdges::TryResolve, 0.0);
  try {
    cdt.insertVertices(vertices);
    cdt.insertEdges(edges);
    cdt.eraseOuterTrianglesAndHoles();
  } catch (const std::exception&) {
    return {};
  }
  if (cdt.triangles.empty()) {
    return {};
  }

  CompactMesh mesh = compact(cdt);

  // 5. Watertight stitch BEFORE elevation: snap each boundary vertex onto the
  //    exact road border vertex it approximates (bitwise-equal doubles), then
  //    cluster-weld sub-feature debris so the seam carries no cracks.
  {
    const std::vector<bool> on_boundary = boundary_flags(mesh);
    std::vector<bool> exact(mesh.vertices.size(), false);
    for (std::size_t i = 0; i < mesh.vertices.size(); ++i) {
      if (!on_boundary[i]) {
        continue;
      }
      double best = std::numeric_limits<double>::max();
      const Vec3* match = nullptr;
      for (const Vec3& p : border) {
        const double d = ((p.x - mesh.vertices[i].x) * (p.x - mesh.vertices[i].x)) +
                         ((p.y - mesh.vertices[i].y) * (p.y - mesh.vertices[i].y));
        if (d < best) {
          best = d;
          match = &p;
        }
      }
      if (match != nullptr && best <= kSeamSnap * kSeamSnap) {
        mesh.vertices[i] = *match;
        exact[i] = true;
      }
    }
    constexpr double kMinFeature = 0.02;
    std::vector<std::uint32_t> parent(mesh.vertices.size());
    for (std::size_t i = 0; i < parent.size(); ++i) {
      parent[i] = static_cast<std::uint32_t>(i);
    }
    const auto find = [&](std::uint32_t a) {
      while (parent[a] != a) {
        parent[a] = parent[parent[a]];
        a = parent[a];
      }
      return a;
    };
    for (std::size_t i = 0; i < mesh.vertices.size(); ++i) {
      for (std::size_t j = i + 1; j < mesh.vertices.size(); ++j) {
        if (exact[i] && exact[j]) {
          continue;
        }
        const double dx = mesh.vertices[i].x - mesh.vertices[j].x;
        const double dy = mesh.vertices[i].y - mesh.vertices[j].y;
        if ((dx * dx) + (dy * dy) < kMinFeature * kMinFeature) {
          const std::uint32_t ri = find(static_cast<std::uint32_t>(i));
          const std::uint32_t rj = find(static_cast<std::uint32_t>(j));
          if (ri != rj) {
            if (exact[rj] && !exact[ri]) {
              parent[ri] = rj;
            } else {
              parent[rj] = ri;
            }
          }
        }
      }
    }
    CompactMesh welded;
    std::vector<std::uint32_t> remap(mesh.vertices.size(),
                                     std::numeric_limits<std::uint32_t>::max());
    for (const auto& t : mesh.triangles) {
      const std::array<std::uint32_t, 3> reps{find(t[0]), find(t[1]), find(t[2])};
      if (reps[0] == reps[1] || reps[1] == reps[2] || reps[2] == reps[0]) {
        continue;
      }
      const Vec3& p0 = mesh.vertices[reps[0]];
      const Vec3& p1 = mesh.vertices[reps[1]];
      const Vec3& p2 = mesh.vertices[reps[2]];
      const double area2 = ((p1.x - p0.x) * (p2.y - p0.y)) - ((p2.x - p0.x) * (p1.y - p0.y));
      if (area2 < 1e-8) { // degenerate or weld-inverted
        continue;
      }
      const double a = std::hypot(p1.x - p0.x, p1.y - p0.y);
      const double b = std::hypot(p2.x - p1.x, p2.y - p1.y);
      const double c = std::hypot(p0.x - p2.x, p0.y - p2.y);
      const double longest = std::max({a, b, c});
      if (area2 < longest * 1e-4) { // height below 0.2 mm
        continue;
      }
      std::array<std::uint32_t, 3> mapped{};
      for (int k = 0; k < 3; ++k) {
        const std::uint32_t rep = reps[static_cast<std::size_t>(k)];
        if (remap[rep] == std::numeric_limits<std::uint32_t>::max()) {
          remap[rep] = static_cast<std::uint32_t>(welded.vertices.size());
          welded.vertices.push_back(mesh.vertices[rep]);
        }
        mapped[static_cast<std::size_t>(k)] = remap[rep];
      }
      welded.triangles.push_back(mapped);
    }
    mesh = std::move(welded);
  }
  if (mesh.triangles.empty()) {
    return {};
  }
  const std::vector<bool> on_boundary = boundary_flags(mesh);

  // 6. Elevation: Dirichlet boundary z from the nearest road border (snapped
  //    vertices already carry the exact z); harmonic interior, or a flat surface
  //    at the mean border elevation for a tiny enclosed area.
  for (std::size_t i = 0; i < mesh.vertices.size(); ++i) {
    if (flat_floor) {
      mesh.vertices[i].z = mean_z;
    } else if (on_boundary[i]) {
      mesh.vertices[i].z = nearest_border_z(border, mesh.vertices[i].x, mesh.vertices[i].y);
    }
  }
  if (!flat_floor) {
    solve_elevation(mesh, on_boundary, centerline);
  }

  return emit(mesh, "surface");
}

} // namespace roadmaker
