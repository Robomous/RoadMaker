#include "roadmaker/edit/assembly.hpp"
#include "roadmaker/edit/connection.hpp"
#include "roadmaker/edit/operations.hpp"
#include "roadmaker/road/authoring.hpp"
#include "roadmaker/road/lane.hpp"
#include "roadmaker/road/lane_section.hpp"
#include "roadmaker/road/network.hpp"
#include "roadmaker/road/object.hpp"
#include "roadmaker/road/road.hpp"
#include "roadmaker/xodr/diagnostic.hpp"
#include "roadmaker/xodr/writer.hpp"

#include <gtest/gtest.h>

#include <QString>
#include <optional>
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

// --- markings (p6-s2) --------------------------------------------------------

LibraryItem marking(const char* type, const char* color, double width = 0.12) {
  LibraryItem item;
  item.key = QStringLiteral("marking.x");
  item.label = QStringLiteral("Marking");
  item.kind = LibraryItem::Kind::Marking;
  item.mark_type = QString::fromLatin1(type);
  item.mark_color = QString::fromLatin1(color);
  item.mark_width = width;
  return item;
}

LibraryItem material_item(const char* name) {
  LibraryItem item;
  item.key = QStringLiteral("material.x");
  item.label = QStringLiteral("Material");
  item.kind = LibraryItem::Kind::Material;
  item.material = QString::fromLatin1(name);
  return item;
}

// The first road-mark record of the lane with OpenDRIVE `odr_id` in the first
// section of the (single) road, or nullopt when unmarked.
std::optional<RoadMark> first_mark_for_odr(const RoadNetwork& network, int odr_id) {
  std::optional<RoadMark> result;
  network.for_each_road([&](RoadId, const Road& road) {
    if (road.sections.empty()) {
      return;
    }
    const LaneSection* section = network.lane_section(road.sections.front());
    if (section == nullptr) {
      return;
    }
    for (const LaneId lid : section->lanes) {
      const Lane* lane = network.lane(lid);
      if (lane != nullptr && lane->odr_id == odr_id && !lane->road_marks.empty()) {
        result = lane->road_marks.front();
      }
    }
  });
  return result;
}

TEST(LibraryDrop, MarkingDropOnLaneBoundaryPushesSetRoadMark) {
  RoadNetwork network = with_straight_road(); // (0,0)-(100,0), heading +x
  // Drop toward the right carriageway edge (heading +x → -t is right).
  LibraryDropAction action =
      resolve_library_drop(marking("solid_solid", "yellow"), network, 50.0, -3.0);
  ASSERT_EQ(action.kind, LibraryDropKind::Marking);
  ASSERT_NE(action.command, nullptr);
  EXPECT_EQ(action.command->name(), "Set Road Mark");
  EXPECT_FALSE(action.toast.isEmpty());
  ASSERT_TRUE(action.command->apply(network).has_value());
  EXPECT_EQ(count_errors(validate_network(network)), 0U);
  // The mutated network still writes valid xodr.
  const auto xodr = roadmaker::write_xodr(network, "marking test");
  ASSERT_TRUE(xodr.has_value());
  // Undo restores the pre-drop mark exactly.
  ASSERT_TRUE(action.command->revert(network).has_value());
  EXPECT_EQ(count_errors(validate_network(network)), 0U);
}

// Regression for the LaneBoundaryHit::centre flag: a drop on the centre line
// must paint lane 0, not the ±1 lane nearest_lane_boundary reports for inserts.
TEST(LibraryDrop, DoubleYellowOnCentreTargetsLaneZero) {
  RoadNetwork network = with_straight_road();
  LibraryDropAction action =
      resolve_library_drop(marking("solid_solid", "yellow"), network, 50.0, 0.0);
  ASSERT_EQ(action.kind, LibraryDropKind::Marking);
  ASSERT_TRUE(action.command->apply(network).has_value());

  const auto centre_mark = first_mark_for_odr(network, 0);
  ASSERT_TRUE(centre_mark.has_value()); // lane 0 (the centre line) got it
  EXPECT_EQ(centre_mark->type, RoadMarkType::SolidSolid);
  EXPECT_EQ(centre_mark->color, RoadMarkColor::Yellow);
}

