// Prop placement helper (p6-s4, issue #238): the pure geometry behind the Prop
// Point and Prop Curve tools. Builds a straight road and asserts road snapping,
// prop-object construction, spacing distribution (including the s=0 sample), the
// off-anchor skip, batch-wide odr-id uniqueness, and the rejection paths.

#include "roadmaker/edit/operations.hpp"
#include "roadmaker/road/network.hpp"
#include "roadmaker/road/object.hpp"

#include <gtest/gtest.h>

#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#include "document/library_manifest.hpp"
#include "document/prop_placement.hpp"
#include "viewport/picking.hpp"

namespace roadmaker::editor {
namespace {

using roadmaker::RoadNetwork;
using roadmaker::Waypoint;

/// A straight road along the x-axis from (-10, 0) to (10, 0): length 20 m, odr
/// id "1", s = 0 at the west end.
RoadNetwork straight_road() {
  RoadNetwork network;
  auto command = roadmaker::edit::create_road(
      {Waypoint{-10.0, 0.0}, Waypoint{10.0, 0.0}}, roadmaker::LaneProfile::two_lane_rural(), "");
  if (command == nullptr || !command->apply(network).has_value()) {
    throw std::runtime_error("road setup failed");
  }
  return network;
}

LibraryItem pine_item() {
  LibraryItem item;
  item.key = "prop.tree.pine";
  item.label = "Pine tree";
  item.kind = LibraryItem::Kind::Tree;
  item.model = "tree_pine";
  return item;
}

LibraryItem shrub_item() {
  LibraryItem item;
  item.key = "prop.shrub";
  item.label = "Shrub";
  item.kind = LibraryItem::Kind::Tree;
  item.model = "shrub";
  return item;
}

TEST(PropPlacement, SnapAcceptsOnRoadRejectsOpenSpace) {
  const RoadNetwork network = straight_road();
  const auto on = nearest_road_station(network, 0.0, 0.5, kObjectSnapThreshold);
  ASSERT_TRUE(on.has_value());
  EXPECT_EQ(on->road, network.find_road("1"));
  // Far off any road: no snap.
  EXPECT_FALSE(nearest_road_station(network, 0.0, 80.0, kObjectSnapThreshold).has_value());
}

TEST(PropPlacement, IsPropAssetOnlyForTrees) {
  EXPECT_TRUE(is_prop_asset(pine_item()));
  LibraryItem not_a_prop;
  not_a_prop.kind = LibraryItem::Kind::Stencil;
  EXPECT_FALSE(is_prop_asset(not_a_prop));
}

TEST(PropPlacement, MakePropObjectCarriesTypeAndDimensions) {
  const Object tree = make_prop_object(pine_item(), "7", 3.0, -1.0);
  EXPECT_EQ(tree.odr_id, "7");
  EXPECT_EQ(tree.name, "tree_pine");
  EXPECT_EQ(tree.type, ObjectType::Tree);
  EXPECT_DOUBLE_EQ(tree.s, 3.0);
  EXPECT_DOUBLE_EQ(tree.t, -1.0);
  ASSERT_TRUE(tree.radius.has_value());
  ASSERT_TRUE(tree.height.has_value());
  EXPECT_GT(*tree.radius, 0.0);
  EXPECT_GT(*tree.height, 0.0);

  // A shrub is Vegetation rather than Tree.
  const Object shrub = make_prop_object(shrub_item(), "8", 0.0, 0.0);
  EXPECT_EQ(shrub.type, ObjectType::Vegetation);
}

TEST(PropPlacement, DistributesEverySpacingIncludingZero) {
  const RoadNetwork network = straight_road();
  const RoadId anchor = network.find_road("1");
  const std::vector<Waypoint> points{{-10.0, 0.0}, {10.0, 0.0}};
  // 20 m curve, 5 m spacing → props at s = 0, 5, 10, 15, 20 = 5 props.
  const auto dist = distribute_props_along_curve(network, anchor, points, pine_item(), 5.0);
  ASSERT_TRUE(dist.has_value());
  EXPECT_EQ(dist->props.size(), 5U);
  EXPECT_EQ(dist->preview_points.size(), 5U);
  EXPECT_EQ(dist->skipped, 0U);
  // The first prop sits at the west end (s ≈ 0).
  EXPECT_NEAR(dist->props.front().second.s, 0.0, 0.25);
}

TEST(PropPlacement, SpacingChangesCount) {
  const RoadNetwork network = straight_road();
  const RoadId anchor = network.find_road("1");
  const std::vector<Waypoint> points{{-10.0, 0.0}, {10.0, 0.0}};
  // 10 m spacing → s = 0, 10, 20 = 3 props.
  const auto dist = distribute_props_along_curve(network, anchor, points, pine_item(), 10.0);
  ASSERT_TRUE(dist.has_value());
  EXPECT_EQ(dist->props.size(), 3U);
}

TEST(PropPlacement, SkipsSamplesThatLeaveTheAnchor) {
  const RoadNetwork network = straight_road();
  const RoadId anchor = network.find_road("1");
  // A curve that runs perpendicular away from the road: the far samples exceed
  // the lateral snap threshold and are skipped, not relocated.
  const std::vector<Waypoint> points{{-10.0, 0.0}, {-10.0, 30.0}};
  const auto dist = distribute_props_along_curve(network, anchor, points, pine_item(), 5.0);
  ASSERT_TRUE(dist.has_value());
  EXPECT_GT(dist->skipped, 0U);
  EXPECT_FALSE(dist->props.empty()); // the near samples still land
}

TEST(PropPlacement, MintsUniqueOdrIdsAcrossBatchAndNetwork) {
  RoadNetwork network = straight_road();
  const RoadId anchor = network.find_road("1");
  // Seed one existing object so the batch must dodge its id.
  auto add =
      roadmaker::edit::add_object(network, anchor, make_prop_object(pine_item(), "1", 5.0, 0.0));
  ASSERT_NE(add, nullptr);
  ASSERT_TRUE(add->apply(network).has_value());

  const std::vector<Waypoint> points{{-10.0, 0.0}, {10.0, 0.0}};
  const auto dist = distribute_props_along_curve(network, anchor, points, pine_item(), 5.0);
  ASSERT_TRUE(dist.has_value());

  std::set<std::string> ids;
  for (const auto& [road, object] : dist->props) {
    EXPECT_TRUE(ids.insert(object.odr_id).second) << "duplicate odr id " << object.odr_id;
    EXPECT_NE(object.odr_id, "1") << "reused the existing object's id";
  }
  EXPECT_EQ(ids.size(), dist->props.size());
}

TEST(PropPlacement, RejectsDegenerateInput) {
  const RoadNetwork network = straight_road();
  const RoadId anchor = network.find_road("1");
  const std::vector<Waypoint> one{{0.0, 0.0}};
  EXPECT_FALSE(distribute_props_along_curve(network, anchor, one, pine_item(), 5.0).has_value());

  const std::vector<Waypoint> points{{-10.0, 0.0}, {10.0, 0.0}};
  // Non-positive spacing is refused.
  EXPECT_FALSE(distribute_props_along_curve(network, anchor, points, pine_item(), 0.0).has_value());

  // A curve entirely off the anchor road: zero survivors → refused.
  const std::vector<Waypoint> off{{0.0, 80.0}, {20.0, 80.0}};
  EXPECT_FALSE(distribute_props_along_curve(network, anchor, off, pine_item(), 5.0).has_value());
}

} // namespace
} // namespace roadmaker::editor
