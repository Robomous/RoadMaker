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

// Shared exporter "stage" primitives used by every mesh exporter (glTF and
// USD). Single source of truth for the two things the file formats must agree
// on: the kernel-frame -> export-frame conversion, and the material naming +
// colors. Keeping these here guarantees a lane surface carries the SAME
// material name and color whether the network is written as .glb or .usda.
//
// This header is private to core/src/io (not shipped in the public include
// tree). It pulls in no third-party format library.
#pragma once

#include "roadmaker/mesh/mesh.hpp"
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

/// The inverse of `to_export_frame`: export frame (Y-up) -> kernel frame (Z-up),
/// (x, y, z) -> (x, -z, y). Kept HERE, beside the forward map, so the boundary
/// rotation has exactly one definition read in both directions — the glTF
/// importer (p6-s8, #322) is the only reader, and its round-trip test exports a
/// prop through `to_export_frame` and imports it back through this, so the two
/// are pinned against each other rather than against arithmetic done twice.
[[nodiscard]] inline std::array<double, 3> from_export_frame(double x, double y, double z) {
  return {x, -z, y};
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

/// The two ground colours, lifted from what the viewport already draws
/// (editor/src/render/scene_builder.cpp): grass green for a surface with no
/// material and for the terrain field, neutral mid-grey the moment a surface
/// stores one. Same decision, same numbers — an exported ground reads like the
/// ground the user was looking at.
inline constexpr std::array<double, 4> kGrassColor{0.28, 0.42, 0.20, 1.0};
inline constexpr std::array<double, 4> kPavedGroundColor{0.34, 0.34, 0.35, 1.0};

// Roughness per material class (PBR metallic-roughness / UsdPreviewSurface).
inline constexpr double kLaneRoughness = 0.95;
inline constexpr double kMarkingRoughness = 0.6;

inline constexpr const char* kMarkingMaterialName = "lane_marking";

/// The scene height field's own material (#390) — named apart from the ground
/// surfaces so a consumer can hide or replace the terrain without touching the
/// authored ones, even though both are drawn the same grass green.
inline constexpr const char* kTerrainMaterialName = "ground_terrain";

/// The message BOTH exporters return when there is nothing to write, and the
/// one the export manifest previews. One definition so a preview cannot
/// promise a refusal the exporter words differently.
inline constexpr const char* kNothingToExportMessage = "nothing to export: empty network mesh";

// Junction floors carry the driving-lane material since the tee visual
// rework (follow-up to issue #103) — the legacy "junction_floor" material
// must never reappear in an export (regression-tested).

/// Material name for a lane surface class: `lane_<enum>` — identical spelling
/// in both exporters (glTF material.name and USD Material prim name).
/// USD prim identifiers admit only [A-Za-z0-9_] and may not lead with a digit.
/// Shared so the USD exporter and the export manifest (io/export_preview.cpp)
/// spell a prop material the same way — they diverged once, and the USD
/// reconciliation gate in test_usd.cpp is what found it.
[[nodiscard]] inline std::string sanitize_usd_identifier(std::string_view in,
                                                         std::string_view fallback) {
  std::string out;
  out.reserve(in.size());
  for (const char c : in) {
    const bool ok =
        (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
    out.push_back(ok ? c : '_');
  }
  if (out.empty()) {
    out = std::string(fallback);
  }
  if (out.front() >= '0' && out.front() <= '9') {
    out.insert(out.begin(), '_');
  }
  return out;
}

/// The material name each format gives one part of a prop model. glTF uses
/// "<model>:<part>"; USD cannot (':' is not a legal prim identifier).
[[nodiscard]] inline std::string gltf_prop_material_name(const std::string& model_id,
                                                         const std::string& part) {
  return model_id + ":" + part;
}

[[nodiscard]] inline std::string usd_prop_material_name(const std::string& model_id,
                                                        const std::string& part) {
  return sanitize_usd_identifier("propmat_" + model_id + "_" + part, "propmat");
}

[[nodiscard]] inline std::string lane_material_name(LaneType type) {
  return "lane_" + std::to_string(static_cast<int>(type));
}

/// Material name for a ground surface carrying material `code` (#390):
/// `ground_grass` when it carries none, `ground_<code>` otherwise. Sanitised
/// for BOTH formats, so a code like "rm:asphalt_worn" spells the same
/// `ground_rm_asphalt_worn` in .glb and .usda — unlike prop materials, ground
/// has no reason to differ per format.
[[nodiscard]] inline std::string ground_material_name(const std::string& code) {
  return sanitize_usd_identifier("ground_" + (code.empty() ? std::string("grass") : code),
                                 "ground");
}

/// The base colour a ground surface is written with: grass unless it stores a
/// material, in which case a neutral pavement grey (the exporters carry no
/// texture library, so an asphalt surface is written as flat pavement and the
/// material NAME is what a consumer maps to its own).
[[nodiscard]] inline std::array<double, 4> ground_material_color(const std::string& code) {
  return code.empty() ? kGrassColor : kPavedGroundColor;
}

/// Whether a mesh holds anything at all to write. THE definition of "nothing
/// to export", shared by both exporters and by the export manifest that
/// previews their verdict — a scene of nothing but terrain, ground, bridges or
/// props is real geometry and exports (#390). Every NetworkMesh channel is
/// named here: adding a channel without adding it below is what made a
/// terrain-only scene unexportable in the first place.
[[nodiscard]] inline bool has_exportable_geometry(const NetworkMesh& mesh) {
  return !mesh.roads.empty() || !mesh.junction_floors.empty() || !mesh.surfaces.empty() ||
         !mesh.terrain.indices.empty() || !mesh.bridges.empty() || !mesh.objects.empty() ||
         !mesh.signal_instances.empty();
}

} // namespace roadmaker::io_common
