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
  OrbitCamera camera;
  const CameraMatrices m = camera.matrices(1.0F);
  // The pivot sits straight down the view axis at `distance`: view * target =
  // (0, 0, -distance). Column-major, so column i is m.view[4i .. 4i+3].
  const std::array<float, 3> t = camera.target();
  const float vx = (m.view[0] * t[0]) + (m.view[4] * t[1]) + (m.view[8] * t[2]) + m.view[12];
  const float vy = (m.view[1] * t[0]) + (m.view[5] * t[1]) + (m.view[9] * t[2]) + m.view[13];
  const float vz = (m.view[2] * t[0]) + (m.view[6] * t[1]) + (m.view[10] * t[2]) + m.view[14];
  EXPECT_NEAR(vx, 0.0F, 1e-4F);
  EXPECT_NEAR(vy, 0.0F, 1e-4F);
  EXPECT_NEAR(vz, -camera.distance(), 1e-3F);
}

// GW-1 step 1: an empty scene orbits a pivot 1.5 m above the origin, not a
// point on the ground plane.
TEST(OrbitCamera, DefaultPivotSitsAboveOrigin) {
  const OrbitCamera camera;
  EXPECT_FLOAT_EQ(camera.target()[0], 0.0F);
  EXPECT_FLOAT_EQ(camera.target()[1], 0.0F);
  EXPECT_FLOAT_EQ(camera.target()[2], 1.5F);
  // The eye rides the orbit sphere around that raised pivot.
  EXPECT_NEAR(camera.matrices(1.0F).eye[2], 1.5F + (80.0F * std::sin(0.9F)), 1e-3F);
}

// GW-1 step 6: ⌥+⇧+LMB+RMB lifts the pivot. The lift uses the SAME per-pixel
// depth scale as the fallback pan (2·distance·tan(fov/2)/height), so a pivot
// drag and a pan drag of the same length move the world by the same amount.
TEST(OrbitCamera, ElevateTargetUsesExactDepthScale) {
  constexpr float kH = 600.0F;
  constexpr double kFovY = 50.0 * 3.14159265358979 / 180.0;

  OrbitCamera camera;
  const float base_z = camera.target()[2];
  const double world_per_px = 2.0 * camera.distance() * std::tan(kFovY / 2.0) / kH;

  constexpr float kDragPx = 120.0F;
  camera.elevate_target_pixels(-kDragPx, kH); // drag up raises the pivot
  EXPECT_NEAR(camera.target()[2] - base_z, kDragPx * world_per_px, 1e-3);
  EXPECT_FLOAT_EQ(camera.target()[0], 0.0F) << "a pivot lift must not slide it in x/y";
  EXPECT_FLOAT_EQ(camera.target()[1], 0.0F);

  camera.elevate_target_pixels(kDragPx, kH); // and back down, symmetrically
  EXPECT_NEAR(camera.target()[2], base_z, 1e-3);

  // Depth-proportional, like the pan: farther out, the same drag lifts more.
  OrbitCamera far_cam;
  far_cam.zoom(-4.0F);
  ASSERT_GT(far_cam.distance(), camera.distance());
  far_cam.elevate_target_pixels(-kDragPx, kH);
  EXPECT_GT(far_cam.target()[2] - base_z, kDragPx * world_per_px);
}

// A raised pivot must not change the ground-anchored pan: the 1:1 contract
// depends on the orbit distance, not on how high the pivot floats. (Regression
// guard for the new default z and for the ⌥+⇧ lift feeding back into panning.)
TEST(OrbitCamera, AnchoredPanUnaffectedByRaisedPivot) {
  constexpr double kW = 800.0;
  constexpr double kH = 600.0;
  const float aspect = static_cast<float>(kW / kH);

  OrbitCamera ground_pivot;
  ground_pivot.set_view(0.7F, 0.8F);
  ground_pivot.move_target(0.0F, 0.0F, -1.5F); // pull the pivot back down to z=0
  ASSERT_FLOAT_EQ(ground_pivot.target()[2], 0.0F);

  OrbitCamera raised;
  raised.set_view(0.7F, 0.8F);
  raised.move_target(0.0F, 0.0F, 18.5F); // 20 m up
  ASSERT_FLOAT_EQ(raised.target()[2], 20.0F);
  ASSERT_FLOAT_EQ(raised.distance(), ground_pivot.distance());

  // Same drag, same world shift of the pivot in x/y.
  const auto before_ground = ground_pivot.target();
  const auto before_raised = raised.target();
  ground_pivot.pan_pixels(35.0F, -70.0F, static_cast<float>(kH));
  raised.pan_pixels(35.0F, -70.0F, static_cast<float>(kH));
  EXPECT_FLOAT_EQ(raised.target()[0] - before_raised[0],
                  ground_pivot.target()[0] - before_ground[0]);
  EXPECT_FLOAT_EQ(raised.target()[1] - before_raised[1],
                  ground_pivot.target()[1] - before_ground[1]);
  EXPECT_FLOAT_EQ(raised.target()[2], 20.0F) << "a pan must never change the pivot height";

  // And the anchored pan still pins its grabbed ground point at 1:1.
  const auto anchor = ground_point(raised.matrices(aspect), 500.0, 380.0, kW, kH);
  const auto current = ground_point(raised.matrices(aspect), 320.0, 250.0, kW, kH);
  ASSERT_TRUE(anchor.has_value() && current.has_value());
  raised.move_target(static_cast<float>((*anchor)[0] - (*current)[0]),
                     static_cast<float>((*anchor)[1] - (*current)[1]));
  const auto after = ground_point(raised.matrices(aspect), 320.0, 250.0, kW, kH);
  ASSERT_TRUE(after.has_value());
  EXPECT_NEAR((*after)[0], (*anchor)[0], 1e-2);
  EXPECT_NEAR((*after)[1], (*anchor)[1], 1e-2);
}

TEST(OrbitCamera, FrameSetsClampedDistance) {
  OrbitCamera camera;
  camera.frame({10.0F, 20.0F, 0.0F}, 100.0F);
  EXPECT_FLOAT_EQ(camera.distance(), 220.0F);

  camera.frame({0.0F, 0.0F, 0.0F}, 0.1F);
  EXPECT_FLOAT_EQ(camera.distance(), 2.0F); // min-distance clamp
}

TEST(OrbitCamera, SetPosePlacesTheEyeAtTheExactWorldPoint) {
  // The GS-1 golden camera: eye (−55, −55, 35) looking at the origin. set_pose
  // must reproduce that eye regardless of any prior framing, so the golden
  // baseline is scene-independent.
  constexpr float kPi = 3.14159265358979F;
  OrbitCamera camera;
  camera.frame({999.0F, 999.0F, 999.0F}, 500.0F); // prior framing must not leak
  camera.set_pose({0.0F, 0.0F, 0.0F}, -3.0F * kPi / 4.0F, 0.42294F, 85.294F);
  const CameraMatrices m = camera.matrices(16.0F / 9.0F);
  EXPECT_NEAR(m.eye[0], -55.0F, 0.05F);
  EXPECT_NEAR(m.eye[1], -55.0F, 0.05F);
  EXPECT_NEAR(m.eye[2], 35.0F, 0.05F);
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
