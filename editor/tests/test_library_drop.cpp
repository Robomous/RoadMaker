#include "roadmaker/edit/assembly.hpp"
#include "roadmaker/road/authoring.hpp"
#include "roadmaker/road/network.hpp"
#include "roadmaker/road/object.hpp"
#include "roadmaker/road/road.hpp"
#include "roadmaker/xodr/diagnostic.hpp"
#include "roadmaker/xodr/writer.hpp"

#include <gtest/gtest.h>

#include <QString>
#include <vector>

#include "document/library_drop.hpp"
#include "document/library_manifest.hpp"
#include "viewport/picking.hpp"

namespace roadmaker::editor {
namespace {

using roadmaker::count_errors;
using roadmaker::LaneProfile;
using roadmaker::RoadNetwork;
using roadmaker::validate_network;
using roadmaker::Waypoint;

RoadNetwork with_straight_road() {
  RoadNetwork network;
  const std::vector<Waypoint> waypoints{Waypoint{.x = 0.0, .y = 0.0},
                                        Waypoint{.x = 100.0, .y = 0.0}};
  auto road = roadmaker::author_clothoid_road(network, waypoints, LaneProfile::two_lane_rural());
  if (!road.has_value()) {
    throw std::runtime_error("with_straight_road: " + road.error().message);
  }
  return network;
}

LibraryItem tree(const char* model) {
  LibraryItem item;
  item.key = QStringLiteral("prop.tree.pine");
  item.label = QStringLiteral("Pine tree");
  item.kind = LibraryItem::Kind::Tree;
  item.model = QString::fromLatin1(model);
  return item;
}

LibraryItem road_template(const char* profile) {
  LibraryItem item;
  item.key = QStringLiteral("road.x");
  item.kind = LibraryItem::Kind::RoadTemplate;
  item.profile = QString::fromLatin1(profile);
  return item;
}

LibraryItem assembly(const char* which) {
  LibraryItem item;
  item.key = QStringLiteral("assembly.x");
  item.kind = LibraryItem::Kind::Assembly;
  item.assembly = QString::fromLatin1(which);
  return item;
}

LibraryItem signal(const char* which) {
  LibraryItem item;
  item.key = QStringLiteral("signal.x");
  item.label = QStringLiteral("Traffic light");
  item.kind = LibraryItem::Kind::Signal;
  item.signal = QString::fromLatin1(which);
  return item;
}

TEST(LibraryDrop, RoadTemplateArmsCreateRoadWithItsProfile) {
  RoadNetwork network;
  const LibraryDropAction action =
      resolve_library_drop(road_template("highway"), network, 10.0, 20.0);
  EXPECT_EQ(action.kind, LibraryDropKind::RoadTemplate);
  EXPECT_EQ(action.command, nullptr); // arms the tool, no command
  EXPECT_EQ(action.profile.right.size(), LaneProfile::highway().right.size());
}

TEST(LibraryDrop, TAssemblyDropPushesAValidStandaloneJunction) {
  RoadNetwork network;
  LibraryDropAction action = resolve_library_drop(assembly("t"), network, 0.0, 0.0);
  ASSERT_EQ(action.kind, LibraryDropKind::Assembly);
  ASSERT_NE(action.command, nullptr);
  EXPECT_FALSE(action.toast.isEmpty());
  ASSERT_TRUE(action.command->apply(network).has_value());
  EXPECT_EQ(network.junction_count(), 1U);
  EXPECT_EQ(count_errors(validate_network(network)), 0U);
  // Undo removes exactly what the drop created.
  ASSERT_TRUE(action.command->revert(network).has_value());
  EXPECT_EQ(network.road_count(), 0U);
  EXPECT_EQ(network.junction_count(), 0U);
}

TEST(LibraryDrop, XAssemblyDropIsValidAtTheDropPoint) {
  RoadNetwork network;
  LibraryDropAction action = resolve_library_drop(assembly("x"), network, 50.0, -30.0);
  ASSERT_EQ(action.kind, LibraryDropKind::Assembly);
  ASSERT_TRUE(action.command->apply(network).has_value());
  EXPECT_EQ(network.junction_count(), 1U);
  EXPECT_EQ(count_errors(validate_network(network)), 0U);
}

TEST(LibraryDrop, TAssemblyDroppedOnARoadTeesIntoIt) {
  RoadNetwork network = with_straight_road(); // (0,0)-(100,0)
  // Drop over the road near its middle — projects on at s≈50, teed in.
  LibraryDropAction action = resolve_library_drop(assembly("t"), network, 50.0, 2.0);
  ASSERT_EQ(action.kind, LibraryDropKind::Assembly);
  ASSERT_NE(action.command, nullptr);
  EXPECT_TRUE(action.toast.contains(QStringLiteral("into the road")));
  ASSERT_TRUE(action.command->apply(network).has_value());
  EXPECT_EQ(network.junction_count(), 1U);
  EXPECT_EQ(count_errors(validate_network(network)), 0U);
}

TEST(LibraryDrop, XAssemblyDroppedOnARoadCrossesIt) {
  RoadNetwork network = with_straight_road();
  LibraryDropAction action = resolve_library_drop(assembly("x"), network, 50.0, 0.0);
  ASSERT_EQ(action.kind, LibraryDropKind::Assembly);
  EXPECT_TRUE(action.toast.contains(QStringLiteral("over the road")));
  ASSERT_TRUE(action.command->apply(network).has_value());
  EXPECT_EQ(network.junction_count(), 1U);
  EXPECT_EQ(count_errors(validate_network(network)), 0U);
}

TEST(LibraryDrop, AssemblyDroppedNearARoadEndFallsBackToStandalone) {
  RoadNetwork network = with_straight_road();
  // s≈5 is inside the end margin — no room to attach, so a standalone lands.
  LibraryDropAction action = resolve_library_drop(assembly("t"), network, 5.0, 2.0);
  ASSERT_EQ(action.kind, LibraryDropKind::Assembly);
  EXPECT_TRUE(action.toast.contains(QStringLiteral("standalone")));
  ASSERT_TRUE(action.command->apply(network).has_value());
  EXPECT_EQ(network.junction_count(), 1U);
  EXPECT_EQ(count_errors(validate_network(network)), 0U);
}

TEST(LibraryDrop, AssemblyDroppedOffAnyRoadIsAStandaloneAtTheCursor) {
  RoadNetwork network = with_straight_road();
  // 200 m off the road — well beyond the snap threshold.
  LibraryDropAction action = resolve_library_drop(assembly("t"), network, 50.0, 200.0);
  ASSERT_EQ(action.kind, LibraryDropKind::Assembly);
  EXPECT_TRUE(action.toast.contains(QStringLiteral("Placed T-intersection")));
}

TEST(LibraryDrop, UnknownItemYieldsNoAction) {
  RoadNetwork network;
  LibraryItem unknown;
  unknown.kind = LibraryItem::Kind::Unknown;
  const LibraryDropAction action = resolve_library_drop(unknown, network, 0.0, 0.0);
  EXPECT_EQ(action.kind, LibraryDropKind::None);
  EXPECT_EQ(action.command, nullptr);
}

TEST(LibraryDrop, TreeSnapsToTheNearbyRoadAndAddsAnObject) {
  RoadNetwork network = with_straight_road();
  // Drop 5 m to the left of the road (heading +x → +t is left) at x = 50.
  LibraryDropAction action = resolve_library_drop(tree("tree_pine"), network, 50.0, 5.0);
  ASSERT_EQ(action.kind, LibraryDropKind::Tree);
  ASSERT_NE(action.command, nullptr);
  EXPECT_FALSE(action.toast.isEmpty());
  ASSERT_TRUE(action.command->apply(network).has_value());
  ASSERT_EQ(network.object_count(), 1U);

  roadmaker::ObjectId id;
  network.for_each_object([&](roadmaker::ObjectId oid, const roadmaker::Object&) { id = oid; });
  const roadmaker::Object* object = network.object(id);
  ASSERT_NE(object, nullptr);
  EXPECT_EQ(object->name, "tree_pine");
  EXPECT_EQ(object->type, roadmaker::ObjectType::Tree);
  EXPECT_NEAR(object->s, 50.0, 2.0);
  EXPECT_NEAR(object->t, 5.0, 1.0);
  EXPECT_EQ(count_errors(validate_network(network)), 0U);

  // Undo removes exactly the placed prop.
  ASSERT_TRUE(action.command->revert(network).has_value());
  EXPECT_EQ(network.object_count(), 0U);
}

TEST(LibraryDrop, ShrubMapsToVegetation) {
  RoadNetwork network = with_straight_road();
  LibraryDropAction action = resolve_library_drop(tree("shrub"), network, 50.0, 4.0);
  ASSERT_EQ(action.kind, LibraryDropKind::Tree);
  ASSERT_TRUE(action.command->apply(network).has_value());
  roadmaker::ObjectId id;
  network.for_each_object([&](roadmaker::ObjectId oid, const roadmaker::Object&) { id = oid; });
  EXPECT_EQ(network.object(id)->type, roadmaker::ObjectType::Vegetation);
}

TEST(LibraryDrop, TrafficLightSnapsToTheNearbyRoadAndAddsADynamicSignal) {
  RoadNetwork network = with_straight_road();
  LibraryDropAction action = resolve_library_drop(signal("light"), network, 90.0, -6.0);
  ASSERT_EQ(action.kind, LibraryDropKind::Signal);
  ASSERT_NE(action.command, nullptr);
  EXPECT_FALSE(action.toast.isEmpty());
  ASSERT_TRUE(action.command->apply(network).has_value());
  ASSERT_EQ(network.signal_count(), 1U);

  roadmaker::SignalId id;
  network.for_each_signal([&](roadmaker::SignalId sid, const roadmaker::Signal&) { id = sid; });
  const roadmaker::Signal* placed = network.signal(id);
  ASSERT_NE(placed, nullptr);
  EXPECT_TRUE(placed->dynamic.value_or(false)); // a traffic light is dynamic
  EXPECT_NEAR(placed->s, 90.0, 2.0);
  EXPECT_NEAR(placed->t, -6.0, 1.0);
  EXPECT_EQ(count_errors(validate_network(network)), 0U);

  // Undo removes exactly the placed signal.
  ASSERT_TRUE(action.command->revert(network).has_value());
  EXPECT_EQ(network.signal_count(), 0U);
}

TEST(LibraryDrop, TrafficSignMapsToAStaticSignal) {
  RoadNetwork network = with_straight_road();
  LibraryDropAction action = resolve_library_drop(signal("sign"), network, 40.0, -6.0);
  ASSERT_EQ(action.kind, LibraryDropKind::Signal);
  ASSERT_TRUE(action.command->apply(network).has_value());
  roadmaker::SignalId id;
  network.for_each_signal([&](roadmaker::SignalId sid, const roadmaker::Signal&) { id = sid; });
  EXPECT_FALSE(network.signal(id)->dynamic.value_or(true)); // a sign is static
  EXPECT_EQ(count_errors(validate_network(network)), 0U);
}

TEST(LibraryDrop, SignalDroppedAwayFromAnyRoadIsRejectedWithAHint) {
  RoadNetwork network = with_straight_road();
  const LibraryDropAction action = resolve_library_drop(signal("light"), network, 50.0, 200.0);
  EXPECT_EQ(action.kind, LibraryDropKind::None);
  EXPECT_EQ(action.command, nullptr);
  EXPECT_FALSE(action.toast.isEmpty());
}

TEST(LibraryDrop, TreeDroppedAwayFromAnyRoadIsRejectedWithAHint) {
  RoadNetwork network = with_straight_road();
  // 200 m off to the side — beyond the snap threshold.
  const LibraryDropAction action = resolve_library_drop(tree("tree_pine"), network, 50.0, 200.0);
  EXPECT_EQ(action.kind, LibraryDropKind::None);
  EXPECT_EQ(action.command, nullptr);
  EXPECT_FALSE(action.toast.isEmpty()); // a "drop near a road" hint
}

TEST(LibraryDrop, TreeInAnEmptyNetworkIsRejected) {
  RoadNetwork network;
  const LibraryDropAction action = resolve_library_drop(tree("tree_pine"), network, 0.0, 0.0);
  EXPECT_EQ(action.kind, LibraryDropKind::None);
  EXPECT_EQ(action.command, nullptr);
}

// --- ghost == commit: the drag preview lands exactly where the drop commits ---

TEST(LibraryDrop, TreeGhostPreviewMatchesTheCommittedObjectPosition) {
  RoadNetwork network = with_straight_road(); // (0,0)-(100,0), heading +x
  const LibraryDropAction action = resolve_library_drop(tree("tree_pine"), network, 50.0, 5.0);
  ASSERT_EQ(action.kind, LibraryDropKind::Tree);
  ASSERT_TRUE(action.preview.valid);

  // Commit, then recover the object's world position from its (road, s, t) via
  // the SAME projection the ghost used — the two must coincide (ghost==commit).
  ASSERT_TRUE(action.command->apply(network).has_value());
  roadmaker::RoadId road_id;
  network.for_each_road([&](roadmaker::RoadId rid, const roadmaker::Road&) { road_id = rid; });
  roadmaker::ObjectId oid;
  network.for_each_object([&](roadmaker::ObjectId id, const roadmaker::Object&) { oid = id; });
  const roadmaker::Road* road = network.road(road_id);
  const roadmaker::Object* object = network.object(oid);
  ASSERT_NE(road, nullptr);
  ASSERT_NE(object, nullptr);

  const auto committed = station_to_world(road->plan_view, object->s, object->t);
  EXPECT_DOUBLE_EQ(action.preview.x, committed[0]);
  EXPECT_DOUBLE_EQ(action.preview.y, committed[1]);
  // And that spot is essentially under the drop point (5 m left of the road).
  EXPECT_NEAR(action.preview.x, 50.0, 1.0);
  EXPECT_NEAR(action.preview.y, 5.0, 0.5);
}

TEST(LibraryDrop, RejectedTreePreviewIsInvalidAtTheCursor) {
  RoadNetwork network = with_straight_road();
  // 200 m off the road — beyond the snap threshold: rejected, ghost tints at the
  // cursor rather than relocating the object.
  const LibraryDropAction action = resolve_library_drop(tree("tree_pine"), network, 50.0, 200.0);
  EXPECT_EQ(action.kind, LibraryDropKind::None);
  EXPECT_FALSE(action.preview.valid);
  EXPECT_DOUBLE_EQ(action.preview.x, 50.0);
  EXPECT_DOUBLE_EQ(action.preview.y, 200.0);
}

TEST(LibraryDrop, TeedAssemblyPreviewSitsOnTheRoad) {
  RoadNetwork network = with_straight_road();
  // Dropped 2 m off the road at x=50 — tees in at s≈50, preview snaps onto the
  // reference line (t=0, y≈0), not at the off-to-the-side cursor.
  const LibraryDropAction action = resolve_library_drop(assembly("t"), network, 50.0, 2.0);
  ASSERT_EQ(action.kind, LibraryDropKind::Assembly);
  EXPECT_TRUE(action.preview.valid);
  EXPECT_NEAR(action.preview.x, 50.0, 1.0);
  EXPECT_NEAR(action.preview.y, 0.0, 0.01); // pulled onto the road
}

TEST(LibraryDrop, StandaloneAssemblyPreviewIsAtTheCursor) {
  RoadNetwork network = with_straight_road();
  const LibraryDropAction action = resolve_library_drop(assembly("t"), network, 50.0, 200.0);
  ASSERT_EQ(action.kind, LibraryDropKind::Assembly);
  EXPECT_TRUE(action.preview.valid);
  EXPECT_DOUBLE_EQ(action.preview.x, 50.0);
  EXPECT_DOUBLE_EQ(action.preview.y, 200.0);
}

TEST(LibraryDrop, ProfileForMapsNamesWithARuralDefault) {
  EXPECT_EQ(profile_for("urban_sidewalk").left.size(), LaneProfile::urban_sidewalk().left.size());
  EXPECT_EQ(profile_for("highway").right.size(), LaneProfile::highway().right.size());
  EXPECT_EQ(profile_for("nonsense").right.size(), LaneProfile::two_lane_rural().right.size());
}

// --- road styles (p2-s8) ----------------------------------------------------

LibraryItem road_style(const char* style) {
  LibraryItem item;
  item.key = QStringLiteral("style.urban");
  item.label = QStringLiteral("Urban 2-lane");
  item.kind = LibraryItem::Kind::RoadStyle;
  item.style = QString::fromLatin1(style);
  return item;
}

TEST(LibraryDrop, RoadStyleDropAppliesToTheNearestRoadAndNamesItForTheHighlight) {
  RoadNetwork network = with_straight_road(); // (0,0)-(100,0), two_lane_rural
  const roadmaker::RoadId road = network.find_road("1");
  LibraryDropAction action = resolve_library_drop(road_style("urban_two_lane"), network, 50.0, 4.0);
  ASSERT_EQ(action.kind, LibraryDropKind::RoadStyle);
  ASSERT_NE(action.command, nullptr);
  EXPECT_EQ(action.target_road, road); // drives the drag highlight
  EXPECT_TRUE(action.preview.valid);
  EXPECT_FALSE(action.toast.isEmpty());

  ASSERT_TRUE(action.command->apply(network).has_value());
  EXPECT_EQ(network.road(road)->sections.size(), 1U);
  EXPECT_EQ(count_errors(validate_network(network)), 0U);
}

TEST(LibraryDrop, RoadStyleDroppedOffAnyRoadIsRejectedWithAHint) {
  RoadNetwork network = with_straight_road();
  LibraryDropAction action =
      resolve_library_drop(road_style("urban_two_lane"), network, 50.0, 200.0);
  EXPECT_EQ(action.kind, LibraryDropKind::None);
  EXPECT_EQ(action.command, nullptr);
  EXPECT_FALSE(action.toast.isEmpty());
}

TEST(LibraryDrop, RoadStyleRefusesAConnectingRoad) {
  RoadNetwork network = with_straight_road();
  // Tee a junction onto the road — that builds connecting roads (junction set).
  auto tee = edit::assembly::tee_onto_road(network, network.find_road("1"), 50.0);
  ASSERT_TRUE(tee->apply(network).has_value());
  roadmaker::RoadId connecting;
  network.for_each_road([&](roadmaker::RoadId id, const Road& road) {
    if (road.junction.is_valid()) {
      connecting = id;
    }
  });
  ASSERT_TRUE(connecting.is_valid());
  // Drop at the connecting road's own midpoint, where it is the nearest road.
  const Road* road = network.road(connecting);
  const auto mid = station_to_world(road->plan_view, road->plan_view.length() * 0.5, 0.0);
  const LibraryDropAction action =
      resolve_library_drop(road_style("urban_two_lane"), network, mid[0], mid[1]);
  EXPECT_EQ(action.kind, LibraryDropKind::None);
  EXPECT_TRUE(action.toast.contains(QStringLiteral("connecting")));
}

TEST(LibraryDrop, StyleForMapsNamesAndItemKeysWithAnUrbanDefault) {
  EXPECT_EQ(style_for("two_lane_rural").left.size(), RoadStyle::two_lane_rural().left.size());
  EXPECT_EQ(style_for("highway").left.size(), RoadStyle::highway().left.size());
  // The Attributes slot passes the item key straight through.
  EXPECT_EQ(style_for("style.highway").left.size(), RoadStyle::highway().left.size());
  EXPECT_EQ(style_for("style.urban").left.size(), RoadStyle::urban_two_lane().left.size());
  EXPECT_EQ(style_for("anything else").left.size(), RoadStyle::urban_two_lane().left.size());
}

} // namespace
} // namespace roadmaker::editor
