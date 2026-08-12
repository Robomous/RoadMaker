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

#pragma once

// Internal (non-installed): every OpenDRIVE enum spelling, one table per enum,
// shared by xodr/reader.cpp and xodr/writer.cpp. Before #563 each of these was
// an if-chain in the reader AND a switch in the writer, in different files —
// the shape #476 came in, where the two drifted and the writer re-spelled
// parsed enums into different semantics on save.
//
// One row per accepted spelling; the FIRST row naming a value is what the
// writer emits, later rows with that value are read-only aliases.
//
// THREE THINGS THAT MUST NEVER BECOME A ROW:
//   1. `*::Other` — the unmodeled bucket. Its lossy write stand-in is
//      `name_of`'s `fallback` argument instead. As a row, "solid" would parse
//      back as `RoadMarkType::Other`: #476, reintroduced.
//   2. The empty string — "attribute absent" is per-attribute policy, and it
//      belongs at the call site that cites the paragraph saying so.
//   3. Any guess for an unknown spelling. `value_of` returns nullopt; a default
//      buried here is a default nobody reviews.
//
// Reference: ASAM OpenDRIVE 1.9.0 §11.7, §11.9 Tables 47-48, §13.2 Table 92,
// §10.2 Table 23. Local copies: third_party/asam/.

#include "roadmaker/road/lane.hpp"
#include "roadmaker/road/object.hpp"
#include "roadmaker/road/traffic_rule.hpp"

#include <array>
#include <cstddef>
#include <optional>
#include <string_view>
#include <utility>

namespace roadmaker::xodr_names {

/// A spelling table: enum value -> the ASCII spelling(s) OpenDRIVE uses.
/// `const char*` rather than `string_view` so `name_of` drops straight into
/// pugixml's `set_value` with no null-termination question.
template <class E, std::size_t N>
using Table = std::array<std::pair<E, const char*>, N>;

/// The spelling the writer emits for `value`: the first row naming it, or
/// `fallback` when the value has no faithful spelling (see rule 1 above).
template <class E, std::size_t N>
[[nodiscard]] constexpr const char*
name_of(const Table<E, N>& table, E value, const char* fallback) {
  for (const auto& [candidate, name] : table) {
    if (candidate == value) {
      return name;
    }
  }
  return fallback;
}

/// The value `spelling` names, or nullopt when no row matches. Never guesses —
/// empty-string and unknown-spelling policy belongs to the caller (rules 2, 3).
template <class E, std::size_t N>
[[nodiscard]] constexpr std::optional<E> value_of(const Table<E, N>& table,
                                                  std::string_view spelling) {
  for (const auto& [value, name] : table) {
    if (spelling == name) {
      return value;
    }
  }
  return std::nullopt;
}

/// e_laneType (§11.7, Table 43). `Other` is absent by rule 1; the writer's
/// fallback for it is "none".
inline constexpr Table<LaneType, 12> kLaneType{{
    {LaneType::Driving, "driving"},
    {LaneType::Stop, "stop"},
    {LaneType::Shoulder, "shoulder"},
    {LaneType::Biking, "biking"},
    {LaneType::Sidewalk, "sidewalk"},
    {LaneType::Border, "border"},
    {LaneType::Restricted, "restricted"},
    {LaneType::Parking, "parking"},
    {LaneType::Median, "median"},
    {LaneType::Curb, "curb"},
    {LaneType::None, "none"},
    // Read-only alias. "walking" is the pre-1.6 spelling of "sidewalk" and is
    // DEPRECATED — accepted on read, never emitted, which is why it sorts after
    // the canonical row rather than replacing it (#476).
    {LaneType::Sidewalk, "walking"},
}};

/// e_roadMarkType (§11.9, Table 47). The multi-token spellings carry their
/// single ASCII space verbatim — it is part of the enumerator, not formatting.
/// `Other` is absent by rule 1; the writer's fallback for it is "solid".
inline constexpr Table<RoadMarkType, 7> kRoadMarkType{{
    {RoadMarkType::None, "none"},
    {RoadMarkType::Solid, "solid"},
    {RoadMarkType::Broken, "broken"},
    {RoadMarkType::SolidSolid, "solid solid"},
    {RoadMarkType::SolidBroken, "solid broken"},
    {RoadMarkType::BrokenSolid, "broken solid"},
    {RoadMarkType::BrokenBroken, "broken broken"},
}};

/// e_roadMarkColor (§11.9, Table 48). `Other` is absent by rule 1; the writer's
/// fallback for it is "standard".
inline constexpr Table<RoadMarkColor, 7> kRoadMarkColor{{
    {RoadMarkColor::Standard, "standard"},
    {RoadMarkColor::White, "white"},
    {RoadMarkColor::Yellow, "yellow"},
    {RoadMarkColor::Red, "red"},
    {RoadMarkColor::Blue, "blue"},
    {RoadMarkColor::Green, "green"},
    {RoadMarkColor::Orange, "orange"},
}};

/// e_lane_direction (§11.7, 1.8.0+). Every value is spellable, so the writer's
/// fallback is unreachable and the reader propagates nullopt for an unknown.
inline constexpr Table<LaneDirection, 3> kLaneDirection{{
    {LaneDirection::Standard, "standard"},
    {LaneDirection::Reversed, "reversed"},
    {LaneDirection::Both, "both"},
}};

/// e_objectType (§13.2, Table 92). `Other` is absent by rule 1; the writer's
/// fallback for it is "none" — the exotic spelling lives in `Object::type_str`.
inline constexpr Table<ObjectType, 8> kObjectType{{
    {ObjectType::Crosswalk, "crosswalk"},
    {ObjectType::Tree, "tree"},
    {ObjectType::Vegetation, "vegetation"},
    {ObjectType::Pole, "pole"},
    {ObjectType::Barrier, "barrier"},
    {ObjectType::Building, "building"},
    {ObjectType::Obstacle, "obstacle"},
    {ObjectType::None, "none"},
}};

/// `@orientation` on `<object>` and `<signal>` (§13). Both readers used to
/// spell this test out by hand, identically (#563).
inline constexpr Table<ObjectOrientation, 3> kObjectOrientation{{
    {ObjectOrientation::Plus, "+"},
    {ObjectOrientation::Minus, "-"},
    {ObjectOrientation::None, "none"},
}};

/// e_trafficRule (§10.2, Table 23). Absent means RHT — the spec mandates it —
/// and that policy stays at the call site with the paragraph that says so.
inline constexpr Table<TrafficRule, 2> kTrafficRule{{
    {TrafficRule::RightHandTraffic, "RHT"},
    {TrafficRule::LeftHandTraffic, "LHT"},
}};

} // namespace roadmaker::xodr_names
