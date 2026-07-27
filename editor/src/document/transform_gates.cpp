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

#include "document/transform_gates.hpp"

#include "roadmaker/road/junction.hpp"
#include "roadmaker/road/network.hpp"
#include "roadmaker/road/road.hpp"

#include <QObject>
#include <algorithm>
#include <variant>
#include <vector>

namespace roadmaker::editor {

namespace {

/// The road id an end links to, or nullptr when the end is unlinked or points
/// at a junction rather than a road.
const RoadId* linked_road(const std::optional<RoadLink>& link) {
  return link.has_value() ? std::get_if<RoadId>(&link->target) : nullptr;
}

} // namespace

std::optional<QString> junction_transform_refusal(const RoadNetwork& network,
                                                  std::span<const RoadId> roads,
                                                  TransformKind kind) {
  for (const RoadId road_id : roads) {
    const std::vector<JunctionId> touched = junctions_touching(network, road_id);
    if (touched.empty()) {
      continue;
    }
    const Road* road = network.road(road_id);
    const Junction* junction = network.junction(touched.front());
    const QString road_name = road != nullptr ? QString::fromStdString(road->odr_id) : QString();
    const QString junction_name =
        junction != nullptr ? QString::fromStdString(junction->odr_id) : QString();
    // "moved" / "rotated" tracks the kernel's own two messages, so the editor
    // never tells the user something the kernel would have phrased differently.
    return kind == TransformKind::Rotate
               ? QObject::tr("Road %1 belongs to Junction %2 — junction roads can't be rotated. "
                             "Delete the junction or move its free end nodes instead.")
                     .arg(road_name, junction_name)
               : QObject::tr("Road %1 belongs to Junction %2 — junction roads can't be moved. "
                             "Delete the junction or move its free end nodes instead.")
                     .arg(road_name, junction_name);
  }
  return std::nullopt;
}

bool transform_breaks_links(const RoadNetwork& network,
                            std::span<const RoadId> roads,
                            TransformKind kind) {
  const auto in_set = [roads](RoadId id) { return std::ranges::find(roads, id) != roads.end(); };
  const auto breaks = [&](const std::optional<RoadLink>& link) {
    const RoadId* target = linked_road(link);
    if (target == nullptr) {
      return false;
    }
    // Rotate severs every road link; Translate only those leaving the set.
    return kind == TransformKind::Rotate || !in_set(*target);
  };
  return std::ranges::any_of(roads, [&](RoadId road_id) {
    const Road* road = network.road(road_id);
    return road != nullptr && (breaks(road->predecessor) || breaks(road->successor));
  });
}

} // namespace roadmaker::editor
