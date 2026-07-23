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

#include "viewport/picking.hpp"

#include "roadmaker/assets/prop_library.hpp"
#include "roadmaker/edit/operations.hpp"
#include "roadmaker/road/network.hpp"

#include <Eigen/Dense>

#include <algorithm>
#include <cmath>
#include <limits>

#include "viewport/projection.hpp"

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

std::optional<std::array<double, 3>> ground_point(
    const CameraMatrices& camera, double px, double py, double width, double height, double max_t) {
  const Ray ray = make_pick_ray(camera, px, py, width, height);
  if (std::abs(ray.direction[2]) < 1e-12) {
    return std::nullopt; // parallel to the ground plane
  }
  const double t = -ray.origin[2] / ray.direction[2];
  if (t < 0.0 || t > max_t) {
    return std::nullopt; // behind the camera or beyond the far cap
  }
  return std::array<double, 3>{ray.origin[0] + (ray.direction[0] * t),
                               ray.origin[1] + (ray.direction[1] * t),
                               ray.origin[2] + (ray.direction[2] * t)};
}

std::optional<PolylineScreenHit>
screen_distance_to_polyline(const ScreenContext& screen,
                            std::span<const std::array<double, 3>> polyline) {
  const auto project = [&screen](const std::array<double, 3>& point) {
    return project_to_screen(
        screen.camera, point[0], point[1], point[2], screen.width, screen.height);
  };

  std::optional<PolylineScreenHit> best;
  const auto consider = [&best](double distance, std::size_t segment, double t) {
    if (!best.has_value() || distance < best->distance) {
      best = PolylineScreenHit{.distance = distance, .segment = segment, .t = t};
    }
  };

  if (polyline.size() == 1) {
    // Degenerate but legitimate: a one-sample path is still something to hover.
    if (const auto a = project(polyline[0])) {
      consider(std::hypot((*a)[0] - screen.px, (*a)[1] - screen.py), 0, 0.0);
    }
    return best;
  }

  for (std::size_t i = 0; i + 1 < polyline.size(); ++i) {
    const std::optional<std::array<double, 2>> a = project(polyline[i]);
    const std::optional<std::array<double, 2>> b = project(polyline[i + 1]);
    if (!a.has_value() || !b.has_value()) {
      continue; // an endpoint at or behind the camera — the segment is not on screen
    }
    const double dx = (*b)[0] - (*a)[0];
    const double dy = (*b)[1] - (*a)[1];
    const double length_sq = (dx * dx) + (dy * dy);
    // A zero-length segment still answers as its own endpoint rather than
    // dividing by zero.
    const double t =
        length_sq <= 1e-12
            ? 0.0
            : std::clamp((((screen.px - (*a)[0]) * dx) + ((screen.py - (*a)[1]) * dy)) / length_sq,
                         0.0,
                         1.0);
    consider(std::hypot((*a)[0] + (dx * t) - screen.px, (*a)[1] + (dy * t) - screen.py), i, t);
  }
  return best;
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

/// Nearest positive ray/sphere hit distance, or nullopt on a miss. `ray`
/// direction is unit length.
std::optional<double>
intersect_sphere(const Ray& ray, const std::array<double, 3>& center, double radius) {
  const Vec3 oc = to_vec(ray.origin) - to_vec(center);
  const Vec3 dir = to_vec(ray.direction);
  const double b = dir.dot(oc);
  const double c = oc.dot(oc) - (radius * radius);
  const double disc = (b * b) - c;
  if (disc < 0.0) {
    return std::nullopt;
  }
  const double root = std::sqrt(disc);
  double t = -b - root;
  if (t < 0.0) {
    t = -b + root;
  }
  if (t < 0.0) {
    return std::nullopt;
  }
  return t;
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

  // Junction floors: the blended surface between the arms is its own
  // selectable entity. Tested after roads so a road patch lying over the floor
  // (they meet coplanar at the stitch seam) still wins on a tie, but the open
  // floor interior — which no road covers — becomes grabbable. Reports the
  // JunctionId with road/lane invalid.
  for (const JunctionFloor& floor : mesh.junction_floors) {
    const std::vector<std::uint32_t>& indices = floor.mesh.indices;
    for (std::size_t i = 0; i + 2 < indices.size(); i += 3) {
      const auto t = intersect_triangle(ray,
                                        vertex(floor.mesh.positions, indices[i]),
                                        vertex(floor.mesh.positions, indices[i + 1]),
                                        vertex(floor.mesh.positions, indices[i + 2]));
      if (t && *t < best_t) {
        best_t = *t;
        best = PickHit{
            .junction = floor.junction,
            .position = {ray.origin[0] + (ray.direction[0] * *t),
                         ray.origin[1] + (ray.direction[1] * *t),
                         ray.origin[2] + (ray.direction[2] * *t)},
            .distance = *t,
        };
      }
    }
  }

  // Enclosed-area ground surfaces (#215): the area a ring of roads surrounds is
  // its own selectable entity, the render/select analog of a junction floor.
  // Tested after roads (and sharing best_t) so a road patch over the surface
  // still wins on a tie — the surface only claims the open interior no road
  // covers. Reports the SurfaceId with road/lane/junction invalid.
  for (const SurfaceMesh& surface : mesh.surfaces) {
    const std::vector<std::uint32_t>& indices = surface.mesh.indices;
    for (std::size_t i = 0; i + 2 < indices.size(); i += 3) {
      const auto t = intersect_triangle(ray,
                                        vertex(surface.mesh.positions, indices[i]),
                                        vertex(surface.mesh.positions, indices[i + 1]),
                                        vertex(surface.mesh.positions, indices[i + 2]));
      if (t && *t < best_t) {
        best_t = *t;
        best = PickHit{
            .surface = surface.surface,
            .position = {ray.origin[0] + (ray.direction[0] * *t),
                         ray.origin[1] + (ray.direction[1] * *t),
                         ray.origin[2] + (ray.direction[2] * *t)},
            .distance = *t,
        };
      }
    }
  }

  // Placed props, bounding-sphere tested and sharing best_t so a prop in front
  // of a road wins the pick. A generous whole-tree sphere makes trunks (thin)
  // as easy to grab as crowns.
  for (const ObjectInstance& instance : mesh.objects) {
    const props::PropModel* model = props::model(instance.model_id);
    if (model == nullptr) {
      continue;
    }
    // The sphere tracks the instance's rendered size (#335) — a prop scaled to
    // twice the model height must be twice as easy to grab, not half.
    const double half_height = model->height * 0.5 * instance.scale;
    const std::array<double, 3> center{
        instance.position[0], instance.position[1], instance.position[2] + half_height};
    const double radius = std::max(model->radius * instance.scale, half_height);
    const auto t = intersect_sphere(ray, center, radius);
    if (t && *t < best_t) {
      best_t = *t;
      best = PickHit{
          .road = instance.road,
          .lane = {},
          .object = instance.object,
          .position = {ray.origin[0] + (ray.direction[0] * *t),
                       ray.origin[1] + (ray.direction[1] * *t),
                       ray.origin[2] + (ray.direction[2] * *t)},
          .distance = *t,
      };
    }
  }

  // Placed signals, same bounding-sphere test as props (their models resolve
  // through props::model too) so a traffic light in front of a road wins.
  for (const SignalInstance& instance : mesh.signal_instances) {
    const props::PropModel* model = props::model(instance.model_id);
    if (model == nullptr) {
      continue;
    }
    const double half_height = model->height * 0.5;
    const std::array<double, 3> center{
        instance.position[0], instance.position[1], instance.position[2] + half_height};
    const double radius = std::max(model->radius, half_height);
    const auto t = intersect_sphere(ray, center, radius);
    if (t && *t < best_t) {
      best_t = *t;
      best = PickHit{
          .road = instance.road,
          .lane = {},
          .signal = instance.signal,
          .position = {ray.origin[0] + (ray.direction[0] * *t),
                       ray.origin[1] + (ray.direction[1] * *t),
                       ray.origin[2] + (ray.direction[2] * *t)},
          .distance = *t,
      };
    }
  }
  return best;
}

std::optional<WaypointHit> pick_waypoint(
    const RoadNetwork& network, std::span<const RoadId> roads, double x, double y, double radius) {
  std::optional<WaypointHit> best;
  double best_dist = radius;
  for (const RoadId road_id : roads) {
    const Road* road = network.road(road_id);
    if (road == nullptr) {
      continue;
    }
    const std::vector<Waypoint> nodes = edit::effective_waypoints(*road);
    for (std::size_t i = 0; i < nodes.size(); ++i) {
      const double dist = std::hypot(x - nodes[i].x, y - nodes[i].y);
      if (dist <= best_dist) {
        best_dist = dist;
        best = WaypointHit{.road = road_id, .index = i, .position = nodes[i]};
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

std::optional<StationCoord>
station_within(const ReferenceLine& line, double x, double y, double max_abs_t) {
  if (line.empty()) {
    return std::nullopt;
  }
  const StationCoord station = find_station(line, x, y);
  if (std::abs(station.t) > max_abs_t) {
    return std::nullopt;
  }
  return station;
}

std::optional<LaneBoundaryHit>
nearest_lane_boundary(const RoadNetwork& network, RoadId road, double s, double cursor_t) {
  // The boundary t-values come from the same kernel routine the mesher uses, so
  // a picked edge sits exactly on the drawn one.
  const std::vector<double> offsets = lane_boundary_offsets(network, road, s);
  if (offsets.empty()) {
    return std::nullopt;
  }
  const LaneSection* section = network.lane_section(section_at(network, road, s));
  if (section == nullptr) {
    return std::nullopt;
  }
  // `offsets` is built leftmost-first (left outer edges, the centre boundary,
  // then right edges), so the centre boundary sits at index = the lane count
  // left of the reference line.
  int nleft = 0;
  for (const LaneId lane_id : section->lanes) {
    const Lane* lane = network.lane(lane_id);
    if (lane != nullptr && lane->odr_id > 0) {
      ++nleft;
    }
  }

  std::size_t best = 0;
  double best_dist = std::abs(offsets[0] - cursor_t);
  for (std::size_t i = 1; i < offsets.size(); ++i) {
    const double d = std::abs(offsets[i] - cursor_t);
    if (d < best_dist) {
      best_dist = d;
      best = i;
    }
  }

  const auto centre = static_cast<std::size_t>(nleft);
  LaneBoundaryHit hit{.t = offsets[best]};
  if (best < centre) {
    // Left boundary at index i is the outer edge of lane +(nleft - i).
    hit.side = 1;
    hit.at_odr_id = nleft - static_cast<int>(best);
  } else if (best > centre) {
    // Right boundary at index i is the outer edge of lane -(i - nleft).
    hit.side = -1;
    hit.at_odr_id = -(static_cast<int>(best) - nleft);
  } else {
    // The centre line itself: carve on the cursor's side, innermost lane,
    // falling back to whichever side actually carries lanes.
    const bool has_left = nleft > 0;
    const bool has_right = offsets.size() > centre + 1;
    int side = cursor_t >= offsets[best] ? 1 : -1;
    if (side == 1 && !has_left) {
      side = -1;
    } else if (side == -1 && !has_right) {
      side = 1;
    }
    hit.side = side;
    hit.at_odr_id = side;
    hit.centre = true; // the picked boundary is the centre line itself (lane 0)
  }
  return hit;
}

std::array<double, 2> station_to_world(const ReferenceLine& line, double s, double t) {
  if (line.empty()) {
    return {0.0, 0.0};
  }
  const PathPoint pose = line.evaluate(s);
  // Left normal (-sin hdg, cos hdg); exact inverse of find_station's t.
  return {pose.x - (std::sin(pose.hdg) * t), pose.y + (std::cos(pose.hdg) * t)};
}

} // namespace roadmaker::editor
