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

#include "render/scene_builder.hpp"

#include "roadmaker/assets/prop_library.hpp"
#include "roadmaker/road/network.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
#include <string_view>
#include <variant>

#include "document/actor_placement.hpp"

namespace roadmaker::editor {

namespace {

/// Render class for an assigned BARE catalog material name ("asphalt",
/// "concrete"), falling back to `fallback` for an empty or unrecognised name.
/// One mapping shared by the ground surfaces and the junction floor/overlays
/// (p4-s2) so the two can never drift apart.
SurfaceKind surface_kind_for(const std::string& material, SurfaceKind fallback) {
  if (material == "asphalt") {
    return SurfaceKind::Asphalt;
  }
  if (material == "concrete") {
    return SurfaceKind::Concrete;
  }
  return fallback;
}

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

InstanceData prop_transform(const std::array<double, 3>& position, double heading, double scale) {
  // Column-major mat4: col0=(kc,ks,0,0) col1=(-ks,kc,0,0) col2=(0,0,k,0)
  // col3=(x,y,z,1) — a uniform scale by k, a +Z rotation by `heading`, then a
  // translate to `position` (which the scale must NOT touch: a resized prop
  // stays where it was placed). Applied to (x,y,z,1):
  //   x' = k*(c*x - s*y) + px,  y' = k*(s*x + c*y) + py,  z' = k*z + pz.
  // At k = 1 this is bit-identical to the pre-#335 bake.
  const auto k = static_cast<float>(scale);
  const auto c = static_cast<float>(std::cos(heading));
  const auto s = static_cast<float>(std::sin(heading));
  const auto px = static_cast<float>(position[0]);
  const auto py = static_cast<float>(position[1]);
  const auto pz = static_cast<float>(position[2]);
  return InstanceData{{k * c,
                       k * s,
                       0.0F,
                       0.0F, //
                       -k * s,
                       k * c,
                       0.0F,
                       0.0F, //
                       0.0F,
                       0.0F,
                       k,
                       0.0F, //
                       px,
                       py,
                       pz,
                       1.0F}};
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
        // Assigned lane <material> code (empty → the surface-kind fallback).
        .material = patch.surface,
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

// Adds one placed instance of a bundled model (props::model(model_id)) to its
// shared batch (find-or-create by model id). The model parts are converted to
// model-space RenderMeshData ONCE per batch; each placement contributes a single
// ScenePropInstance (transform + owning road + source entity id — object OR
// signal, the other stays invalid). Shared by the prop and signal paths so both
// draw through the identical instanced batch.
void append_model_items(std::string_view model_id,
                        const std::array<double, 3>& origin,
                        double heading,
                        double scale,
                        RoadId road,
                        ObjectId object,
                        SignalId signal,
                        Scene& scene) {
  const props::PropModel* model = props::model(model_id);
  if (model == nullptr) {
    return;
  }
  // Find-or-create the batch for this model (linear scan; ~14 bundled models,
  // so O(models) is fine). First-encounter order is preserved and deterministic.
  ScenePropBatch* batch = nullptr;
  for (ScenePropBatch& existing : scene.prop_batches) {
    if (existing.model_id == model_id) {
      batch = &existing;
      break;
    }
  }
  if (batch == nullptr) {
    ScenePropBatch created;
    created.model_id = std::string(model_id);
    created.parts.reserve(model->parts.size());
    for (const props::PropPart& part : model->parts) {
      // MODEL-space geometry (the instanced draw applies the per-instance matrix).
      created.parts.push_back(to_render_data(part.positions,
                                             part.normals,
                                             part.indices,
                                             {part.color[0], part.color[1], part.color[2], 1.0F}));
    }
    scene.prop_batches.push_back(std::move(created));
    batch = &scene.prop_batches.back();
  }
  batch->instances.push_back(ScenePropInstance{
      .road = road,
      .object = object,
      .signal = signal,
      .transform = prop_transform(origin, heading, scale),
  });

  // Grow scene bounds from the model bounding cylinder (radius/height) at the
  // instance origin — cheap and pose-independent (heading only spins the crown).
  // The cylinder scales with the instance, so a resized prop frames correctly.
  const auto radius = static_cast<float>(model->radius * scale);
  const auto height = static_cast<float>(model->height * scale);
  const auto ox = static_cast<float>(origin[0]);
  const auto oy = static_cast<float>(origin[1]);
  const auto oz = static_cast<float>(origin[2]);
  scene.bounds.lo[0] = std::min(scene.bounds.lo[0], ox - radius);
  scene.bounds.lo[1] = std::min(scene.bounds.lo[1], oy - radius);
  scene.bounds.lo[2] = std::min(scene.bounds.lo[2], oz);
  scene.bounds.hi[0] = std::max(scene.bounds.hi[0], ox + radius);
  scene.bounds.hi[1] = std::max(scene.bounds.hi[1], oy + radius);
  scene.bounds.hi[2] = std::max(scene.bounds.hi[2], oz + height);
}

} // namespace

void append_object_items(const ObjectInstance& instance, Scene& scene) {
  append_model_items(instance.model_id,
                     instance.position,
                     instance.heading,
                     instance.scale,
                     instance.road,
                     instance.object,
                     {},
                     scene);
}

namespace {

/// Bakes a model-space SignalFaceOverlay to a world-space textured quad using
/// the instance's transform `m` (column-major mat4 from prop_transform). The
/// mat4 is a uniform scale + Z-rotation + translation, so positions transform in
/// full and normals by the rotation (uniform scale keeps them unit after the
/// shader renormalises). UVs pass through unchanged.
RenderMeshData bake_face_to_world(const SignalFaceOverlay& face, const InstanceData& m) {
  const auto& t = m.model; // column-major: col c, row r → t[c*4 + r]
  const auto apply = [&](double x, double y, double z, bool translate) {
    const auto fx = static_cast<float>(x);
    const auto fy = static_cast<float>(y);
    const auto fz = static_cast<float>(z);
    const float tx = translate ? t[12] : 0.0F;
    const float ty = translate ? t[13] : 0.0F;
    const float tz = translate ? t[14] : 0.0F;
    return std::array<float, 3>{t[0] * fx + t[4] * fy + t[8] * fz + tx,
                                t[1] * fx + t[5] * fy + t[9] * fz + ty,
                                t[2] * fx + t[6] * fy + t[10] * fz + tz};
  };
  RenderMeshData data;
  data.kind = PrimitiveKind::Triangles;
  data.color = {1.0F, 1.0F, 1.0F, 1.0F}; // tinted by the sampled texture
  const std::size_t verts = face.positions.size() / 3;
  data.positions.reserve(verts * 3);
  data.normals.reserve(verts * 3);
  for (std::size_t v = 0; v < verts; ++v) {
    const auto p =
        apply(face.positions[v * 3], face.positions[v * 3 + 1], face.positions[v * 3 + 2], true);
    const auto n =
        apply(face.normals[v * 3], face.normals[v * 3 + 1], face.normals[v * 3 + 2], false);
    data.positions.insert(data.positions.end(), p.begin(), p.end());
    data.normals.insert(data.normals.end(), n.begin(), n.end());
  }
  data.uvs.reserve(face.uvs.size());
  for (const double uv : face.uvs) {
    data.uvs.push_back(static_cast<float>(uv));
  }
  data.indices = face.indices;
  return data;
}

} // namespace

void append_signal_items(const SignalInstance& instance, Scene& scene) {
  // Signals are not resizable (#335 scopes per-instance size to props).
  append_model_items(instance.model_id,
                     instance.position,
                     instance.heading,
                     1.0,
                     instance.road,
                     {},
                     instance.signal,
                     scene);
  // Editable text face (static sign with @text on a face-plate model): a single
  // world-space textured quad, drawn on its own because its texture is unique.
  if (instance.face.has_value()) {
    scene.sign_faces.push_back(SceneSignFace{
        .road = instance.road,
        .signal = instance.signal,
        .model_id = instance.model_id,
        .text = instance.face->text,
        .data = bake_face_to_world(*instance.face,
                                   prop_transform(instance.position, instance.heading, 1.0)),
    });
  }
}

Scene build_scene(const NetworkMesh& mesh, const RoadNetwork* network) {
  Scene scene;
  for (const RoadMesh& road : mesh.roads) {
    append_road_items(road, scene);
  }
  for (const JunctionFloor& floor : mesh.junction_floors) {
    // Same material class as the lanes feeding the junction — one
    // continuous asphalt (a distinct floor color read as a patch) unless the
    // junction authors a carriageway material (p4-s2), which then resolves
    // through the same catalog as a lane or ground-surface material.
    scene.items.push_back(SceneItem{
        .data = to_render_data(floor.mesh.positions,
                               floor.mesh.normals,
                               floor.mesh.indices,
                               lane_color(floor.mesh.material)),
        .road = {},
        .lane = {},
        .junction = floor.junction,
        .surface = surface_kind_for(floor.mesh.surface, SurfaceKind::Asphalt),
        .material = floor.mesh.surface,
    });
    grow_bounds(scene.bounds, floor.mesh.positions);
    // Authored corner overlays (p4-s2): sidewalk wedges and median noses, each
    // its own item so it can carry its own material and lane-type base colour.
    // They belong to the junction for picking, exactly like the floor.
    for (const SubMesh& detail : floor.details) {
      scene.items.push_back(SceneItem{
          .data = to_render_data(
              detail.positions, detail.normals, detail.indices, lane_color(detail.material)),
          .road = {},
          .lane = {},
          .junction = floor.junction,
          .surface = surface_kind_for(detail.surface, surface_for(detail.material)),
          .material = detail.surface,
      });
      grow_bounds(scene.bounds, detail.positions);
    }
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
        // An unrecognised material keeps the Grass class (flat paved colour).
        kind = surface_kind_for(entity->material, SurfaceKind::Grass);
      }
    }
    const Surface* entity = network != nullptr ? network->surface(surface.surface) : nullptr;
    scene.items.push_back(SceneItem{
        .data = to_render_data(
            surface.mesh.positions, surface.mesh.normals, surface.mesh.indices, color),
        .surface_id = surface.surface,
        .surface = kind,
        // A surface's stored material (p6-s2) resolves through the same catalog
        // as lane materials, so asphalt_worn works here for free.
        .material = entity != nullptr ? entity->material : std::string{},
    });
    grow_bounds(scene.bounds, surface.mesh.positions);
  }
  // The scene terrain (p5-s2, #232): the sampled ground around the network.
  // Drawn as flat grass-green like the enclosed surfaces, but with NO entity id
  // — terrain is not selectable in this sprint, so a pick that lands on it hits
  // nothing. Empty unless the network carries a height field.
  if (!mesh.terrain.positions.empty()) {
    scene.items.push_back(SceneItem{
        .data = to_render_data(mesh.terrain.positions,
                               mesh.terrain.normals,
                               mesh.terrain.indices,
                               std::array<float, 4>{0.28F, 0.42F, 0.20F, 1.0F}),
        .surface = SurfaceKind::Grass,
    });
    grow_bounds(scene.bounds, mesh.terrain.positions);
  }
  // Generated bridge solids (p5-s3, #233): the deck/piers/abutments/guardrails,
  // drawn a concrete grey. Not selectable this sprint (no entity id) — the
  // interactive span control is a follow-up; a pick lands on the road beneath.
  for (const BridgeMesh& span : mesh.bridges) {
    scene.items.push_back(SceneItem{
        .data = to_render_data(span.mesh.positions,
                               span.mesh.normals,
                               span.mesh.indices,
                               std::array<float, 4>{0.62F, 0.62F, 0.64F, 1.0F}),
        .surface = SurfaceKind::Concrete,
    });
    grow_bounds(scene.bounds, span.mesh.positions);
  }
  for (const ObjectInstance& instance : mesh.objects) {
    append_object_items(instance, scene);
  }
  for (const SignalInstance& instance : mesh.signal_instances) {
    append_signal_items(instance, scene);
  }
  return scene;
}

