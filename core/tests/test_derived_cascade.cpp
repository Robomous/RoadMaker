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

// cascade-s3 (#463) — stage [3] of the move funnel: the layers DERIVED from the
// roads follow a move, or say why they could not.
//
// Two derived layers, failing in opposite directions. An enclosed-area ground
// surface is a function of the road ring and can simply be recomputed — unless
// the user reshaped it, at which point the boundary is their geometry and must
// be left alone and reported. A `<bridge>` span records nothing about the
// crossing it was built for, so it cannot be recomputed at all, only re-anchored
// from provenance read before the gesture.
//
// See docs/domain/connection_contract.md §derived-layer recompute on move.

#include "roadmaker/edit/operations.hpp"
#include "roadmaker/mesh/mesh.hpp"
#include "roadmaker/mesh/mesh_builder.hpp"
#include "roadmaker/road/authoring.hpp"
#include "roadmaker/road/bridge.hpp"
#include "roadmaker/road/grade_separation.hpp"
#include "roadmaker/road/network.hpp"
#include "roadmaker/road/road.hpp"
#include "roadmaker/road/surface.hpp"
#include "roadmaker/road/surface_derivation.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include "support/network_compare.hpp"

namespace roadmaker {
namespace {

using edit::DerivedChange;
using edit::DerivedRecord;
using edit::ElevationPoint;
using test::expect_network_matches;
using test::snapshot_xodr;

/// A straight two-lane road, optionally raised to a constant elevation.
RoadId
segment(RoadNetwork& network, const char* odr_id, Waypoint from, Waypoint to, double z = 0.0) {
  const std::array<Waypoint, 2> waypoints{from, to};
  const auto road =
      author_clothoid_road(network, waypoints, LaneProfile::two_lane_default(), "", odr_id);
  EXPECT_TRUE(road.has_value());
  const RoadId id = road.value_or(RoadId{});
  if (z != 0.0 && id.is_valid()) {
    const double length = network.road(id)->plan_view.length();
    const std::vector<ElevationPoint> profile{{.s = 0.0, .z = z, .grade = 0.0},
                                              {.s = length, .z = z, .grade = 0.0}};
    EXPECT_TRUE(edit::set_elevation_profile(network, id, profile)->apply(network).has_value());
  }
  return id;
}

/// A 40 m square of four coincident-ended roads, corners (0,0)-(40,0)-(40,40)-
/// (0,40). They weld by POSITION, not by `<link>`, which is what makes a plain
/// translate of one side open the loop — a linked chain would be refit by the
/// follow stage instead (cascade-s1) and the enclosure would survive.
std::vector<RoadId> author_square(RoadNetwork& network, const char* prefix = "") {
  const std::string p(prefix);
  return {segment(network, (p + "a").c_str(), {0, 0}, {40, 0}),
          segment(network, (p + "b").c_str(), {40, 0}, {40, 40}),
          segment(network, (p + "c").c_str(), {40, 40}, {0, 40}),
          segment(network, (p + "d").c_str(), {0, 40}, {0, 0})};
}

std::vector<SurfaceId> all_surfaces(const RoadNetwork& network) {
  std::vector<SurfaceId> ids;
  network.for_each_surface([&](SurfaceId id, const Surface&) { ids.push_back(id); });
  return ids;
}

/// Applies a command and fails the test with its message if it refuses.
void expect_applies(RoadNetwork& network, const std::unique_ptr<edit::Command>& command) {
  ASSERT_NE(command, nullptr);
  const auto applied = command->apply(network);
  EXPECT_TRUE(applied.has_value()) << applied.error().message;
}

std::vector<DerivedRecord> records_of(const std::unique_ptr<edit::Command>& command,
                                      DerivedChange change) {
  std::vector<DerivedRecord> out;
  for (const DerivedRecord& record : command->derived_records()) {
    if (record.change == change) {
      out.push_back(record);
    }
  }
  return out;
}

/// The oracle acceptance asks for: whatever the stage did, the surface set must
/// now be exactly what a from-scratch derivation on this network would produce.
/// It doubles as proof that Document's own `derive_surfaces` call on the
/// topology path is a provable no-op rather than a second, uncommanded mutation.
void expect_surface_set_is_settled(const RoadNetwork& network) {
  const SurfaceReconciliation plan = plan_surface_reconciliation(network);
  EXPECT_TRUE(plan.erase.empty()) << plan.erase.size() << " surface(s) still to erase";
  EXPECT_TRUE(plan.create.empty()) << plan.create.size() << " surface(s) still to create";
}

/// The meshed vertices of one surface, for the "an untouched surface is bitwise
/// identical" criterion.
std::vector<double> surface_vertices(const RoadNetwork& network, SurfaceId id) {
  NetworkMesh mesh;
  const std::array<SurfaceId, 1> ids{id};
  remesh_surfaces(network, mesh, ids);
  for (const SurfaceMesh& surface : mesh.surfaces) {
    if (surface.surface == id) {
      return surface.mesh.positions;
    }
  }
  return {};
}

/// The overpass fixture: `high` runs west-east 5 m up, `low` runs south-north at
/// ground level, and a 24 m deck already carries the crossing.
struct Overpass {
  RoadId high;
  RoadId low;
  double s_upper = 0.0;
};

Overpass author_overpass(RoadNetwork& network) {
  Overpass over;
  over.high = segment(network, "high", {-60, 0}, {60, 0}, 5.0);
  over.low = segment(network, "low", {0, -60}, {0, 60}, 0.0);
  const std::vector<GradeSeparation> crossings = find_grade_separations(network);
  EXPECT_EQ(crossings.size(), 1U);
  if (!crossings.empty()) {
    over.s_upper = crossings.front().s_upper;
    auto authored = edit::author_bridge(network, over.high, over.s_upper - 12.0, 24.0);
    EXPECT_TRUE(authored->apply(network).has_value());
  }
  return over;
}

// --- ground surfaces --------------------------------------------------------

TEST(DerivedCascade, MovingABoundingRoadOpensTheLoopAndTheSurfaceGoes) {
  RoadNetwork network;
  const std::vector<RoadId> square = author_square(network);
  derive_surfaces(network);
  ASSERT_EQ(network.surface_count(), 1U);

  auto move = edit::translate_road(network, square[0], 0.0, -25.0);
  expect_applies(network, move);

  EXPECT_EQ(network.surface_count(), 0U);
  expect_surface_set_is_settled(network);
  EXPECT_EQ(records_of(move, DerivedChange::SurfaceRemoved).size(), 1U);
}

TEST(DerivedCascade, MovingARoadIntoPlaceClosesTheLoopAndASurfaceAppears) {
  RoadNetwork network;
  const std::vector<RoadId> square = author_square(network);
  // Push one side out first, so the network starts with no enclosure at all.
  ASSERT_TRUE(edit::translate_road(network, square[0], 0.0, -25.0)->apply(network).has_value());
  derive_surfaces(network);
  ASSERT_EQ(network.surface_count(), 0U);

  auto move = edit::translate_road(network, square[0], 0.0, 25.0);
  expect_applies(network, move);

  EXPECT_EQ(network.surface_count(), 1U);
  expect_surface_set_is_settled(network);
  const std::vector<DerivedRecord> added = records_of(move, DerivedChange::SurfaceAdded);
  ASSERT_EQ(added.size(), 1U);
  EXPECT_TRUE(added.front().surface.is_valid());
}

TEST(DerivedCascade, TheSurfaceSetMatchesAFreshDeriveOnTheMovedNetwork) {
  // Two stacked blocks sharing their middle edge. Nodes are welded ENDPOINTS, so
  // the shared road has to run corner to corner — a road ending mid-way along
  // another welds nothing and splits nothing.
  RoadNetwork network;
  segment(network, "s", {0, 0}, {40, 0});
  segment(network, "e0", {40, 0}, {40, 20});
  segment(network, "e1", {40, 20}, {40, 40});
  segment(network, "n", {40, 40}, {0, 40});
  segment(network, "w1", {0, 40}, {0, 20});
  segment(network, "w0", {0, 20}, {0, 0});
  const RoadId divider = segment(network, "mid", {40, 20}, {0, 20});
  derive_surfaces(network);
  ASSERT_EQ(network.surface_count(), 2U);

  auto move = edit::translate_road(network, divider, 0.0, 8.0);
  expect_applies(network, move);

  // Both of the divider's ends left their corners, so the two halves merged
  // back into the single block the outer ring still encloses.
  expect_surface_set_is_settled(network);
  ASSERT_EQ(network.surface_count(), 1U);
  const Surface* surface = network.surface(all_surfaces(network).front());
  ASSERT_NE(surface, nullptr);
  EXPECT_EQ(std::ranges::find(surface->bounding_roads, divider), surface->bounding_roads.end())
      << "the ring still names the road that walked away";
  EXPECT_EQ(surface->bounding_roads.size(), 6U);
}

TEST(DerivedCascade, AnAuthoredBoundarySurvivesAMoveAndIsReported) {
  RoadNetwork network;
  const std::vector<RoadId> square = author_square(network);
  derive_surfaces(network);
  ASSERT_EQ(network.surface_count(), 1U);
  const SurfaceId id = all_surfaces(network).front();

  // Reshaping detaches it: the boundary stops being a function of the roads.
  const std::vector<SurfaceNode> nodes{
      {.x = 5, .y = 5}, {.x = 35, .y = 5}, {.x = 35, .y = 35}, {.x = 5, .y = 35}};
  ASSERT_TRUE(edit::set_surface_boundary(network, id, nodes)->apply(network).has_value());
  ASSERT_EQ(network.surface(id)->source, BoundarySource::Authored);
  const Surface before = *network.surface(id);

  auto move = edit::translate_road(network, square[0], 0.0, -25.0);
  expect_applies(network, move);

  const Surface* after = network.surface(id);
  ASSERT_NE(after, nullptr) << "an authored surface must outlive the loop it detached from";
  EXPECT_EQ(after->source, BoundarySource::Authored);
  EXPECT_EQ(after->nodes, before.nodes) << "a move must never re-derive an authored boundary";
  EXPECT_EQ(after->bounding_roads, before.bounding_roads);

  const std::vector<DerivedRecord> stale = records_of(move, DerivedChange::AuthoredBoundaryStale);
  ASSERT_EQ(stale.size(), 1U) << "leaving it alone must be said out loud, not inferred";
  EXPECT_EQ(stale.front().surface, id);
  EXPECT_EQ(stale.front().road, square[0]);
}

TEST(DerivedCascade, UndoRestoresTheSurfaceIncludingItsMaterial) {
  RoadNetwork network;
  const std::vector<RoadId> square = author_square(network);
  derive_surfaces(network);
  const SurfaceId id = all_surfaces(network).front();
  ASSERT_TRUE(edit::set_surface_material(network, id, "concrete")->apply(network).has_value());
  const std::string before = snapshot_xodr(network);

  auto move = edit::translate_road(network, square[0], 0.0, -25.0);
  expect_applies(network, move);
  ASSERT_EQ(network.surface_count(), 0U);

  ASSERT_TRUE(move->revert(network).has_value());
  ASSERT_EQ(network.surface_count(), 1U);
  const Surface* restored = network.surface(id);
  ASSERT_NE(restored, nullptr) << "the surface must come back under its own id";
  // The whole point of snapshotting the VALUE rather than re-deriving: a plain
  // re-derive hands back a fresh, default-material surface and the file changes.
  EXPECT_EQ(restored->material, "concrete");
  expect_network_matches(network, before);
}

TEST(DerivedCascade, ADistantSurfaceIsUntouched) {
  RoadNetwork network;
  const std::vector<RoadId> near = author_square(network);
  const std::vector<RoadId> far{segment(network, "fa", {500, 0}, {540, 0}),
                                segment(network, "fb", {540, 0}, {540, 40}),
                                segment(network, "fc", {540, 40}, {500, 40}),
                                segment(network, "fd", {500, 40}, {500, 0})};
  derive_surfaces(network);
  ASSERT_EQ(network.surface_count(), 2U);

  // Identify the distant block by a road it rings, not by arena position.
  SurfaceId distant;
  for (const SurfaceId id : all_surfaces(network)) {
    if (std::ranges::find(network.surface(id)->bounding_roads, far[0]) !=
        network.surface(id)->bounding_roads.end()) {
      distant = id;
    }
  }
  ASSERT_TRUE(distant.is_valid());
  const Surface before = *network.surface(distant);
  const std::vector<double> vertices_before = surface_vertices(network, distant);
  ASSERT_FALSE(vertices_before.empty());

  auto move = edit::translate_road(network, near[0], 0.0, -25.0);
  expect_applies(network, move);

  const Surface* after = network.surface(distant);
  ASSERT_NE(after, nullptr) << "the far block kept its id";
  EXPECT_EQ(*after, before);
  EXPECT_EQ(surface_vertices(network, distant), vertices_before)
      << "re-derivation must be scoped to the surfaces the move actually reached";
}

TEST(DerivedCascade, ARigidMoveOfEveryBoundingRoadKeepsTheSameSurface) {
  RoadNetwork network;
  const std::vector<RoadId> square = author_square(network);
  derive_surfaces(network);
  const SurfaceId id = all_surfaces(network).front();
  const Surface before = *network.surface(id);

  auto move = edit::translate_roads(network, square, 120.0, -35.0);
  expect_applies(network, move);

  ASSERT_EQ(network.surface_count(), 1U);
  const Surface* after = network.surface(id);
  ASSERT_NE(after, nullptr) << "the block travelled whole — its id must survive";
  EXPECT_EQ(*after, before);
  expect_surface_set_is_settled(network);
  EXPECT_TRUE(move->derived_records().empty());
}

TEST(DerivedCascade, NoGestureLeavesTheSurfaceSetStale) {
  // The claim the funnel rests on: every gesture decides in the same place, so
  // none of them can hold its own opinion about the derived layers.
  struct Gesture {
    const char* label;
    std::unique_ptr<edit::Command> (*build)(RoadNetwork&, RoadId);
  };

  const std::array<Gesture, 6> gestures{
      Gesture{
          "translate",
          [](RoadNetwork& net, RoadId road) { return edit::translate_road(net, road, 0, -25); }},
      Gesture{
          "rotate",
          [](RoadNetwork& net, RoadId road) { return edit::rotate_road(net, road, 0.6, 0, 0); }},
      Gesture{"move_waypoint",
              [](RoadNetwork& net, RoadId road) {
                return edit::move_waypoint(net, road, 0, Waypoint{.x = 0, .y = -25});
              }},
      Gesture{"insert_waypoint",
              [](RoadNetwork& net, RoadId road) {
                return edit::insert_waypoint(net, road, 1, Waypoint{.x = 20, .y = -25});
              }},
      Gesture{"delete_waypoint",
              [](RoadNetwork& net, RoadId road) { return edit::delete_waypoint(net, road, 1); }},
      Gesture{"insert_node_at",
              [](RoadNetwork& net, RoadId road) { return edit::insert_node_at(net, road, 10.0); }},
  };

  for (const Gesture& gesture : gestures) {
    SCOPED_TRACE(gesture.label);
    RoadNetwork network;
    // Three waypoints, so delete_waypoint has an interior one to drop.
    const std::array<Waypoint, 3> bottom{
        Waypoint{.x = 0, .y = 0}, Waypoint{.x = 20, .y = 0}, Waypoint{.x = 40, .y = 0}};
    const RoadId a =
        author_clothoid_road(network, bottom, LaneProfile::two_lane_default(), "", "a").value();
    segment(network, "b", {40, 0}, {40, 40});
    segment(network, "c", {40, 40}, {0, 40});
    segment(network, "d", {0, 40}, {0, 0});
    derive_surfaces(network);
    ASSERT_EQ(network.surface_count(), 1U);

    auto command = gesture.build(network, a);
    expect_applies(network, command);
    expect_surface_set_is_settled(network);

    ASSERT_TRUE(command->revert(network).has_value());
    expect_surface_set_is_settled(network);
  }
}

// --- bridge spans -----------------------------------------------------------

TEST(DerivedCascade, ABridgeSpanFollowsItsCrossing) {
  RoadNetwork network;
  const Overpass over = author_overpass(network);
  const Bridge before = network.road(over.high)->bridges.front();

  // Slide the LOW road along the high one: the crossing station moves with it.
  auto move = edit::translate_road(network, over.low, 30.0, 0.0);
  expect_applies(network, move);

  const std::vector<GradeSeparation> after = find_grade_separations(network);
  ASSERT_EQ(after.size(), 1U);
  const Bridge& span = network.road(over.high)->bridges.front();
  EXPECT_NEAR(span.s, before.s + 30.0, 1e-6) << "the deck must travel with the crossing";
  EXPECT_TRUE(bridge_covering(network.road(over.high)->bridges, after.front().s_upper).has_value())
      << "the relocated span must actually carry the new crossing";
  // Everything except the anchor is authored data and must come through intact.
  EXPECT_EQ(span.odr_id, before.odr_id);
  EXPECT_EQ(span.length, before.length);
  EXPECT_EQ(span.type, before.type);

  const std::vector<DerivedRecord> moved = records_of(move, DerivedChange::BridgeRelocated);
  ASSERT_EQ(moved.size(), 1U);
  EXPECT_EQ(moved.front().road, over.high);
  EXPECT_EQ(moved.front().bridge_index, 0U);
}

TEST(DerivedCascade, ABridgeSpanIsOrphanedWhenTheCrossingGoes) {
  RoadNetwork network;
  const Overpass over = author_overpass(network);
  const Bridge before = network.road(over.high)->bridges.front();

  // Push the lower road clear of the high one — there is no crossing left.
  auto move = edit::translate_road(network, over.low, 400.0, 0.0);
  expect_applies(network, move);

  EXPECT_TRUE(find_grade_separations(network).empty());
  ASSERT_EQ(network.road(over.high)->bridges.size(), 1U);
  EXPECT_EQ(network.road(over.high)->bridges.front(), before)
      << "an orphaned span is reported, never deleted — a move must not destroy authored data";

  const std::vector<DerivedRecord> orphans = records_of(move, DerivedChange::BridgeOrphaned);
  ASSERT_EQ(orphans.size(), 1U);
  EXPECT_EQ(orphans.front().road, over.high);
  EXPECT_FALSE(orphans.front().detail.empty());
}

TEST(DerivedCascade, ADistantBridgeIsUntouched) {
  RoadNetwork network;
  const Overpass over = author_overpass(network);
  const RoadId elsewhere = segment(network, "elsewhere", {-400, 200}, {-300, 200});
  const std::string before = snapshot_xodr(network);

  auto move = edit::translate_road(network, elsewhere, 0.0, 15.0);
  expect_applies(network, move);

  EXPECT_EQ(network.road(over.high)->bridges.size(), 1U);
  EXPECT_TRUE(move->derived_records().empty());
  ASSERT_TRUE(move->revert(network).has_value());
  expect_network_matches(network, before);
}

TEST(DerivedCascade, UndoRestoresARelocatedSpanByteIdentically) {
  RoadNetwork network;
  const Overpass over = author_overpass(network);
  const std::string before = snapshot_xodr(network);

  auto move = edit::translate_road(network, over.low, 30.0, 0.0);
  expect_applies(network, move);
  ASSERT_TRUE(move->revert(network).has_value());
  expect_network_matches(network, before);

  // And a redo puts it back where the first apply did.
  expect_applies(network, move);
  const std::string after_redo = snapshot_xodr(network);
  ASSERT_TRUE(move->revert(network).has_value());
  expect_network_matches(network, before);
  expect_applies(network, move);
  expect_network_matches(network, after_redo);
}

TEST(DerivedCascade, GenerateNoLongerStacksASecondDeckOnACarriedCrossing) {
  RoadNetwork network;
  const Overpass over = author_overpass(network);
  ASSERT_EQ(network.road(over.high)->bridges.size(), 1U);

  // The case that matters is the one Generate hits after a move: the crossing
  // has drifted, so the span it asks for starts a few metres along from the one
  // already there. Asking for the IDENTICAL span would be refused by either
  // guard, which is why the exact-(s, length) test looked adequate for so long —
  // it never saw this. Coverage refuses it; exact equality did not, and stacked
  // a second deck on a stretch the first already carried.
  auto drifted =
      edit::author_bridge(network, over.high, over.s_upper - 9.0, 24.0, "concrete", "again");
  EXPECT_FALSE(drifted->apply(network).has_value())
      << "a crossing already carried must not gain a second deck";
  EXPECT_EQ(network.road(over.high)->bridges.size(), 1U);

  // And the guard must not become a blanket refusal: a stretch no span covers is
  // still authorable, or the fix Generate offers would be unreachable.
  auto elsewhere = edit::author_bridge(network, over.high, 4.0, 24.0, "concrete", "far");
  EXPECT_TRUE(elsewhere->apply(network).has_value())
      << "a stretch no span covers must still be authorable";
  EXPECT_EQ(network.road(over.high)->bridges.size(), 2U);
}

TEST(DerivedCascade, RemoveOrphanedBridgesClearsExactlyTheOrphans) {
  RoadNetwork network;
  const Overpass over = author_overpass(network);
  auto orphan = edit::author_bridge(network, over.high, 4.0, 24.0, "concrete", "orphan");
  ASSERT_TRUE(orphan->apply(network).has_value());
  ASSERT_EQ(network.road(over.high)->bridges.size(), 2U);
  const std::string before = snapshot_xodr(network);

  auto sweep = edit::remove_orphaned_bridges(network);
  expect_applies(network, sweep);

  ASSERT_EQ(network.road(over.high)->bridges.size(), 1U);
  EXPECT_TRUE(bridge_covering(network.road(over.high)->bridges, over.s_upper).has_value())
      << "the span still carrying the crossing must survive";
  ASSERT_TRUE(sweep->revert(network).has_value());
  expect_network_matches(network, before);

  // With nothing orphaned it refuses rather than pushing an empty undo entry.
  auto again = edit::remove_orphaned_bridges(network);
  ASSERT_TRUE(again->apply(network).has_value());
  auto nothing_left = edit::remove_orphaned_bridges(network);
  EXPECT_FALSE(nothing_left->apply(network).has_value());
}

} // namespace
} // namespace roadmaker
