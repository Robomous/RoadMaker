#pragma once

// Kernel-frame → screen projection for the QPainter handle overlay. Pure math
// (no Qt/GL), so it is unit-testable headless; the viewport wraps the result
// in a QPointF.

#include <array>
#include <optional>

#include "render/renderer.hpp"

namespace roadmaker::editor {

/// Projects a kernel-frame point (meters) to widget-LOGICAL pixels with a
/// top-left origin (QPainter's convention), using the same column-major
/// view/projection the renderer draws with. Returns nullopt when the point is
/// at or behind the camera (clip w ≤ 0) — the caller skips it.
[[nodiscard]] inline std::optional<std::array<double, 2>> project_to_screen(
    const CameraMatrices& camera, double x, double y, double z, double width, double height) {
  const std::array<double, 4> world{x, y, z, 1.0};

  // clip = projection * view * world. Both matrices are column-major
  // (element (row r, col c) = m[c*4 + r]).
  const auto multiply = [](const std::array<float, 16>& m, const std::array<double, 4>& v) {
    std::array<double, 4> out{};
    for (std::size_t row = 0; row < 4; ++row) {
      double sum = 0.0;
      for (std::size_t col = 0; col < 4; ++col) {
        sum += static_cast<double>(m[(col * 4) + row]) * v[col];
      }
      out[row] = sum;
    }
    return out;
  };
  const std::array<double, 4> clip = multiply(camera.projection, multiply(camera.view, world));

  if (clip[3] <= 1e-6) {
    return std::nullopt; // at or behind the camera plane
  }
  const double ndc_x = clip[0] / clip[3];
  const double ndc_y = clip[1] / clip[3];
  return std::array<double, 2>{((ndc_x * 0.5) + 0.5) * width,
                               (1.0 - ((ndc_y * 0.5) + 0.5)) * height};
}

} // namespace roadmaker::editor