Scene build_object_scene(const NetworkMesh& mesh, std::span<const RoadId> roads) {
  const auto owned = [&roads](RoadId road) {
    return std::ranges::find(roads, road) != roads.end();
  };
  Scene scene;
  for (const ObjectInstance& instance : mesh.objects) {
    if (owned(instance.road)) {
      append_object_items(instance, scene);
    }
  }
  for (const SignalInstance& instance : mesh.signal_instances) {
    if (owned(instance.road)) {
      append_signal_items(instance, scene);
    }
  }
  return scene;
}

// --- Imported reference layers (p7-s2, #242) -------------------------------

RenderMeshData underlay_quad(const std::array<double, 4>& extent, float z) {
  RenderMeshData data;
  data.kind = PrimitiveKind::Triangles;
  // Counter-clockwise seen from +Z, so the quad faces up under the default
  // winding — a back-facing underlay is invisible and reads as a failed import.
  const auto x0 = static_cast<float>(extent[0]);
  const auto y0 = static_cast<float>(extent[1]);
  const auto x1 = static_cast<float>(extent[2]);
  const auto y1 = static_cast<float>(extent[3]);
  data.positions = {x0, y0, z, x1, y0, z, x1, y1, z, x0, y1, z};
  data.normals = {0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 1.0F};
  // Row 0 of the image is its NORTH edge, which is max-y here — so v runs 1 at
  // the bottom to 0 at the top. Getting this the other way up flips every
  // imported orthophoto, which looks plausible until you compare it to a road.
  data.uvs = {0.0F, 1.0F, 1.0F, 1.0F, 1.0F, 0.0F, 0.0F, 0.0F};
  data.indices = {0, 1, 2, 0, 2, 3};
  data.color = {1.0F, 1.0F, 1.0F, 1.0F};
  return data;
}

