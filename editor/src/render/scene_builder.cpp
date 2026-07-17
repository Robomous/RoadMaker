#include "render/scene_builder.hpp"

#include "roadmaker/assets/prop_library.hpp"
#include "roadmaker/road/network.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <string_view>

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

std::array<float, 4> mark_paint(RoadMarkColor color) {
  // e_roadMarkColor (§11.9) → paint. `Standard` is "the standard colour for
  // this mark type", which for every mark RoadMaker authors is white — so it
  // and White share the slightly-off-white the markings have always used.
  switch (color) {
  case RoadMarkColor::Yellow:
    return {0.90F, 0.75F, 0.16F, 1.0F};
  case RoadMarkColor::Red:
    return {0.72F, 0.16F, 0.14F, 1.0F};
  case RoadMarkColor::Blue:
    return {0.16F, 0.34F, 0.70F, 1.0F};
  case RoadMarkColor::Green:
    return {0.18F, 0.55F, 0.28F, 1.0F};
  case RoadMarkColor::Orange:
    return {0.88F, 0.48F, 0.12F, 1.0F};
  case RoadMarkColor::Standard:
  case RoadMarkColor::White:
  case RoadMarkColor::Other:
    break;
  }
  return {0.92F, 0.92F, 0.87F, 1.0F};
}

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
            marking.positions, marking.normals, marking.indices, mark_paint(marking.mark_color)),
        .road = road.road,
        .lane = {},
        .surface = SurfaceKind::Paint,
    });
  }
}

namespace {

// Bakes a bundled model (props::model(model_id)) into world-space SceneItems at
// the given pose — one item per part, each tagged with the owning road plus the
// source entity id (object OR signal; the other stays invalid). Shared by the
// prop and signal instance paths so both draw through the identical bake.
void append_model_items(std::string_view model_id,
                        const std::array<double, 3>& origin,
                        double heading,
                        RoadId road,
                        ObjectId object,
                        SignalId signal,
                        Scene& scene) {
  const props::PropModel* model = props::model(model_id);
  if (model == nullptr) {
    return;
  }
  const double cos_h = std::cos(heading);
  const double sin_h = std::sin(heading);
  for (const props::PropPart& part : model->parts) {
    // Bake the model-space part to world space (rotate about +Z by heading,
    // translate to the instance origin); the renderer draws pre-baked world
    // positions with no model matrix.
    std::vector<double> world_pos(part.positions.size());
    std::vector<double> world_nrm(part.normals.size());
    for (std::size_t i = 0; i + 2 < part.positions.size(); i += 3) {
      const double x = part.positions[i];
      const double y = part.positions[i + 1];
      world_pos[i] = (cos_h * x) - (sin_h * y) + origin[0];
      world_pos[i + 1] = (sin_h * x) + (cos_h * y) + origin[1];
      world_pos[i + 2] = part.positions[i + 2] + origin[2];
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
        .road = road,
        .lane = {},
        .object = object,
        .signal = signal,
    });
    grow_bounds(scene.bounds, world_pos);
  }
}

} // namespace

void append_object_items(const ObjectInstance& instance, Scene& scene) {
  append_model_items(instance.model_id,
                     instance.position,
                     instance.heading,
                     instance.road,
                     instance.object,
                     {},
                     scene);
}

void append_signal_items(const SignalInstance& instance, Scene& scene) {
  append_model_items(instance.model_id,
                     instance.position,
                     instance.heading,
                     instance.road,
                     {},
                     instance.signal,
                     scene);
}

Scene build_scene(const NetworkMesh& mesh, const RoadNetwork* network) {
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
  for (const SurfaceMesh& surface : mesh.surfaces) {
    // Enclosed-area ground (#215): a lit flat grass-green by default, carrying
    // its SurfaceId so a pick maps back to the selectable entity. When the
    // surface stores a material (p6-s2), it takes the matching render class and
    // a neutral pavement base color instead of the grass green.
    SurfaceKind kind = SurfaceKind::Grass;
    std::array<float, 4> color{0.28F, 0.42F, 0.20F, 1.0F};
    if (network != nullptr) {
      if (const Surface* entity = network->surface(surface.surface);
          entity != nullptr && !entity->material.empty()) {
        // Neutral mid-grey pavement so the color reads as paved even in Sober
        // mode; textured mode overlays the asphalt/concrete texture.
        color = {0.34F, 0.34F, 0.35F, 1.0F};
        if (entity->material == "asphalt") {
          kind = SurfaceKind::Asphalt;
        } else if (entity->material == "concrete") {
          kind = SurfaceKind::Concrete;
        }
      }
    }
    scene.items.push_back(SceneItem{
        .data = to_render_data(
            surface.mesh.positions, surface.mesh.normals, surface.mesh.indices, color),
        .surface_id = surface.surface,
        .surface = kind,
    });
    grow_bounds(scene.bounds, surface.mesh.positions);
  }
  for (const ObjectInstance& instance : mesh.objects) {
    append_object_items(instance, scene);
  }
  for (const SignalInstance& instance : mesh.signal_instances) {
    append_signal_items(instance, scene);
  }
  return scene;
}

} // namespace roadmaker::editor
