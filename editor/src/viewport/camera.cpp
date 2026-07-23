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

#include "viewport/camera.hpp"

#include <algorithm>
#include <cmath>

namespace roadmaker::editor {

namespace {

constexpr float kPi = 3.14159265358979F;
constexpr float kMinDistance = 2.0F;
constexpr float kMaxDistance = 5000.0F;

/// Vertical field of view. The ONE definition: matrices() builds the
/// projection from it and pan_pixels()/elevate_target_pixels() derive the
/// per-pixel world scale from it, so they can never drift apart.
constexpr float kFovY = 50.0F * kPi / 180.0F;

/// World units per pixel at the target depth: the world height the viewport
/// spans at `distance`, divided by its pixel height.
float world_per_pixel(float distance, float viewport_height) {
  return 2.0F * distance * std::tan(kFovY / 2.0F) / std::max(viewport_height, 1.0F);
}

} // namespace

void OrbitCamera::orbit(float delta_yaw, float delta_pitch) {
  yaw_ += delta_yaw;
  pitch_ = std::clamp(pitch_ + delta_pitch, 0.05F, kPi / 2.0F - 0.01F);
}

void OrbitCamera::set_view(float yaw, float pitch) {
  yaw_ = yaw;
  pitch_ = std::clamp(pitch, 0.05F, kPi / 2.0F - 0.01F);
}

void OrbitCamera::set_pose(const std::array<float, 3>& target,
                           float yaw,
                           float pitch,
                           float distance) {
  target_ = target;
  distance_ = std::clamp(distance, kMinDistance, kMaxDistance);
  set_view(yaw, pitch);
}

void OrbitCamera::pan_pixels(float dx_pixels, float dy_pixels, float viewport_height) {
  const float world_per_px = world_per_pixel(distance_, viewport_height);
  const float sin_yaw = std::sin(yaw_);
  const float cos_yaw = std::cos(yaw_);
  // Ground-projected camera axes: right = (-sin, cos), screen-up = (-cos, -sin).
  // Grab-world signs: the target moves opposite the cursor so content tracks it
  // (Qt y grows downward).
  target_[0] += world_per_px * ((dx_pixels * sin_yaw) - (dy_pixels * cos_yaw));
  target_[1] += world_per_px * ((-dx_pixels * cos_yaw) - (dy_pixels * sin_yaw));
}

void OrbitCamera::elevate_target_pixels(float dy_pixels, float viewport_height) {
  // Drag up (negative dy in Qt's downward y) raises the pivot.
  target_[2] -= dy_pixels * world_per_pixel(distance_, viewport_height);
}

namespace {

/// Unit vector from the eye toward the target (the view direction), Z-up.
std::array<float, 3> forward_of(float yaw, float pitch) {
  const float cos_pitch = std::cos(pitch);
  // The eye sits at target + distance·(cos·cos, cos·sin, sin), so forward — the
  // direction the camera looks — is the negation of that offset direction.
  return {-cos_pitch * std::cos(yaw), -cos_pitch * std::sin(yaw), -std::sin(pitch)};
}

} // namespace

void OrbitCamera::zoom(float scroll) {
  const float desired = distance_ * std::pow(0.9F, scroll);
  if (desired >= kMinDistance) {
    distance_ = std::min(desired, kMaxDistance);
    return;
  }
  // Push past the pivot (GW-1 step 4). The dolly wants to close `distance_ −
  // desired` metres but only `distance_ − kMinDistance` are available, so the
  // pivot absorbs the rest by sliding forward along the view direction. The eye
  // ends up exactly where an unclamped dolly would have put it, which is what
  // makes the travel continuous rather than a dead stop.
  const std::array<float, 3> forward = forward_of(yaw_, pitch_);
  const float overshoot = kMinDistance - desired;
  for (std::size_t i = 0; i < 3; ++i) {
    target_[i] += forward[i] * overshoot;
  }
  distance_ = kMinDistance;
}

void OrbitCamera::zoom_about(float scroll,
                             const std::array<float, 2>& anchor_ndc,
                             float aspect,
                             const std::optional<std::array<float, 3>>& world_anchor) {
  if (projection_ == ProjectionMode::Perspective) {
    if (!world_anchor.has_value()) {
      // No world point under the cursor (near-horizon ray): a plain dolly along
      // the view axis, same as the chord zoom.
      zoom(scroll);
      return;
    }
    // Map-style cursor pin (#358): move the eye ALONG the cursor ray toward the
    // anchor by the zoom factor. eye1 = factor·eye0 + (1−factor)·anchor stays
    // collinear with (eye0, anchor), so the anchor keeps the same view-space
    // direction and therefore the same pixel; yaw/pitch are unchanged so the
    // view axis is fixed. Push-past falls out for free — as the factor shrinks
    // the eye keeps sliding toward the anchor even after the pivot distance
    // bottoms out at the minimum. Unlike the ortho branch this needs no NDC:
    // the ray through the resolved world point carries the depth NDC lacks.
    const float factor = std::pow(0.9F, scroll);
    const float cos_pitch = std::cos(pitch_);
    // Offset from target to eye (matrices() uses the same expression).
    const std::array<float, 3> to_eye{
        cos_pitch * std::cos(yaw_), cos_pitch * std::sin(yaw_), std::sin(pitch_)};
    const std::array<float, 3>& w = *world_anchor;
    const float new_distance = std::clamp(distance_ * factor, kMinDistance, kMaxDistance);
    for (std::size_t i = 0; i < 3; ++i) {
      const float eye0 = target_[i] + (distance_ * to_eye[i]);
      const float eye1 = (factor * eye0) + ((1.0F - factor) * w[i]);
      // Pivot sits new_distance in front of the new eye along the (fixed) view
      // axis: target = eye1 + new_distance·forward = eye1 − new_distance·to_eye.
      target_[i] = eye1 - (new_distance * to_eye[i]);
    }
    distance_ = new_distance;
    return;
  }
  // Orthographic: scale is half_height (∝ distance), and the projection is
  // centred on the pivot, so a plain zoom would slide the anchor away from the
  // cursor. Shift the pivot across the view plane by the anchor's change in
  // world offset, which pins it exactly.
  const float half_height_before = ortho_half_height();
  zoom(scroll);
  const float delta_half_height = ortho_half_height() - half_height_before;

  // View-plane basis: right and up in world space, from the look-at basis.
  const float sin_yaw = std::sin(yaw_);
  const float cos_yaw = std::cos(yaw_);
  const float sin_pitch = std::sin(pitch_);
  const float cos_pitch = std::cos(pitch_);
  const std::array<float, 3> right{-sin_yaw, cos_yaw, 0.0F};
  const std::array<float, 3> up{-sin_pitch * cos_yaw, -sin_pitch * sin_yaw, cos_pitch};

  // The anchor sat at (ndc.x·half_width, ndc.y·half_height) from the pivot on
  // the view plane; after the scale change it would sit at the same NDC but a
  // different world offset. Move the pivot by the difference (negated) so the
  // world point stays put.
  const float dx = -anchor_ndc[0] * delta_half_height * aspect;
  const float dy = -anchor_ndc[1] * delta_half_height;
  for (std::size_t i = 0; i < 3; ++i) {
    target_[i] += (right[i] * dx) + (up[i] * dy);
  }
}

float OrbitCamera::ortho_half_height() const {
  // Match the perspective frustum's world height AT THE PIVOT DEPTH. That is
  // what makes the O/P toggle jump-free for free: at the pivot plane both
  // projections span exactly the same world extent, so framed content keeps its
  // size. It also means zoom() needs no mode-specific branch — distance IS the
  // ortho scale.
  return distance_ * std::tan(kFovY / 2.0F);
}

void OrbitCamera::frame(const std::array<float, 3>& center, float radius) {
  target_ = center;
  distance_ = std::clamp(radius * 2.2F, kMinDistance, kMaxDistance);
}

CameraMatrices OrbitCamera::matrices(float aspect) const {
  // Eye position on the orbit sphere (Z-up).
  const float cos_pitch = std::cos(pitch_);
  const std::array<float, 3> eye{
      target_[0] + (distance_ * cos_pitch * std::cos(yaw_)),
      target_[1] + (distance_ * cos_pitch * std::sin(yaw_)),
      target_[2] + (distance_ * std::sin(pitch_)),
  };

  // Look-at basis: forward f, right r, up u (all normalized).
  std::array<float, 3> f{target_[0] - eye[0], target_[1] - eye[1], target_[2] - eye[2]};
  const float f_len = std::sqrt((f[0] * f[0]) + (f[1] * f[1]) + (f[2] * f[2]));
  for (float& c : f) {
    c /= f_len;
  }
  const std::array<float, 3> world_up{0.0F, 0.0F, 1.0F};
  std::array<float, 3> r{(f[1] * world_up[2]) - (f[2] * world_up[1]),
                         (f[2] * world_up[0]) - (f[0] * world_up[2]),
                         (f[0] * world_up[1]) - (f[1] * world_up[0])};
  const float r_len = std::sqrt((r[0] * r[0]) + (r[1] * r[1]) + (r[2] * r[2]));
  for (float& c : r) {
    c /= r_len;
  }
  const std::array<float, 3> u{
      (r[1] * f[2]) - (r[2] * f[1]), (r[2] * f[0]) - (r[0] * f[2]), (r[0] * f[1]) - (r[1] * f[0])};

  CameraMatrices out;
  out.eye = eye;
  // Column-major view matrix (world -> camera).
  out.view = {r[0],
              u[0],
              -f[0],
              0.0F,
              r[1],
              u[1],
              -f[1],
              0.0F,
              r[2],
              u[2],
              -f[2],
              0.0F,
              -((r[0] * eye[0]) + (r[1] * eye[1]) + (r[2] * eye[2])),
              -((u[0] * eye[0]) + (u[1] * eye[1]) + (u[2] * eye[2])),
              ((f[0] * eye[0]) + (f[1] * eye[1]) + (f[2] * eye[2])),
              1.0F};

  const float near_plane = 0.1F;
  const float far_plane = 10000.0F;
  if (projection_ == ProjectionMode::Orthographic) {
    // Symmetric ortho box around the view axis. The near plane is pulled BEHIND
    // the eye (−far) so geometry between the eye and the pivot — everything a
    // perspective view would show — is not clipped away when the pivot sits
    // inside the scene.
    const float half_h = ortho_half_height();
    const float half_w = half_h * aspect;
    out.projection = {1.0F / half_w,
                      0.0F,
                      0.0F,
                      0.0F,
                      0.0F,
                      1.0F / half_h,
                      0.0F,
                      0.0F,
                      0.0F,
                      0.0F,
                      -2.0F / (far_plane - (-far_plane)),
                      0.0F,
                      0.0F,
                      0.0F,
                      -(far_plane + (-far_plane)) / (far_plane - (-far_plane)),
                      1.0F};
    return out;
  }

  // Perspective projection (kFovY vertical FOV).
  const float tan_half = std::tan(kFovY / 2.0F);
  out.projection = {1.0F / (aspect * tan_half),
                    0.0F,
                    0.0F,
                    0.0F,
                    0.0F,
                    1.0F / tan_half,
                    0.0F,
                    0.0F,
                    0.0F,
                    0.0F,
                    -(far_plane + near_plane) / (far_plane - near_plane),
                    -1.0F,
                    0.0F,
                    0.0F,
                    -(2.0F * far_plane * near_plane) / (far_plane - near_plane),
                    0.0F};
  return out;
}

} // namespace roadmaker::editor
