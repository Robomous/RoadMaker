#pragma once

#include <array>

#include "render/renderer.hpp"

namespace roadmaker::editor {

/// How the camera projects (GW-1 step 11). Both modes share the orbit state —
/// see matrices() for why the O/P toggle cannot jump.
enum class ProjectionMode {
  Perspective,
  Orthographic,
};

/// A cardinal viewing direction (GW-1 steps 12-13). Named for where the camera
/// LOOKS FROM: North means standing north of the pivot, looking south.
enum class CardinalView {
  North,
  South,
  West,
  East,
  Top,
};

/// Orbit camera in the KERNEL frame: right-handed, Z-up, meters. The editor
/// renders kernel coordinates directly — no Y-up conversion here (that
/// happens only at the glTF boundary).
class OrbitCamera {
public:
  void orbit(float delta_yaw, float delta_pitch);

  /// Exponential dolly toward (positive) or away from (negative) the pivot.
  ///
  /// Zooming THROUGH the pivot pushes it forward rather than dead-stopping
  /// (GW-1 step 4): once the requested distance would fall below the minimum,
  /// the remainder moves the pivot along the view direction instead, so the
  /// eye keeps travelling at exactly the rate it was. Zooming out never
  /// relocates the pivot.
  void zoom(float scroll);

  /// Zoom keeping the world point under a viewport pixel pinned there.
  /// `anchor_ndc` is the cursor in normalized device coordinates ([-1,1], y
  /// up). Perspective already zooms toward the cursor's ray by dollying the
  /// eye, so this only shifts the pivot in ORTHOGRAPHIC mode — where a plain
  /// zoom would otherwise scale about the viewport centre and slide the
  /// content under the cursor away.
  void zoom_about(float scroll, const std::array<float, 2>& anchor_ndc, float aspect);

  void frame(const std::array<float, 3>& center, float radius);

  void set_projection(ProjectionMode mode) { projection_ = mode; }

  [[nodiscard]] ProjectionMode projection() const { return projection_; }

  /// Shifts the look-at target (the pivot point of interest). The
  /// ground-anchored pan pins a grabbed ground point under the cursor by
  /// feeding it (anchor − current) in x/y; `dz` raises or lowers the pivot.
  void move_target(float dx, float dy, float dz = 0.0F) {
    target_[0] += dx;
    target_[1] += dy;
    target_[2] += dz;
  }

  /// Raises (drag up) or lowers (drag down) the pivot by a pixel delta, scaled
  /// to exact world units at the target depth — the same depth scale
  /// pan_pixels() uses, so a pivot lift tracks the cursor at the same rate a
  /// pan does. Qt y grows downward, hence the sign flip.
  void elevate_target_pixels(float dy_pixels, float viewport_height);

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

  /// The pivot point of interest the camera orbits, zooms toward, and pans
  /// with.
  [[nodiscard]] std::array<float, 3> target() const { return target_; }

  [[nodiscard]] float yaw() const { return yaw_; }

  [[nodiscard]] float pitch() const { return pitch_; }

  /// Half the world height the orthographic frustum spans — the ortho "zoom".
  /// Derived from `distance` so it matches what the perspective frustum spans
  /// at the pivot depth, which is what makes the O/P toggle jump-free.
  [[nodiscard]] float ortho_half_height() const;

  /// Moves the pivot to `point`, keeping the viewing angle and zoom distance —
  /// frame-on-cursor (GW-1 step 10) is a pivot move, not a dolly.
  void look_at(const std::array<float, 3>& point) { target_ = point; }

  /// Below-vertical top-down pitch. Exactly π/2 would make the look-at basis
  /// degenerate (forward parallel to world up, so the right vector vanishes);
  /// this is indistinguishable on screen and keeps the basis well-defined.
  static constexpr float kTopDownPitch = 1.5607963F; // π/2 − 0.01

private:
  /// The pivot starts 1.5 m above the world origin — roughly eye height, so an
  /// empty scene orbits around a point in the world rather than a spot on the
  /// ground plane (GW-1 step 1).
  std::array<float, 3> target_{0.0F, 0.0F, 1.5F};
  float yaw_ = 0.8F;   // rad, around +Z
  float pitch_ = 0.9F; // rad above the XY plane, clamped
  float distance_ = 80.0F;
  ProjectionMode projection_ = ProjectionMode::Perspective;
};

} // namespace roadmaker::editor
