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

#pragma once

// Screen-space transform-gizmo math: project a world pivot to a constant-size
// on-screen gizmo (three axis arrows, a yaw ring, an XY plane pad), hit-test the
// cursor against its handles, and constrain a drag to the grabbed handle. Pure
// (no Qt/GL) so it is unit-testable headless; the viewport renders the result
// and drives the edit through Document's preview session. Kernel frame:
// right-handed, Z-up, meters (arrows: X red, Y green, Z blue).

#include <array>

#include "render/renderer.hpp" // CameraMatrices

namespace roadmaker::editor {

enum class GizmoHandle { None, AxisX, AxisY, AxisZ, PlaneXY, YawRing };

/// The gizmo projected to widget-LOGICAL pixels (QPainter convention, top-left
/// origin). Arrow tips are a constant pixel distance from the origin regardless
/// of zoom (the projected world-axis direction, renormalized). `valid` is false
/// when the pivot is at/behind the camera.
struct GizmoScreen {
  bool valid = false;
  std::array<double, 2> origin{};
  std::array<double, 2> x_tip{};
  std::array<double, 2> y_tip{};
  std::array<double, 2> z_tip{};
  double ring_radius = 0.0;
  double pad_half = 0.0;
};

/// Constant on-screen sizes [logical px].
struct GizmoSizes {
  double axis_len = 64.0;
  double ring_radius = 58.0;
  double pad_half = 16.0;
};

/// Projects the gizmo at world `pivot` to screen. Arrow directions follow the
/// world axes as seen on screen; lengths are fixed in pixels (zoom-independent).
[[nodiscard]] GizmoScreen gizmo_screen(const CameraMatrices& camera,
                                       std::array<double, 3> pivot,
                                       double width,
                                       double height,
                                       GizmoSizes sizes = {});

/// Nearest handle under `cursor` (logical px) within `tol` px, or None. The
/// centre pad and the axis arms take priority over the ring.
[[nodiscard]] GizmoHandle
gizmo_hit_test(const GizmoScreen& gizmo, std::array<double, 2> cursor, double tol = 8.0);

/// Constrains a world-plane drag delta (from→to on the z=0 plane) to `handle`:
/// AxisX → (dx, 0), AxisY → (0, dy), PlaneXY → (dx, dy); AxisZ / YawRing / None
/// → (0, 0) (Z and rotation are handled by their own paths).
[[nodiscard]] std::array<double, 2> gizmo_constrain_translation(GizmoHandle handle,
                                                                std::array<double, 2> from,
                                                                std::array<double, 2> to);

/// Signed yaw angle [rad] for a ring drag about `pivot` (xy): the wrapped angle
/// from (from−pivot) to (to−pivot). When `detent` > 0 the result snaps to the
/// nearest multiple of `detent` (e.g. 15° = π/12); pass 0 for a free rotation.
[[nodiscard]] double gizmo_yaw_angle(std::array<double, 2> pivot,
                                     std::array<double, 2> from,
                                     std::array<double, 2> to,
                                     double detent = 0.0);

} // namespace roadmaker::editor
