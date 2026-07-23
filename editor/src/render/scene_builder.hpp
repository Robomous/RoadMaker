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

#pragma once

// Pure CPU-side scene construction: kernel NetworkMesh -> renderer-ready
// RenderMeshData plus the entity ids needed for selection/picking. No GL,
// no Qt — unit-testable headless.

#include "roadmaker/mesh/mesh.hpp"

#include <array>
#include <string>
#include <vector>

#include "render/renderer.hpp"

namespace roadmaker {
class RoadNetwork;
} // namespace roadmaker

namespace roadmaker::editor {

/// Same palette as the glTF exporter, kept in the editor so the viewport
/// matches exported files.
[[nodiscard]] std::array<float, 4> lane_color(LaneType type);

/// Paint colour for a lane marking's e_roadMarkColor (§11.9). `Standard` means
/// "the standard colour for this mark type" and resolves to white, which every
/// mark RoadMaker authors uses.
[[nodiscard]] std::array<float, 4> mark_paint(RoadMarkColor color);

/// Explicit double -> float narrowing: this is the kernel -> render
/// boundary, the one place precision is deliberately dropped.
[[nodiscard]] RenderMeshData to_render_data(const std::vector<double>& positions,
                                            const std::vector<double>& normals,
                                            const std::vector<std::uint32_t>& indices,
                                            const std::array<float, 4>& color,
                                            const std::vector<double>& uvs = {});

/// Which textured-mode surface material an item wants. Resolved to a concrete
/// Material (texture handle / paint) by the viewport, which owns the uploaded
/// textures; Sober mode ignores it and draws the flat per-mesh color.
enum class SurfaceKind {
  Untextured, ///< props and anything with a baked color
  Asphalt,    ///< driving/shoulder/junction-floor road surface
  Concrete,   ///< sidewalks and curbs
  Paint,      ///< lane markings — bright unlit paint, no texture
  Grass,      ///< enclosed-area ground surface (#215) — lit flat grass-green
};

/// The textured-mode surface class for a lane by its type (asphalt for the
/// travelled way, concrete for the walked way).
[[nodiscard]] SurfaceKind surface_for(LaneType type);

/// One uploadable mesh plus the entity it visualizes. `lane` is invalid for
/// markings and junction floors; `object` is valid only for prop parts (a
/// placed tree/vegetation instance), where `road` is its owning road and
/// `lane` is invalid. All three ids are invalid for undecorated geometry.
struct SceneItem {
  RenderMeshData data;
  RoadId road;
  LaneId lane;
  ObjectId object;
  SignalId signal;                               // valid for a signal-instance part
  JunctionId junction;                           // valid for a junction-floor item
  SurfaceId surface_id;                          // valid for a ground-surface item (#215)
  SurfaceKind surface = SurfaceKind::Untextured; // textured-mode material class fallback
  /// Assigned material code (p6-s3): the lane <material> surface code, or a
  /// surface's stored material. Empty → resolve by SurfaceKind. The viewport
  /// resolves this through MaterialCatalog to a textured Material.
  std::string material;
};

/// One placed prop/signal in an instanced batch: the shared model's per-instance
/// transform plus the entity ids a pick/hover addresses. Exactly one of `object`
/// / `signal` is valid (the other is a null id), mirroring the source
/// ObjectInstance / SignalInstance; `road` is the owning road.
struct ScenePropInstance {
  RoadId road;
  ObjectId object;
  SignalId signal;
  InstanceData transform; ///< model-space -> world (column-major mat4)
};

/// All placed instances of ONE prop model, sharing a single uploaded mesh set.
/// `parts` is the model geometry in MODEL space (baked ONCE, drawn via the GL
/// instanced fast path); `instances` carries the per-instance transforms + ids.
struct ScenePropBatch {
  std::string model_id;
  std::vector<RenderMeshData> parts;        ///< model-space part meshes
  std::vector<ScenePropInstance> instances; ///< one per placed prop/signal
};

/// Column-major model matrix for a prop at `position` rotated by `heading` about
/// +Z and uniformly scaled by `scale` (ObjectInstance::scale — the prop's
/// declared OpenDRIVE @height relative to the model). Pure and header-declared
/// so both the scene builder and its tests share the one bake definition. At
/// scale 1 it matches the pre-instancing world-space bake exactly.
[[nodiscard]] InstanceData
prop_transform(const std::array<double, 3>& position, double heading, double scale = 1.0);

/// Axis-aligned bounds of the built scene (kernel frame, meters).
struct SceneBounds {
  std::array<float, 3> lo{1e9F, 1e9F, 1e9F};
  std::array<float, 3> hi{-1e9F, -1e9F, -1e9F};

