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

// What the editor is currently authoring (p8-s2, issue #246): the road network,
// or the scenario that plays on top of it.
//
// A MODE, NOT A SEPARATE DOCUMENT WINDOW. Both documents stay loaded and both
// stay live — switching to Scenario mode does not close the map, and switching
// back returns to it with the undo history intact (GW-6 step 1). What changes
// is which TOOLS are available: the road network stays visible and pickable for
// reference, and only road EDITING is withheld.
//
// The mode is per-scene Layer-2 state, stored in `<scene>.rmscene.json`
// (document/scene_sidecar.hpp). The active scenario FILE is not stored at all:
// ADR-0014 §9 stem-matches `town.xosc` to `town.xodr`, so there is nothing to
// remember. That pair of answers is what the #246 review comment asked for.

#include <cstdint>
#include <string_view>

namespace roadmaker::editor {

enum class EditorMode : std::uint8_t {
  Map = 0,  ///< authoring the road network (the default, and every pre-p8 scene)
  Scenario, ///< authoring the scenario that plays on it
};

/// The spelling persisted in the sidecar. Lowercase and stable — it is written
/// to a file, so it is never derived from a UI label.
[[nodiscard]] constexpr std::string_view editor_mode_key(EditorMode mode) {
  return mode == EditorMode::Scenario ? "scenario" : "map";
}

/// Parses a persisted spelling. An unknown one yields `Map` rather than
/// failing: a sidecar written by a newer build must degrade to a working
/// default, never block a load (the fmt-s1 contract). Callers that want to warn
/// about it compare the round trip.
[[nodiscard]] constexpr EditorMode editor_mode_from_key(std::string_view key) {
  return key == "scenario" ? EditorMode::Scenario : EditorMode::Map;
}

/// True when `key` is a spelling this build understands — the check a reader
/// uses to decide whether to warn before degrading to `Map`.
[[nodiscard]] constexpr bool is_known_editor_mode_key(std::string_view key) {
  return key == "map" || key == "scenario";
}

} // namespace roadmaker::editor
