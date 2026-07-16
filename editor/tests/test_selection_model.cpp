#include "roadmaker/edit/operations.hpp"

#include <gtest/gtest.h>

#include <QSignalSpy>
#include <algorithm>
#include <vector>

#include "document/document.hpp"
#include "document/selection_model.hpp"

namespace roadmaker::editor {
namespace {

const std::filesystem::path kSample = std::filesystem::path(RM_SAMPLES_DIR) / "t_junction.xodr";

std::vector<RoadId> all_roads(const Document& document) {
  std::vector<RoadId> roads;
  document.network().for_each_road([&](RoadId id, const Road&) { roads.push_back(id); });
  return roads;
}

JunctionId first_junction(const Document& document) {
  JunctionId found;
  document.network().for_each_junction([&](JunctionId id, const Junction&) {
    if (!found.is_valid()) {
      found = id;
    }
  });
  return found;
}

LaneId first_lane_of(const Document& document, RoadId road_id) {
  const Road* road = document.network().road(road_id);
  if (road == nullptr || road->sections.empty()) {
    return {};
  }
  const LaneSection* section = document.network().lane_section(road->sections.front());
  return (section == nullptr || section->lanes.empty()) ? LaneId{} : section->lanes.front();
}

/// The class-level invariants every test asserts after mutating: no
/// duplicate entries, and the primary is the last entry when non-empty.
void expect_invariants(const SelectionModel& selection) {
  const auto& entries = selection.entries();
  for (std::size_t i = 0; i < entries.size(); ++i) {
    for (std::size_t j = i + 1; j < entries.size(); ++j) {
      EXPECT_FALSE(entries[i] == entries[j]) << "duplicate entry at " << i << " and " << j;
    }
  }
  if (entries.empty()) {
    EXPECT_TRUE(selection.empty());
    EXPECT_FALSE(selection.primary().road.is_valid());
  } else {
    EXPECT_TRUE(selection.contains(selection.primary()));
    EXPECT_TRUE(selection.primary() == entries.back());
  }
}

TEST(SelectionModel, ReplaceSelectsOneEntryAndEmitsOnlyOnChange) {
  Document document;
  ASSERT_TRUE(document.load(kSample).has_value());
  SelectionModel selection(document);
  QSignalSpy spy(&selection, &SelectionModel::selection_changed);

  const RoadId road = all_roads(document).front();
  selection.select({.road = road});
  EXPECT_EQ(spy.count(), 1);
  EXPECT_EQ(selection.entries().size(), 1U);
  EXPECT_EQ(selection.primary().road, road);
  EXPECT_FALSE(selection.primary().lane.is_valid());

  selection.select({.road = road}); // no-op: same selection
  EXPECT_EQ(spy.count(), 1);

  const LaneId lane = first_lane_of(document, road);
  ASSERT_TRUE(lane.is_valid());
  selection.select({.road = road, .lane = lane});
  EXPECT_EQ(spy.count(), 2);
  EXPECT_EQ(selection.entries().size(), 1U);
  EXPECT_EQ(selection.primary().lane, lane);

  selection.clear();
  EXPECT_EQ(spy.count(), 3);
  selection.clear(); // already empty
  EXPECT_EQ(spy.count(), 3);
  EXPECT_TRUE(selection.empty());
  expect_invariants(selection);
}

TEST(SelectionModel, AddAccumulatesInSelectionOrder) {
  Document document;
  ASSERT_TRUE(document.load(kSample).has_value());
  SelectionModel selection(document);

  const std::vector<RoadId> roads = all_roads(document);
  ASSERT_GE(roads.size(), 2U);

  selection.select({.road = roads[0]});
  selection.select({.road = roads[1]}, SelectMode::Add);
  ASSERT_EQ(selection.entries().size(), 2U);
  EXPECT_EQ(selection.entries()[0].road, roads[0]);
  EXPECT_EQ(selection.entries()[1].road, roads[1]);
  EXPECT_EQ(selection.primary().road, roads[1]);

  // Re-adding an existing entry makes it primary instead of duplicating it.
  selection.select({.road = roads[0]}, SelectMode::Add);
  ASSERT_EQ(selection.entries().size(), 2U);
  EXPECT_EQ(selection.primary().road, roads[0]);
  expect_invariants(selection);
}

TEST(SelectionModel, ToggleAddsThenRemoves) {
  Document document;
  ASSERT_TRUE(document.load(kSample).has_value());
  SelectionModel selection(document);
  QSignalSpy spy(&selection, &SelectionModel::selection_changed);

  const std::vector<RoadId> roads = all_roads(document);
  ASSERT_GE(roads.size(), 2U);

  selection.select({.road = roads[0]});
  selection.select({.road = roads[1]}, SelectMode::Toggle);
  EXPECT_EQ(selection.entries().size(), 2U);
  EXPECT_TRUE(selection.contains({.road = roads[0]}));
  EXPECT_TRUE(selection.contains({.road = roads[1]}));

  selection.select({.road = roads[0]}, SelectMode::Toggle);
  EXPECT_EQ(selection.entries().size(), 1U);
  EXPECT_FALSE(selection.contains({.road = roads[0]}));
  EXPECT_EQ(selection.primary().road, roads[1]);
  EXPECT_EQ(spy.count(), 3);
  expect_invariants(selection);
}

TEST(SelectionModel, SelectManyIsOneChangeAndDeduplicates) {
  Document document;
  ASSERT_TRUE(document.load(kSample).has_value());
  SelectionModel selection(document);
  QSignalSpy spy(&selection, &SelectionModel::selection_changed);

  const std::vector<RoadId> roads = all_roads(document);
  ASSERT_GE(roads.size(), 2U);

  // Rubber band delivering a duplicate: one signal, no duplicate entries.
  const std::vector<SelectionEntry> band = {
      {.road = roads[0]}, {.road = roads[1]}, {.road = roads[0]}};
  selection.select_many(band);
  EXPECT_EQ(spy.count(), 1);
  ASSERT_EQ(selection.entries().size(), 2U);
  EXPECT_EQ(selection.primary().road, roads[0]); // duplicate moved it to the back
  expect_invariants(selection);

  // Same set re-applied in the same order: no change, no signal.
  const std::vector<SelectionEntry> same = {{.road = roads[1]}, {.road = roads[0]}};
  selection.select_many(same);
  EXPECT_EQ(spy.count(), 1);
}

TEST(SelectionModel, StaleEntriesAreDropped) {
  Document document;
  ASSERT_TRUE(document.load(kSample).has_value());
  SelectionModel selection(document);

  const RoadId road = all_roads(document).front();
  selection.select({.road = road});
  EXPECT_FALSE(selection.empty());

  // Replace-selecting an invalid entry clears (the old single-select rule).
  selection.select({.road = RoadId{}});
  EXPECT_TRUE(selection.empty());

  // A stale entry inside a batch vanishes; live ones survive.
  selection.select_many(
      std::vector<SelectionEntry>{{.road = RoadId{}}, {.road = road, .lane = LaneId{}}});
  ASSERT_EQ(selection.entries().size(), 1U);
  EXPECT_EQ(selection.primary().road, road);
  expect_invariants(selection);
}

TEST(SelectionModel, ReloadClearsSelectionEvenWhenIdsAlias) {
  Document document;
  ASSERT_TRUE(document.load(kSample).has_value());
  SelectionModel selection(document);

  const std::vector<RoadId> roads = all_roads(document);
  ASSERT_GE(roads.size(), 2U);
  selection.select_many(std::vector<SelectionEntry>{{.road = roads[0]}, {.road = roads[1]}});
  ASSERT_FALSE(selection.empty());

  // Reloading rebuilds the arenas from scratch: the old ids would alias the
  // new file's roads, which lookups alone cannot detect. The model must
  // hard-clear on loaded().
  QSignalSpy spy(&selection, &SelectionModel::selection_changed);
  ASSERT_TRUE(document.load(kSample).has_value());
  EXPECT_TRUE(selection.empty());
  EXPECT_EQ(spy.count(), 1);
  expect_invariants(selection);
}

TEST(SelectionModel, DeletionCommandPrunesItsEntries) {
  Document document;
  ASSERT_TRUE(document.load(kSample).has_value());
  SelectionModel selection(document);

  const std::vector<RoadId> roads = all_roads(document);
  ASSERT_GE(roads.size(), 2U);
  selection.select_many(std::vector<SelectionEntry>{{.road = roads[0]}, {.road = roads[1]}});

  // Deleting one selected road drops exactly its entry (topology_changed →
  // prune); undoing the deletion restores the id but not the selection —
  // selection is view state, not undoable.
  QSignalSpy spy(&selection, &SelectionModel::selection_changed);
  ASSERT_TRUE(document.push_command(edit::delete_road(document.network(), roads[0])).has_value());
  EXPECT_EQ(spy.count(), 1);
  ASSERT_EQ(selection.entries().size(), 1U);
  EXPECT_EQ(selection.primary().road, roads[1]);
  expect_invariants(selection);

  document.undo_stack()->undo();
  EXPECT_EQ(selection.entries().size(), 1U);
  expect_invariants(selection);
}

TEST(SelectionModel, JunctionEntrySelectsAndClassifies) {
  // Gate finding 4: a junction floor pick lands as a junction entry —
  // selectable, reported by selected_junctions(), and never mistaken for a
  // road (its arms are selected separately).
  Document document;
  ASSERT_TRUE(document.load(kSample).has_value());
  SelectionModel selection(document);

  const JunctionId junction = first_junction(document);
  ASSERT_TRUE(junction.is_valid());

  selection.select({.junction = junction});
  ASSERT_EQ(selection.entries().size(), 1U);
  EXPECT_EQ(selection.primary().junction, junction);
  ASSERT_EQ(selection.selected_junctions().size(), 1U);
  EXPECT_EQ(selection.selected_junctions().front(), junction);
  EXPECT_TRUE(selection.selected_roads().empty());
  EXPECT_TRUE(selection.selected_objects().empty());
  expect_invariants(selection);
}

TEST(SelectionModel, StaleJunctionEntryIsDropped) {
  // A junction entry is view state like any other: reloading the document
  // clears it, and an entry naming a since-removed junction never survives.
  Document document;
  ASSERT_TRUE(document.load(kSample).has_value());
  SelectionModel selection(document);

  const JunctionId junction = first_junction(document);
  ASSERT_TRUE(junction.is_valid());
  selection.select({.junction = junction});
  ASSERT_EQ(selection.entries().size(), 1U);

  // Reload → SelectionModel::clear via Document::loaded.
  ASSERT_TRUE(document.load(kSample).has_value());
  EXPECT_TRUE(selection.empty());
}

TEST(SelectionModel, SignalEntrySelectsClassifiesAndPrunesOnDelete) {
  // A placed signal is a first-class selection: reported by selected_signals(),
  // never mistaken for a road/object, and pruned when a delete_signal command
  // removes it.
  Document document;
  ASSERT_TRUE(document.load(kSample).has_value());
  const RoadId road = all_roads(document).front();

  Signal sign;
  sign.odr_id = "sel1";
  sign.type = "274";
  sign.subtype = "50";
  sign.country = "DE";
  sign.dynamic = false;
  sign.s = 5.0;
  sign.t = -4.0;
  ASSERT_TRUE(document.push_command(edit::add_signal(document.network(), road, sign)).has_value());
  SignalId signal;
  document.network().for_each_signal([&](SignalId id, const Signal&) { signal = id; });
  ASSERT_TRUE(signal.is_valid());

  SelectionModel selection(document);
  selection.select({.signal = signal});
  ASSERT_EQ(selection.entries().size(), 1U);
  EXPECT_EQ(selection.primary().signal, signal);
  ASSERT_EQ(selection.selected_signals().size(), 1U);
  EXPECT_EQ(selection.selected_signals().front(), signal);
  EXPECT_TRUE(selection.selected_roads().empty());
  EXPECT_TRUE(selection.selected_objects().empty());
  expect_invariants(selection);

  // Deleting the signal prunes the entry (topology_changed → prune_stale).
  ASSERT_TRUE(document.push_command(edit::delete_signal(document.network(), signal)).has_value());
  EXPECT_TRUE(selection.empty());
}

/// A ~20 m square loop of four roads whose coincident endpoints enclose exactly
/// one area. Pushing the loop through the Document runs derive_surfaces (from
/// after_kernel_mutation on the topology change), so a Surface exists in the
/// live network — returns its id.
SurfaceId author_square_loop(Document& document) {
  const auto seg = [&](double x0, double y0, double x1, double y1, const char* name) {
    return edit::create_road({Waypoint{.x = x0, .y = y0}, Waypoint{.x = x1, .y = y1}},
                             LaneProfile::two_lane_default(),
                             name);
  };
  EXPECT_TRUE(document.push_command(seg(0.0, 0.0, 20.0, 0.0, "a")).has_value());
  EXPECT_TRUE(document.push_command(seg(20.0, 0.0, 20.0, 20.0, "b")).has_value());
  EXPECT_TRUE(document.push_command(seg(20.0, 20.0, 0.0, 20.0, "c")).has_value());
  EXPECT_TRUE(document.push_command(seg(0.0, 20.0, 0.0, 0.0, "d")).has_value());
  SurfaceId found;
  document.network().for_each_surface([&](SurfaceId id, const Surface&) {
    if (!found.is_valid()) {
      found = id;
    }
  });
  return found;
}

TEST(SelectionModel, SurfaceEntrySelectsAndClassifies) {
  // #215: an enclosed-area ground surface pick lands as a surface entry —
  // selectable, reported by selected_surfaces(), never mistaken for a junction
  // or object.
  Document document;
  const SurfaceId surface = author_square_loop(document);
  ASSERT_TRUE(surface.is_valid());
  SelectionModel selection(document);

  selection.select({.surface = surface});
  ASSERT_EQ(selection.entries().size(), 1U);
  EXPECT_EQ(selection.primary().surface, surface);
  ASSERT_EQ(selection.selected_surfaces().size(), 1U);
  EXPECT_EQ(selection.selected_surfaces().front(), surface);
  EXPECT_TRUE(selection.selected_junctions().empty());
  EXPECT_TRUE(selection.selected_objects().empty());
  expect_invariants(selection);
}

TEST(SelectionModel, SelectedRoadsExcludesSurface) {
  // The guard fix: a surface entry carries an INVALID road, so it must never
  // feed a (nonexistent) road to the editing tools through selected_roads().
  Document document;
  const SurfaceId surface = author_square_loop(document);
  ASSERT_TRUE(surface.is_valid());
  SelectionModel selection(document);

  selection.select({.surface = surface});
  ASSERT_EQ(selection.entries().size(), 1U);
  EXPECT_TRUE(selection.selected_roads().empty());
}

TEST(SelectionModel, StaleSurfaceEntryIsPrunedWhenItsLoopBreaks) {
  // A surface entry is view state: undoing a bounding road opens the loop, so
  // derive_surfaces erases the surface and topology_changed prunes the entry.
  Document document;
  const SurfaceId surface = author_square_loop(document);
  ASSERT_TRUE(surface.is_valid());
  SelectionModel selection(document);
  selection.select({.surface = surface});
  ASSERT_EQ(selection.entries().size(), 1U);

  document.undo_stack()->undo(); // reverts road "d" — the loop opens
  EXPECT_EQ(document.network().surface(surface), nullptr);
  EXPECT_TRUE(selection.empty());
}

} // namespace
} // namespace roadmaker::editor
