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

#include "roadmaker/road/road_type.hpp"

#include <algorithm>
#include <array>

namespace roadmaker {
namespace {

/// `e_roadType` (ASAM OpenDRIVE §16 A.6.2, Table 194). **Byte-identical to
/// 1.8.1's A.6.3 Table 188** — same 13 literals in the same order, diffed
/// rather than assumed — so this list is not revision-conditional.
constexpr std::array<std::string_view, 13> kRoadTypes{{
    "bicycle",
    "lowSpeed",
    "motorway",
    "pedestrian",
    "rural",
    "townArterial",
    "townCollector",
    "townExpressway",
    "townLocal",
    "townPlayStreet",
    "townPrivate",
    "town",
    "unknown",
}};

/// `e_unitSpeed` (§16 A.1.5, Table 158).
constexpr std::array<std::string_view, 3> kSpeedUnits{{"km/h", "m/s", "mph"}};

} // namespace

bool is_known_road_type(std::string_view type) {
  return std::ranges::find(kRoadTypes, type) != kRoadTypes.end();
}

bool is_known_speed_unit(std::string_view unit) {
  return std::ranges::find(kSpeedUnits, unit) != kSpeedUnits.end();
}

RoadTypeRecord road_type_for_class(defaults::RoadClass road_class,
                                   const RoadTypeRecord* carry_over) {
  // Start from what the road already said, so a restyle rewrites only the one
  // thing the class actually determines. `@country`, the `<speed>` and any
  // preserved extras are facts about THIS ROAD that no class knows.
  RoadTypeRecord record = carry_over != nullptr ? *carry_over : RoadTypeRecord{};
  record.s = 0.0;
  record.type = defaults::road_type_name(road_class);
  return record;
}

} // namespace roadmaker