TEST(LibraryDrop, MarkingGhostSitsOnTheBoundary) {
  RoadNetwork network = with_straight_road();
  // Drop on the centre line: the ghost lands on it (t = 0 → y ≈ 0).
  const LibraryDropAction action =
      resolve_library_drop(marking("solid", "white"), network, 50.0, 0.0);
  ASSERT_EQ(action.kind, LibraryDropKind::Marking);
  EXPECT_TRUE(action.preview.valid);
  EXPECT_NEAR(action.preview.x, 50.0, 1.0);
  EXPECT_NEAR(action.preview.y, 0.0, 0.01);
}

TEST(LibraryDrop, MarkingDroppedOffAnyRoadIsRejectedWithAHint) {
  RoadNetwork network = with_straight_road();
  const LibraryDropAction action =
      resolve_library_drop(marking("solid", "white"), network, 50.0, 200.0);
  EXPECT_EQ(action.kind, LibraryDropKind::None);
  EXPECT_EQ(action.command, nullptr);
  EXPECT_FALSE(action.toast.isEmpty());
}

TEST(LibraryDrop, IdenticalMarkingIsANoOpWithAToast) {
  RoadNetwork network = with_straight_road();
  LibraryDropAction first =
      resolve_library_drop(marking("solid_solid", "yellow"), network, 50.0, 0.0);
  ASSERT_EQ(first.kind, LibraryDropKind::Marking);
  ASSERT_TRUE(first.command->apply(network).has_value());
  // The same marking on the same boundary now changes nothing → rejected.
  const LibraryDropAction again =
      resolve_library_drop(marking("solid_solid", "yellow"), network, 50.0, 0.0);
  EXPECT_EQ(again.kind, LibraryDropKind::None);
  EXPECT_EQ(again.command, nullptr);
  EXPECT_TRUE(again.toast.contains(QStringLiteral("already")));
}

TEST(LibraryDrop, UnknownMarkTypeIsRejected) {
  RoadNetwork network = with_straight_road();
  const LibraryDropAction action =
      resolve_library_drop(marking("dotted", "white"), network, 50.0, 0.0);
  EXPECT_EQ(action.kind, LibraryDropKind::None);
  EXPECT_EQ(action.command, nullptr);
  EXPECT_FALSE(action.toast.isEmpty());
}

TEST(LibraryDrop, BrokenBrokenDropSetsTypeAndWritesSpaceSpelling) {
  RoadNetwork network = with_straight_road();
  const LibraryDropAction action =
      resolve_library_drop(marking("broken_broken", "yellow"), network, 50.0, 0.0);
  ASSERT_EQ(action.kind, LibraryDropKind::Marking);
  ASSERT_TRUE(action.command->apply(network).has_value());

  const auto centre_mark = first_mark_for_odr(network, 0);
  ASSERT_TRUE(centre_mark.has_value());
  EXPECT_EQ(centre_mark->type, RoadMarkType::BrokenBroken);
  EXPECT_EQ(centre_mark->color, RoadMarkColor::Yellow);

  // The manifest spelling is underscored; the xodr writer emits the ASAM space
  // form (Annex A.3.4 Table 173).
  const auto xodr = roadmaker::write_xodr(network, "marking test");
  ASSERT_TRUE(xodr.has_value());
  EXPECT_NE(xodr->find("broken broken"), std::string::npos);
  EXPECT_EQ(count_errors(validate_network(network)), 0U);
}

TEST(LibraryDrop, DashedMarkingDropSetsBrokenType) {
  RoadNetwork network = with_straight_road();
  const LibraryDropAction action =
      resolve_library_drop(marking("broken", "white"), network, 50.0, 0.0);
  ASSERT_EQ(action.kind, LibraryDropKind::Marking);
  ASSERT_TRUE(action.command->apply(network).has_value());
  const auto centre_mark = first_mark_for_odr(network, 0);
  ASSERT_TRUE(centre_mark.has_value());
  EXPECT_EQ(centre_mark->type, RoadMarkType::Broken);
  EXPECT_EQ(centre_mark->color, RoadMarkColor::White);
}

