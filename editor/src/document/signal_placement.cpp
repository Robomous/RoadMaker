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

#include "document/signal_placement.hpp"

#include "roadmaker/assets/sign_catalog.hpp"
#include "roadmaker/road/network.hpp"
#include "roadmaker/road/object.hpp"
#include "roadmaker/road/road.hpp"

#include <array>
#include <set>
#include <string>
#include <utility>

#include "viewport/picking.hpp" // station_to_world

namespace roadmaker::editor {

std::optional<RoadStation> nearest_signal_station(const RoadNetwork& network, double x, double y) {
  return nearest_road_station(network, x, y, kSignalSnapThreshold);
}

std::optional<std::array<double, 3>> signal_world(const RoadNetwork& network, SignalId id) {
  const Signal* signal = network.signal(id);
  if (signal == nullptr) {
    return std::nullopt;
  }
  const Road* road = network.road(signal->road);
  if (road == nullptr || road->plan_view.empty()) {
    return std::nullopt;
  }
  const std::array<double, 2> plan = station_to_world(road->plan_view, signal->s, signal->t);
  return std::array<double, 3>{plan[0], plan[1], signal->z_offset};
}

std::string next_signal_odr_id(const RoadNetwork& network) {
  std::set<std::string> taken;
  network.for_each_signal([&](SignalId, const Signal& signal) { taken.insert(signal.odr_id); });
  int candidate = 1;
  while (taken.contains(std::to_string(candidate))) {
    ++candidate;
  }
  return std::to_string(candidate);
}

bool is_signal_asset(const LibraryItem& item) {
  return item.kind == LibraryItem::Kind::Signal && !item.signal.isEmpty();
}

bool authors_editable_legend(const LibraryItem& item) {
  if (!is_signal_asset(item)) {
    return false;
  }
  const signs::SignDef* def = signs::find_by_key(item.signal.toStdString());
  return def != nullptr && def->legend_editable;
}

Signal make_signal(const QString& tag, std::string odr_id, double s, double t) {
  Signal signal;
  signal.odr_id = std::move(odr_id);
  signal.orientation = ObjectOrientation::Plus;
  signal.s = s;
  signal.t = t;

  // The shipped sign catalogue (roadmaker::signs, spec §1.4) IS the identity:
  // this function transcribes an entry, it does not decide one. An unknown tag
  // can only come from a hand-edited manifest, so it falls back to the pack's
  // stop sign rather than authoring a signal with no type at all.
  const signs::SignDef* def = signs::find_by_key(tag.toStdString());
  if (def == nullptr) {
    def = signs::find_by_key("us.r1_1");
  }
  if (def == nullptr) {
    return signal;
  }
  signal.dynamic = def->dynamic;
  signal.type = std::string(def->type);
  signal.subtype = std::string(def->subtype);
  signal.country = std::string(def->country);
  signal.text = std::string(def->default_text);
  // §14.1 recommends @height/@width "for proper representation"; §1.4 supplies
  // both. A street-name blade's length follows its legend (face_width 0), so it
  // declares no @width — the spec's "length fits text".
  if (def->face_height > 0.0) {
    signal.height = def->face_height;
  }
  if (def->face_width > 0.0) {
    signal.width = def->face_width;
  }
  // §14.1 Table 122: @unit is mandatory exactly when @value is given. The
  // catalogue holds that invariant (test_defaults_registry asserts it), so the
  // two move together here too.
  if (def->default_value.has_value()) {
    signal.value = def->default_value;
    signal.unit = std::string(def->unit);
  }
  return signal;
}

} // namespace roadmaker::editor
