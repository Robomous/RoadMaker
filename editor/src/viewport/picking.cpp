#include "viewport/picking.hpp"

#include <Eigen/Dense>

#include <algorithm>
#include <cmath>
#include <limits>

namespace roadmaker::editor {

namespace {

using Vec3 = Eigen::Vector3d;

Vec3 to_vec(const std::array<double, 3>& a) {
  return {a[0], a[1], a[2]};
}

/// Ray/AABB slab test; returns false when the box is missed or lies entirely
/// beyond `max_distance`.
bool intersects_aabb(const Ray& ray, const RoadAabb& box, double max_distance) {
  double t_near = 0.0;
  double t_far = max_distance;
  for (std::size_t axis = 0; axis < 3; ++axis) {
    const double o = ray.origin[axis];
    const double d = ray.direction[axis];
    if (std::abs(d) < std::numeric_limits<double>::epsilon()) {
      if (o < box.lo[axis] || o > box.hi[axis]) {
        return false;
      }
      continue;
    }
    double t0 = (box.lo[axis] - o) / d;
    double t1 = (box.hi[axis] - o) / d;
    if (t0 > t1) {
      std::swap(t0, t1);
    }
    t_near = std::max(t_near, t0);
    t_far = std::min(t_far, t1);
    if (t_near > t_far) {
      return false;
    }
  }
  return true;
}

/// Möller–Trumbore, no backface culling. Returns the ray parameter t >= 0 or
/// nullopt.
std::optional<double>
intersect_triangle(const Ray& ray, const Vec3& v0, const Vec3& v1, const Vec3& v2) {
  constexpr double kEpsilon = 1e-12;
  const Vec3 origin = to_vec(ray.origin);
  const Vec3 dir = to_vec(ray.direction);
  const Vec3 edge1 = v1 - v0;
  const Vec3 edge2 = v2 - v0;
  const Vec3 pvec = dir.cross(edge2);
  const double det = edge1.dot(pvec);
  if (std::abs(det) < kEpsilon) {
    return std::nullopt; // parallel to the triangle plane
  }
  const double inv_det = 1.0 / det;
  const Vec3 tvec = origin - v0;
  const double u = tvec.dot(pvec) * inv_det;
  if (u < 0.0 || u > 1.0) {
    return std::nullopt;
  }
  const Vec3 qvec = tvec.cross(edge1);
  const double v = dir.dot(qvec) * inv_det;
  if (v < 0.0 || u + v > 1.0) {
    return std::nullopt;
  }
  const double t = edge2.dot(qvec) * inv_det;
  if (t < 0.0) {
    return std::nullopt;
  }
  return t;
}

Vec3 vertex(const std::vector<double>& positions, std::uint32_t index) {
  const std::size_t base = static_cast<std::size_t>(index) * 3;
  return {positions[base], positions[base + 1], positions[base + 2]};
}

/// Squared plan-view distance from (x, y) to the pose at station s.
double distance_sq(const ReferenceLine& line, double s, double x, double y) {
  const PathPoint p = line.evaluate(s);
  const double dx = x - p.x;
  const double dy = y - p.y;
  return (dx * dx) + (dy * dy);
}

} // namespace

Ray make_pick_ray(const CameraMatrices& camera, double px, double py, double width, double height) {
  const Eigen::Matrix4d view = Eigen::Map<const Eigen::Matrix4f>(camera.view.data()).cast<double>();
  const Eigen::Matrix4d projection =
      Eigen::Map<const Eigen::Matrix4f>(camera.projection.data()).cast<double>();
  const Eigen::Matrix4d inverse = (projection * view).inverse();

  // Pixel -> NDC; Qt's y grows downward, NDC's grows upward.
  const double ndc_x = (2.0 * px / width) - 1.0;
  const double ndc_y = 1.0 - (2.0 * py / height);

  auto unproject = [&](double ndc_z) {
    Eigen::Vector4d p = inverse * Eigen::Vector4d(ndc_x, ndc_y, ndc_z, 1.0);
    return Vec3(p.head<3>() / p.w());
  };
  const Vec3 near_point = unproject(-1.0);
  const Vec3 far_point = unproject(1.0);
  const Vec3 dir = (far_point - near_point).normalized();

  return Ray{
      .origin = {near_point.x(), near_point.y(), near_point.z()},
      .direction = {dir.x(), dir.y(), dir.z()},
  };
}

RoadAabb compute_road_aabb(const RoadMesh& road) {
  constexpr double kMax = std::numeric_limits<double>::max();
  RoadAabb box{.lo = {kMax, kMax, kMax}, .hi = {-kMax, -kMax, -kMax}};
  for (std::size_t i = 0; i + 2 < road.positions.size(); i += 3) {
    for (std::size_t axis = 0; axis < 3; ++axis) {
      box.lo[axis] = std::min(box.lo[axis], road.positions[i + axis]);
      box.hi[axis] = std::max(box.hi[axis], road.positions[i + axis]);
    }
  }
  return box;
}

std::vector<RoadAabb> compute_road_aabbs(const NetworkMesh& mesh) {
  std::vector<RoadAabb> boxes;
  boxes.reserve(mesh.roads.size());
  for (const RoadMesh& road : mesh.roads) {
    boxes.push_back(compute_road_aabb(road));
  }
  return boxes;
}

std::optional<PickHit>
pick(const NetworkMesh& mesh, std::span<const RoadAabb> road_aabbs, const Ray& ray) {
  std::optional<PickHit> best;
  double best_t = std::numeric_limits<double>::max();

  for (std::size_t road_index = 0; road_index < mesh.roads.size(); ++road_index) {
    if (road_index < road_aabbs.size() && !intersects_aabb(ray, road_aabbs[road_index], best_t)) {
      continue;
    }
    const RoadMesh& road = mesh.roads[road_index];
    for (const RoadMesh::LanePatch& patch : road.lanes) {
      for (std::size_t i = 0; i + 2 < patch.indices.size(); i += 3) {
        const auto t = intersect_triangle(ray,
                                          vertex(road.positions, patch.indices[i]),
                                          vertex(road.positions, patch.indices[i + 1]),
                                          vertex(road.positions, patch.indices[i + 2]));
        if (t && *t < best_t) {
          best_t = *t;
          best = PickHit{
              .road = road.road,
              .lane = patch.lane,
              .position = {ray.origin[0] + (ray.direction[0] * *t),
                           ray.origin[1] + (ray.direction[1] * *t),
                           ray.origin[2] + (ray.direction[2] * *t)},
              .distance = *t,
          };
        }
      }
    }
  }
  return best;
}

StationCoord find_station(const ReferenceLine& line, double x, double y) {
  if (line.empty()) {
    return {};
  }
  const double length = line.length();

  // Coarse global scan: keeps multimodal cases (hairpins, loops) on the
  // correct branch as long as the step undercuts half the loop spacing.
  const double step = std::clamp(length / 512.0, 0.25, 5.0);
  double best_s = 0.0;
  double best_d = std::numeric_limits<double>::max();
  for (double s = 0.0;; s += step) {
    const double clamped = std::min(s, length);
    const double d = distance_sq(line, clamped, x, y);
    if (d < best_d) {
      best_d = d;
      best_s = clamped;
    }
    if (clamped >= length) {
      break;
    }
  }

  // Golden-section refinement of the bracketing interval.
  constexpr double kInvPhi = 0.6180339887498949;
  double a = std::max(0.0, best_s - step);
  double b = std::min(length, best_s + step);
  double c = b - ((b - a) * kInvPhi);
  double d = a + ((b - a) * kInvPhi);
  double fc = distance_sq(line, c, x, y);
  double fd = distance_sq(line, d, x, y);
  while (b - a > 1e-4) {
    if (fc < fd) {
      b = d;
      d = c;
      fd = fc;
      c = b - ((b - a) * kInvPhi);
      fc = distance_sq(line, c, x, y);
    } else {
      a = c;
      c = d;
      fc = fd;
      d = a + ((b - a) * kInvPhi);
      fd = distance_sq(line, d, x, y);
    }
  }
  const double s = (a + b) / 2.0;

  // t per OpenDRIVE §8.3: positive to the LEFT of the direction of travel —
  // the projection onto the left normal (-sin hdg, cos hdg).
  const PathPoint pose = line.evaluate(s);
  const double t = (-std::sin(pose.hdg) * (x - pose.x)) + (std::cos(pose.hdg) * (y - pose.y));
  return StationCoord{.s = s, .t = t};
}

} // namespace roadmaker::editor
