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

/// Explicit double -> float narrowing: this is the kernel -> render
/// boundary, the one place precision is deliberately dropped.
[[nodiscard]] RenderMeshData to_render_data(const std::vector<double>& positions,
                                            const std::vector<double>& normals,
                                            const std::vector<std::uint32_t>& indices,
                                            const std::array<float, 4>& color);

/// One uploadable mesh plus the entity it visualizes. `lane` is invalid for
/// markings and junction floors; both ids are invalid for decorations.
struct SceneItem {
  RenderMeshData data;
  RoadId road;
  LaneId lane;
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

struct Scene {
  std::vector<SceneItem> items;
  SceneBounds bounds;
};

/// Appends one road's items (lane patches + markings) to `scene`, growing
/// its bounds — the unit of work for partial viewport re-uploads.
void append_road_items(const RoadMesh& road, Scene& scene);

/// Flattens a NetworkMesh into upload-ready items: one per lane patch (with
/// road+lane ids), one per marking (road id only), one per junction floor.
[[nodiscard]] Scene build_scene(const NetworkMesh& mesh);

} // namespace roadmaker::editor