TEST(LibraryDrop, EdgeWidthPropagatesToTheMark) {
  RoadNetwork network = with_straight_road();
  // A wide edge line: the 0.30 m width must reach the authored mark.
  const LibraryDropAction action =
      resolve_library_drop(marking("solid", "white", 0.30), network, 50.0, -3.0);
  ASSERT_EQ(action.kind, LibraryDropKind::Marking);
  ASSERT_TRUE(action.command->apply(network).has_value());
  const auto mark = first_mark_for_odr(network, -1);
  ASSERT_TRUE(mark.has_value());
  EXPECT_DOUBLE_EQ(mark->width, 0.30);
}

// The <material> records of the lane with OpenDRIVE `odr_id` in the first
// section of the (single) road.
std::vector<LaneMaterial> materials_for_odr(const RoadNetwork& network, int odr_id) {
  std::vector<LaneMaterial> result;
  network.for_each_road([&](RoadId, const Road& road) {
    if (road.sections.empty()) {
      return;
    }
    const LaneSection* section = network.lane_section(road.sections.front());
    if (section == nullptr) {
      return;
    }
    for (const LaneId lid : section->lanes) {
      const Lane* lane = network.lane(lid);
      if (lane != nullptr && lane->odr_id == odr_id) {
        result = lane->materials;
      }
    }
  });
  return result;
}

TEST(LibraryDrop, MaterialDropOnLanePushesSetLaneMaterial) {
  RoadNetwork network = with_straight_road(); // (0,0)-(100,0), heading +x
  // Drop into the right driving lane (-1): heading +x → -t is right.
  LibraryDropAction action =
      resolve_library_drop(material_item("asphalt_worn"), network, 50.0, -2.0);
  ASSERT_EQ(action.kind, LibraryDropKind::Material);
  ASSERT_NE(action.command, nullptr);
  EXPECT_EQ(action.command->name(), "Set Lane Material");
  EXPECT_TRUE(action.preview.valid);
  ASSERT_TRUE(action.command->apply(network).has_value());
  EXPECT_EQ(count_errors(validate_network(network)), 0U);

  const std::vector<LaneMaterial> records = materials_for_odr(network, -1);
  ASSERT_EQ(records.size(), 1U);
  EXPECT_DOUBLE_EQ(records.front().s_offset, 0.0);
  ASSERT_TRUE(records.front().surface.has_value());
  EXPECT_EQ(*records.front().surface, "rm:asphalt_worn"); // rm: prefix authored
  EXPECT_TRUE(records.front().friction.has_value());      // nominal catalog friction

  // Round-trips to valid xodr; undo restores the unmarked lane.
  const auto xodr = roadmaker::write_xodr(network, "material test");
  ASSERT_TRUE(xodr.has_value());
  ASSERT_TRUE(action.command->revert(network).has_value());
  EXPECT_TRUE(materials_for_odr(network, -1).empty());
}

TEST(LibraryDrop, MaterialDropOnCentreLineIsRejected) {
  RoadNetwork network = with_straight_road();
  const LibraryDropAction action =
      resolve_library_drop(material_item("asphalt"), network, 50.0, 0.0);
  EXPECT_EQ(action.kind, LibraryDropKind::None);
  EXPECT_EQ(action.command, nullptr);
  EXPECT_FALSE(action.toast.isEmpty());
}

TEST(LibraryDrop, MaterialDropOffAnyRoadIsRejectedWithAHint) {
  RoadNetwork network = with_straight_road();
  const LibraryDropAction action =
      resolve_library_drop(material_item("asphalt"), network, 50.0, 200.0);
  EXPECT_EQ(action.kind, LibraryDropKind::None);
  EXPECT_EQ(action.command, nullptr);
  EXPECT_FALSE(action.toast.isEmpty());
}

TEST(LibraryDrop, IdenticalMaterialIsANoOpWithAToast) {
  RoadNetwork network = with_straight_road();
  LibraryDropAction first = resolve_library_drop(material_item("asphalt"), network, 50.0, -2.0);
  ASSERT_EQ(first.kind, LibraryDropKind::Material);
  ASSERT_TRUE(first.command->apply(network).has_value());
  // The same material on the same lane now changes nothing → rejected.
  const LibraryDropAction again =
      resolve_library_drop(material_item("asphalt"), network, 50.0, -2.0);
  EXPECT_EQ(again.kind, LibraryDropKind::None);
  EXPECT_EQ(again.command, nullptr);
  EXPECT_TRUE(again.toast.contains(QStringLiteral("already")));
}