RenderMeshData cloud_points(const lidar::PointCloud& cloud, float z_min, float z_max) {
  RenderMeshData data;
  data.kind = PrimitiveKind::Points;
  data.color = {1.0F, 1.0F, 1.0F, 1.0F};
  if (cloud.empty()) {
    return data;
  }

  // ★ POSITIONS ARE COPIED VERBATIM, IN THE CLOUD'S OWN FRAME. The translation
  // to world travels as the draw's single InstanceData model matrix. Adding
  // cloud.origin here instead would put a UTM-magnitude value back into a
  // float and undo the whole point of the offset representation.
  data.positions = cloud.xyz;

  // The renderer's upload() returns a null handle for a mesh with no indices,
  // so points need a trivial 0..N-1 buffer. Four bytes a point, and the reason
  // kMaxCloudPoints is a RENDER budget rather than a parse one.
  const std::size_t count = cloud.size();
  data.indices.resize(count);
  for (std::size_t i = 0; i < count; ++i) {
    data.indices[i] = static_cast<std::uint32_t>(i);
  }

  // uv.x is the height ramp coordinate; uv.y is unused but the vertex format is
  // interleaved and fixed, so it is written rather than omitted.
  const float span = z_max - z_min;
  // A cloud of one elevation (a flat car park, or a single scan line) has no
  // range to normalise over. Mid-ramp is the honest answer; dividing would be
  // a division by zero rendered as a NaN-coloured cloud.
  const bool has_range = span > 1e-6F;
  data.uvs.resize(count * 2);
  for (std::size_t i = 0; i < count; ++i) {
    const float z = cloud.xyz[(i * 3) + 2] + static_cast<float>(cloud.origin[2]);
    const float t = has_range ? (z - z_min) / span : 0.5F;
    data.uvs[i * 2] = std::clamp(t, 0.0F, 1.0F);
    data.uvs[(i * 2) + 1] = 0.5F;
  }

  // Normals are unused: Material::unlit is set by the caller and the fragment
  // shader's u_lit is 0 for anything that is not Triangles. Leaving them empty
  // saves 12 bytes a point over writing a placeholder.
  return data;
}

