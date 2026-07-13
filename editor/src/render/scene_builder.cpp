#include "render/scene_builder.hpp"

#include <algorithm>

namespace roadmaker::editor {

namespace {

void grow_bounds(SceneBounds& bounds, const std::vector<double>& positions) {
  for (std::size_t i = 0; i + 2 < positions.size(); i += 3) {
    for (std::size_t axis = 0; axis < 3; ++axis) {
      const auto v = static_cast<float>(positions[i + axis]);
      bounds.lo[axis] = std::min(bounds.lo[axis], v);
      bounds.hi[axis] = std::max(bounds.hi[axis], v);
    }
  }
}

} // namespace

std::array<float, 4> lane_color(LaneType type) {
  switch (type) {
  case LaneType::Driving:
    return {0.25F, 0.25F, 0.27F, 1.0F};
  case LaneType::Stop:
    return {0.45F, 0.22F, 0.20F, 1.0F};
  case LaneType::Shoulder:
    return {0.42F, 0.42F, 0.39F, 1.0F};
  case LaneType::Biking:
    return {0.55F, 0.28F, 0.24F, 1.0F};
  case LaneType::Sidewalk:
    return {0.65F, 0.65F, 0.63F, 1.0F};
  case LaneType::Border:
    return {0.50F, 0.50F, 0.50F, 1.0F};
  case LaneType::Restricted:
    return {0.50F, 0.40F, 0.30F, 1.0F};
  case LaneType::Parking:
    return {0.30F, 0.32F, 0.48F, 1.0F};
  case LaneType::Median:
    return {0.30F, 0.45F, 0.30F, 1.0F};
  case LaneType::Curb:
    return {0.55F, 0.55F, 0.50F, 1.0F};
  case LaneType::None:
  case LaneType::Other:
    return {0.35F, 0.35F, 0.35F, 1.0F};
  }
  return {0.35F, 0.35F, 0.35F, 1.0F};
}

RenderMeshData to_render_data(const std::vector<double>& positions,
                              const std::vector<double>& normals,
                              const std::vector<std::uint32_t>& indices,
                              const std::array<float, 4>& color) {
  auto narrow = [](const std::vector<double>& values) {
    std::vector<float> out;
    out.reserve(values.size());
    for (const double v : values) {
      out.push_back(static_cast<float>(v));
    }
    return out;
  };
  RenderMeshData data;
  data.positions = narrow(positions);
  data.normals = narrow(normals);
  data.indices = indices;
  data.color = color;
  return data;
}

float SceneBounds::framing_radius() const {
  const float dx = hi[0] - lo[0];
  const float dy = hi[1] - lo[1];
  return std::max({dx, dy, 10.0F}) / 2.0F;
}

void append_road_items(const RoadMesh& road, Scene& scene) {
  grow_bounds(scene.bounds, road.positions);
  for (const RoadMesh::LanePatch& patch : road.lanes) {
    scene.items.push_back(SceneItem{
        .data =
            to_render_data(road.positions, road.normals, patch.indices, lane_color(patch.material)),
        .road = road.road,
        .lane = patch.lane,
    });
  }
  for (const SubMesh& marking : road.markings) {
    scene.items.push_back(SceneItem{
        .data = to_render_data(
            marking.positions, marking.normals, marking.indices, {0.92F, 0.92F, 0.87F, 1.0F}),
        .road = road.road,
        .lane = {},
    });
  }
}

Scene build_scene(const NetworkMesh& mesh) {
  Scene scene;
  for (const RoadMesh& road : mesh.roads) {
    append_road_items(road, scene);
  }
  for (const JunctionFloor& floor : mesh.junction_floors) {
    // Same material class as the lanes feeding the junction — one
    // continuous asphalt (a distinct floor color read as a patch).
    scene.items.push_back(SceneItem{
        .data = to_render_data(floor.mesh.positions,
                               floor.mesh.normals,
                               floor.mesh.indices,
                               lane_color(floor.mesh.material)),
        .road = {},
        .lane = {},
    });
    grow_bounds(scene.bounds, floor.mesh.positions);
  }
  return scene;
}

} // namespace roadmaker::editor
