#include <gtest/gtest.h>

#include <array>
#include <cmath>

#include "viewport/camera.hpp"

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