  [[nodiscard]] bool valid() const { return lo[0] <= hi[0]; }

  [[nodiscard]] std::array<float, 3> center() const {
    return {(lo[0] + hi[0]) / 2, (lo[1] + hi[1]) / 2, (lo[2] + hi[2]) / 2};
  }

  /// Plan-view framing radius (max of x/y extent, floor 10 m), halved.
  [[nodiscard]] float framing_radius() const;
};

/// World height (Z-up, meters) of the procedural ground plane: just below the
/// network floor (`bounds.lo[2]`) so coplanar road surfaces don't z-fight, or a
/// small default drop when there is no geometry yet. See Renderer::set_ground.
[[nodiscard]] float ground_base_z(const SceneBounds& bounds);

/// One placed text sign's editable face: a single textured quad baked to WORLD
/// space (via prop_transform), kept OUT of the instanced prop batches because
/// each face carries its own text→texture. The viewport uploads/caches the
/// rasterised bitmap keyed on (model_id, text), draws it with a ClampToEdge
/// texture, and highlights it with the owning signal like the other sign parts.
struct SceneSignFace {
  RoadId road;
  SignalId signal;
  std::string model_id;
  std::string text;    ///< texture cache key (with model_id)
  RenderMeshData data; ///< world-space quad; uvs in [0,1]
};

struct Scene {
  std::vector<SceneItem> items;
  /// Instanced props/signals: one batch per model, each drawn with a single
  /// instanced call instead of one baked SceneItem per placement.
  std::vector<ScenePropBatch> prop_batches;
  /// Editable text-sign faces — one textured world-space quad per placed text
  /// sign (drawn individually because each carries its own text texture).
  std::vector<SceneSignFace> sign_faces;
  SceneBounds bounds;
};

/// Appends one road's items (lane patches + markings) to `scene`, growing
/// its bounds — the unit of work for partial viewport re-uploads.
void append_road_items(const RoadMesh& road, Scene& scene);

/// Adds a placed prop to `scene` as an instance of its shared model batch
/// (find-or-create by model id): the model parts are converted to model-space
/// RenderMeshData ONCE per batch, and the placement contributes one
/// ScenePropInstance carrying its transform + owning road + ObjectId (so hover,
/// selection, and picking address the whole tree). Grows the scene bounds from
/// the model bounding cylinder.
void append_object_items(const ObjectInstance& instance, Scene& scene);

/// Adds a placed signal to `scene` as an instance of its shared model batch
/// (as append_object_items, but tagged with the owning road + SignalId). Grows
/// the scene bounds from the model bounding cylinder.
void append_signal_items(const SignalInstance& instance, Scene& scene);

/// Flattens a NetworkMesh into upload-ready items: one per lane patch (with
/// road+lane ids), one per marking (road id only), one per junction floor, and
/// one per enclosed-area ground surface (with its SurfaceId). When `network` is
/// non-null, each ground surface's stored material selects its render class
/// (asphalt/concrete vs. the default grass); a null network keeps grass.
[[nodiscard]] Scene build_scene(const NetworkMesh& mesh, const RoadNetwork* network = nullptr);

} // namespace roadmaker::editor
