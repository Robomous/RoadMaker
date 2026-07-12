#pragma once

#include <array>

#include "render/renderer.hpp"

namespace roadmaker::editor {

/// Orbit camera in the KERNEL frame: right-handed, Z-up, meters. The editor
/// renders kernel coordinates directly — no Y-up conversion here (that
/// happens only at the glTF boundary).
class OrbitCamera {
public:
  void orbit(float delta_yaw, float delta_pitch);
  void pan(float delta_x, float delta_y); // in view plane, scaled by distance
  void zoom(float scroll);                // exponential
  void frame(const std::array<float, 3>& center, float radius);

  /// Absolute orientation (screenshot presets); same pitch clamp as orbit().
  void set_view(float yaw, float pitch);

  [[nodiscard]] CameraMatrices matrices(float aspect) const;

  [[nodiscard]] float distance() const { return distance_; }

private:
  std::array<float, 3> target_{0.0F, 0.0F, 0.0F};
  float yaw_ = 0.8F;   // rad, around +Z
  float pitch_ = 0.9F; // rad above the XY plane, clamped
  float distance_ = 80.0F;
};

} // namespace roadmaker::editor
