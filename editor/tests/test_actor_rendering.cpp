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

// Drawing scenario actors as box proxies (p8-s2, issue #246).
//
// THE TWO CLAIMS WORTH PINNING, because both fail in ways that render
// convincingly:
//   1. an actor whose road is GONE is SKIPPED, never drawn at the origin —
//      "it is not there" is honest, "it is silently somewhere wrong" is not;
//   2. the box's centre offset is ROTATED into the actor's frame. An entity's
//      reference point is its rear axle, so center_x pushes the body FORWARD
//      along its own heading; adding it unrotated would push every actor east,
//      which looks correct for exactly the one actor heading along +x.

#include "roadmaker/edit/operations.hpp"
#include "roadmaker/osc/catalog.hpp"
#include "roadmaker/osc/edit.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <numbers>
#include <string>

#include "document/actor_placement.hpp"
#include "document/document.hpp"
#include "render/scene_builder.hpp"

namespace roadmaker::editor {
namespace {

void author_straight_road(Document& document) {
  auto command = edit::create_road({Waypoint{.x = 0.0, .y = 0.0}, Waypoint{.x = 100.0, .y = 0.0}},
                                   LaneProfile::two_lane_default(),
                                   "main");
  ASSERT_NE(command, nullptr);
  ASSERT_TRUE(document.push_command(std::move(command)).has_value());
}

osc::LanePosition lane_at(const std::string& road, const char* lane, double s) {
  osc::LanePosition position;
  position.road_id = road;
  position.lane_id = lane;
  position.s = s;
  return position;
}

std::string road_odr_id(const Document& document) {
  std::string id;
  document.network().for_each_road([&](RoadId, const Road& road) {
    if (id.empty()) {
      id = road.odr_id;
    }
  });
  return id;
}

void place(Document& document, const char* name, const osc::LanePosition& where) {
  ASSERT_TRUE(
      document.push_scenario_command(osc::edit::set_logic_file(document.scenario(), "s.xodr"))
          .has_value());
  ASSERT_TRUE(document
                  .push_scenario_command(osc::edit::place_scenario_object(
                      document.scenario(), osc::make_actor(osc::ActorKind::Car, name), where))
                  .has_value());
}

/// Every actor instance in the built scene.
std::vector<ScenePropInstance> actor_instances(const Scene& scene) {
  std::vector<ScenePropInstance> found;
  for (const ScenePropBatch& batch : scene.prop_batches) {
    for (const ScenePropInstance& instance : batch.instances) {
      if (!instance.actor.empty()) {
        found.push_back(instance);
      }
    }
  }
  return found;
}

Scene build_with_actors(const Document& document) {
  Scene scene = build_scene(document.mesh(), &document.network());
  append_scenario_actors(document.scenario(), document.network(), scene);
  return scene;
}

} // namespace

TEST(ActorRendering, AnEmptyScenarioAddsNoBatchAtAll) {
  // A scene with no actors must upload nothing — an empty batch would cost a
  // GL upload per frame for geometry nobody can see.
  Document document;
  author_straight_road(document);
  const Scene scene = build_with_actors(document);
  EXPECT_TRUE(actor_instances(scene).empty());
}

TEST(ActorRendering, APlacedActorBecomesOneInstanceCarryingItsName) {
  Document document;
  author_straight_road(document);
  place(document, "Car1", lane_at(road_odr_id(document), "-1", 50.0));

  const Scene scene = build_with_actors(document);
  const std::vector<ScenePropInstance> actors = actor_instances(scene);
  ASSERT_EQ(actors.size(), 1U);
  EXPECT_EQ(actors[0].actor, "Car1");
  // The ids stay INVALID: an actor is not arena content, and an aliased id is
  // how a pick resolves to the wrong entity kind.
  EXPECT_FALSE(actors[0].object.is_valid());
  EXPECT_FALSE(actors[0].signal.is_valid());
  EXPECT_FALSE(actors[0].road.is_valid());
}

TEST(ActorRendering, EveryActorSharesOneBoxBatch) {
  Document document;
  author_straight_road(document);
  const std::string road = road_odr_id(document);
  place(document, "Car1", lane_at(road, "-1", 20.0));
  ASSERT_TRUE(document
                  .push_scenario_command(osc::edit::place_scenario_object(
                      document.scenario(),
                      osc::make_actor(osc::ActorKind::Truck, "Truck1"),
                      lane_at(road, "-1", 60.0)))
                  .has_value());

  const Scene scene = build_with_actors(document);
  EXPECT_EQ(actor_instances(scene).size(), 2U);

  std::size_t actor_batches = 0;
  for (const ScenePropBatch& batch : scene.prop_batches) {
    if (batch.model_id == "rm:actor_box") {
      ++actor_batches;
    }
  }
  EXPECT_EQ(actor_batches, 1U) << "the box geometry was uploaded more than once";
}

TEST(ActorRendering, AnActorWhoseRoadIsGoneIsSkippedNotDrawnAtTheOrigin) {
  // ★ The .xosc holds STRINGS, so a scenario outlives the roads it references.
  // Drawing such an actor at (0,0,0) would put a car in the middle of the scene
  // with nothing to explain it.
  Document document;
  author_straight_road(document);
  place(document, "Car1", lane_at("999", "-1", 50.0)); // a road that never existed

  const Scene scene = build_with_actors(document);
  EXPECT_TRUE(actor_instances(scene).empty())
      << "an unresolvable actor was drawn instead of skipped";
}

TEST(ActorRendering, TheBoxIsScaledToItsDeclaredDimensions) {
  Document document;
  author_straight_road(document);
  place(document, "Car1", lane_at(road_odr_id(document), "-1", 50.0));

  const std::vector<ScenePropInstance> actors = actor_instances(build_with_actors(document));
  ASSERT_EQ(actors.size(), 1U);

  // The road runs +x and lane -1 travels with +s, so heading is 0: the model
  // axes are unrotated and the scale lands on the diagonal.
  // ★ LENGTH ON THE FORWARD AXIS, width across it. The first version of this
  // test asserted the opposite and PASSED — a box of the right volume lying
  // across its lane. Checking that the three scale factors are merely PRESENT
  // is not enough; the axes have to be named.
  const osc::ActorArchetype& car = osc::actor_archetype(osc::ActorKind::Car);
  const std::array<float, 16>& m = actors[0].transform.model;
  EXPECT_NEAR(m[0], static_cast<float>(car.length), 1e-5F) << "length runs along +x (forward)";
  EXPECT_NEAR(m[5], static_cast<float>(car.width), 1e-5F) << "width runs across it";
  EXPECT_NEAR(m[10], static_cast<float>(car.height), 1e-5F) << "height";
  EXPECT_GT(car.length, car.width) << "the fixture assumes a car is longer than it is wide";
}

TEST(ActorRendering, TheCentreOffsetIsRotatedIntoTheActorsFrame) {
  // ★ THE TRAP. An entity's reference point is the centre of its rear axle, so
  // center_x pushes the body FORWARD along its own heading. Adding it unrotated
  // would push every actor toward +x — which is indistinguishable from correct
  // for an actor that happens to face that way, and wrong for every other.
  //
  // Lane -1 heads +x and lane +1 heads -x on this road, so the two actors face
  // opposite ways and their body offsets must go opposite ways too.
  Document document;
  author_straight_road(document);
  const std::string road = road_odr_id(document);
  place(document, "Car1", lane_at(road, "-1", 50.0));
  ASSERT_TRUE(document
                  .push_scenario_command(
                      osc::edit::place_scenario_object(document.scenario(),
                                                       osc::make_actor(osc::ActorKind::Car, "Car2"),
                                                       lane_at(road, "1", 50.0)))
                  .has_value());

  const std::vector<ScenePropInstance> actors = actor_instances(build_with_actors(document));
  ASSERT_EQ(actors.size(), 2U);

  // Both anchors sit at s = 50, so their ANCHORS share an x. Their box centres
  // must straddle it, because the two face opposite ways.
  const float x0 = actors[0].transform.model[12];
  const float x1 = actors[1].transform.model[12];
  const osc::ActorArchetype& car = osc::actor_archetype(osc::ActorKind::Car);
  ASSERT_GT(car.center_x, 0.0) << "the fixture assumes a forward body offset";

  EXPECT_GT(x0, 50.0F) << "the +x-facing actor's body is not ahead of its anchor";
  EXPECT_LT(x1, 50.0F) << "the -x-facing actor's body was pushed east anyway — "
                          "the centre offset was added unrotated";
  EXPECT_NEAR((x0 + x1) / 2.0F, 50.0F, 1e-4F) << "the two offsets are not symmetric";
}

TEST(ActorRendering, TheBoxSitsOnTheGroundRatherThanHalfBuriedInIt) {
  // center_z is half the height, so the box's bottom face lands at z = 0. A
  // box centred on the anchor would be half underground and would still look
  // plausible from a shallow camera angle.
  Document document;
  author_straight_road(document);
  place(document, "Car1", lane_at(road_odr_id(document), "-1", 50.0));

  const std::vector<ScenePropInstance> actors = actor_instances(build_with_actors(document));
  ASSERT_EQ(actors.size(), 1U);
  const osc::ActorArchetype& car = osc::actor_archetype(osc::ActorKind::Car);
  EXPECT_NEAR(actors[0].transform.model[14], static_cast<float>(car.height / 2.0), 1e-5F);
}

TEST(ActorRendering, TheUnitCubeIsClosedAndFlatShaded) {
  const RenderMeshData mesh = actor_box_mesh();
  // Six faces x four corners, each face with its own vertices so the normals
  // stay flat — sharing eight corners would average them and lose every edge.
  EXPECT_EQ(mesh.positions.size(), 6U * 4U * 3U);
  EXPECT_EQ(mesh.normals.size(), mesh.positions.size());
  EXPECT_EQ(mesh.indices.size(), 6U * 6U) << "six faces of two triangles";
  EXPECT_EQ(mesh.kind, PrimitiveKind::Triangles);

  // Every vertex on the unit cube's surface.
  for (std::size_t i = 0; i < mesh.positions.size(); ++i) {
    EXPECT_NEAR(std::abs(mesh.positions[i]), 0.5F, 1e-6F) << "vertex component " << i;
  }
}

} // namespace roadmaker::editor
