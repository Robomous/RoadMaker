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

// The toolbar is GENERATED from the action registry (p1-s5, issue #317), so
// these tests are what keeps the taxonomy honest: the categories are pinned,
// every tool must be categorized, and every Id must resolve to a QAction.
// Without the gate, the next pillar's tools would quietly land off-toolbar or
// re-flatten the rows — which is what #317 existed to stop.

#include <gtest/gtest.h>

#include <QSet>
#include <QUndoStack>
#include <array>
#include <string_view>
#include <vector>

#include "app/actions.hpp"
#include "app/shortcut_registry.hpp"

namespace roadmaker::editor {
namespace {

using shortcuts::Entry;
using shortcuts::Id;
using shortcuts::ToolbarGroup;
using shortcuts::ToolbarGroupLayout;
using shortcuts::ToolbarTab;

/// The ids of one group, in layout order.
std::vector<Id> ids_of(const std::vector<ToolbarGroupLayout>& layout, const char* name) {
  for (const ToolbarGroupLayout& group : layout) {
    if (QString::fromUtf8(group.group->name) == QString::fromUtf8(name)) {
      return group.ids;
    }
  }
  ADD_FAILURE() << "no toolbar group named '" << name << "'";
  return {};
}

// The taxonomy is a product decision (#317/#368), not an implementation detail:
// pin it so a reshuffle has to be deliberate. The reserved tabs are listed
// before they hold anything — that is the point of committing them.
TEST(ToolbarRegistry, TaxonomyIsFixed) {
  const std::array<std::pair<const char*, ToolbarTab>, 10> expected{{
      {"File", ToolbarTab::kCore},
      {"Edit", ToolbarTab::kCore},
      {"Roads", ToolbarTab::kRoadsLanes},
      {"Lanes", ToolbarTab::kRoadsLanes},
      {"Markings", ToolbarTab::kMarkings},
      {"Props", ToolbarTab::kProps},
      {"Terrain & Structures", ToolbarTab::kTerrain},
      {"Signals & Signs", ToolbarTab::kSignals},
      {"Scenario", ToolbarTab::kScenario},
      {"Library & View", ToolbarTab::kCore},
  }};

  const std::span<const ToolbarGroup> groups = shortcuts::toolbar_groups();
  ASSERT_EQ(groups.size(), expected.size());
  for (std::size_t i = 0; i < expected.size(); ++i) {
    EXPECT_EQ(QString::fromUtf8(groups[i].name), QString::fromUtf8(expected[i].first))
        << "group " << i;
    EXPECT_EQ(groups[i].tab, expected[i].second) << "group " << i << " is on the wrong tab";
  }
}

TEST(ToolbarRegistry, TableHasNoViolations) {
  const QStringList problems =
      shortcuts::toolbar_violations(shortcuts::table(), shortcuts::toolbar_groups());
  EXPECT_TRUE(problems.isEmpty()) << problems.join(QStringLiteral("\n")).toStdString();
}

// The gate itself. A new tool that forgets its toolbar_group is exactly the
// regression #317 exists to prevent, so prove the check fires — against a
// fixture, so the real table never has to be broken to test it.
TEST(ToolbarRegistry, UncategorizedToolIsDetected) {
  const std::array bad{
      Entry{.id = Id::ToolSelect, .category = "Tools", .description = "orphan tool"}};

  const QStringList problems = shortcuts::toolbar_violations(bad, shortcuts::toolbar_groups());
  ASSERT_FALSE(problems.isEmpty()) << "an uncategorized tool must be reported";
  EXPECT_TRUE(problems.join(QStringLiteral("\n")).contains(QStringLiteral("orphan tool")))
      << "the message must name the offending action";
}

TEST(ToolbarRegistry, UnknownGroupIsDetected) {
  const std::array bad{Entry{.id = Id::ToolSelect,
                             .category = "Tools",
                             .description = "misfiled tool",
                             .toolbar_group = "Bogus"}};

  const QStringList problems = shortcuts::toolbar_violations(bad, shortcuts::toolbar_groups());
  ASSERT_FALSE(problems.isEmpty()) << "a group outside the taxonomy must be reported";
  EXPECT_TRUE(problems.join(QStringLiteral("\n")).contains(QStringLiteral("Bogus")));
}

TEST(ToolbarRegistry, CollidingOrderIsDetected) {
  const std::array bad{Entry{.id = Id::ToolSelect,
                             .category = "Tools",
                             .description = "first tool",
                             .toolbar_group = "Edit",
                             .toolbar_order = 10},
                       Entry{.id = Id::ToolMove,
                             .category = "Tools",
                             .description = "second tool",
                             .toolbar_group = "Edit",
                             .toolbar_order = 10}};

  const QStringList problems = shortcuts::toolbar_violations(bad, shortcuts::toolbar_groups());
  ASSERT_FALSE(problems.isEmpty()) << "two actions in one slot must be reported";
  EXPECT_TRUE(problems.join(QStringLiteral("\n")).contains(QStringLiteral("second tool")));
}

// toolbar_violations only sees rows that ARE in the registry. A tool added to
// the action group with no Id at all would slip past it — this catches that.
TEST(ToolbarRegistry, EveryToolGroupActionIsOnTheToolbar) {
  QUndoStack stack;
  Actions actions(stack);

  QSet<const QAction*> on_toolbar;
  for (const Entry& row : shortcuts::table()) {
    if (row.toolbar_group != nullptr) {
      on_toolbar.insert(actions.action(row.id));
    }
  }

  const QList<QAction*> tools = actions.tool_group->actions();
  ASSERT_FALSE(tools.isEmpty());
  for (const QAction* tool : tools) {
    EXPECT_TRUE(on_toolbar.contains(tool))
        << "'" << tool->iconText().toStdString()
        << "' is an editing tool that never reaches a toolbar row — give it a "
           "shortcuts::Id and a toolbar_group";
  }
}

// The layout the issue specifies, per tab and group, in order. Reserved tabs
// are present-but-empty rather than absent: the taxonomy is committed now, the
// contents arrive later.
TEST(ToolbarRegistry, LayoutMatchesTheIssue) {
  // The persistent core strip: file ops, the universal edit tools, framing.
  const std::vector<ToolbarGroupLayout> core = shortcuts::toolbar_layout(ToolbarTab::kCore);
  ASSERT_EQ(core.size(), 3u);
  EXPECT_EQ(ids_of(core, "File"), (std::vector{Id::NewScene, Id::Open, Id::Save, Id::ExportGlb}));
  EXPECT_EQ(
      ids_of(core, "Edit"),
      (std::vector{Id::ToolSelect, Id::ToolMove, Id::ToolSplit, Id::ToolDelete, Id::MergeRoads}));
  EXPECT_EQ(ids_of(core, "Library & View"),
            (std::vector{Id::AddFromLibrary, Id::ResetCamera, Id::FrameSelection}));

  const std::vector<ToolbarGroupLayout> roads_lanes =
      shortcuts::toolbar_layout(ToolbarTab::kRoadsLanes);
  ASSERT_EQ(roads_lanes.size(), 2u);
  EXPECT_EQ(ids_of(roads_lanes, "Roads"),
            (std::vector{Id::ToolCreateRoad,
                         Id::ToolEditNodes,
                         Id::ToolCreateJunction,
                         Id::ToolCorner,
                         Id::ToolStopLine,
                         Id::ToolJunctionSpan,
                         Id::ToolJunctionSurface,
                         Id::ToolManeuver,
                         Id::ToolElevation}));
  EXPECT_EQ(ids_of(roads_lanes, "Lanes"),
            (std::vector{Id::ToolLaneProfile,
                         Id::ToolLaneAdd,
                         Id::ToolLaneForm,
                         Id::ToolLaneCarve,
                         Id::LaneWidthEditor}));

  EXPECT_EQ(ids_of(shortcuts::toolbar_layout(ToolbarTab::kMarkings), "Markings"),
            (std::vector{Id::ToolCrosswalk, Id::ToolMarkingPoint, Id::ToolMarkingCurve}));
  EXPECT_EQ(
      ids_of(shortcuts::toolbar_layout(ToolbarTab::kProps), "Props"),
      (std::vector{Id::ToolPropPoint, Id::ToolPropCurve, Id::ToolPropSpan, Id::ToolPropPolygon}));
  // Populated by p4-s7..s9 (#228/#229/#230): Signal tool, Phase editor, Sign.
  EXPECT_EQ(ids_of(shortcuts::toolbar_layout(ToolbarTab::kSignals), "Signals & Signs"),
            (std::vector{Id::ToolSignal, Id::SignalPhaseEditor, Id::ToolSign}));

  // Terrain stopped being reserved-empty when p5-s1 (#231) landed the Surface
  // tool in it; Scenario is still waiting on P8.
  EXPECT_EQ(ids_of(shortcuts::toolbar_layout(ToolbarTab::kTerrain), "Terrain & Structures"),
            (std::vector{Id::ToolSurface}));

  const std::vector<ToolbarGroupLayout> scenario = shortcuts::toolbar_layout(ToolbarTab::kScenario);
  ASSERT_EQ(scenario.size(), 1u);
  EXPECT_TRUE(scenario.front().ids.empty()) << "a reserved tab must render nothing yet";
}

// The tabs actually shown skip the core strip AND any empty reserved tab —
// Terrain/Scenario appear only once their pillar (P5/P8) lands its first tool.
TEST(ToolbarRegistry, ShownTabsSkipCoreAndEmptyReserved) {
  std::vector<QString> titles;
  for (const shortcuts::ToolbarTabInfo& info : shortcuts::toolbar_tabs()) {
    titles.push_back(QString::fromUtf8(info.title));
    EXPECT_NE(info.tab, ToolbarTab::kCore);
    EXPECT_NE(info.tab, ToolbarTab::kScenario) << "reserved-empty tab must stay hidden";
  }
  EXPECT_EQ(titles,
            (std::vector<QString>{QStringLiteral("Roads & Lanes"),
                                  QStringLiteral("Markings"),
                                  QStringLiteral("Props"),
                                  QStringLiteral("Terrain & Structures"),
                                  QStringLiteral("Signals & Signs")}));
}

// The persistent strip is what the user must never lose behind a tab. Pin the
// contract: these actions are always in the core strip, never a tab.
TEST(ToolbarRegistry, CoreStripAlwaysHoldsFileEditAndFraming) {
  QSet<Id> core_ids;
  for (const ToolbarGroupLayout& group : shortcuts::toolbar_layout(ToolbarTab::kCore)) {
    for (const Id id : group.ids) {
      core_ids.insert(id);
    }
  }
  for (const Id must : {Id::NewScene,
                        Id::Save,
                        Id::ToolSelect,
                        Id::ToolMove,
                        Id::ToolDelete,
                        Id::FrameSelection,
                        Id::ResetCamera,
                        Id::AddFromLibrary}) {
    EXPECT_TRUE(core_ids.contains(must))
        << "the core strip must always hold " << shortcuts::entry(must).description;
  }
}

// reveal-on-activation maps a tool to its tab (toolbar_tab_of): every tool must
// map to the tab whose layout actually contains it, exactly once — so a hidden
// tool always reveals the right section.
TEST(ToolbarRegistry, EveryToolMapsToTheTabThatHoldsIt) {
  for (const Entry& row : shortcuts::table()) {
    if (row.toolbar_group == nullptr ||
        std::string_view(row.category) != std::string_view("Tools")) {
      continue;
    }
    const ToolbarTab tab = shortcuts::toolbar_tab_of(row.id);
    int appearances = 0;
    for (const ToolbarGroupLayout& group : shortcuts::toolbar_layout(tab)) {
      for (const Id id : group.ids) {
        if (id == row.id) {
          ++appearances;
        }
      }
    }
    EXPECT_EQ(appearances, 1) << QString::fromUtf8(row.description).toStdString()
                              << " must sit in exactly one group of the tab it maps to";
  }
}

// MainWindow Q_ASSERTs on the mapping while generating the rows; a release
// build would silently drop the button instead. Prove it total here.
TEST(ToolbarRegistry, ActionMappingCoversEveryId) {
  QUndoStack stack;
  Actions actions(stack);

  for (int i = 0; i < static_cast<int>(Id::kIdCount); ++i) {
    const Id id = static_cast<Id>(i);
    EXPECT_NE(actions.action(id), nullptr)
        << "shortcuts::Id " << i << " ('" << shortcuts::entry(id).description
        << "') maps to no QAction";
  }
  EXPECT_EQ(actions.action(Id::kIdCount), nullptr) << "the sentinel names no action";
}

} // namespace
} // namespace roadmaker::editor
