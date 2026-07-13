#include "roadmaker/road/network.hpp"
#include "roadmaker/xodr/diagnostic.hpp"
#include "roadmaker/xodr/writer.hpp"

#include <gtest/gtest.h>

#include <QString>

#include "document/library_drop.hpp"
#include "document/library_manifest.hpp"

namespace roadmaker::editor {
namespace {

using roadmaker::count_errors;
using roadmaker::LaneProfile;
using roadmaker::RoadNetwork;
using roadmaker::validate_network;

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

TEST(LibraryDrop, ProfileForMapsNamesWithARuralDefault) {
  EXPECT_EQ(profile_for("urban_sidewalk").left.size(), LaneProfile::urban_sidewalk().left.size());
  EXPECT_EQ(profile_for("highway").right.size(), LaneProfile::highway().right.size());
  EXPECT_EQ(profile_for("nonsense").right.size(), LaneProfile::two_lane_rural().right.size());
}

} // namespace
} // namespace roadmaker::editor
