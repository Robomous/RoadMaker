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
#include "roadmaker/xodr/reader.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
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
  // The tree declares exactly the model height, so it draws at model size.
  EXPECT_EQ(instance.scale, 1.0);
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

TEST(ObjectOps, RepeatExpandsToMeshInstances) {
  // A <repeat> with distance>0 places a SERIES and suppresses the base single
  // instance (§13.4 "the <repeat> element takes precedence"). length=100,
  // distance=20 -> floor(100/20)=5 -> 6 instances; the road is 120 m so every
  // origin (s = 10,30,...,110) is on it.
  RoadNetwork network;
  const RoadId road = author_street(network);
  Object tree = make_tree("1", 10.0, 4.0);
  ObjectRepeat repeat;
  repeat.s = 10.0;
  repeat.length = 100.0;
  repeat.distance = 20.0;
  repeat.t_start = repeat.t_end = 4.0;
  tree.repeats = {repeat};
  network.add_object(road, tree);

  const NetworkMesh mesh = build_network_mesh(network, {});
  ASSERT_EQ(mesh.objects.size(), 6U);
  EXPECT_NEAR(mesh.objects.front().position[0], 10.0, 1.0);
  EXPECT_NEAR(mesh.objects.back().position[0], 110.0, 1.0);
  for (const ObjectInstance& instance : mesh.objects) {
    EXPECT_EQ(instance.model_id, "tree_pine");
    EXPECT_EQ(instance.scale, 1.0);
    EXPECT_NEAR(instance.position[1], 4.0, 1e-6); // constant t on a +x road
  }
}

TEST(ObjectOps, ContinuousRepeatFallsBackToSingleInstance) {
  // distance == 0 is a continuous (extruded) object, not a series: expand_repeat
  // yields nothing and the object keeps its single-instance placement.
  RoadNetwork network;
  const RoadId road = author_street(network);
  Object tree = make_tree("1", 40.0, 6.0);
  ObjectRepeat repeat;
  repeat.s = 5.0;
  repeat.length = 100.0;
  repeat.distance = 0.0; // continuous
  tree.repeats = {repeat};
  network.add_object(road, tree);

  const NetworkMesh mesh = build_network_mesh(network, {});
  ASSERT_EQ(mesh.objects.size(), 1U);
  EXPECT_NEAR(mesh.objects.front().position[0], 40.0, 1.0); // the object's own s/t
  EXPECT_NEAR(mesh.objects.front().position[1], 6.0, 1.0);
}

