#include "viewport/camera.hpp"

#include <algorithm>
#include <cmath>

namespace roadmaker::editor {

namespace {

constexpr float kPi = 3.14159265358979F;
constexpr float kMinDistance = 2.0F;
constexpr float kMaxDistance = 5000.0F;

} // namespace

void OrbitCamera::orbit(float delta_yaw, float delta_pitch) {
  yaw_ += delta_yaw;
  pitch_ = std::clamp(pitch_ + delta_pitch, 0.05F, kPi / 2.0F - 0.01F);
}

void OrbitCamera::set_view(float yaw, float pitch) {
  yaw_ = yaw;
  pitch_ = std::clamp(pitch, 0.05F, kPi / 2.0F - 0.01F);
}

void OrbitCamera::pan_pixels(float dx_pixels, float dy_pixels, float viewport_height) {
  // Exact per-pixel world scale at the target depth. fov_y matches matrices().
  const float fov_y = 50.0F * kPi / 180.0F;
  const float world_per_px =
      2.0F * distance_ * std::tan(fov_y / 2.0F) / std::max(viewport_height, 1.0F);
  const float sin_yaw = std::sin(yaw_);
  const float cos_yaw = std::cos(yaw_);
  // Ground-projected camera axes: right = (-sin, cos), screen-up = (-cos, -sin).
  // Grab-world signs: the target moves opposite the cursor so content tracks it
  // (Qt y grows downward).
  target_[0] += world_per_px * ((dx_pixels * sin_yaw) - (dy_pixels * cos_yaw));
  target_[1] += world_per_px * ((-dx_pixels * cos_yaw) - (dy_pixels * sin_yaw));
}

void OrbitCamera::zoom(float scroll) {
  distance_ = std::clamp(distance_ * std::pow(0.9F, scroll), kMinDistance, kMaxDistance);
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

  // Perspective projection, 50 deg vertical FOV.
  const float fov_y = 50.0F * kPi / 180.0F;
  const float tan_half = std::tan(fov_y / 2.0F);
  const float near_plane = 0.1F;
  const float far_plane = 10000.0F;
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