TextureData cloud_ramp_texture() {
  // Low ground cool, high ground warm — the convention every DEM viewer uses,
  // so a cloud reads the same way as the terrain it will become.
  TextureData texture;
  texture.width = 256;
  texture.height = 1;
  texture.wrap = TextureWrap::ClampToEdge;
  texture.rgba.resize(static_cast<std::size_t>(texture.width) * 4);
  for (int i = 0; i < texture.width; ++i) {
    const float t = static_cast<float>(i) / static_cast<float>(texture.width - 1);
    const auto at = static_cast<std::size_t>(i) * 4;
    texture.rgba[at] = static_cast<std::uint8_t>(std::lround(40.0F + (215.0F * t)));
    texture.rgba[at + 1] = static_cast<std::uint8_t>(std::lround(90.0F + (110.0F * t)));
    texture.rgba[at + 2] = static_cast<std::uint8_t>(std::lround(190.0F - (140.0F * t)));
    texture.rgba[at + 3] = 255;
  }
  return texture;
}

RenderMeshData
underlay_lines(const gis::GisVectorLayer& layer, float z, const std::array<float, 4>& color) {
  RenderMeshData data;
  data.kind = PrimitiveKind::Lines;
  data.color = color;

  const auto push = [&data, z](const std::array<double, 2>& v) {
    data.positions.push_back(static_cast<float>(v[0]));
    data.positions.push_back(static_cast<float>(v[1]));
    data.positions.push_back(z);
    return static_cast<std::uint32_t>((data.positions.size() / 3) - 1);
  };

  for (const gis::GisFeature& feature : layer.features) {
    if (feature.geometry == gis::GisFeature::Geometry::Point) {
      // A GL point of width 1 is invisible at most zooms, and a point layer
      // that draws nothing reads as an import that silently failed. A small
      // fixed-size cross is legible and unambiguous.
      constexpr float kArm = 1.0F;
      for (const std::array<double, 2>& v : feature.vertices) {
        const auto x = static_cast<float>(v[0]);
        const auto y = static_cast<float>(v[1]);
        const auto base = static_cast<std::uint32_t>(data.positions.size() / 3);
        data.positions.insert(data.positions.end(),
                              {x - kArm, y, z, x + kArm, y, z, x, y - kArm, z, x, y + kArm, z});
        data.indices.insert(data.indices.end(), {base, base + 1, base + 2, base + 3});
      }
      continue;
    }

    const bool closed = feature.geometry == gis::GisFeature::Geometry::Polygon;
    for (std::size_t ring = 0; ring < feature.ring_starts.size(); ++ring) {
      const std::size_t begin = feature.ring_starts[ring];
      const std::size_t end = ring + 1 < feature.ring_starts.size() ? feature.ring_starts[ring + 1]
                                                                    : feature.vertices.size();
      if (end <= begin + 1) {
        continue;
      }
      std::uint32_t first = 0;
      std::uint32_t previous = 0;
      for (std::size_t i = begin; i < end; ++i) {
        const std::uint32_t index = push(feature.vertices[i]);
        if (i == begin) {
          first = index;
        } else {
          data.indices.push_back(previous);
          data.indices.push_back(index);
        }
        previous = index;
      }
      // Shapefile rings usually repeat their first point and GeoJSON's always
      // do, so only close a ring that has not closed itself — otherwise every
      // polygon gets a zero-length segment.
      if (closed && feature.vertices[begin] != feature.vertices[end - 1]) {
        data.indices.push_back(previous);
        data.indices.push_back(first);
      }
    }
  }
  return data;
}

