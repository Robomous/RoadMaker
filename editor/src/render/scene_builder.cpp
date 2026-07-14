#include "render/scene_builder.hpp"

#include "roadmaker/assets/prop_library.hpp"

#include <algorithm>
#include <cmath>

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

SurfaceKind surface_for(LaneType type) {
  switch (type) {
  case LaneType::Sidewalk:
  case LaneType::Curb:
  case LaneType::Border:
    return SurfaceKind::Concrete;
  default:
    // Driving, shoulder, biking, parking, median, restricted, stop, none/other —
    // the travelled/paved way reads as asphalt.
    return SurfaceKind::Asphalt;
  }
}

RenderMeshData to_render_data(const std::vector<double>& positions,
                              const std::vector<double>& normals,
                              const std::vector<std::uint32_t>& indices,
                              const std::array<float, 4>& color,
                              const std::vector<double>& uvs) {
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
  data.uvs = narrow(uvs);
  data.indices = indices;
  data.color = color;
  return data;
}

float SceneBounds::framing_radius() const {
  const float dx = hi[0] - lo[0];
  const float dy = hi[1] - lo[1];
  return std::max({dx, dy, 10.0F}) / 2.0F;
}

float ground_base_z(const SceneBounds& bounds) {
  // 5 cm below the network floor: enough that a road/junction surface sitting
  // exactly at the floor draws over the opaque ground without z-fighting, small
  // enough to be invisible. No geometry yet → drop below the z = 0 datum.
  return (bounds.valid() ? bounds.lo[2] : 0.0F) - 0.05F;
}

void append_road_items(const RoadMesh& road, Scene& scene) {
  grow_bounds(scene.bounds, road.positions);
  for (const RoadMesh::LanePatch& patch : road.lanes) {
    scene.items.push_back(SceneItem{
        .data = to_render_data(
            road.positions, road.normals, patch.indices, lane_color(patch.material), road.uvs),
        .road = road.road,
        .lane = patch.lane,
        .surface = surface_for(patch.material),
    });
  }
  for (const SubMesh& marking : road.markings) {
    scene.items.push_back(SceneItem{
        .data = to_render_data(
            marking.positions, marking.normals, marking.indices, {0.92F, 0.92F, 0.87F, 1.0F}),
        .road = road.road,
        .lane = {},
        .surface = SurfaceKind::Paint,
    });
  }
}

void append_object_items(const ObjectInstance& instance, Scene& scene) {
  const props::PropModel* model = props::model(instance.model_id);
  if (model == nullptr) {
    return;
  }
  const double cos_h = std::cos(instance.heading);
  const double sin_h = std::sin(instance.heading);
  for (const props::PropPart& part : model->parts) {
    // Bake the model-space part to world space (rotate about +Z by heading,
    // translate to the instance origin); the renderer draws pre-baked world
    // positions with no model matrix.
    std::vector<double> world_pos(part.positions.size());
    std::vector<double> world_nrm(part.normals.size());
    for (std::size_t i = 0; i + 2 < part.positions.size(); i += 3) {
      const double x = part.positions[i];
      const double y = part.positions[i + 1];
      world_pos[i] = (cos_h * x) - (sin_h * y) + instance.position[0];
      world_pos[i + 1] = (sin_h * x) + (cos_h * y) + instance.position[1];
      world_pos[i + 2] = part.positions[i + 2] + instance.position[2];
      const double nx = part.normals[i];
      const double ny = part.normals[i + 1];
      world_nrm[i] = (cos_h * nx) - (sin_h * ny);
      world_nrm[i + 1] = (sin_h * nx) + (cos_h * ny);
      world_nrm[i + 2] = part.normals[i + 2];
    }
    scene.items.push_back(SceneItem{
        .data = to_render_data(world_pos,
                               world_nrm,
                               part.indices,
                               {part.color[0], part.color[1], part.color[2], 1.0F}),
        .road = instance.road,
        .lane = {},
        .object = instance.object,
    });
    grow_bounds(scene.bounds, world_pos);
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
        .junction = floor.junction,
        .surface = SurfaceKind::Asphalt,
    });
    grow_bounds(scene.bounds, floor.mesh.positions);
  }
  for (const ObjectInstance& instance : mesh.objects) {
    append_object_items(instance, scene);
  }
  return scene;
}

} // namespace roadmaker::editor