TEST(LibraryDrop, UnknownMaterialIsRejected) {
  RoadNetwork network = with_straight_road();
  const LibraryDropAction action =
      resolve_library_drop(material_item("granite"), network, 50.0, -2.0);
  EXPECT_EQ(action.kind, LibraryDropKind::None);
  EXPECT_EQ(action.command, nullptr);
  EXPECT_FALSE(action.toast.isEmpty());
}

// --- crosswalk placement (p3-s3) --------------------------------------------

LibraryItem crosswalk_item() {
  LibraryItem item;
  item.key = QStringLiteral("crosswalk.zebra");
  item.label = QStringLiteral("Zebra crosswalk");
  item.kind = LibraryItem::Kind::Crosswalk;
  item.crosswalk_width = 3.0; // walking depth along the road
  item.crosswalk_dash = 0.5;
  item.crosswalk_gap = 0.5;
  item.crosswalk_material = QStringLiteral("material.paint_white");
  item.crosswalk_segmentation = QStringLiteral("crosswalk");
  return item;
}

/// Three straight two-lane arms welded into one junction near the origin — the
/// signalized approach layout a crosswalk drop targets.
RoadNetwork crosswalk_junction() {
  RoadNetwork network;
  const auto arm = [&](Waypoint a, Waypoint b) {
    auto command = edit::create_road({a, b}, LaneProfile::two_lane_rural(), "");
    if (command == nullptr || !command->apply(network).has_value()) {
      throw std::runtime_error("arm setup failed");
    }
  };
  arm(Waypoint{-40.0, 0.0}, Waypoint{-6.0, 0.0});
  arm(Waypoint{40.0, 0.0}, Waypoint{6.0, 0.0});
  arm(Waypoint{0.0, -40.0}, Waypoint{0.0, -6.0});
  const std::vector<roadmaker::RoadEnd> ends{
      {network.find_road("1"), roadmaker::ContactPoint::End},
      {network.find_road("2"), roadmaker::ContactPoint::End},
      {network.find_road("3"), roadmaker::ContactPoint::End}};
  auto junction = edit::create_junction(network, ends);
  if (junction == nullptr || !junction->apply(network).has_value()) {
    throw std::runtime_error("junction setup failed");
  }
  return network;
}

TEST(LibraryDrop, CrosswalkDropOnAnApproachPlacesPairOnThatArm) {
  RoadNetwork network = crosswalk_junction();
  // Drop over the west approach, 10 m out from the junction.
  LibraryDropAction action = resolve_library_drop(crosswalk_item(), network, -10.0, 0.0);
  ASSERT_EQ(action.kind, LibraryDropKind::Crosswalk);
  EXPECT_EQ(action.command, nullptr); // the pair goes through action.objects
  EXPECT_TRUE(action.preview.valid);
  EXPECT_NEAR(action.preview.x, -6.0, 1e-6); // ghost at the arm/junction anchor
  EXPECT_NEAR(action.preview.y, 0.0, 1e-6);
  EXPECT_FALSE(action.toast.isEmpty());

  // A crosswalk AND its stop line, both on the west arm; adding them stays valid.
  const RoadId west = network.find_road("1");
  int crosswalks = 0;
  int stop_lines = 0;
  for (auto& [road, object] : action.objects) {
    EXPECT_EQ(road, west);
    if (object.type == ObjectType::Crosswalk) {
      ++crosswalks;
    } else if (object.type_str == "roadMark" && object.subtype == "signalLines") {
      ++stop_lines;
    }
    auto command = edit::add_object(network, road, object);
    ASSERT_NE(command, nullptr);
    ASSERT_TRUE(command->apply(network).has_value());
  }
  EXPECT_EQ(crosswalks, 1);
  EXPECT_EQ(stop_lines, 1);
  EXPECT_EQ(count_errors(validate_network(network)), 0U);
}

TEST(LibraryDrop, CrosswalkDroppedInOpenSpaceIsRejectedWithAHint) {
  RoadNetwork network = crosswalk_junction();
  const LibraryDropAction action = resolve_library_drop(crosswalk_item(), network, 0.0, 50.0);
  EXPECT_EQ(action.kind, LibraryDropKind::None);
  EXPECT_TRUE(action.objects.empty());
  EXPECT_FALSE(action.preview.valid);
  EXPECT_FALSE(action.toast.isEmpty());
}

} // namespace
} // namespace roadmaker::editor