float underlay_z(const SceneBounds& bounds, std::size_t index) {
  // Just above the procedural ground so an underlay hides the grass it covers,
  // and 1 mm per layer so a stack is ordered rather than z-fighting.
  return ground_base_z(bounds) + 0.005F + (static_cast<float>(index) * 0.001F);
}

// --- scenario actors (p8-s2, #246) ------------------------------------------

InstanceData actor_transform(const std::array<double, 3>& position,
                             double heading,
                             const osc::BoundingBox& box) {
  // Column-major mat4 mapping the unit cube onto the actor's OWN frame, which
  // is OpenSCENARIO's and the kernel's alike: +x longitudinal (forward), +y
  // lateral (left), +z up.
  //
  //   col0 = length * forward   forward = ( cos h, sin h, 0)
  //   col1 = width  * left      left    = (-sin h, cos h, 0)
  //   col2 = height * up
  //   col3 = position + the centre offset ROTATED into the same frame
  //
  // ★ TWO THINGS HERE ARE EASY TO GET WRONG AND BOTH RENDER CONVINCINGLY.
  //
  // First, LENGTH GOES ON THE FORWARD AXIS. Putting width there yields a car
  // 2.13 m long and 5.79 m wide — a box of exactly the right volume, sitting
  // across its lane instead of along it. A test that only checks the three
  // scale factors are present passes on that; it takes a test that knows which
  // axis is which to catch it.
  //
  // Second, THE CENTRE OFFSET IS ROTATED. An entity's reference point is the
  // centre of its rear axle, so `center_x` pushes the body FORWARD along the
  // actor's own heading. Adding it to the world x unrotated pushes every actor
  // east instead — indistinguishable from correct for an actor that happens to
  // face +x, and wrong for every other one.
  const auto c = static_cast<float>(std::cos(heading));
  const auto s = static_cast<float>(std::sin(heading));
  const auto l = static_cast<float>(box.length);
  const auto w = static_cast<float>(box.width);
  const auto h = static_cast<float>(box.height);

  const double ox = (box.center_x * std::cos(heading)) - (box.center_y * std::sin(heading));
  const double oy = (box.center_x * std::sin(heading)) + (box.center_y * std::cos(heading));

  const auto px = static_cast<float>(position[0] + ox);
  const auto py = static_cast<float>(position[1] + oy);
  const auto pz = static_cast<float>(position[2] + box.center_z);

  return InstanceData{{l * c,
                       l * s,
                       0.0F,
                       0.0F, //
                       -w * s,
                       w * c,
                       0.0F,
                       0.0F, //
                       0.0F,
                       0.0F,
                       h,
                       0.0F, //
                       px,
                       py,
                       pz,
                       1.0F}};
}

