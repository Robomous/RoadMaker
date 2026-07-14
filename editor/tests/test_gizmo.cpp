// Pure transform-gizmo math (A3): screen projection at constant pixel size,
// handle hit-testing, axis-constrained translation, and detented yaw. No Qt/GL.

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <numbers>

#include "render/renderer.hpp"
#include "viewport/camera.hpp"
#include "viewport/gizmo.hpp"

namespace roadmaker::editor {
namespace {

CameraMatrices centered_camera(double aspect) {
  OrbitCamera camera;
  camera.frame({0.0F, 0.0F, 0.0F}, 40.0F);
  camera.set_view(-1.5708F, 1.2F); // three-quarter view so all three axes read
  return camera.matrices(static_cast<float>(aspect));
}

// A hand-built gizmo for hit-test tests (no camera): origin at (100,100),
// X arm to the right, Y arm up-left, Z arm up, ring r=58, pad 16.
GizmoScreen fixed_gizmo() {
  GizmoScreen g;
  g.valid = true;
  g.origin = {100.0, 100.0};
  g.x_tip = {164.0, 100.0}; // +x screen-right
  g.y_tip = {100.0, 36.0};  // up
  g.z_tip = {60.0, 55.0};   // up-left
  g.ring_radius = 58.0;
  g.pad_half = 16.0;
  return g;
}

} // namespace

TEST(Gizmo, ScreenProjectsOriginAndConstantLengthArrows) {
  constexpr double kW = 800.0;
  constexpr double kH = 600.0;
  const GizmoScreen g = gizmo_screen(centered_camera(kW / kH), {0.0, 0.0, 0.0}, kW, kH);
  ASSERT_TRUE(g.valid);
  EXPECT_NEAR(g.origin[0], kW / 2.0, 2.0);
  EXPECT_NEAR(g.origin[1], kH / 2.0, 2.0);

  // Each visible arrow is exactly the configured pixel length from the origin.
  const GizmoSizes sizes;
  for (const auto& tip : {g.x_tip, g.y_tip, g.z_tip}) {
    const double len = std::hypot(tip[0] - g.origin[0], tip[1] - g.origin[1]);
    // Degenerate arms collapse to the origin (len 0); visible ones are axis_len.
    EXPECT_TRUE(len < 1e-6 || std::abs(len - sizes.axis_len) < 0.5) << "len=" << len;
  }
}

TEST(Gizmo, ScreenArrowsAreZoomInvariant) {
  constexpr double kW = 800.0;
  constexpr double kH = 600.0;
  // Two very different camera distances must yield the same on-screen arrow px.
  OrbitCamera near_cam;
  near_cam.frame({0.0F, 0.0F, 0.0F}, 15.0F);
  near_cam.set_view(-1.5708F, 1.2F);
  OrbitCamera far_cam;
  far_cam.frame({0.0F, 0.0F, 0.0F}, 120.0F);
  far_cam.set_view(-1.5708F, 1.2F);
  const GizmoScreen a =
      gizmo_screen(near_cam.matrices(static_cast<float>(kW / kH)), {0.0, 0.0, 0.0}, kW, kH);
  const GizmoScreen b =
      gizmo_screen(far_cam.matrices(static_cast<float>(kW / kH)), {0.0, 0.0, 0.0}, kW, kH);
  ASSERT_TRUE(a.valid && b.valid);
  const double la = std::hypot(a.x_tip[0] - a.origin[0], a.x_tip[1] - a.origin[1]);
  const double lb = std::hypot(b.x_tip[0] - b.origin[0], b.x_tip[1] - b.origin[1]);
  EXPECT_NEAR(la, lb, 0.5); // identical pixel length regardless of zoom
}

TEST(Gizmo, HitTestPicksTheNearestArmPadAndRing) {
  const GizmoScreen g = fixed_gizmo();
  EXPECT_EQ(gizmo_hit_test(g, {100.0, 100.0}), GizmoHandle::PlaneXY); // dead centre
  EXPECT_EQ(gizmo_hit_test(g, {140.0, 100.0}), GizmoHandle::AxisX);   // along +x arm
  EXPECT_EQ(gizmo_hit_test(g, {100.0, 60.0}), GizmoHandle::AxisY);    // along up arm
  EXPECT_EQ(gizmo_hit_test(g, {70.0, 63.0}), GizmoHandle::AxisZ);     // along up-left arm
  // Down-right on the ring (r=58 at ~-45°), clear of every arm (which point
  // right / up / up-left) — the ring wins there.
  EXPECT_EQ(gizmo_hit_test(g, {141.0, 141.0}), GizmoHandle::YawRing);
  EXPECT_EQ(gizmo_hit_test(g, {400.0, 400.0}), GizmoHandle::None); // empty space
}

TEST(Gizmo, HitTestOnAnInvalidGizmoIsNone) {
  GizmoScreen g;
  g.valid = false;
  EXPECT_EQ(gizmo_hit_test(g, {0.0, 0.0}), GizmoHandle::None);
}

TEST(Gizmo, ConstrainTranslationLocksToTheGrabbedAxis) {
  const std::array<double, 2> from{10.0, 20.0};
  const std::array<double, 2> to{17.0, 25.0}; // dx=7, dy=5
  EXPECT_EQ(gizmo_constrain_translation(GizmoHandle::AxisX, from, to),
            (std::array<double, 2>{7.0, 0.0}));
  EXPECT_EQ(gizmo_constrain_translation(GizmoHandle::AxisY, from, to),
            (std::array<double, 2>{0.0, 5.0}));
  EXPECT_EQ(gizmo_constrain_translation(GizmoHandle::PlaneXY, from, to),
            (std::array<double, 2>{7.0, 5.0}));
  // Z and ring carry no planar delta (handled by their own paths).
  EXPECT_EQ(gizmo_constrain_translation(GizmoHandle::AxisZ, from, to),
            (std::array<double, 2>{0.0, 0.0}));
  EXPECT_EQ(gizmo_constrain_translation(GizmoHandle::YawRing, from, to),
            (std::array<double, 2>{0.0, 0.0}));
}

TEST(Gizmo, YawAngleIsTheSignedWrappedDelta) {
  const std::array<double, 2> pivot{0.0, 0.0};
  // From +x axis (angle 0) to +y axis (angle pi/2): +90°.
  EXPECT_NEAR(gizmo_yaw_angle(pivot, {1.0, 0.0}, {0.0, 1.0}), std::numbers::pi / 2.0, 1e-9);
  // From +x to -y: -90° (wrapped, not +270°).
  EXPECT_NEAR(gizmo_yaw_angle(pivot, {1.0, 0.0}, {0.0, -1.0}), -std::numbers::pi / 2.0, 1e-9);
}

TEST(Gizmo, YawDetentSnapsToFifteenDegrees) {
  const std::array<double, 2> pivot{0.0, 0.0};
  constexpr double kDetent = std::numbers::pi / 12.0; // 15°
  // A drag of ~20° snaps to 15°; ~52° snaps to 45°.
  const double twenty = 20.0 * std::numbers::pi / 180.0;
  const double fifty_two = 52.0 * std::numbers::pi / 180.0;
  const std::array<double, 2> at20{std::cos(twenty), std::sin(twenty)};
  const std::array<double, 2> at52{std::cos(fifty_two), std::sin(fifty_two)};
  EXPECT_NEAR(gizmo_yaw_angle(pivot, {1.0, 0.0}, at20, kDetent), kDetent, 1e-9);
  EXPECT_NEAR(gizmo_yaw_angle(pivot, {1.0, 0.0}, at52, kDetent), 3.0 * kDetent, 1e-9);
  // Free (detent 0) keeps the exact angle.
  EXPECT_NEAR(gizmo_yaw_angle(pivot, {1.0, 0.0}, at20, 0.0), twenty, 1e-9);
}

} // namespace roadmaker::editor
