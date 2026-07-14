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
  void zoom(float scroll); // exponential
  void frame(const std::array<float, 3>& center, float radius);

  /// Shifts the look-at target in world x/y. The ground-anchored MMB pan pins
  /// a grabbed ground point under the cursor by feeding it (anchor − current).
  void move_target(float dx, float dy) {
    target_[0] += dx;
    target_[1] += dy;
  }

  /// Fallback view-plane pan for degenerate near-horizon rays (low pitch, no
  /// ground hit): shifts the target by a pixel delta scaled to exact world
  /// units at the target depth — 2·distance·tan(fov/2)/viewport_height per
  /// pixel — ground-projected with grab-world signs so content still tracks
  /// the cursor.
  void pan_pixels(float dx_pixels, float dy_pixels, float viewport_height);

  /// Absolute orientation (screenshot presets); same pitch clamp as orbit().
  void set_view(float yaw, float pitch);

  /// Absolute pose: target, orientation, and distance in one call — for a fixed
  /// golden-scene camera whose eye must land at an exact world point regardless
  /// of scene bounds (unlike frame(), which fits the distance to a radius).
  void set_pose(const std::array<float, 3>& target, float yaw, float pitch, float distance);

  [[nodiscard]] CameraMatrices matrices(float aspect) const;

  [[nodiscard]] float distance() const { return distance_; }

private:
  std::array<float, 3> target_{0.0F, 0.0F, 0.0F};
  float yaw_ = 0.8F;   // rad, around +Z
  float pitch_ = 0.9F; // rad above the XY plane, clamped
  float distance_ = 80.0F;
};

} // namespace roadmaker::editor