RenderMeshData actor_box_mesh() {
  // A unit cube on [-0.5, 0.5]^3, with FLAT per-face normals — so the six faces
  // shade distinctly and the box reads as a solid rather than as a smooth blob.
  // Each face gets its own four vertices for that reason; sharing eight corner
  // vertices would average the normals and lose every edge.
  RenderMeshData mesh;
  mesh.color = {0.85F, 0.45F, 0.15F, 1.0F}; // a warm accent, distinct from props
  mesh.kind = PrimitiveKind::Triangles;

  struct Face {
    std::array<float, 3> normal;
    std::array<std::array<float, 3>, 4> corners;
  };

  constexpr float k = 0.5F;
  const std::array<Face, 6> faces{{
      {{0, 0, 1}, {{{-k, -k, k}, {k, -k, k}, {k, k, k}, {-k, k, k}}}},      // top
      {{0, 0, -1}, {{{-k, k, -k}, {k, k, -k}, {k, -k, -k}, {-k, -k, -k}}}}, // bottom
      {{0, 1, 0}, {{{-k, k, -k}, {-k, k, k}, {k, k, k}, {k, k, -k}}}},      // front (+y)
      {{0, -1, 0}, {{{k, -k, -k}, {k, -k, k}, {-k, -k, k}, {-k, -k, -k}}}}, // back
      {{1, 0, 0}, {{{k, k, -k}, {k, k, k}, {k, -k, k}, {k, -k, -k}}}},      // right (+x)
      {{-1, 0, 0}, {{{-k, -k, -k}, {-k, -k, k}, {-k, k, k}, {-k, k, -k}}}}, // left
  }};

  for (const Face& face : faces) {
    const auto base = static_cast<std::uint32_t>(mesh.positions.size() / 3);
    for (const std::array<float, 3>& corner : face.corners) {
      mesh.positions.insert(mesh.positions.end(), corner.begin(), corner.end());
      mesh.normals.insert(mesh.normals.end(), face.normal.begin(), face.normal.end());
    }
    for (const std::uint32_t offset : {0U, 1U, 2U, 0U, 2U, 3U}) {
      mesh.indices.push_back(base + offset);
    }
  }
  return mesh;
}

