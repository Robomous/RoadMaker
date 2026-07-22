// Copyright 2026 Robomous
// SPDX-License-Identifier: Apache-2.0

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

// --- orthographic (P1/GW-1 step 11) ------------------------------------------
// The screen math is projection-agnostic (it goes through proj·view), so ortho
// needs no new code — only proof, since the gizmo and every overlay ride on it.

namespace {

CameraMatrices ortho_camera(double aspect) {
  OrbitCamera camera;
  camera.frame({0.0F, 0.0F, 0.0F}, 40.0F);
  camera.set_view(-1.5708F, 1.5508F);
  camera.set_projection(ProjectionMode::Orthographic);
  return camera.matrices(static_cast<float>(aspect));
}

} // namespace

TEST(Projection, OrthoKeepsTheOriginAtTheViewportCenter) {
  constexpr double kWidth = 800.0;
  constexpr double kHeight = 600.0;
  const auto screen =
      project_to_screen(ortho_camera(kWidth / kHeight), 0.0, 0.0, 0.0, kWidth, kHeight);
  ASSERT_TRUE(screen.has_value());
  EXPECT_NEAR((*screen)[0], kWidth / 2.0, 1.0);
  EXPECT_NEAR((*screen)[1], kHeight / 2.0, 1.0);
}

TEST(Projection, OrthoKeepsTheScreenXAxisPointingRight) {
  constexpr double kWidth = 800.0;
  constexpr double kHeight = 600.0;
  const CameraMatrices camera = ortho_camera(kWidth / kHeight);
  const auto left = project_to_screen(camera, -5.0, 0.0, 0.0, kWidth, kHeight);
  const auto right = project_to_screen(camera, 5.0, 0.0, 0.0, kWidth, kHeight);
  ASSERT_TRUE(left.has_value() && right.has_value());
  EXPECT_LT((*left)[0], (*right)[0]);
}

// The defining property of a parallel projection: depth must not change screen
// size. Two equal-length segments at different depths project identically.
TEST(Projection, OrthoDoesNotForeshorten) {
  constexpr double kWidth = 800.0;
  constexpr double kHeight = 600.0;
  const CameraMatrices camera = ortho_camera(kWidth / kHeight);

  const auto near_a = project_to_screen(camera, -5.0, 0.0, 0.0, kWidth, kHeight);
  const auto near_b = project_to_screen(camera, 5.0, 0.0, 0.0, kWidth, kHeight);
  const auto far_a = project_to_screen(camera, -5.0, 0.0, -20.0, kWidth, kHeight);
  const auto far_b = project_to_screen(camera, 5.0, 0.0, -20.0, kWidth, kHeight);
  ASSERT_TRUE(near_a && near_b && far_a && far_b);

  const double near_span = (*near_b)[0] - (*near_a)[0];
  const double far_span = (*far_b)[0] - (*far_a)[0];
  EXPECT_NEAR(near_span, far_span, 1e-6) << "ortho must not shrink with depth";

  // ...whereas perspective does, which is what makes this test meaningful.
  const CameraMatrices persp = centered_camera(kWidth / kHeight);
  const auto p_near_a = project_to_screen(persp, -5.0, 0.0, 0.0, kWidth, kHeight);
  const auto p_near_b = project_to_screen(persp, 5.0, 0.0, 0.0, kWidth, kHeight);
  const auto p_far_a = project_to_screen(persp, -5.0, 0.0, -20.0, kWidth, kHeight);
  const auto p_far_b = project_to_screen(persp, 5.0, 0.0, -20.0, kWidth, kHeight);
  ASSERT_TRUE(p_near_a && p_near_b && p_far_a && p_far_b);
  EXPECT_GT((*p_near_b)[0] - (*p_near_a)[0], (*p_far_b)[0] - (*p_far_a)[0]);
}

} // namespace
} // namespace roadmaker::editor