TEST(ObjectOps, DetachedRepeatFollowsChord) {
  // detachFromReferenceLine draws instances along the straight chord between the
  // section's start/end anchors. length=100, distance=50 -> 3 instances at
  // ds=0,50,100; the middle sits at the chord midpoint.
  RoadNetwork network;
  const RoadId road = author_street(network);
  Object tree = make_tree("1", 0.0, 0.0);
  ObjectRepeat repeat;
  repeat.s = 10.0;
  repeat.length = 100.0;
  repeat.distance = 50.0;
  repeat.t_start = 2.0;
  repeat.t_end = 8.0;
  repeat.detach_from_reference_line = true;
  tree.repeats = {repeat};
  network.add_object(road, tree);

  const NetworkMesh mesh = build_network_mesh(network, {});
  ASSERT_EQ(mesh.objects.size(), 3U);
  // Chord runs from (10, 2) to (110, 8) on this straight +x road; the midpoint
  // is (60, 5) and the chord heading is atan2(6, 100) about +x.
  EXPECT_NEAR(mesh.objects[1].position[0], 60.0, 1e-6);
  EXPECT_NEAR(mesh.objects[1].position[1], 5.0, 1e-6);
  EXPECT_NEAR(mesh.objects.front().position[0], 10.0, 1e-6);
  EXPECT_NEAR(mesh.objects.back().position[1], 8.0, 1e-6);
  EXPECT_NEAR(mesh.objects.front().heading, std::atan2(6.0, 100.0), 1e-6);
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

// --- per-instance prop size (#335) -------------------------------------------

TEST(InstanceScale, AbsentHeightIsUnit) {
  const props::PropModel* model = props::model("tree_pine");
  ASSERT_NE(model, nullptr);
  Object tree = make_tree("1", 10.0, 4.0);
  tree.height.reset();
  EXPECT_DOUBLE_EQ(props::instance_scale(tree, model), 1.0);
}

TEST(InstanceScale, UnknownModelIsUnit) {
  const Object tree = make_tree("1", 10.0, 4.0);
  EXPECT_DOUBLE_EQ(props::instance_scale(tree, nullptr), 1.0);
}

TEST(InstanceScale, NonPositiveHeightsAreUnit) {
  // A model with no authored height cannot define a ratio, and a non-positive
  // declared height is meaningless — both fall back to model size rather than
  // collapsing or mirroring the prop.
  const props::PropModel degenerate{.id = "degenerate", .height = 0.0};
  Object tree = make_tree("1", 10.0, 4.0);
  EXPECT_DOUBLE_EQ(props::instance_scale(tree, &degenerate), 1.0);

  const props::PropModel* model = props::model("tree_pine");
  ASSERT_NE(model, nullptr);
  tree.height = 0.0;
  EXPECT_DOUBLE_EQ(props::instance_scale(tree, model), 1.0);
  tree.height = -3.0;
  EXPECT_DOUBLE_EQ(props::instance_scale(tree, model), 1.0);
}

TEST(InstanceScale, HeightRatio) {
  const props::PropModel* model = props::model("tree_pine");
  ASSERT_NE(model, nullptr);
  Object tree = make_tree("1", 10.0, 4.0);

  tree.height = model->height * 2.0;
  EXPECT_DOUBLE_EQ(props::instance_scale(tree, model), 2.0);
  tree.height = model->height * 0.25;
  EXPECT_DOUBLE_EQ(props::instance_scale(tree, model), 0.25);
  // Declaring exactly the model height must be EXACTLY unit (IEEE x/x == 1.0)
  // — that is what keeps every pre-#335 scene rendering identically.
  tree.height = model->height;
  EXPECT_EQ(props::instance_scale(tree, model), 1.0);
}

TEST(ObjectOps, ScaledTreeObjectInstanceCarriesScale) {
  RoadNetwork network;
  const RoadId road = author_street(network);
  const props::PropModel* model = props::model("tree_pine");
  ASSERT_NE(model, nullptr);

  Object tree = make_tree("1", 40.0, 6.0);
  tree.height = model->height * 2.0;
  network.add_object(road, tree);

  const NetworkMesh mesh = build_network_mesh(network, {});
  ASSERT_EQ(mesh.objects.size(), 1U);
  EXPECT_DOUBLE_EQ(mesh.objects.front().scale, 2.0);
}

TEST(ObjectOps, RepeatSeriesSharesTheObjectScale) {
  RoadNetwork network;
  const RoadId road = author_street(network);
  const props::PropModel* model = props::model("tree_pine");
  ASSERT_NE(model, nullptr);

  Object tree = make_tree("1", 10.0, 4.0);
  tree.height = model->height * 3.0;
  ObjectRepeat repeat;
  repeat.s = 10.0;
  repeat.length = 100.0;
  repeat.distance = 20.0;
  repeat.t_start = repeat.t_end = 4.0;
  tree.repeats = {repeat};
  network.add_object(road, tree);

  const NetworkMesh mesh = build_network_mesh(network, {});
  ASSERT_EQ(mesh.objects.size(), 6U);
  for (const ObjectInstance& instance : mesh.objects) {
    EXPECT_DOUBLE_EQ(instance.scale, 3.0) << "every repeated instance renders at the declared size";
  }
}

TEST(ObjectOps, ResizedPropRoundTripsAsPlainOpenDriveAttributes) {
  // The instance dimensions are standard @height/@radius — resizing a prop must
  // not introduce any rm: userData, and must survive a parse unchanged.
  RoadNetwork network;
  const RoadId road = author_street(network);
  const ObjectId id = network.add_object(road, make_tree("1", 40.0, 6.0));
  Object resized = *network.object(id);
  resized.height = 9.0;
  resized.radius = 2.4;
  auto command = edit::update_objects(network, {{id, resized}}, "Resize Props");
  ASSERT_NE(command, nullptr);
  ASSERT_TRUE(command->apply(network).has_value());

  const std::string written = snapshot_xodr(network);
  // Scoped to the <objects> block — the road itself always carries an
  // rm:waypoints userData, which has nothing to do with the prop.
  const std::size_t objects_begin = written.find("<objects>");
  const std::size_t objects_end = written.find("</objects>");
  ASSERT_NE(objects_begin, std::string::npos);
  ASSERT_NE(objects_end, std::string::npos);
  const std::string objects_block = written.substr(objects_begin, objects_end - objects_begin);
  EXPECT_EQ(objects_block.find("rm:"), std::string::npos)
      << "prop dimensions are Layer 0 — no extension record:\n"
      << objects_block;
  EXPECT_EQ(objects_block.find("userData"), std::string::npos) << objects_block;
  EXPECT_NE(objects_block.find("height=\"9"), std::string::npos)
      << "the declared height is a plain OpenDRIVE attribute:\n"
      << objects_block;

  auto parsed = parse_xodr(written, "resized");
  ASSERT_TRUE(parsed.has_value()) << parsed.error().message;
  EXPECT_EQ(snapshot_xodr(parsed->network), written);

  const NetworkMesh mesh = build_network_mesh(parsed->network, {});
  ASSERT_EQ(mesh.objects.size(), 1U);
  EXPECT_DOUBLE_EQ(mesh.objects.front().scale, 9.0 / props::model("tree_pine")->height);
}

TEST(ObjectOps, ResizePropsCommandRoundTrip) {
  RoadNetwork network;
  const RoadId road = author_street(network);
  const ObjectId id = network.add_object(road, make_tree("1", 40.0, 6.0));

  Object resized = *network.object(id);
  ASSERT_TRUE(resized.height.has_value());
  ASSERT_TRUE(resized.radius.has_value());
  resized.height = *resized.height * 2.0;
  resized.radius = *resized.radius * 2.0;
  auto command = edit::update_objects(network, {{id, resized}}, "Resize Props");
  ASSERT_NE(command, nullptr);

  // §8 oracle: apply changes the doc, revert restores it byte-identically.
  const std::string before = snapshot_xodr(network);
  ASSERT_TRUE(command->apply(network).has_value());
  const std::string after = snapshot_xodr(network);
  EXPECT_NE(before, after);
  ASSERT_TRUE(command->revert(network).has_value());
  test::expect_network_matches(network, before);
  ASSERT_TRUE(command->apply(network).has_value());
  test::expect_network_matches(network, after);
  ASSERT_TRUE(command->revert(network).has_value());
  test::expect_network_matches(network, before);
}

} // namespace
} // namespace roadmaker
