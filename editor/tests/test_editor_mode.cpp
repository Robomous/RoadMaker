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

// Map <-> Scenario mode (p8-s2, issue #246) — the issue's own acceptance:
// "mode switch preserves both documents".
//
// ★ THE GATE IS DERIVED FROM THE REGISTRY, NOT HAND-WRITTEN, and that is what
// these tests actually protect. Every tool declares its toolbar group and
// toolbar_violations() fails the build for one that does not, so gating on
// toolbar_tab_of() means a tool added by a later pillar is gated correctly the
// day it lands. A hand-written list would silently leave it enabled in both
// modes — and nothing would notice, because a tool that is merely available
// when it should not be still works.
//
// NOTE there is no toolbar TAB to switch to: #377 replaced the tabbed toolbar
// with one flat row, so the mode enables and disables actions in place.

#include <gtest/gtest.h>

#include <algorithm>
#include <string_view>
#include <vector>

#include "app/shortcut_registry.hpp"
#include "document/editor_mode.hpp"

namespace roadmaker::editor {
namespace {

using shortcuts::Id;
using shortcuts::ToolbarTab;

/// Every id that carries a toolbar group, split by the tab it lives in. This
/// mirrors what apply_editor_mode() walks, so the tests reason about the same
/// classification the implementation does.
std::vector<Id> ids_in(ToolbarTab want) {
  std::vector<Id> found;
  for (int raw = 0; raw < static_cast<int>(Id::kIdCount); ++raw) {
    const auto id = static_cast<Id>(raw);
    if (shortcuts::entry(id).toolbar_group == nullptr) {
      continue;
    }
    if (shortcuts::toolbar_tab_of(id) == want) {
      found.push_back(id);
    }
  }
  return found;
}

} // namespace

TEST(EditorMode, TheKeyRoundTripsAndAnUnknownOneDegradesToMap) {
  for (const EditorMode mode : {EditorMode::Map, EditorMode::Scenario}) {
    EXPECT_TRUE(is_known_editor_mode_key(editor_mode_key(mode)));
    EXPECT_EQ(editor_mode_from_key(editor_mode_key(mode)), mode);
  }
  // A sidecar from a NEWER build must never block a load.
  EXPECT_FALSE(is_known_editor_mode_key("storyboard"));
  EXPECT_EQ(editor_mode_from_key("storyboard"), EditorMode::Map);
  EXPECT_EQ(editor_mode_from_key(""), EditorMode::Map);
}

TEST(EditorMode, MapIsTheDefaultSoEveryPreP8SceneOpensUnchanged) {
  // Absent means Map. Every scene written before p8-s2 is that scene, and a
  // plain .xodr always will be.
  EXPECT_EQ(EditorMode{}, EditorMode::Map);
  EXPECT_EQ(static_cast<int>(EditorMode::Map), 0);
}

TEST(EditorMode, TheScenarioTabHoldsSomethingToGateOn) {
  // If this is ever empty the gate silently becomes a no-op: every road tool
  // would be disabled in Scenario mode with nothing enabled in their place.
  // Bound to a NAMED vector: std::ranges::find over a temporary yields
  // std::ranges::dangling, and comparing it against the end() of a SECOND
  // temporary would be comparing iterators into two different containers.
  const std::vector<Id> scenario = ids_in(ToolbarTab::kScenario);
  EXPECT_FALSE(scenario.empty());
  EXPECT_NE(std::ranges::find(scenario, Id::ToolActorPlace), scenario.end());
}

TEST(EditorMode, EveryToolIsClassifiedSoTheGateCannotMissOne) {
  // ★ The property the derived gate rests on. A tool with no toolbar group
  // would be gated by neither branch and stay enabled in both modes.
  // toolbar_violations() already fails the build for that; this asserts the
  // consequence the mode switch depends on, so the link is explicit rather
  // than incidental.
  for (int raw = 0; raw < static_cast<int>(Id::kIdCount); ++raw) {
    const auto id = static_cast<Id>(raw);
    const shortcuts::Entry& entry = shortcuts::entry(id);
    if (std::string_view(entry.category) != "Tools") {
      continue;
    }
    EXPECT_NE(entry.toolbar_group, nullptr)
        << "a Tools row with no toolbar group is gated by neither mode";
  }
}

TEST(EditorMode, TheCoreStripIsNeverWithheldByEitherMode) {
  // Select, Move, Delete, File ops and framing must work in both modes — a mode
  // that took away Select would leave the user unable to inspect anything.
  const std::vector<Id> core = ids_in(ToolbarTab::kCore);
  EXPECT_FALSE(core.empty());
  for (const Id id : {Id::ToolSelect, Id::ToolMove, Id::ToolDelete}) {
    EXPECT_NE(std::ranges::find(core, id), core.end())
        << "a universal edit tool left the core strip and would now be mode-gated";
  }
}

TEST(EditorMode, MapAndScenarioToolSetsAreDisjointAndBothNonEmpty) {
  // The two branches of the gate must actually partition the tools: if the
  // scenario set were a subset of the map set, switching mode would change
  // nothing observable.
  const std::vector<Id> scenario = ids_in(ToolbarTab::kScenario);
  ASSERT_FALSE(scenario.empty());

  std::vector<Id> map_side;
  for (const ToolbarTab tab : {ToolbarTab::kRoadsLanes,
                               ToolbarTab::kMarkings,
                               ToolbarTab::kProps,
                               ToolbarTab::kTerrain,
                               ToolbarTab::kSignals}) {
    const std::vector<Id> here = ids_in(tab);
    map_side.insert(map_side.end(), here.begin(), here.end());
  }
  ASSERT_FALSE(map_side.empty());

  for (const Id id : scenario) {
    EXPECT_EQ(std::ranges::find(map_side, id), map_side.end())
        << "a tool is classified as both map-side and scenario-side";
  }
}

} // namespace roadmaker::editor
