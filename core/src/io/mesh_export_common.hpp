// Copyright 2026 Robomous
// SPDX-License-Identifier: Apache-2.0

// Shared exporter "stage" primitives used by every mesh exporter (glTF and
// USD). Single source of truth for the two things the file formats must agree
// on: the kernel-frame -> export-frame conversion, and the material naming +
// colors. Keeping these here guarantees a lane surface carries the SAME
// material name and color whether the network is written as .glb or .usda.
//
// This header is private to core/src/io (not shipped in the public include
// tree). It pulls in no third-party format library.
#pragma once

#include "roadmaker/road/lane.hpp"

#include <array>
#include <string>

namespace roadmaker::io_common {

/// Kernel frame (right-handed, Z-up, meters) -> export frame (right-handed,
/// Y-up, meters). THE single definition of the boundary rotation:
/// (x, y, z) -> (x, z, -y). A proper rotation, so triangle winding is
/// preserved. glTF consumes the result as floats; USD as point3f/normal3f.
[[nodiscard]] inline std::array<float, 3> to_export_frame(double x, double y, double z) {
  return {static_cast<float>(x), static_cast<float>(z), static_cast<float>(-y)};
}

/// Base colors per material class (linear RGBA). Shared so glTF baseColorFactor
/// and USD UsdPreviewSurface diffuseColor never drift apart.
[[nodiscard]] inline std::array<double, 4> lane_material_color(LaneType type) {
  switch (type) {
  case LaneType::Driving:
    return {0.25, 0.25, 0.27, 1.0};
  case LaneType::Stop:
    return {0.45, 0.22, 0.20, 1.0};
  case LaneType::Shoulder:
    return {0.42, 0.42, 0.39, 1.0};
  case LaneType::Biking:
    return {0.55, 0.28, 0.24, 1.0};
  case LaneType::Sidewalk:
    return {0.65, 0.65, 0.63, 1.0};
  case LaneType::Border:
    return {0.50, 0.50, 0.50, 1.0};
  case LaneType::Restricted:
    return {0.50, 0.40, 0.30, 1.0};
  case LaneType::Parking:
    return {0.30, 0.32, 0.48, 1.0};
  case LaneType::Median:
    return {0.30, 0.45, 0.30, 1.0};
  case LaneType::Curb:
    return {0.55, 0.55, 0.50, 1.0};
  case LaneType::None:
  case LaneType::Other:
    return {0.35, 0.35, 0.35, 1.0};
  }
  return {0.35, 0.35, 0.35, 1.0};
}

inline constexpr std::array<double, 4> kMarkingColor{0.92, 0.92, 0.87, 1.0};

// Roughness per material class (PBR metallic-roughness / UsdPreviewSurface).
inline constexpr double kLaneRoughness = 0.95;
inline constexpr double kMarkingRoughness = 0.6;

inline constexpr const char* kMarkingMaterialName = "lane_marking";

// Junction floors carry the driving-lane material since the tee visual
// rework (follow-up to issue #103) — the legacy "junction_floor" material
// must never reappear in an export (regression-tested).

/// Material name for a lane surface class: `lane_<enum>` — identical spelling
/// in both exporters (glTF material.name and USD Material prim name).
[[nodiscard]] inline std::string lane_material_name(LaneType type) {
  return "lane_" + std::to_string(static_cast<int>(type));
}

} // namespace roadmaker::io_common
