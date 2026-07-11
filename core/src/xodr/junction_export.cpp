#include "junction_export.hpp"

#include "roadmaker/road/junction.hpp"
#include "roadmaker/tol.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

#include "../mesh/junction_surface.hpp"

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

} // namespace roadmaker
