/*
 * Copyright 2026 Robomous
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

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
  // The diagonal showcase camera: eye (−55, −55, 35) looking at the origin.
  // set_pose must reproduce that eye regardless of any prior framing, so the
  // baseline render pose is scene-independent.
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

// --- push-past-pivot zoom (GW-1 step 4) --------------------------------------

namespace {

/// Distance from the camera eye to a fixed world point — the thing that must
/// keep changing smoothly as the zoom pushes through the pivot.
float eye_distance_to(const OrbitCamera& camera, const std::array<float, 3>& point) {
  const auto eye = camera.matrices(1.0F).eye;
  const float dx = eye[0] - point[0];
  const float dy = eye[1] - point[1];
  const float dz = eye[2] - point[2];
  return std::sqrt((dx * dx) + (dy * dy) + (dz * dz));
}

} // namespace

// The headline behaviour: zooming in past the pivot must not stall. Before P1
// the distance clamped at kMinDistance and the eye simply stopped.
TEST(OrbitCamera, ZoomDoesNotStallAtThePivot) {
  OrbitCamera camera;
  const std::array<float, 3> landmark = camera.target();

  float previous = eye_distance_to(camera, landmark);
  for (int i = 0; i < 200; ++i) {
    camera.zoom(1.0F);
    const float now = eye_distance_to(camera, landmark);
    // Either still approaching the landmark, or already past it and receding —
    // what must never happen is the eye sitting still.
    if (i > 60) {
      EXPECT_NE(now, previous) << "the eye stopped moving at step " << i;
    }
    previous = now;
  }
  // 200 steps of ×0.9 from 80 m is astronomically far past the pivot: the eye
  // must have gone through and out the other side.
  const auto eye = camera.matrices(1.0F).eye;
  EXPECT_GT(eye_distance_to(camera, landmark), 1.0F) << "eye " << eye[0] << "," << eye[1];
}

TEST(OrbitCamera, ZoomNeverPutsDistanceBelowTheMinimum) {
  OrbitCamera camera;
  for (int i = 0; i < 100; ++i) {
    camera.zoom(1.0F);
    EXPECT_GE(camera.distance(), 2.0F);
  }
}

// Push-past moves the pivot ALONG THE VIEW DIRECTION only: it must never drift
// sideways, or zooming in would slew the view off its subject.
TEST(OrbitCamera, PushPastMovesTheTargetAlongTheViewAxisOnly) {
  OrbitCamera camera;
  camera.set_view(0.7F, 0.6F);
  // 0.9^40 · 80 m ≈ 0.24 m — well under the 2 m minimum, so the next zoom is
  // squarely in the push-past regime. (0.9^20 only reaches ~9.7 m: still a
  // plain dolly.)
  camera.zoom(40.0F);
  ASSERT_FLOAT_EQ(camera.distance(), 2.0F);
  const std::array<float, 3> before = camera.target();
  const auto eye_before = camera.matrices(1.0F).eye;

  camera.zoom(1.0F);
  const std::array<float, 3> after = camera.target();

  // The step vector must be parallel to the (unchanged) view direction.
  const std::array<float, 3> step{after[0] - before[0], after[1] - before[1], after[2] - before[2]};
  const std::array<float, 3> forward{
      before[0] - eye_before[0], before[1] - eye_before[1], before[2] - eye_before[2]};
  const float step_len = std::sqrt((step[0] * step[0]) + (step[1] * step[1]) + (step[2] * step[2]));
  const float fwd_len =
      std::sqrt((forward[0] * forward[0]) + (forward[1] * forward[1]) + (forward[2] * forward[2]));
  ASSERT_GT(step_len, 1e-6F) << "the pivot must move once past the minimum";
  const float cos_angle =
      ((step[0] * forward[0]) + (step[1] * forward[1]) + (step[2] * forward[2])) /
      (step_len * fwd_len);
  EXPECT_NEAR(cos_angle, 1.0F, 1e-4F) << "the pivot slid off the view axis";
}

// Continuity across the boundary: the step the eye takes as it crosses into
// push-past must match the step a plain dolly would have taken. A discontinuity
// here is exactly the "dead stop then jump" the old clamp produced.
TEST(OrbitCamera, EyeTravelIsContinuousAcrossThePushPastBoundary) {
  OrbitCamera camera;
  camera.set_view(0.0F, 0.5F);
  // Walk down to just above the minimum distance.
  while (camera.distance() > 2.0F / 0.9F) {
    camera.zoom(1.0F);
  }
  ASSERT_GT(camera.distance(), 2.0F);
  const float step = camera.distance() * (1.0F - 0.9F); // what this zoom should close
  const auto eye_before = camera.matrices(1.0F).eye;

  camera.zoom(1.0F); // crosses the boundary
  const auto eye_after = camera.matrices(1.0F).eye;
  const float travelled = std::sqrt(std::pow(eye_after[0] - eye_before[0], 2.0F) +
                                    std::pow(eye_after[1] - eye_before[1], 2.0F) +
                                    std::pow(eye_after[2] - eye_before[2], 2.0F));
  EXPECT_NEAR(travelled, step, 1e-3F);
}

TEST(OrbitCamera, ZoomingOutNeverRelocatesThePivot) {
  OrbitCamera camera;
  camera.zoom(30.0F); // push past first, so the pivot has moved
  const std::array<float, 3> pushed = camera.target();

  for (int i = 0; i < 50; ++i) {
    camera.zoom(-1.0F);
    EXPECT_FLOAT_EQ(camera.target()[0], pushed[0]);
    EXPECT_FLOAT_EQ(camera.target()[1], pushed[1]);
    EXPECT_FLOAT_EQ(camera.target()[2], pushed[2]);
  }
}

// --- projection (GW-1 step 11) -----------------------------------------------

TEST(OrbitCamera, DefaultsToPerspective) {
  EXPECT_EQ(OrbitCamera{}.projection(), ProjectionMode::Perspective);
}

// The no-jump O/P toggle: at the pivot depth both projections span the same
// world height, which is what "no jump in the framed content" means.
TEST(OrbitCamera, OrthoTogglePreservesPivotPlaneScale) {
  constexpr double kFovY = 50.0 * 3.14159265358979 / 180.0;
  OrbitCamera camera;
  camera.frame({10.0F, -4.0F, 0.0F}, 30.0F);

  const float perspective_half_height =
      camera.distance() * static_cast<float>(std::tan(kFovY / 2.0));
  camera.set_projection(ProjectionMode::Orthographic);
  EXPECT_NEAR(camera.ortho_half_height(), perspective_half_height, 1e-3F);

  // The toggle changes only the projection: pose and zoom are untouched.
  const auto target = camera.target();
  const float distance = camera.distance();
  camera.set_projection(ProjectionMode::Perspective);
  EXPECT_FLOAT_EQ(camera.distance(), distance);
  EXPECT_FLOAT_EQ(camera.target()[0], target[0]);
}

TEST(OrbitCamera, OrthoProjectionIsParallelAndSpansTheHalfHeight) {
  OrbitCamera camera;
  camera.set_projection(ProjectionMode::Orthographic);
  const CameraMatrices m = camera.matrices(2.0F);
  // Column-major: an orthographic matrix has no perspective divide.
  EXPECT_FLOAT_EQ(m.projection[11], 0.0F) << "w must not depend on view z";
  EXPECT_FLOAT_EQ(m.projection[15], 1.0F);
  EXPECT_NEAR(m.projection[5], 1.0F / camera.ortho_half_height(), 1e-6F);
  EXPECT_NEAR(m.projection[0], 1.0F / (camera.ortho_half_height() * 2.0F), 1e-6F);
}

// Ortho zoom must pin the point under the cursor, or scrolling would slide the
// scene out from under it (perspective gets this free from the eye dolly).
TEST(OrbitCamera, OrthoZoomAboutPinsTheAnchorPoint) {
  constexpr double kW = 800.0;
  constexpr double kH = 600.0;
  const float aspect = static_cast<float>(kW / kH);
  constexpr double kPixelX = 620.0;
  constexpr double kPixelY = 180.0;

  OrbitCamera camera;
  camera.set_view(0.0F, OrbitCamera::kTopDownPitch); // plan view: ground == view plane
  camera.set_projection(ProjectionMode::Orthographic);

  const auto before = ground_point(camera.matrices(aspect), kPixelX, kPixelY, kW, kH);
  ASSERT_TRUE(before.has_value());

  const std::array<float, 2> anchor_ndc{
      static_cast<float>((2.0 * kPixelX / kW) - 1.0),
      static_cast<float>(1.0 - (2.0 * kPixelY / kH)),
  };
  camera.zoom_about(3.0F, anchor_ndc, aspect);

  const auto after = ground_point(camera.matrices(aspect), kPixelX, kPixelY, kW, kH);
  ASSERT_TRUE(after.has_value());
  EXPECT_NEAR((*after)[0], (*before)[0], 1e-2);
  EXPECT_NEAR((*after)[1], (*before)[1], 1e-2);
}

// Perspective must ALSO pin the world point under the cursor (#358), not just
// dolly along the pivot ray. Mirror of the ortho pin: resolve the ground point
// under an off-centre pixel, zoom about it, and it must stay under that pixel.
TEST(OrbitCamera, PerspectiveZoomAboutPinsTheAnchorPoint) {
  constexpr double kW = 800.0;
  constexpr double kH = 600.0;
  const float aspect = static_cast<float>(kW / kH);
  constexpr double kPixelX = 560.0; // off-centre, so a plain dolly would slide it
  constexpr double kPixelY = 250.0;

  OrbitCamera camera; // default perspective pose looks down at the ground plane
  const auto before = ground_point(camera.matrices(aspect), kPixelX, kPixelY, kW, kH);
  ASSERT_TRUE(before.has_value());

  const std::array<float, 2> anchor_ndc{
      static_cast<float>((2.0 * kPixelX / kW) - 1.0),
      static_cast<float>(1.0 - (2.0 * kPixelY / kH)),
  };
  const std::array<float, 3> world_anchor{static_cast<float>((*before)[0]),
                                          static_cast<float>((*before)[1]),
                                          static_cast<float>((*before)[2])};
  camera.zoom_about(2.0F, anchor_ndc, aspect, world_anchor);

  const auto after = ground_point(camera.matrices(aspect), kPixelX, kPixelY, kW, kH);
  ASSERT_TRUE(after.has_value());
  EXPECT_NEAR((*after)[0], (*before)[0], 1e-2);
  EXPECT_NEAR((*after)[1], (*before)[1], 1e-2);
  EXPECT_LT(camera.distance(), 80.0F) << "it must still zoom in";
}

// Without a resolved world anchor (near-horizon ray, nothing under the cursor)
// perspective falls back to a plain dolly — the pivot does not move.
TEST(OrbitCamera, PerspectiveZoomAboutWithoutAnAnchorIsAPlainZoom) {
  OrbitCamera zoomed;
  OrbitCamera about;
  const std::array<float, 3> target = about.target();
  zoomed.zoom(2.0F);
  about.zoom_about(2.0F, {0.8F, -0.6F}, 1.6F); // no world_anchor
  EXPECT_FLOAT_EQ(about.distance(), zoomed.distance());
  EXPECT_FLOAT_EQ(about.target()[0], target[0]);
  EXPECT_FLOAT_EQ(about.target()[1], target[1]);
}

// Wheel push-past follows the CURSOR RAY: once the pivot distance bottoms out at
// the minimum, continued zoom keeps sliding the eye toward the anchor (rather
// than dead-stopping or lurching the subject sideways).
TEST(OrbitCamera, PerspectiveZoomAboutPushPastFollowsTheCursorRay) {
  constexpr double kW = 800.0;
  constexpr double kH = 600.0;
  const float aspect = static_cast<float>(kW / kH);
  constexpr double kPixelX = 520.0;
  constexpr double kPixelY = 300.0;

  OrbitCamera camera;
  const auto anchor = ground_point(camera.matrices(aspect), kPixelX, kPixelY, kW, kH);
  ASSERT_TRUE(anchor.has_value());
  const std::array<float, 2> anchor_ndc{
      static_cast<float>((2.0 * kPixelX / kW) - 1.0),
      static_cast<float>(1.0 - (2.0 * kPixelY / kH)),
  };
  const std::array<float, 3> world_anchor{static_cast<float>((*anchor)[0]),
                                          static_cast<float>((*anchor)[1]),
                                          static_cast<float>((*anchor)[2])};
  // Zoom in until the pivot distance is clamped to the minimum (push-past).
  for (int i = 0; i < 40; ++i) {
    camera.zoom_about(1.0F, anchor_ndc, aspect, world_anchor);
  }
  ASSERT_FLOAT_EQ(camera.distance(), 2.0F);
  const float before = eye_distance_to(camera, world_anchor);
  camera.zoom_about(1.0F, anchor_ndc, aspect, world_anchor); // one more, past the floor
  const float after = eye_distance_to(camera, world_anchor);
  EXPECT_LT(after, before) << "the eye must keep approaching the anchor in push-past";
  EXPECT_FLOAT_EQ(camera.distance(), 2.0F) << "the pivot distance stays clamped";
}

// --- cardinal views (GW-1 steps 12-13) ---------------------------------------

// Cardinals snap yaw only: pivot and distance survive, so pressing one
// re-angles the view without losing your place.
TEST(OrbitCamera, CardinalSnapKeepsPivotAndDistance) {
  OrbitCamera camera;
  camera.frame({12.0F, -7.0F, 3.0F}, 25.0F);
  const auto target = camera.target();
  const float distance = camera.distance();
  const float pitch = camera.pitch();

  camera.set_view(3.14159265F / 2.0F, camera.pitch()); // "north"
  EXPECT_FLOAT_EQ(camera.distance(), distance);
  EXPECT_FLOAT_EQ(camera.pitch(), pitch) << "a cardinal must not change the pitch";
  EXPECT_FLOAT_EQ(camera.target()[0], target[0]);
  EXPECT_FLOAT_EQ(camera.target()[1], target[1]);
}

// Looking from the north means the eye is north (+y) of the pivot, looking
// south. Verified through the eye rather than the yaw literal, so the test
// would catch a sign flip the convention comment couldn't.
TEST(OrbitCamera, CardinalYawValuesPutTheEyeOnTheRightSide) {
  constexpr float kHalfPi = 3.14159265F / 2.0F;
  const float low_pitch = 0.2F; // near-level, so the horizontal offset dominates

  const auto eye_for = [low_pitch](float yaw) {
    OrbitCamera camera;
    camera.frame({0.0F, 0.0F, 0.0F}, 20.0F);
    camera.set_view(yaw, low_pitch);
    return camera.matrices(1.0F).eye;
  };

  EXPECT_GT(eye_for(kHalfPi)[1], 5.0F) << "north: eye at +y";
  EXPECT_LT(eye_for(-kHalfPi)[1], -5.0F) << "south: eye at -y";
  EXPECT_LT(eye_for(3.14159265F)[0], -5.0F) << "west: eye at -x";
  EXPECT_GT(eye_for(0.0F)[0], 5.0F) << "east: eye at +x";
}

// Top-down is near-vertical, not vertical: at exactly π/2 the look-at basis
// degenerates (forward ∥ world up). GW-1 step 13 amendment.
TEST(OrbitCamera, TopDownIsNearVerticalAndKeepsAWellFormedBasis) {
  OrbitCamera camera;
  camera.frame({0.0F, 0.0F, 0.0F}, 20.0F);
  camera.set_view(-3.14159265F / 2.0F, OrbitCamera::kTopDownPitch);

  const CameraMatrices m = camera.matrices(1.0F);
  EXPECT_NEAR(m.eye[0], 0.0F, 0.5F);
  EXPECT_NEAR(m.eye[1], 0.0F, 0.5F);
  EXPECT_GT(m.eye[2], 19.0F) << "the eye is essentially overhead";

  // North-up: world +y must project onto screen +y. The view matrix's up row
  // (row 1) dotted with world +y is positive when north points up the screen.
  EXPECT_GT(m.view[5], 0.9F);

  // The basis stays orthonormal — the point of not using exactly π/2.
  const auto rows = rotation_rows(m);
  for (std::size_t i = 0; i < 3; ++i) {
    EXPECT_NEAR(dot(rows[i], rows[i]), 1.0F, 1e-5F);
  }
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
