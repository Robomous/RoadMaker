// The toolbar is GENERATED from the action registry (p1-s5, issue #317), so
// these tests are what keeps the taxonomy honest: the categories are pinned,
// every tool must be categorized, and every Id must resolve to a QAction.
// Without the gate, the next pillar's tools would quietly land off-toolbar or
// re-flatten the rows — which is what #317 existed to stop.

#include <gtest/gtest.h>

#include <QSet>
#include <QUndoStack>
#include <array>
#include <vector>

#include "app/actions.hpp"
#include "app/shortcut_registry.hpp"

namespace roadmaker::editor {
namespace {

using shortcuts::Entry;
using shortcuts::Id;
using shortcuts::ToolbarGroup;
using shortcuts::ToolbarGroupLayout;
using shortcuts::ToolbarRow;

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

// The taxonomy is a product decision (#317), not an implementation detail:
// pin it so a reshuffle has to be deliberate. The three reserved groups are
// listed before they hold anything — that is the point of committing them.
TEST(ToolbarRegistry, TaxonomyIsFixed) {
  const std::array<std::pair<const char*, ToolbarRow>, 10> expected{{
      {"File", ToolbarRow::kAuthoring},
      {"Edit", ToolbarRow::kAuthoring},
      {"Roads", ToolbarRow::kAuthoring},
      {"Lanes", ToolbarRow::kAuthoring},
      {"Markings", ToolbarRow::kLayers},
      {"Props", ToolbarRow::kLayers},
      {"Terrain & Structures", ToolbarRow::kLayers},
      {"Signals & Signs", ToolbarRow::kLayers},
      {"Scenario", ToolbarRow::kLayers},
      {"Library & View", ToolbarRow::kLayers},
  }};

  const std::span<const ToolbarGroup> groups = shortcuts::toolbar_groups();
  ASSERT_EQ(groups.size(), expected.size());
  for (std::size_t i = 0; i < expected.size(); ++i) {
    EXPECT_EQ(QString::fromUtf8(groups[i].name), QString::fromUtf8(expected[i].first))
        << "group " << i;
    EXPECT_EQ(groups[i].row, expected[i].second) << "group " << i << " is on the wrong row";
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

// The layout the issue specifies, per group and in order. Reserved groups are
// present-but-empty rather than absent: the taxonomy is committed now, the
// contents arrive later.
TEST(ToolbarRegistry, LayoutMatchesTheIssue) {
  const std::vector<ToolbarGroupLayout> authoring =
      shortcuts::toolbar_layout(ToolbarRow::kAuthoring);
  ASSERT_EQ(authoring.size(), 4u);
  EXPECT_EQ(ids_of(authoring, "File"),
            (std::vector{Id::NewScene, Id::Open, Id::Save, Id::ExportGlb}));
  EXPECT_EQ(
      ids_of(authoring, "Edit"),
      (std::vector{Id::ToolSelect, Id::ToolMove, Id::ToolSplit, Id::ToolDelete, Id::MergeRoads}));
  EXPECT_EQ(ids_of(authoring, "Roads"),
            (std::vector{Id::ToolCreateRoad,
                         Id::ToolEditNodes,
                         Id::ToolCreateJunction,
                         Id::ToolCorner,
                         Id::ToolStopLine,
                         Id::ToolJunctionSpan,
                         Id::ToolJunctionSurface,
                         Id::ToolManeuver,
                         Id::ToolElevation}));
  EXPECT_EQ(ids_of(authoring, "Lanes"),
            (std::vector{Id::ToolLaneProfile,
                         Id::ToolLaneAdd,
                         Id::ToolLaneForm,
                         Id::ToolLaneCarve,
                         Id::LaneWidthEditor}));

  const std::vector<ToolbarGroupLayout> layers = shortcuts::toolbar_layout(ToolbarRow::kLayers);
  ASSERT_EQ(layers.size(), 6u);
  EXPECT_EQ(ids_of(layers, "Markings"),
            (std::vector{Id::ToolCrosswalk, Id::ToolMarkingPoint, Id::ToolMarkingCurve}));
  EXPECT_EQ(
      ids_of(layers, "Props"),
      (std::vector{Id::ToolPropPoint, Id::ToolPropCurve, Id::ToolPropSpan, Id::ToolPropPolygon}));
  EXPECT_EQ(ids_of(layers, "Library & View"),
            (std::vector{Id::AddFromLibrary, Id::ResetCamera, Id::FrameSelection}));

  // Populated by p4-s7 (issue #228): the reserved group now renders the Signal
  // tool, which is what the group was reserved for.
  EXPECT_EQ(ids_of(layers, "Signals & Signs"), (std::vector{Id::ToolSignal}));

  for (const char* reserved : {"Terrain & Structures", "Scenario"}) {
    EXPECT_TRUE(ids_of(layers, reserved).empty())
        << reserved << " is reserved and must render nothing yet";
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
