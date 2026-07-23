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

#include "viewport/framing.hpp"

#include "roadmaker/assets/prop_library.hpp"

#include <algorithm>

namespace roadmaker::editor {

namespace {

void grow(SceneBounds& bounds, double x, double y, double z) {
  const std::array<double, 3> point{x, y, z};
  for (std::size_t i = 0; i < 3; ++i) {
    bounds.lo[i] = std::min(bounds.lo[i], static_cast<float>(point[i]));
    bounds.hi[i] = std::max(bounds.hi[i], static_cast<float>(point[i]));
  }
}

/// Grows `bounds` by every xyz triple of a flat position array.
void grow_positions(SceneBounds& bounds, const std::vector<double>& positions) {
  for (std::size_t i = 0; i + 2 < positions.size(); i += 3) {
    grow(bounds, positions[i], positions[i + 1], positions[i + 2]);
  }
}

/// Grows `bounds` by only the vertices `indices` reference — how a single lane
/// is isolated from the road mesh it shares vertices with.
void grow_indexed(SceneBounds& bounds,
                  const std::vector<double>& positions,
                  const std::vector<std::uint32_t>& indices) {
  for (const std::uint32_t index : indices) {
    const std::size_t base = static_cast<std::size_t>(index) * 3U;
    if (base + 2 < positions.size()) {
      grow(bounds, positions[base], positions[base + 1], positions[base + 2]);
    }
  }
}

/// Grows `bounds` by a prop/signal instance: its base position, expanded by the
/// bundled model's radius and height. Falls back to a small box when the model
/// is unknown, so an unrenderable instance still frames to something sane
/// rather than a zero-size point the camera would slam into.
void grow_instance(SceneBounds& bounds,
                   const std::array<double, 3>& position,
                   std::string_view model_id,
                   double scale) {
  constexpr double kUnknownModelExtent = 1.0;
  double radius = kUnknownModelExtent;
  double height = kUnknownModelExtent;
  // An unknown model keeps its unscaled fallback box — it derives scale 1.0
  // anyway, and there is no model height for a ratio to mean anything against.
  if (const props::PropModel* model = props::model(model_id); model != nullptr) {
    radius = model->radius * scale;
    height = model->height * scale;
  }
  grow(bounds, position[0] - radius, position[1] - radius, position[2]);
  grow(bounds, position[0] + radius, position[1] + radius, position[2] + height);
}

} // namespace

SceneBounds selection_bounds(const NetworkMesh& mesh, std::span<const SelectionEntry> entries) {
  SceneBounds bounds;
  for (const SelectionEntry& entry : entries) {
    // Order matters: an object/signal entry also carries its owning road, so
    // the specific kinds must win or a selected prop would frame its road —
    // the bug this function exists to fix.
    if (entry.object.is_valid()) {
      for (const ObjectInstance& instance : mesh.objects) {
        if (instance.object == entry.object) {
          grow_instance(bounds, instance.position, instance.model_id, instance.scale);
        }
      }
      continue;
    }
    if (entry.signal.is_valid()) {
      for (const SignalInstance& instance : mesh.signal_instances) {
        if (instance.signal == entry.signal) {
          // Signals are not resizable (#335).
          grow_instance(bounds, instance.position, instance.model_id, 1.0);
        }
      }
      continue;
    }
    if (entry.junction.is_valid()) {
      for (const JunctionFloor& floor : mesh.junction_floors) {
        if (floor.junction == entry.junction) {
          grow_positions(bounds, floor.mesh.positions);
        }
      }
      continue;
    }
    if (entry.surface.is_valid()) {
      for (const SurfaceMesh& surface : mesh.surfaces) {
        if (surface.surface == entry.surface) {
          grow_positions(bounds, surface.mesh.positions);
        }
      }
      continue;
    }
    for (const RoadMesh& road : mesh.roads) {
      if (road.road != entry.road) {
        continue;
      }
      if (!entry.lane.is_valid()) {
        grow_positions(bounds, road.positions);
        break;
      }
      // A lane patch indexes into its road's shared vertex array, so only the
      // vertices its triangles touch belong to the lane.
      for (const RoadMesh::LanePatch& patch : road.lanes) {
        if (patch.lane == entry.lane) {
          grow_indexed(bounds, road.positions, patch.indices);
        }
      }
      break;
    }
  }
  return bounds;
}

} // namespace roadmaker::editor
