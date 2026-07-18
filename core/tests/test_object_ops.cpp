// Tests for the object edit-command factories (add/delete/move_object) and the
// instanced-prop mesh they feed. The commands follow the M2 contract:
// apply→revert is byte-identical, a failed apply leaves the network untouched,
// and restore-in-place keeps the ObjectId across undo/redo.

#include "roadmaker/assets/prop_library.hpp"
#include "roadmaker/edit/operations.hpp"
#include "roadmaker/mesh/mesh_builder.hpp"
#include "roadmaker/road/authoring.hpp"
#include "roadmaker/road/network.hpp"
#include "roadmaker/road/object.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

#include "support/network_compare.hpp"

namespace roadmaker {
namespace {

using test::snapshot_xodr;

RoadId author_street(RoadNetwork& network) {
  const std::vector<Waypoint> waypoints{Waypoint{.x = 0.0, .y = 0.0},
                                        Waypoint{.x = 120.0, .y = 0.0}};
  auto road = author_clothoid_road(network, waypoints, LaneProfile::two_lane_default(), "", "1");
  if (!road.has_value()) {
    throw std::runtime_error("author_street: " + road.error().message);
  }
  return *road;
}

Object make_tree(std::string odr_id, double s, double t) {
  Object tree;
  tree.odr_id = std::move(odr_id);
  tree.name = "tree_pine"; // a bundled prop model
  tree.type = ObjectType::Tree;
  tree.s = s;
  tree.t = t;
  tree.radius = 1.2;
  tree.height = 4.2;
  return tree;
}

TEST(ObjectOps, AddApplyRevertIsByteIdentical) {
  RoadNetwork network;
  const RoadId road = author_street(network);
  const std::string before = snapshot_xodr(network);

  auto command = edit::add_object(network, road, make_tree("1", 10.0, 4.0));
  ASSERT_TRUE(command->apply(network).has_value());
  EXPECT_EQ(network.object_count(), 1U);
  EXPECT_NE(snapshot_xodr(network), before) << "adding an object must change the output";

  ASSERT_TRUE(command->revert(network).has_value());
  EXPECT_EQ(network.object_count(), 0U);
  EXPECT_EQ(snapshot_xodr(network), before) << "undo must be byte-identical";

  // Redo resurrects the object identically.
  ASSERT_TRUE(command->apply(network).has_value());
  EXPECT_EQ(network.object_count(), 1U);
}

TEST(ObjectOps, AddDirtiesOwningRoad) {
  RoadNetwork network;
  const RoadId road = author_street(network);
  auto command = edit::add_object(network, road, make_tree("1", 10.0, 4.0));
  ASSERT_TRUE(command->apply(network).has_value());
  const edit::DirtySet dirty = command->dirty();
  EXPECT_TRUE(std::ranges::find(dirty.objects, road) != dirty.objects.end());
  EXPECT_FALSE(dirty.topology);
}

TEST(ObjectOps, AddRejectsStaleRoadWithoutMutating) {
  RoadNetwork network;
  author_street(network);
  const std::string before = snapshot_xodr(network);
  auto command = edit::add_object(network, RoadId{}, make_tree("1", 10.0, 4.0));
  EXPECT_FALSE(command->apply(network).has_value());
  EXPECT_EQ(network.object_count(), 0U);
  EXPECT_EQ(snapshot_xodr(network), before);
}

TEST(ObjectOps, AddRejectsStationOutsideRoad) {
  RoadNetwork network;
  const RoadId road = author_street(network);
  auto command = edit::add_object(network, road, make_tree("1", 10000.0, 4.0));
  EXPECT_FALSE(command->apply(network).has_value());
  EXPECT_EQ(network.object_count(), 0U);
}

TEST(ObjectOps, DeleteRestoresExactOnUndo) {
  RoadNetwork network;
  const RoadId road = author_street(network);
  const ObjectId id = network.add_object(road, make_tree("1", 10.0, 4.0));
  ASSERT_TRUE(id.is_valid());
  const std::string with_object = snapshot_xodr(network);

  auto command = edit::delete_object(network, id);
  ASSERT_TRUE(command->apply(network).has_value());
  EXPECT_EQ(network.object(id), nullptr);

  ASSERT_TRUE(command->revert(network).has_value());
  ASSERT_NE(network.object(id), nullptr) << "undo must restore the same ObjectId";
  EXPECT_EQ(snapshot_xodr(network), with_object);
}

TEST(ObjectOps, MoveApplyRevertIsByteIdentical) {
  RoadNetwork network;
  const RoadId road = author_street(network);
  const ObjectId id = network.add_object(road, make_tree("1", 10.0, 4.0));
  const std::string before = snapshot_xodr(network);

  auto command = edit::move_object(network, id, 50.0, -3.0, 0.25);
  ASSERT_TRUE(command->apply(network).has_value());
  EXPECT_DOUBLE_EQ(network.object(id)->s, 50.0);
  EXPECT_DOUBLE_EQ(network.object(id)->t, -3.0);
  EXPECT_DOUBLE_EQ(network.object(id)->hdg, 0.25);

  ASSERT_TRUE(command->revert(network).has_value());
  EXPECT_DOUBLE_EQ(network.object(id)->s, 10.0);
  EXPECT_EQ(snapshot_xodr(network), before);
}

TEST(ObjectOps, MoveRejectsStaleObject) {
  RoadNetwork network;
  author_street(network);
  auto command = edit::move_object(network, ObjectId{}, 5.0, 0.0);
  EXPECT_FALSE(command->apply(network).has_value());
}

// set_object_model backs the Attributes pane's "Model" slot (P1/GW-3): a
// library item dropped on the slot re-points the prop at another model.
TEST(ObjectOps, SetModelRetargetsThePropAndIsByteIdenticalOnRevert) {
  RoadNetwork network;
  const RoadId road = author_street(network);
  const ObjectId id = network.add_object(road, make_tree("1", 10.0, 4.0));
  const std::string before = snapshot_xodr(network);
  ASSERT_NE(props::model("shrub"), nullptr) << "fixture needs a second bundled model";

  auto command = edit::set_object_model(network, id, "shrub");
  ASSERT_TRUE(command->apply(network).has_value());
  EXPECT_EQ(network.object(id)->name, "shrub");
  // The bounding volume must describe the NEW model, not the pine it was.
  EXPECT_DOUBLE_EQ(network.object(id)->radius.value(), props::model("shrub")->radius);
  EXPECT_DOUBLE_EQ(network.object(id)->height.value(), props::model("shrub")->height);
  // Pose is untouched — this changes what the prop is, not where it is.
  EXPECT_DOUBLE_EQ(network.object(id)->s, 10.0);
  EXPECT_DOUBLE_EQ(network.object(id)->t, 4.0);

  ASSERT_TRUE(command->revert(network).has_value());
  EXPECT_EQ(snapshot_xodr(network), before);
  EXPECT_EQ(network.object(id)->name, "tree_pine");
}

TEST(ObjectOps, SetModelDirtiesTheOwningRoadSoThePropReMeshes) {
  RoadNetwork network;
  const RoadId road = author_street(network);
  const ObjectId id = network.add_object(road, make_tree("1", 10.0, 4.0));

  const edit::DirtySet dirty = edit::set_object_model(network, id, "shrub")->dirty();
  EXPECT_EQ(dirty.objects, std::vector<RoadId>{road});
}

TEST(ObjectOps, SetModelRejectsBadInputWithoutMutating) {
  RoadNetwork network;
  const RoadId road = author_street(network);
  const ObjectId id = network.add_object(road, make_tree("1", 10.0, 4.0));
  const std::string before = snapshot_xodr(network);

  EXPECT_FALSE(edit::set_object_model(network, ObjectId{}, "shrub")->apply(network).has_value())
      << "stale object id";
  EXPECT_FALSE(edit::set_object_model(network, id, "")->apply(network).has_value()) << "empty id";
  EXPECT_FALSE(edit::set_object_model(network, id, "no_such_model")->apply(network).has_value())
      << "an unknown model would render as nothing";
  EXPECT_EQ(snapshot_xodr(network), before) << "a failed apply must leave the network untouched";
}

TEST(ObjectOps, TreeObjectEmitsOneInstance) {
  RoadNetwork network;
  const RoadId road = author_street(network);
  network.add_object(road, make_tree("1", 40.0, 6.0));

  const NetworkMesh mesh = build_network_mesh(network, {});
  ASSERT_EQ(mesh.objects.size(), 1U);
  const ObjectInstance& instance = mesh.objects.front();
  EXPECT_EQ(instance.model_id, "tree_pine");
  EXPECT_EQ(instance.road, road);
  // The road runs along +x at y=0; s=40,t=6 lands near (40, 6) with z on the
  // surface (flat road → z≈0).
  EXPECT_NEAR(instance.position[0], 40.0, 1.0);
  EXPECT_NEAR(instance.position[1], 6.0, 1.0);
  EXPECT_NEAR(instance.position[2], 0.0, 0.01);
}

TEST(ObjectOps, NonPropObjectEmitsNoInstance) {
  RoadNetwork network;
  const RoadId road = author_street(network);
  Object pole;
  pole.odr_id = "1";
  pole.type = ObjectType::Pole;
  pole.s = 20.0;
  pole.t = -5.0;
  network.add_object(road, pole);

  const NetworkMesh mesh = build_network_mesh(network, {});
  EXPECT_TRUE(mesh.objects.empty()) << "a pole is not a bundled prop model";
}

// --- update_objects (batch replace behind an asset edit) --------------------

TEST(UpdateObjects, RewritesInstancesWithByteIdenticalUndo) {
  RoadNetwork network;
  const RoadId road = author_street(network);
  Object cw;
  cw.odr_id = "cw1";
  cw.type = ObjectType::Crosswalk;
  cw.s = 30.0;
  cw.crosswalk = CrosswalkData{.asset = "crosswalk.zebra", .dash_length = 0.5, .dash_gap = 0.5};
  const ObjectId id = network.add_object(road, cw);
  const std::string before = snapshot_xodr(network);

  Object updated = *network.object(id);
  updated.crosswalk->dash_length = 0.3; // asset width edit re-materialized
  auto command = edit::update_objects(network, {{id, updated}}, "Edit Crosswalk Asset");
  ASSERT_NE(command, nullptr);
  EXPECT_EQ(command->name(), "Edit Crosswalk Asset");
  ASSERT_TRUE(command->apply(network).has_value());

  EXPECT_EQ(network.object(id)->crosswalk->dash_length, 0.3); // same id, new value
  EXPECT_NE(snapshot_xodr(network), before);
  const edit::DirtySet dirty = command->dirty();
  EXPECT_TRUE(std::ranges::find(dirty.objects, road) != dirty.objects.end());

  ASSERT_TRUE(command->revert(network).has_value());
  EXPECT_EQ(snapshot_xodr(network), before) << "undo byte-identical";
  ASSERT_TRUE(command->apply(network).has_value()); // redo parity
  EXPECT_EQ(network.object(id)->crosswalk->dash_length, 0.3);
}

TEST(UpdateObjects, StaleIdIsCleanError) {
  RoadNetwork network;
  const RoadId road = author_street(network);
  const ObjectId id = network.add_object(road, make_tree("1", 10.0, 4.0));
  Object updated = *network.object(id);
  network.erase_object(id);
  const std::string before = snapshot_xodr(network);
  auto command = edit::update_objects(network, {{id, updated}}, "");
  ASSERT_NE(command, nullptr);
  EXPECT_FALSE(command->apply(network).has_value()); // stale id refused
  EXPECT_EQ(snapshot_xodr(network), before) << "failed apply leaves network untouched";
}

TEST(UpdateObjects, RefusesChangingOwningRoad) {
  RoadNetwork network;
  const RoadId road = author_street(network);
  const std::vector<Waypoint> other_wp{Waypoint{0.0, 40.0}, Waypoint{120.0, 40.0}};
  const RoadId other =
      author_clothoid_road(network, other_wp, LaneProfile::two_lane_default(), "", "2").value();
  const ObjectId id = network.add_object(road, make_tree("1", 10.0, 4.0));
  Object moved = *network.object(id);
  moved.road = other; // illegal: update_objects never re-parents
  auto command = edit::update_objects(network, {{id, moved}}, "");
  ASSERT_NE(command, nullptr);
  EXPECT_FALSE(command->apply(network).has_value());
}

TEST(UpdateObjects, EmptyBatchIsANoOp) {
  RoadNetwork network;
  author_street(network);
  const std::string before = snapshot_xodr(network);
  auto command = edit::update_objects(network, {}, "");
  ASSERT_NE(command, nullptr);
  ASSERT_TRUE(command->apply(network).has_value());
  EXPECT_EQ(snapshot_xodr(network), before);
  EXPECT_TRUE(command->dirty().objects.empty());
}

} // namespace
} // namespace roadmaker
