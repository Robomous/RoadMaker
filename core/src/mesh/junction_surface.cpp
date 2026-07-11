#include "junction_surface.hpp"

#include "roadmaker/geometry/poly3.hpp"
#include "roadmaker/road/junction.hpp"
#include "roadmaker/tol.hpp"

#include <CDT.h>
#include <Eigen/SparseCholesky>
#include <Eigen/SparseCore>
#include <clipper2/clipper.h>
#include <fmt/format.h>

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

namespace roadmaker {

namespace {

using mesh_detail::boundary_offsets;
using mesh_detail::lateral_point;
using mesh_detail::make_frame;
using mesh_detail::mandatory_stations;
using mesh_detail::section_at;
using mesh_detail::StationFrame;

// Footprints below this plan area collapse to a flat floor at the mean border
// elevation (03 §6, "tiny footprint" strategy): the harmonic solve has no room
// to bend and the CDT would produce only slivers.
constexpr double kFlatFloorMinArea = 4.0;

// Interior Steiner spacing for the elevation field (03 §3, "~1–2 m"). Grid
// points give the harmonic membrane room to bend between the road edges.
constexpr double kSteinerStep = 2.0;

// Soft-constraint weight pinning the field toward connecting-road centerline
// elevations (03 §2) so long through-paths do not sag.
constexpr double kCenterlineWeight = 0.1;

// Cotangent weights are clamped to stay positive: keeps the interior Laplacian
// an SPD M-matrix (obtuse triangles otherwise yield negative weights and a
// non-definite system), so SimplicialLDLT is always valid and deterministic.
constexpr double kMinCotWeight = 1e-4;

struct Vec3 {
  double x, y, z;
};

/// One connecting road's contribution: plan-view footprint (for the union),
/// the exact 3D border ring (snap targets + Dirichlet source), and centerline
/// samples (soft interior constraints).
struct RoadContribution {
  Clipper2Lib::PathD footprint;
  std::vector<Vec3> border;     // outer border ring, exact road-mesh vertices
  std::vector<Vec3> centerline; // reference-line samples with elevation
};

/// Stations for a connecting road, identical to build_one_road so the border
/// ring lands on the road mesh's exact vertices (watertight stitch, §5).
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
  // Right border reversed — closes the ring the same way road_footprint did.
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

/// Connecting roads of this junction, in connection order, de-duplicated.
std::vector<RoadId> connecting_roads(const Junction& junction) {
  std::vector<RoadId> roads;
  for (const JunctionConnection& conn : junction.connections) {
    if (std::ranges::find(roads, conn.connecting_road) == roads.end()) {
      roads.push_back(conn.connecting_road);
    }
  }
  return roads;
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

  // Dense interior index for the reduced system.
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

  // Cotangent Laplacian weights per edge, accumulated over triangles.
  std::map<std::pair<std::uint32_t, std::uint32_t>, double> weight;
  auto key = [](std::uint32_t a, std::uint32_t b) {
    return a < b ? std::pair{a, b} : std::pair{b, a};
  };
  auto cot = [&](std::uint32_t a, std::uint32_t o, std::uint32_t b) {
    // Cotangent of the angle at `o` in triangle (a, o, b), planar geometry.
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

  // Reduced SPD system A z_int = b. A_ii = sum of incident weights; the
  // boundary neighbors move to the right-hand side (Dirichlet).
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
      // Eigen::Triplet<double>'s storage index is int; cast explicitly so MSVC
      // does not flag a narrowing conversion (warnings are errors in CI).
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

  // Soft centerline constraints: pull the nearest interior vertex toward the
  // sampled road elevation (03 §2, weight 0.1).
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
    return; // leave interior at boundary-interpolated 0; degenerate input
  }
  const Eigen::VectorXd z = solver.solve(rhs);
  if (solver.info() != Eigen::Success) {
    return;
  }
  for (std::size_t r = 0; r < m; ++r) {
    mesh.vertices[interior_of[r]].z = z[static_cast<Eigen::Index>(r)];
  }
}

/// Emits the compacted mesh as a SubMesh with per-vertex normals (always in
/// the +Z hemisphere — the surface is a height field).
SubMesh emit(const CompactMesh& mesh, std::string name) {
  SubMesh out;
  out.material = LaneType::None;
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

} // namespace

SubMesh build_junction_surface(const RoadNetwork& network,
                               const Junction& junction,
                               const SamplingOptions& sampling) {
  // 1. Gather footprints, exact border rings, and centerline samples.
  Clipper2Lib::PathsD footprints;
  std::vector<Vec3> border;
  std::vector<Vec3> centerline;
  double z_sum = 0.0;
  std::size_t z_count = 0;
  for (const RoadId road_id : connecting_roads(junction)) {
    const Road* road = network.road(road_id);
    if (road == nullptr || road->plan_view.empty() || road->sections.empty()) {
      continue;
    }
    RoadContribution contribution = build_contribution(network, *road, sampling);
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

  // 2. Union → boundary polygon(s).
  const Clipper2Lib::PathsD merged = Clipper2Lib::Union(footprints, Clipper2Lib::FillRule::NonZero);
  if (merged.empty()) {
    return {};
  }
  double area = 0.0;
  for (const Clipper2Lib::PathD& path : merged) {
    area += Clipper2Lib::Area(path);
  }
  const double mean_z = z_count > 0 ? z_sum / static_cast<double>(z_count) : 0.0;

  // 3. Constrained Delaunay of the boundary, with interior Steiner refinement
  //    (skipped for the flat-floor fallback — nothing to bend).
  const bool flat_floor = std::abs(area) < kFlatFloorMinArea;

  std::vector<CDT::V2d<double>> vertices;
  std::vector<CDT::Edge> edges;
  double min_x = std::numeric_limits<double>::max(), min_y = min_x;
  double max_x = std::numeric_limits<double>::lowest(), max_y = max_x;
  for (const Clipper2Lib::PathD& path : merged) {
    const std::size_t first = vertices.size();
    for (const Clipper2Lib::PointD& point : path) {
      vertices.push_back(CDT::V2d<double>{point.x, point.y});
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
    const std::size_t nx = static_cast<std::size_t>((max_x - min_x) / kSteinerStep);
    const std::size_t ny = static_cast<std::size_t>((max_y - min_y) / kSteinerStep);
    for (std::size_t iy = 1; iy < ny; ++iy) {
      for (std::size_t ix = 1; ix < nx; ++ix) {
        const Clipper2Lib::PointD pt{min_x + (static_cast<double>(ix) * kSteinerStep),
                                     min_y + (static_cast<double>(iy) * kSteinerStep)};
        if (inside_region(merged, pt)) {
          vertices.push_back(CDT::V2d<double>{pt.x, pt.y});
        }
      }
    }
  }

  CDT::Triangulation<double> cdt;
  cdt.insertVertices(vertices);
  cdt.insertEdges(edges);
  cdt.eraseOuterTrianglesAndHoles();
  if (cdt.triangles.empty()) {
    return {};
  }

  CompactMesh mesh = compact(cdt);
  const std::vector<bool> on_boundary = boundary_flags(mesh);

  // 4. Elevation: Dirichlet boundary z from the nearest road border; harmonic
  //    interior (or flat floor for tiny footprints).
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

  // 5. Watertight stitch: snap each boundary vertex onto the exact road border
  //    vertex it approximates (bitwise-equal doubles, §5).
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
    if (match != nullptr && best <= tol::kLength * tol::kLength) {
      mesh.vertices[i] = *match;
    }
  }

  return emit(mesh, fmt::format("junction {} surface", junction.odr_id));
}

} // namespace roadmaker
