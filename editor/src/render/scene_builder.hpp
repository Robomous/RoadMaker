#pragma once

// Pure CPU-side scene construction: kernel NetworkMesh -> renderer-ready
// RenderMeshData plus the entity ids needed for selection/picking. No GL,
// no Qt — unit-testable headless.

#include "roadmaker/mesh/mesh.hpp"

#include <array>
#include <vector>

#include "render/renderer.hpp"

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
  SurfaceKind surface = SurfaceKind::Untextured; // textured-mode material class
};

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

struct Scene {
  std::vector<SceneItem> items;
  SceneBounds bounds;
};

/// Appends one road's items (lane patches + markings) to `scene`, growing
/// its bounds — the unit of work for partial viewport re-uploads.
void append_road_items(const RoadMesh& road, Scene& scene);

/// Appends a placed prop's parts (trunk, crown, …) to `scene` as one SceneItem
/// each, baking the bundled model geometry into world space at the instance's
/// pose and tagging every part with the owning road + ObjectId (so hover,
/// selection, and picking address the whole tree). Grows the scene bounds.
void append_object_items(const ObjectInstance& instance, Scene& scene);

/// Appends a placed signal's parts (pole, housing, lamps / plate) to `scene`,
/// baking the bundled signal model into world space at the instance pose and
/// tagging every part with the owning road + SignalId (so hover, selection, and
/// picking address the whole signal). Grows the scene bounds.
void append_signal_items(const SignalInstance& instance, Scene& scene);

/// Flattens a NetworkMesh into upload-ready items: one per lane patch (with
/// road+lane ids), one per marking (road id only), one per junction floor, and
/// one per enclosed-area ground surface (with its SurfaceId).
[[nodiscard]] Scene build_scene(const NetworkMesh& mesh);

} // namespace roadmaker::editor
