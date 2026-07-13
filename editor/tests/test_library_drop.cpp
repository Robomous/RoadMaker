#include "roadmaker/road/authoring.hpp"
#include "roadmaker/road/network.hpp"
#include "roadmaker/road/object.hpp"
#include "roadmaker/xodr/diagnostic.hpp"
#include "roadmaker/xodr/writer.hpp"

#include <gtest/gtest.h>

#include <QString>
#include <vector>

#include "document/library_drop.hpp"
#include "document/library_manifest.hpp"

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

TEST(LibraryDrop, ProfileForMapsNamesWithARuralDefault) {
  EXPECT_EQ(profile_for("urban_sidewalk").left.size(), LaneProfile::urban_sidewalk().left.size());
  EXPECT_EQ(profile_for("highway").right.size(), LaneProfile::highway().right.size());
  EXPECT_EQ(profile_for("nonsense").right.size(), LaneProfile::two_lane_rural().right.size());
}

} // namespace
} // namespace roadmaker::editor