void append_scenario_actors(const osc::Scenario& scenario,
                            const RoadNetwork& network,
                            Scene& scene) {
  // One batch for every actor: they all instance the same unit cube, and the
  // per-instance transform carries the dimensions. Created lazily so a scene
  // with no actors uploads nothing at all.
  ScenePropBatch* batch = nullptr;

  for (const osc::Private& entry : scenario.storyboard.init.actions.privates) {
    // The entity this <Private> places, and its box. An entityRef naming no
    // entity is refused by the writer, so it cannot reach a saved file — but it
    // CAN exist mid-edit, and drawing a box for it would be inventing content.
    const osc::BoundingBox* box = nullptr;
    for (const osc::ScenarioObject& object : scenario.entities.scenario_objects) {
      if (object.name != entry.entity_ref) {
        continue;
      }
      if (const auto* vehicle = std::get_if<osc::Vehicle>(&object.entity_object)) {
        box = &vehicle->bounding_box;
      } else if (const auto* pedestrian = std::get_if<osc::Pedestrian>(&object.entity_object)) {
        box = &pedestrian->bounding_box;
      }
      break;
    }
    if (box == nullptr) {
      continue; // a catalog reference or a MiscObject — nothing to size a box from
    }

    for (const osc::PrivateAction& action : entry.actions) {
      if (!action.teleport.has_value()) {
        continue;
      }
      const auto* lane = std::get_if<osc::LanePosition>(&action.teleport->position);
      if (lane == nullptr) {
        continue; // a world/road position is not drawn yet — p8-s3's
      }
      const std::optional<ActorPose> pose = actor_world_pose(network, *lane);
      if (!pose.has_value()) {
        // The road this actor names is gone. SKIPPED, not drawn at the origin:
        // "it is not there" is honest, "it is silently somewhere wrong" is not.
        continue;
      }

      if (batch == nullptr) {
        scene.prop_batches.push_back(ScenePropBatch{
            .model_id = "rm:actor_box", .parts = {actor_box_mesh()}, .instances = {}});
        batch = &scene.prop_batches.back();
      }
      batch->instances.push_back(
          ScenePropInstance{.road = {},
                            .object = {},
                            .signal = {},
                            .actor = entry.entity_ref,
                            .transform = actor_transform(pose->position, pose->heading, *box)});
      // Half the longest dimension is enough padding for any heading.
      const auto reach = static_cast<float>(std::max({box->width, box->length, box->height}) / 2.0);
      for (int axis = 0; axis < 3; ++axis) {
        const auto v = static_cast<float>(pose->position[static_cast<std::size_t>(axis)]);
        scene.bounds.lo[static_cast<std::size_t>(axis)] =
            std::min(scene.bounds.lo[static_cast<std::size_t>(axis)], v - reach);
        scene.bounds.hi[static_cast<std::size_t>(axis)] =
            std::max(scene.bounds.hi[static_cast<std::size_t>(axis)], v + reach);
      }
      break; // one box per entity, from its first lane-positioned teleport
    }
  }
}

} // namespace roadmaker::editor
