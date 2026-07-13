#include <gtest/gtest.h>

#include <array>

#include "render/renderer.hpp"
#include "viewport/camera.hpp"
#include "viewport/projection.hpp"

namespace roadmaker::editor {
namespace {

// A camera framed on the origin: the target projects to the viewport center,
// and points offset in +x land to its right (screen +x).
CameraMatrices centered_camera(double aspect) {
  OrbitCamera camera;
  camera.frame({0.0F, 0.0F, 0.0F}, 40.0F);
  camera.set_view(-1.5708F, 1.5508F); // near-top-down, north up
  return camera.matrices(static_cast<float>(aspect));
}

TEST(Projection, OriginProjectsNearViewportCenter) {
  constexpr double kWidth = 800.0;
  constexpr double kHeight = 600.0;
  const auto screen =
      project_to_screen(centered_camera(kWidth / kHeight), 0.0, 0.0, 0.0, kWidth, kHeight);
  ASSERT_TRUE(screen.has_value());
  EXPECT_NEAR((*screen)[0], kWidth / 2.0, 1.0);
  EXPECT_NEAR((*screen)[1], kHeight / 2.0, 1.0);
}

TEST(Projection, ScreenXIncreasesWithWorldX) {
  constexpr double kWidth = 800.0;
  constexpr double kHeight = 600.0;
  const CameraMatrices camera = centered_camera(kWidth / kHeight);
  const auto left = project_to_screen(camera, -5.0, 0.0, 0.0, kWidth, kHeight);
  const auto right = project_to_screen(camera, 5.0, 0.0, 0.0, kWidth, kHeight);
  ASSERT_TRUE(left.has_value());
  ASSERT_TRUE(right.has_value());
  EXPECT_LT((*left)[0], (*right)[0]); // +x world → +x screen (right)
}

TEST(Projection, StaysWithinBoundsForOnScreenPoints) {
  constexpr double kWidth = 800.0;
  constexpr double kHeight = 600.0;
  const auto screen =
      project_to_screen(centered_camera(kWidth / kHeight), 3.0, -3.0, 0.0, kWidth, kHeight);
  ASSERT_TRUE(screen.has_value());
  EXPECT_GT((*screen)[0], 0.0);
  EXPECT_LT((*screen)[0], kWidth);
  EXPECT_GT((*screen)[1], 0.0);
  EXPECT_LT((*screen)[1], kHeight);
}

} // namespace
} // namespace roadmaker::editor
