#include "viewport/gizmo.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

#include "viewport/projection.hpp"

namespace roadmaker::editor {

namespace {

/// Distance from point `p` to the segment [a, b] (all logical px).
double
point_segment_distance(std::array<double, 2> p, std::array<double, 2> a, std::array<double, 2> b) {
  const double abx = b[0] - a[0];
  const double aby = b[1] - a[1];
  const double len_sq = (abx * abx) + (aby * aby);
  if (len_sq < 1e-9) {
    return std::hypot(p[0] - a[0], p[1] - a[1]);
  }
  double t = (((p[0] - a[0]) * abx) + ((p[1] - a[1]) * aby)) / len_sq;
  t = std::clamp(t, 0.0, 1.0);
  const double cx = a[0] + (t * abx);
  const double cy = a[1] + (t * aby);
  return std::hypot(p[0] - cx, p[1] - cy);
}

std::array<double, 2> axis_tip(const CameraMatrices& camera,
                               std::array<double, 3> pivot,
                               std::array<double, 3> axis,
                               std::array<double, 2> origin,
                               double width,
                               double height,
                               double len_px) {
  // Project pivot + 1 m along the world axis; the screen direction, renormalized
  // to a fixed pixel length, gives a zoom-independent arrow. Degenerate (axis
  // near-parallel to the view ray) collapses the tip onto the origin.
  const auto probe = project_to_screen(
      camera, pivot[0] + axis[0], pivot[1] + axis[1], pivot[2] + axis[2], width, height);
  if (!probe.has_value()) {
    return origin;
  }
  const double dx = (*probe)[0] - origin[0];
  const double dy = (*probe)[1] - origin[1];
  const double len = std::hypot(dx, dy);
  if (len < 1e-6) {
    return origin;
  }
  return {origin[0] + (dx / len * len_px), origin[1] + (dy / len * len_px)};
}

} // namespace

GizmoScreen gizmo_screen(const CameraMatrices& camera,
                         std::array<double, 3> pivot,
                         double width,
                         double height,
                         GizmoSizes sizes) {
  GizmoScreen gizmo;
  const auto origin = project_to_screen(camera, pivot[0], pivot[1], pivot[2], width, height);
  if (!origin.has_value()) {
    return gizmo; // behind the camera
  }
  gizmo.valid = true;
  gizmo.origin = *origin;
  gizmo.ring_radius = sizes.ring_radius;
  gizmo.pad_half = sizes.pad_half;
  gizmo.x_tip =
      axis_tip(camera, pivot, {1.0, 0.0, 0.0}, gizmo.origin, width, height, sizes.axis_len);
  gizmo.y_tip =
      axis_tip(camera, pivot, {0.0, 1.0, 0.0}, gizmo.origin, width, height, sizes.axis_len);
  gizmo.z_tip =
      axis_tip(camera, pivot, {0.0, 0.0, 1.0}, gizmo.origin, width, height, sizes.axis_len);
  return gizmo;
}

GizmoHandle gizmo_hit_test(const GizmoScreen& gizmo, std::array<double, 2> cursor, double tol) {
  if (!gizmo.valid) {
    return GizmoHandle::None;
  }
  // Centre pad (free planar move) wins near the origin.
  if (std::abs(cursor[0] - gizmo.origin[0]) <= gizmo.pad_half &&
      std::abs(cursor[1] - gizmo.origin[1]) <= gizmo.pad_half) {
    return GizmoHandle::PlaneXY;
  }
  // Then the nearest axis arm within tolerance.
  const std::array<std::pair<GizmoHandle, std::array<double, 2>>, 3> arms{{
      {GizmoHandle::AxisX, gizmo.x_tip},
      {GizmoHandle::AxisY, gizmo.y_tip},
      {GizmoHandle::AxisZ, gizmo.z_tip},
  }};
  GizmoHandle best = GizmoHandle::None;
  double best_dist = tol;
  for (const auto& [handle, tip] : arms) {
    // Skip a collapsed (degenerate) arm.
    if (std::hypot(tip[0] - gizmo.origin[0], tip[1] - gizmo.origin[1]) < 1e-6) {
      continue;
    }
    const double dist = point_segment_distance(cursor, gizmo.origin, tip);
    if (dist < best_dist) {
      best_dist = dist;
      best = handle;
    }
  }
  if (best != GizmoHandle::None) {
    return best;
  }
  // Finally the yaw ring.
  const double dr = std::abs(std::hypot(cursor[0] - gizmo.origin[0], cursor[1] - gizmo.origin[1]) -
                             gizmo.ring_radius);
  if (dr <= tol) {
    return GizmoHandle::YawRing;
  }
  return GizmoHandle::None;
}

std::array<double, 2> gizmo_constrain_translation(GizmoHandle handle,
                                                  std::array<double, 2> from,
                                                  std::array<double, 2> to) {
  const double dx = to[0] - from[0];
  const double dy = to[1] - from[1];
  switch (handle) {
  case GizmoHandle::AxisX:
    return {dx, 0.0};
  case GizmoHandle::AxisY:
    return {0.0, dy};
  case GizmoHandle::PlaneXY:
    return {dx, dy};
  default:
    return {0.0, 0.0};
  }
}

double gizmo_yaw_angle(std::array<double, 2> pivot,
                       std::array<double, 2> from,
                       std::array<double, 2> to,
                       double detent) {
  const double a0 = std::atan2(from[1] - pivot[1], from[0] - pivot[0]);
  const double a1 = std::atan2(to[1] - pivot[1], to[0] - pivot[0]);
  // Wrapped delta in (-pi, pi].
  double delta = std::atan2(std::sin(a1 - a0), std::cos(a1 - a0));
  if (detent > 0.0) {
    delta = std::round(delta / detent) * detent;
  }
  return delta;
}

} // namespace roadmaker::editor
