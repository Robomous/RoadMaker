#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <optional>

#include "viewport/camera.hpp"
#include "viewport/picking.hpp"

namespace roadmaker::editor {
namespace {

/// Rows of the view matrix's rotation block (column-major storage).
std::array<std::array<float, 3>, 3> rotation_rows(const CameraMatrices& m) {
  return {{{m.view[0], m.view[4], m.view[8]},
           {m.view[1], m.view[5], m.view[9]},
           {m.view[2], m.view[6], m.view[10]}}};
}

float dot(const std::array<float, 3>& a, const std::array<float, 3>& b) {
  return (a[0] * b[0]) + (a[1] * b[1]) + (a[2] * b[2]);
}

TEST(OrbitCamera, ViewRotationIsOrthonormal) {
  OrbitCamera camera;
  camera.orbit(0.3F, -0.2F);
  const auto rows = rotation_rows(camera.matrices(1.5F));
  for (std::size_t i = 0; i < 3; ++i) {
    EXPECT_NEAR(dot(rows[i], rows[i]), 1.0F, 1e-5F);
    for (std::size_t j = i + 1; j < 3; ++j) {
      EXPECT_NEAR(dot(rows[i], rows[j]), 0.0F, 1e-5F);
    }
  }
}

TEST(OrbitCamera, ViewPlacesTargetOnNegativeZAxis) {
  OrbitCamera camera; // target (0,0,0), distance 80
  const CameraMatrices m = camera.matrices(1.0F);
  // view * target(0,0,0,1) = translation column: (0, 0, -distance).
  EXPECT_NEAR(m.view[12], 0.0F, 1e-4F);
  EXPECT_NEAR(m.view[13], 0.0F, 1e-4F);
  EXPECT_NEAR(m.view[14], -camera.distance(), 1e-3F);
}

TEST(OrbitCamera, FrameSetsClampedDistance) {
  OrbitCamera camera;
  camera.frame({10.0F, 20.0F, 0.0F}, 100.0F);
  EXPECT_FLOAT_EQ(camera.distance(), 220.0F);

  camera.frame({0.0F, 0.0F, 0.0F}, 0.1F);
  EXPECT_FLOAT_EQ(camera.distance(), 2.0F); // min-distance clamp
}

TEST(OrbitCamera, ZoomIsExponentialAndClamped) {
  OrbitCamera camera;
  const float before = camera.distance();
  camera.zoom(1.0F);
  EXPECT_FLOAT_EQ(camera.distance(), before * 0.9F);
  for (int i = 0; i < 200; ++i) {
    camera.zoom(-1.0F);
  }
  EXPECT_FLOAT_EQ(camera.distance(), 5000.0F); // max-distance clamp
}

// Ground-anchored MMB pan: after grabbing the ground point under a pixel and
// applying (anchor − current) to the target, re-projecting that same pixel must
// land back on the grabbed world point — the "layer follows the cursor at 1:1"
// contract — across pitch, zoom, and yaw.
TEST(OrbitCamera, AnchoredPanPinsGrabbedPointUnderCursor) {
  constexpr double kW = 800.0;
  constexpr double kH = 600.0;
  const float aspect = static_cast<float>(kW / kH);

  for (const float yaw : {0.0F, 0.8F, 2.3F, -1.1F}) {
    for (const float pitch : {0.3F, 0.9F, 1.4F}) {
      for (const int zoom_steps : {-6, 0, 8}) {
        OrbitCamera camera;
        camera.set_view(yaw, pitch);
        camera.zoom(static_cast<float>(zoom_steps));

        // Grab the ground point under the press pixel.
        const auto anchor = ground_point(camera.matrices(aspect), 500.0, 380.0, kW, kH);
        ASSERT_TRUE(anchor.has_value());

        // Cursor moves; pin the anchor by shifting the target by (anchor − current).
        const auto current = ground_point(camera.matrices(aspect), 320.0, 250.0, kW, kH);
        ASSERT_TRUE(current.has_value());
        camera.move_target(static_cast<float>((*anchor)[0] - (*current)[0]),
                           static_cast<float>((*anchor)[1] - (*current)[1]));

        // The grabbed world point now sits under the new cursor pixel.
        const auto after = ground_point(camera.matrices(aspect), 320.0, 250.0, kW, kH);
        ASSERT_TRUE(after.has_value());
        EXPECT_NEAR((*after)[0], (*anchor)[0], 1e-2)
            << "yaw=" << yaw << " pitch=" << pitch << " zoom=" << zoom_steps;
        EXPECT_NEAR((*after)[1], (*anchor)[1], 1e-2)
            << "yaw=" << yaw << " pitch=" << pitch << " zoom=" << zoom_steps;
      }
    }
  }
}

// Fallback view-plane pan scale: a vertical drag shifts the target by exactly
// dy·2·distance·tan(fov/2)/height meters — the world size of a pixel at the
// target depth. Measured via the center-pixel ground hit, which equals the
// target (the camera looks at a z=0 target, so the center ray meets z=0 there).
TEST(OrbitCamera, PanPixelsUsesExactDepthScale) {
  constexpr double kW = 800.0;
  constexpr double kH = 600.0;
  constexpr double kFovY = 50.0 * 3.14159265358979 / 180.0;
  const float aspect = static_cast<float>(kW / kH);

  OrbitCamera camera;
  camera.set_view(0.0F, 0.9F); // yaw 0 → a +y drag maps onto −x cleanly

  const auto before = ground_point(camera.matrices(aspect), kW / 2.0, kH / 2.0, kW, kH);
  ASSERT_TRUE(before.has_value());

  constexpr float kDragPx = 100.0F;
  camera.pan_pixels(0.0F, kDragPx, static_cast<float>(kH));

  const auto after = ground_point(camera.matrices(aspect), kW / 2.0, kH / 2.0, kW, kH);
  ASSERT_TRUE(after.has_value());

  const double world_per_px = 2.0 * camera.distance() * std::tan(kFovY / 2.0) / kH;
  const double expected_dx = -static_cast<double>(kDragPx) * world_per_px;
  EXPECT_NEAR((*after)[0] - (*before)[0], expected_dx, 1e-2);
  EXPECT_NEAR((*after)[1] - (*before)[1], 0.0, 1e-3);

  // Farther out, the same drag moves the world more (depth-proportional scale).
  OrbitCamera far_cam;
  far_cam.set_view(0.0F, 0.9F);
  far_cam.zoom(-4.0F);
  ASSERT_GT(far_cam.distance(), camera.distance());
  const auto far_before = ground_point(far_cam.matrices(aspect), kW / 2.0, kH / 2.0, kW, kH);
  far_cam.pan_pixels(0.0F, kDragPx, static_cast<float>(kH));
  const auto far_after = ground_point(far_cam.matrices(aspect), kW / 2.0, kH / 2.0, kW, kH);
  ASSERT_TRUE(far_before.has_value() && far_after.has_value());
  EXPECT_GT(std::abs((*far_after)[0] - (*far_before)[0]), std::abs(expected_dx));
}

TEST(OrbitCamera, PitchStaysAboveGround) {
  OrbitCamera camera;
  camera.orbit(0.0F, -10.0F); // way below the clamp
  const CameraMatrices m = camera.matrices(1.0F);
  // Forward row is -row2; with pitch clamped positive the camera stays above
  // the target, so looking direction has a negative world-Z component.
  EXPECT_LT(-(-m.view[2] * 0.0F + -m.view[6] * 0.0F + -m.view[10]) * -1.0F, 0.0F);
}

} // namespace
} // namespace roadmaker::editor
