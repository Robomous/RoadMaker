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

// Composite prop assemblies (p6-s9, #323): the catalogue and its project
// overlay, the four edit commands, the rm:assembly round trip, and the claim
// that the mesher needed no change to draw the parts.
//
// The commands follow the M2 contract like every other: apply→revert is
// byte-identical, a failed apply leaves the network untouched, and
// restore-in-place keeps ObjectIds across undo/redo.

#include "roadmaker/assets/prop_assembly.hpp"
#include "roadmaker/assets/prop_library.hpp"
#include "roadmaker/edit/operations.hpp"
#include "roadmaker/mesh/mesh_builder.hpp"
#include "roadmaker/road/authoring.hpp"
#include "roadmaker/road/defaults.hpp"
#include "roadmaker/road/network.hpp"
#include "roadmaker/road/object.hpp"
#include "roadmaker/xodr/reader.hpp"
#include "roadmaker/xodr/rm_codes.hpp"
#include "roadmaker/xodr/writer.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <set>
#include <string>
#include <vector>

#include "support/network_compare.hpp"

namespace roadmaker {
namespace {

using test::snapshot_xodr;

RoadId author_street(RoadNetwork& network) {
  const std::vector<Waypoint> waypoints{Waypoint{.x = 0.0, .y = 0.0},
                                        Waypoint{.x = 200.0, .y = 0.0}};
  auto road = author_clothoid_road(network, waypoints, LaneProfile::two_lane_default(), "", "1");
  if (!road.has_value()) {
    throw std::runtime_error("author_street: " + road.error().message);
  }
  return *road;
}

/// The parts of the one assembly in the network, in part order.
std::vector<ObjectId> only_assembly(const RoadNetwork& network) {
  ObjectId seed;
  network.for_each_object([&](ObjectId id, const Object& object) {
    if (object.assembly.has_value() && !seed.is_valid()) {
      seed = id;
    }
  });
  return assembly_parts(network, seed);
}

/// A two-part test assembly whose offsets are all non-zero, so a bug that drops
/// one of them cannot hide behind a default.
std::vector<props::PropAssembly> two_part_fixture() {
  props::PropAssembly asm_def;
  asm_def.id = "test_pair";
  asm_def.label = "Test pair";
  asm_def.parts = {
      props::AssemblyPart{.model = "tree_pine"},
      props::AssemblyPart{
          .model = "tree_oak", .du = 3.0, .dv = 4.0, .dz = 1.5, .dyaw = 0.25, .scale = 2.0},
  };
  return {asm_def};
}

// --------------------------------------------------------------------------- //
// The catalogue and its project overlay
// --------------------------------------------------------------------------- //

/// Every overlay test must leave the process as it found it: the overlay is
/// static state, so a leaked registration would poison every later test in the
/// binary. Same fixture shape as test_prop_library.cpp's PropOverlay.
class AssemblyOverlay : public ::testing::Test {
protected:
  void SetUp() override { props::clear_project_assemblies(); }

  void TearDown() override { props::clear_project_assemblies(); }
};

TEST(PropAssemblyCatalogue, TheBundledMastArmResolvesAndEveryPartModelExists) {
  const props::PropAssembly* mast = props::assembly("signal_mast");
  ASSERT_NE(mast, nullptr);
  EXPECT_EQ(mast->id, "signal_mast");
  EXPECT_FALSE(mast->label.empty());
  ASSERT_FALSE(mast->parts.empty());
  EXPECT_LE(mast->parts.size(), props::kMaxAssemblyParts);
  for (const props::AssemblyPart& part : mast->parts) {
    EXPECT_NE(props::model(part.model), nullptr) << "unresolvable part model " << part.model;
    EXPECT_GT(part.scale, 0.0) << part.model;
  }
}

TEST(PropAssemblyCatalogue, UnknownIdReturnsNull) {
  EXPECT_EQ(props::assembly("does_not_exist"), nullptr);
  EXPECT_EQ(props::assembly(""), nullptr);
}

TEST(PropAssemblyCatalogue, EveryAdvertisedIdResolves) {
  const std::vector<std::string>& all = props::assembly_ids();
  EXPECT_FALSE(all.empty());
  const std::set<std::string> unique(all.begin(), all.end());
  EXPECT_EQ(unique.size(), all.size()) << "assembly ids must be unique";
  for (const std::string& id : all) {
    EXPECT_NE(props::assembly(id), nullptr) << id;
  }
}

// §1.5 gives the mast-arm geometry its numbers; this is the join, exactly as
// test_defaults_registry.cpp joins the prop MESHES to the registry. Retune the
// bundled assembly away from the spec and CI fails here.
TEST(PropAssemblyCatalogue, TheMastArmHangsItsHeadsAtTheRegistryClearance) {
  const props::PropAssembly* mast = props::assembly("signal_mast");
  ASSERT_NE(mast, nullptr);

  std::vector<const props::AssemblyPart*> heads;
  const props::AssemblyPart* arm = nullptr;
  for (const props::AssemblyPart& part : mast->parts) {
    if (part.model == "signal_head") {
      heads.push_back(&part);
    } else if (part.model == "mast_arm") {
      arm = &part;
    }
  }
  ASSERT_EQ(heads.size(), 2U) << "the demo assembly is a two-head mast arm";
  ASSERT_NE(arm, nullptr);

  for (const props::AssemblyPart* head : heads) {
    EXPECT_DOUBLE_EQ(head->dz, defaults::kSignalClearance);
  }
  // The arm's underside clears the housings exactly, so the heads hang flush.
  EXPECT_DOUBLE_EQ(arm->dz, defaults::kSignalClearance + defaults::kSignalHousingHeight);
  // One head per arterial lane, at each lane's centre.
  EXPECT_DOUBLE_EQ(heads[0]->dv, defaults::kArterialLaneWidth * 0.5);
  EXPECT_DOUBLE_EQ(heads[1]->dv, defaults::kArterialLaneWidth * 1.5);
  // ★ Without the quarter turn the arm reaches ALONG the road, not across it —
  // every part model faces +x at local heading 0.
  EXPECT_DOUBLE_EQ(arm->dyaw, std::numbers::pi / 2.0);
}

TEST_F(AssemblyOverlay, WithNoProjectTheCatalogueIsExactlyTheBuiltInOne) {
  EXPECT_EQ(props::assembly_ids(), props::detail::builtin_assembly_ids());
  EXPECT_FALSE(props::is_project_assembly("signal_mast"));
}

TEST_F(AssemblyOverlay, AProjectDefinitionResolvesAndJoinsTheCatalogue) {
  const std::size_t builtin_count = props::detail::builtin_assembly_ids().size();
  props::register_project_assemblies(two_part_fixture());

  const props::PropAssembly* found = props::assembly("test_pair");
  ASSERT_NE(found, nullptr);
  EXPECT_EQ(found->parts.size(), 2U);
  EXPECT_TRUE(props::is_project_assembly("test_pair"));
  EXPECT_EQ(props::assembly_ids().size(), builtin_count + 1);
  EXPECT_NE(props::assembly("signal_mast"), nullptr) << "the bundled one must survive";
}

TEST_F(AssemblyOverlay, ClearingRestoresTheBuiltInCatalogueExactly) {
  const std::vector<std::string> before = props::assembly_ids();
  props::register_project_assemblies(two_part_fixture());
  ASSERT_NE(props::assembly("test_pair"), nullptr);

  props::clear_project_assemblies();
  EXPECT_EQ(props::assembly_ids(), before);
  EXPECT_EQ(props::assembly("test_pair"), nullptr);
  EXPECT_FALSE(props::is_project_assembly("test_pair"));
}

TEST_F(AssemblyOverlay, RegisteringReplacesWholesaleRatherThanAccumulating) {
  props::PropAssembly first;
  first.id = "project_a";
  first.parts = {props::AssemblyPart{.model = "tree_pine"}};
  props::register_project_assemblies({first});
  ASSERT_NE(props::assembly("project_a"), nullptr);

  props::PropAssembly second;
  second.id = "project_b";
  second.parts = {props::AssemblyPart{.model = "tree_oak"}};
  props::register_project_assemblies({second});

  EXPECT_EQ(props::assembly("project_a"), nullptr) << "a project switch must not leak";
  EXPECT_NE(props::assembly("project_b"), nullptr);
  EXPECT_EQ(props::assembly_ids().size(), props::detail::builtin_assembly_ids().size() + 1);
}

TEST_F(AssemblyOverlay, AProjectDefinitionShadowsABundledIdAndIsListedOnce) {
  props::PropAssembly shadow;
  shadow.id = "signal_mast";
  shadow.label = "My own mast";
  shadow.parts = {props::AssemblyPart{.model = "tree_pine"}};
  props::register_project_assemblies({shadow});

  const props::PropAssembly* found = props::assembly("signal_mast");
  ASSERT_NE(found, nullptr);
  EXPECT_EQ(found->label, "My own mast");
  EXPECT_EQ(props::assembly_ids().size(), props::detail::builtin_assembly_ids().size());
  const std::vector<std::string>& all = props::assembly_ids();
  const std::set<std::string> unique(all.begin(), all.end());
  EXPECT_EQ(unique.size(), all.size());
}

// --------------------------------------------------------------------------- //
// place_assembly
// --------------------------------------------------------------------------- //

TEST(PlaceAssembly, PlacesEveryPartAsOneCommandAndUndoIsByteIdentical) {
  RoadNetwork network;
  const RoadId road = author_street(network);
  const std::string before = snapshot_xodr(network);
  const std::size_t part_count = props::assembly("signal_mast")->parts.size();

  auto command = edit::place_assembly(network, road, 40.0, -6.0, 0.0, "signal_mast");
  ASSERT_TRUE(command->apply(network).has_value());
  EXPECT_EQ(network.object_count(), part_count);
  EXPECT_NE(snapshot_xodr(network), before);

  ASSERT_TRUE(command->revert(network).has_value());
  EXPECT_EQ(network.object_count(), 0U);
  EXPECT_EQ(snapshot_xodr(network), before) << "undo must be byte-identical";

  ASSERT_TRUE(command->apply(network).has_value());
  EXPECT_EQ(network.object_count(), part_count);
}

TEST(PlaceAssembly, EveryPartGetsItsOwnOdrIdAcrossTheBatch) {
  RoadNetwork network;
  const RoadId road = author_street(network);
  auto command = edit::place_assembly(network, road, 40.0, -6.0, 0.0, "signal_mast");
  ASSERT_TRUE(command->apply(network).has_value());

  // ★ THE COLLISION THIS GUARDS: minting ids one at a time against the live
  // network hands every part the SAME id, because none of them exists yet.
  std::set<std::string> ids;
  network.for_each_object([&](ObjectId, const Object& object) { ids.insert(object.odr_id); });
  EXPECT_EQ(ids.size(), network.object_count()) << "part odr ids must be unique";
}

TEST(PlaceAssembly, PartPosesFollowTheAnchorAndTheAuthoredOffsets) {
  RoadNetwork network;
  const RoadId road = author_street(network);
  props::clear_project_assemblies();
  props::register_project_assemblies(two_part_fixture());

  constexpr double kAnchorS = 50.0;
  constexpr double kAnchorT = -4.0;
  constexpr double kAnchorHdg = 0.0;
  auto command = edit::place_assembly(network, road, kAnchorS, kAnchorT, kAnchorHdg, "test_pair");
  ASSERT_TRUE(command->apply(network).has_value());

  const std::vector<ObjectId> parts = only_assembly(network);
  ASSERT_EQ(parts.size(), 2U);
  const Object& anchor = *network.object(parts[0]);
  const Object& offset = *network.object(parts[1]);

  EXPECT_DOUBLE_EQ(anchor.s, kAnchorS);
  EXPECT_DOUBLE_EQ(anchor.t, kAnchorT);
  EXPECT_DOUBLE_EQ(anchor.z_offset, 0.0);
  EXPECT_EQ(anchor.assembly->part, 0);

  // At heading 0 the local frame IS the road frame, so du→s and dv→t directly.
  EXPECT_DOUBLE_EQ(offset.s, kAnchorS + 3.0);
  EXPECT_DOUBLE_EQ(offset.t, kAnchorT + 4.0);
  EXPECT_DOUBLE_EQ(offset.z_offset, 1.5);
  EXPECT_DOUBLE_EQ(offset.hdg, 0.25);
  EXPECT_EQ(offset.assembly->part, 1);
  EXPECT_EQ(offset.assembly->asset, "test_pair");
  EXPECT_EQ(offset.assembly->instance, anchor.assembly->instance)
      << "one placement is one instance";

  // The part scale multiplies the model's own dimensions.
  const props::PropModel* oak = props::model("tree_oak");
  ASSERT_NE(oak, nullptr);
  ASSERT_TRUE(offset.height.has_value());
  EXPECT_DOUBLE_EQ(*offset.height, oak->height * 2.0);

  props::clear_project_assemblies();
}

TEST(PlaceAssembly, TheAnchorHeadingRotatesTheWholeLocalFrame) {
  RoadNetwork network;
  const RoadId road = author_street(network);
  props::clear_project_assemblies();
  props::register_project_assemblies(two_part_fixture());

  // A quarter turn sends du across t and dv back along s: (du, dv) = (3, 4)
  // becomes (s, t) += (-4, +3).
  constexpr double kQuarter = std::numbers::pi / 2.0;
  auto command = edit::place_assembly(network, road, 50.0, -4.0, kQuarter, "test_pair");
  ASSERT_TRUE(command->apply(network).has_value());

  const std::vector<ObjectId> parts = only_assembly(network);
  ASSERT_EQ(parts.size(), 2U);
  const Object& offset = *network.object(parts[1]);
  EXPECT_NEAR(offset.s, 50.0 - 4.0, 1e-9);
  EXPECT_NEAR(offset.t, -4.0 + 3.0, 1e-9);
  EXPECT_NEAR(offset.hdg, kQuarter + 0.25, 1e-9);

  props::clear_project_assemblies();
}

TEST(PlaceAssembly, RefusesAnUnknownAssemblyWithoutMutating) {
  RoadNetwork network;
  const RoadId road = author_street(network);
  const std::string before = snapshot_xodr(network);
  auto command = edit::place_assembly(network, road, 40.0, -6.0, 0.0, "no_such_assembly");
  EXPECT_FALSE(command->apply(network).has_value());
  EXPECT_EQ(network.object_count(), 0U);
  EXPECT_EQ(snapshot_xodr(network), before);
}

TEST(PlaceAssembly, RefusesAStaleRoad) {
  RoadNetwork network;
  author_street(network);
  auto command = edit::place_assembly(network, RoadId{}, 40.0, -6.0, 0.0, "signal_mast");
  EXPECT_FALSE(command->apply(network).has_value());
  EXPECT_EQ(network.object_count(), 0U);
}

TEST(PlaceAssembly, RefusesANonPositiveScale) {
  RoadNetwork network;
  const RoadId road = author_street(network);
  for (const double scale : {0.0, -1.0, std::numeric_limits<double>::quiet_NaN()}) {
    auto command = edit::place_assembly(network, road, 40.0, -6.0, 0.0, "signal_mast", scale);
    EXPECT_FALSE(command->apply(network).has_value()) << "scale=" << scale;
    EXPECT_EQ(network.object_count(), 0U);
  }
}

// Whole or not at all: an assembly whose arm would hang off the end of the road
// is refused, not clipped down to the parts that happen to fit.
TEST(PlaceAssembly, RefusesWhenAnyPartWouldLeaveTheRoadAndPlacesNothing) {
  RoadNetwork network;
  const RoadId road = author_street(network);
  props::clear_project_assemblies();
  props::register_project_assemblies(two_part_fixture());
  const double length = network.road(road)->plan_view.length();

  // The anchor itself is on the road; only the du=3 part runs past the end.
  auto command = edit::place_assembly(network, road, length - 1.0, -4.0, 0.0, "test_pair");
  EXPECT_FALSE(command->apply(network).has_value());
  EXPECT_EQ(network.object_count(), 0U) << "no part may survive a refused placement";

  props::clear_project_assemblies();
}

TEST(PlaceAssembly, RefusesADefinitionNamingAnUnknownModel) {
  RoadNetwork network;
  const RoadId road = author_street(network);
  props::clear_project_assemblies();
  props::PropAssembly broken;
  broken.id = "broken";
  broken.parts = {props::AssemblyPart{.model = "tree_pine"},
                  props::AssemblyPart{.model = "no_such_model", .dv = 1.0}};
  props::register_project_assemblies({broken});

  auto command = edit::place_assembly(network, road, 40.0, -4.0, 0.0, "broken");
  EXPECT_FALSE(command->apply(network).has_value());
  EXPECT_EQ(network.object_count(), 0U);

  props::clear_project_assemblies();
}

TEST(PlaceAssembly, TwoPlacementsAreTwoDistinctInstances) {
  RoadNetwork network;
  const RoadId road = author_street(network);
  auto first = edit::place_assembly(network, road, 40.0, -6.0, 0.0, "signal_mast");
  ASSERT_TRUE(first->apply(network).has_value());
  auto second = edit::place_assembly(network, road, 120.0, -6.0, 0.0, "signal_mast");
  ASSERT_TRUE(second->apply(network).has_value());

  std::set<std::string> instances;
  network.for_each_object([&](ObjectId, const Object& object) {
    ASSERT_TRUE(object.assembly.has_value());
    instances.insert(object.assembly->instance);
  });
  EXPECT_EQ(instances.size(), 2U) << "a second placement must not join the first group";
}

// --------------------------------------------------------------------------- //
// assembly_parts
// --------------------------------------------------------------------------- //

TEST(AssemblyParts, ReturnsEveryPartInPartOrderIncludingTheSeed) {
  RoadNetwork network;
  const RoadId road = author_street(network);
  auto command = edit::place_assembly(network, road, 40.0, -6.0, 0.0, "signal_mast");
  ASSERT_TRUE(command->apply(network).has_value());

  const std::vector<ObjectId> parts = only_assembly(network);
  ASSERT_EQ(parts.size(), props::assembly("signal_mast")->parts.size());
  for (std::size_t i = 0; i < parts.size(); ++i) {
    EXPECT_EQ(network.object(parts[i])->assembly->part, static_cast<int>(i));
  }
  // Any member is a valid seed and yields the same list.
  EXPECT_EQ(assembly_parts(network, parts.back()), parts);
}

// ★ ARENA ORDER IS NOT PART ORDER, and this is what the explicit sort in
// assembly_parts() is for. The scrambling is driven from the FILE rather than
// from arena internals: a document is free to list an assembly's parts in any
// order (hand-edited, or written by a tool that sorted objects by station), and
// the parser adds them in document order. `move_assembly` indexes poses by part,
// so a shuffled list would recompute every part against the wrong offsets.
//
// An earlier version of this test recycled an arena slot instead, and PASSED with
// the sort deleted: freeing slot 0 and then allocating four parts hands them
// slots 0-3, which is part order anyway. It proved nothing.
TEST(AssemblyParts, IsSortedByPartEvenWhenTheFileListsThemShuffled) {
  const std::string xodr = R"(<?xml version="1.0"?>
<OpenDRIVE>
  <header revMajor="1" revMinor="8" name="shuffled" />
  <road length="80" id="1" junction="-1">
    <planView><geometry s="0" x="0" y="0" hdg="0" length="80"><line /></geometry></planView>
    <lanes><laneSection s="0">
      <center><lane id="0" type="none" level="false" /></center>
      <right><lane id="-1" type="driving" level="false">
        <width sOffset="0" a="3.6" b="0" c="0" d="0" />
      </lane></right>
    </laneSection></lanes>
    <objects>
      <object type="pole" name="signal_head" id="d" s="40" t="-0.6" zOffset="5.2" radius="0.2" height="1.07">
        <userData code="rm:assembly" asset="signal_mast" inst="1" part="3" dv="5.4" dz="5.2" />
      </object>
      <object type="pole" name="mast_arm" id="b" s="40" t="-6" zOffset="6.27" radius="7.3" height="0.2">
        <userData code="rm:assembly" asset="signal_mast" inst="1" part="1" dz="6.27" />
      </object>
      <object type="pole" name="pole_signal" id="a" s="40" t="-6" zOffset="0" radius="0.14" height="6.47">
        <userData code="rm:assembly" asset="signal_mast" inst="1" part="0" />
      </object>
      <object type="pole" name="signal_head" id="c" s="40" t="-4.2" zOffset="5.2" radius="0.2" height="1.07">
        <userData code="rm:assembly" asset="signal_mast" inst="1" part="2" dv="1.8" dz="5.2" />
      </object>
    </objects>
  </road>
</OpenDRIVE>)";

  const auto parsed = parse_xodr(xodr);
  ASSERT_TRUE(parsed.has_value()) << parsed.error().message;
  ASSERT_EQ(parsed->network.object_count(), 4U);

  const std::vector<ObjectId> parts = only_assembly(parsed->network);
  ASSERT_EQ(parts.size(), 4U);
  const std::vector<std::string> expected{"a", "b", "c", "d"};
  for (std::size_t i = 0; i < parts.size(); ++i) {
    const Object& part = *parsed->network.object(parts[i]);
    EXPECT_EQ(part.assembly->part, static_cast<int>(i)) << "index " << i;
    EXPECT_EQ(part.odr_id, expected[i]) << "index " << i;
  }
}

TEST(AssemblyParts, IsEmptyForAPlainPropAndForAStaleId) {
  RoadNetwork network;
  const RoadId road = author_street(network);
  Object tree;
  tree.odr_id = "1";
  tree.name = "tree_pine";
  tree.type = ObjectType::Tree;
  tree.s = 10.0;
  tree.t = 4.0;
  auto add = edit::add_object(network, road, tree);
  ASSERT_TRUE(add->apply(network).has_value());

  ObjectId plain;
  network.for_each_object([&](ObjectId id, const Object&) { plain = id; });
  EXPECT_TRUE(assembly_parts(network, plain).empty());
  EXPECT_TRUE(assembly_parts(network, ObjectId{}).empty());
}

// --------------------------------------------------------------------------- //
// move_assembly / delete_assembly / detach_assembly_part / the move_object refusal
// --------------------------------------------------------------------------- //

TEST(MoveAssembly, MovesEveryPartRigidlyAndUndoIsByteIdentical) {
  RoadNetwork network;
  const RoadId road = author_street(network);
  auto place = edit::place_assembly(network, road, 40.0, -6.0, 0.0, "signal_mast");
  ASSERT_TRUE(place->apply(network).has_value());

  const std::vector<ObjectId> parts = only_assembly(network);
  ASSERT_GE(parts.size(), 2U);
  std::vector<double> relative_s;
  std::vector<double> relative_t;
  const Object& anchor_before = *network.object(parts.front());
  for (const ObjectId id : parts) {
    relative_s.push_back(network.object(id)->s - anchor_before.s);
    relative_t.push_back(network.object(id)->t - anchor_before.t);
  }
  const std::string before = snapshot_xodr(network);

  auto move = edit::move_assembly(network, parts[1], 90.0, -8.0);
  ASSERT_TRUE(move->apply(network).has_value());

  const Object& anchor_after = *network.object(parts.front());
  EXPECT_DOUBLE_EQ(anchor_after.s, 90.0);
  EXPECT_DOUBLE_EQ(anchor_after.t, -8.0);
  for (std::size_t i = 0; i < parts.size(); ++i) {
    // ★ A move that only re-stationed the grabbed part would pass every "the
    // assembly moved" assertion and still tear the unit apart. This is the check
    // that says it moved RIGIDLY.
    EXPECT_NEAR(network.object(parts[i])->s - anchor_after.s, relative_s[i], 1e-9) << i;
    EXPECT_NEAR(network.object(parts[i])->t - anchor_after.t, relative_t[i], 1e-9) << i;
  }

  ASSERT_TRUE(move->revert(network).has_value());
  EXPECT_EQ(snapshot_xodr(network), before) << "undo must be byte-identical";
}

TEST(MoveAssembly, RotatingTheAnchorSwingsEveryPartAroundIt) {
  RoadNetwork network;
  const RoadId road = author_street(network);
  props::clear_project_assemblies();
  props::register_project_assemblies(two_part_fixture());
  auto place = edit::place_assembly(network, road, 50.0, -4.0, 0.0, "test_pair");
  ASSERT_TRUE(place->apply(network).has_value());

  const std::vector<ObjectId> parts = only_assembly(network);
  ASSERT_EQ(parts.size(), 2U);
  constexpr double kQuarter = std::numbers::pi / 2.0;
  auto move = edit::move_assembly(network, parts[0], 50.0, -4.0, kQuarter);
  ASSERT_TRUE(move->apply(network).has_value());

  const Object& offset = *network.object(parts[1]);
  EXPECT_NEAR(offset.s, 50.0 - 4.0, 1e-9);
  EXPECT_NEAR(offset.t, -4.0 + 3.0, 1e-9);
  EXPECT_NEAR(offset.hdg, kQuarter + 0.25, 1e-9);

  props::clear_project_assemblies();
}

TEST(MoveAssembly, KeepsTheCurrentHeadingWhenNoneIsGiven) {
  RoadNetwork network;
  const RoadId road = author_street(network);
  auto place = edit::place_assembly(network, road, 40.0, -6.0, 0.7, "signal_mast");
  ASSERT_TRUE(place->apply(network).has_value());
  const std::vector<ObjectId> parts = only_assembly(network);
  const double anchor_hdg_before = network.object(parts.front())->hdg;

  auto move = edit::move_assembly(network, parts.front(), 90.0, -6.0);
  ASSERT_TRUE(move->apply(network).has_value());
  EXPECT_DOUBLE_EQ(network.object(parts.front())->hdg, anchor_hdg_before);
}

// The drag-shaped entry point. `move_assembly` re-anchors; `move_assembly_by_part`
// puts the GRABBED PART where it is asked to go. Every editor drag needs the
// second, and the difference is invisible only for the anchor part — which is
// exactly why this test grabs one that is offset from it.
TEST(MoveAssembly, ByPartPutsTheGRABBEDPartWhereItIsAskedNotTheAnchor) {
  RoadNetwork network;
  const RoadId road = author_street(network);
  auto place = edit::place_assembly(network, road, 40.0, -6.0, 0.0, "signal_mast");
  ASSERT_TRUE(place->apply(network).has_value());

  const std::vector<ObjectId> parts = only_assembly(network);
  ASSERT_GE(parts.size(), 3U);
  // Pick a part that is genuinely offset from the anchor, or the two entry
  // points agree by accident and the test proves nothing.
  const ObjectId grabbed = parts[2];
  const Object& before_grabbed = *network.object(grabbed);
  ASSERT_GT(std::abs(before_grabbed.t - network.object(parts.front())->t), 1.0)
      << "the grabbed part must not sit on the anchor";

  std::vector<std::pair<double, double>> relative;
  for (const ObjectId id : parts) {
    relative.emplace_back(network.object(id)->s - before_grabbed.s,
                          network.object(id)->t - before_grabbed.t);
  }

  auto move = edit::move_assembly_by_part(network, grabbed, 90.0, -2.0);
  ASSERT_TRUE(move->apply(network).has_value());

  // The grabbed part lands EXACTLY where it was asked to.
  EXPECT_NEAR(network.object(grabbed)->s, 90.0, 1e-9);
  EXPECT_NEAR(network.object(grabbed)->t, -2.0, 1e-9);
  // …and the unit came with it, still rigid.
  for (std::size_t i = 0; i < parts.size(); ++i) {
    EXPECT_NEAR(network.object(parts[i])->s - 90.0, relative[i].first, 1e-9) << i;
    EXPECT_NEAR(network.object(parts[i])->t - (-2.0), relative[i].second, 1e-9) << i;
  }

  // And the two entry points really do disagree: handing the same station to
  // `move_assembly` puts the ANCHOR there instead.
  ASSERT_TRUE(move->revert(network).has_value());
  auto anchored = edit::move_assembly(network, grabbed, 90.0, -2.0);
  ASSERT_TRUE(anchored->apply(network).has_value());
  EXPECT_GT(std::abs(network.object(grabbed)->t - (-2.0)), 1.0)
      << "if these two agreed, move_assembly_by_part would be redundant";
}

TEST(MoveAssembly, ByPartRefusesAPlainPropAndAStaleId) {
  RoadNetwork network;
  const RoadId road = author_street(network);
  Object tree;
  tree.odr_id = "plain";
  tree.name = "tree_pine";
  tree.type = ObjectType::Tree;
  tree.s = 10.0;
  tree.t = 4.0;
  const ObjectId plain = network.add_object(road, tree);

  EXPECT_FALSE(
      edit::move_assembly_by_part(network, ObjectId{}, 10.0, 0.0)->apply(network).has_value());
  EXPECT_FALSE(edit::move_assembly_by_part(network, plain, 10.0, 0.0)->apply(network).has_value());
}

TEST(MoveAssembly, RefusesAPlainPropAStaleIdAndAMoveOffTheRoad) {
  RoadNetwork network;
  const RoadId road = author_street(network);
  auto place = edit::place_assembly(network, road, 40.0, -6.0, 0.0, "signal_mast");
  ASSERT_TRUE(place->apply(network).has_value());
  const std::vector<ObjectId> parts = only_assembly(network);
  const std::string before = snapshot_xodr(network);

  EXPECT_FALSE(edit::move_assembly(network, ObjectId{}, 10.0, 0.0)->apply(network).has_value());

  Object tree;
  tree.odr_id = "plain";
  tree.name = "tree_pine";
  tree.type = ObjectType::Tree;
  tree.s = 10.0;
  tree.t = 4.0;
  auto add = edit::add_object(network, road, tree);
  ASSERT_TRUE(add->apply(network).has_value());
  ObjectId plain;
  network.for_each_object([&](ObjectId id, const Object& object) {
    if (!object.assembly.has_value()) {
      plain = id;
    }
  });
  EXPECT_FALSE(edit::move_assembly(network, plain, 20.0, 4.0)->apply(network).has_value());
  ASSERT_TRUE(add->revert(network).has_value());

  const double length = network.road(road)->plan_view.length();
  EXPECT_FALSE(
      edit::move_assembly(network, parts.front(), length + 50.0, -6.0)->apply(network).has_value());
  EXPECT_EQ(snapshot_xodr(network), before) << "a refused move must not mutate";
}

TEST(DeleteAssembly, RemovesEveryPartAndUndoRestoresTheSameIds) {
  RoadNetwork network;
  const RoadId road = author_street(network);
  auto place = edit::place_assembly(network, road, 40.0, -6.0, 0.0, "signal_mast");
  ASSERT_TRUE(place->apply(network).has_value());
  const std::vector<ObjectId> parts = only_assembly(network);
  const std::string before = snapshot_xodr(network);

  auto remove = edit::delete_assembly(network, parts[1]);
  ASSERT_TRUE(remove->apply(network).has_value());
  EXPECT_EQ(network.object_count(), 0U) << "one part deleted means the unit deleted";

  ASSERT_TRUE(remove->revert(network).has_value());
  EXPECT_EQ(snapshot_xodr(network), before) << "undo must be byte-identical";
  for (const ObjectId id : parts) {
    EXPECT_NE(network.object(id), nullptr) << "restore-in-place must keep the ObjectId";
  }
}

TEST(DeleteAssembly, LeavesAnotherInstanceAlone) {
  RoadNetwork network;
  const RoadId road = author_street(network);
  auto first = edit::place_assembly(network, road, 40.0, -6.0, 0.0, "signal_mast");
  ASSERT_TRUE(first->apply(network).has_value());
  const std::size_t part_count = network.object_count();
  auto second = edit::place_assembly(network, road, 120.0, -6.0, 0.0, "signal_mast");
  ASSERT_TRUE(second->apply(network).has_value());

  ObjectId victim;
  network.for_each_object([&](ObjectId id, const Object& object) {
    if (object.s < 80.0 && !victim.is_valid()) {
      victim = id;
    }
  });
  auto remove = edit::delete_assembly(network, victim);
  ASSERT_TRUE(remove->apply(network).has_value());
  EXPECT_EQ(network.object_count(), part_count) << "only the victim's instance goes";
}

TEST(DeleteAssembly, RefusesAPlainPropAndAStaleId) {
  RoadNetwork network;
  const RoadId road = author_street(network);
  Object tree;
  tree.odr_id = "1";
  tree.name = "tree_pine";
  tree.type = ObjectType::Tree;
  tree.s = 10.0;
  tree.t = 4.0;
  auto add = edit::add_object(network, road, tree);
  ASSERT_TRUE(add->apply(network).has_value());
  ObjectId plain;
  network.for_each_object([&](ObjectId id, const Object&) { plain = id; });

  EXPECT_FALSE(edit::delete_assembly(network, plain)->apply(network).has_value());
  EXPECT_FALSE(edit::delete_assembly(network, ObjectId{})->apply(network).has_value());
  EXPECT_EQ(network.object_count(), 1U);
}

TEST(DetachAssemblyPart, BreaksOnePartOutAndLeavesTheRestGrouped) {
  RoadNetwork network;
  const RoadId road = author_street(network);
  auto place = edit::place_assembly(network, road, 40.0, -6.0, 0.0, "signal_mast");
  ASSERT_TRUE(place->apply(network).has_value());
  const std::vector<ObjectId> parts = only_assembly(network);
  ASSERT_GE(parts.size(), 3U);
  const double s_before = network.object(parts[1])->s;
  const std::string before = snapshot_xodr(network);

  auto detach = edit::detach_assembly_part(network, parts[1]);
  ASSERT_TRUE(detach->apply(network).has_value());

  EXPECT_FALSE(network.object(parts[1])->assembly.has_value());
  EXPECT_DOUBLE_EQ(network.object(parts[1])->s, s_before) << "detaching must not move anything";
  EXPECT_EQ(assembly_parts(network, parts[0]).size(), parts.size() - 1)
      << "the siblings stay grouped";
  EXPECT_TRUE(assembly_parts(network, parts[1]).empty());

  ASSERT_TRUE(detach->revert(network).has_value());
  EXPECT_EQ(snapshot_xodr(network), before) << "undo must be byte-identical";
}

TEST(DetachAssemblyPart, MakesThePartMovableAgain) {
  RoadNetwork network;
  const RoadId road = author_street(network);
  auto place = edit::place_assembly(network, road, 40.0, -6.0, 0.0, "signal_mast");
  ASSERT_TRUE(place->apply(network).has_value());
  const std::vector<ObjectId> parts = only_assembly(network);

  EXPECT_FALSE(edit::move_object(network, parts[1], 60.0, -6.0)->apply(network).has_value())
      << "a part is not individually movable";
  ASSERT_TRUE(edit::detach_assembly_part(network, parts[1])->apply(network).has_value());
  EXPECT_TRUE(edit::move_object(network, parts[1], 60.0, -6.0)->apply(network).has_value())
      << "detaching is the escape hatch that makes the refusal tolerable";
}

TEST(DetachAssemblyPart, RefusesAPlainPropAndAStaleId) {
  RoadNetwork network;
  const RoadId road = author_street(network);
  Object tree;
  tree.odr_id = "1";
  tree.name = "tree_pine";
  tree.type = ObjectType::Tree;
  tree.s = 10.0;
  tree.t = 4.0;
  auto add = edit::add_object(network, road, tree);
  ASSERT_TRUE(add->apply(network).has_value());
  ObjectId plain;
  network.for_each_object([&](ObjectId id, const Object&) { plain = id; });

  EXPECT_FALSE(edit::detach_assembly_part(network, plain)->apply(network).has_value());
  EXPECT_FALSE(edit::detach_assembly_part(network, ObjectId{})->apply(network).has_value());
}

TEST(MoveObject, RefusesAnAssemblyPartWithoutMutating) {
  RoadNetwork network;
  const RoadId road = author_street(network);
  auto place = edit::place_assembly(network, road, 40.0, -6.0, 0.0, "signal_mast");
  ASSERT_TRUE(place->apply(network).has_value());
  const std::vector<ObjectId> parts = only_assembly(network);
  const std::string before = snapshot_xodr(network);

  auto move = edit::move_object(network, parts.front(), 60.0, -6.0);
  EXPECT_FALSE(move->apply(network).has_value());
  EXPECT_EQ(snapshot_xodr(network), before)
      << "the record would otherwise claim an offset the object no longer sits at";
}

// --------------------------------------------------------------------------- //
// rm:assembly persistence
// --------------------------------------------------------------------------- //

TEST(AssemblyPersistence, TheCodeIsRegisteredAtObjectScope) {
  EXPECT_TRUE(is_registered_rm_code("rm:assembly"));
  const auto entry =
      std::ranges::find_if(kRmCodes, [](const RmCode& code) { return code.code == "rm:assembly"; });
  ASSERT_NE(entry, kRmCodes.end());
  EXPECT_EQ(entry->scope, RmCodeScope::Object);
}

TEST(AssemblyPersistence, RoundTripsEveryFieldThroughXodr) {
  RoadNetwork network;
  const RoadId road = author_street(network);
  props::clear_project_assemblies();
  props::register_project_assemblies(two_part_fixture());
  auto place = edit::place_assembly(network, road, 50.0, -4.0, 0.3, "test_pair");
  ASSERT_TRUE(place->apply(network).has_value());
  const std::vector<ObjectId> parts = only_assembly(network);
  const AssemblyData authored = *network.object(parts[1])->assembly;

  const auto text = write_xodr(network, "assembly");
  ASSERT_TRUE(text.has_value()) << text.error().message;
  const auto reloaded = parse_xodr(*text);
  ASSERT_TRUE(reloaded.has_value()) << reloaded.error().message;

  const std::vector<ObjectId> reread = only_assembly(reloaded->network);
  ASSERT_EQ(reread.size(), 2U);
  const Object& part = *reloaded->network.object(reread[1]);
  ASSERT_TRUE(part.assembly.has_value());
  EXPECT_EQ(part.assembly->asset, authored.asset);
  EXPECT_EQ(part.assembly->instance, authored.instance);
  EXPECT_EQ(part.assembly->part, authored.part);
  EXPECT_NEAR(part.assembly->du, authored.du, 1e-9);
  EXPECT_NEAR(part.assembly->dv, authored.dv, 1e-9);
  EXPECT_NEAR(part.assembly->dz, authored.dz, 1e-9);
  EXPECT_NEAR(part.assembly->dyaw, authored.dyaw, 1e-9);

  // Write→read→write is byte-stable, the round-trip oracle.
  const auto again = write_xodr(reloaded->network, "assembly");
  ASSERT_TRUE(again.has_value());
  EXPECT_EQ(*again, *text);

  props::clear_project_assemblies();
}

// The omitted-at-default rule, asserted on the BYTES: the anchor part carries no
// offsets, so none of the four attributes may appear on it.
TEST(AssemblyPersistence, OffsetsAreOmittedAtTheirDefault) {
  RoadNetwork network;
  const RoadId road = author_street(network);
  props::clear_project_assemblies();
  props::PropAssembly plain;
  plain.id = "one_part";
  plain.parts = {props::AssemblyPart{.model = "tree_pine"}};
  props::register_project_assemblies({plain});
  auto place = edit::place_assembly(network, road, 50.0, -4.0, 0.0, "one_part");
  ASSERT_TRUE(place->apply(network).has_value());

  const auto text = write_xodr(network, "assembly");
  ASSERT_TRUE(text.has_value());
  EXPECT_NE(text->find("code=\"rm:assembly\""), std::string::npos)
      << "the record itself must be written";
  for (const char* attr : {"du=", "dv=", "dz=", "dyaw="}) {
    EXPECT_EQ(text->find(attr), std::string::npos)
        << attr << " must be omitted at its default of zero";
  }

  props::clear_project_assemblies();
}

TEST(AssemblyPersistence, AMalformedRecordDropsTheRecordAndKeepsTheObject) {
  // Four ways to be unreadable, each of which must cost the grouping and nothing
  // else. Asserting the object SURVIVES is the point: an over-eager reader that
  // dropped the <object> too would lose the user's prop.
  const std::vector<std::pair<const char*, const char*>> cases{
      {"no inst", R"(<userData code="rm:assembly" asset="a" part="0" />)"},
      {"no part", R"(<userData code="rm:assembly" asset="a" inst="1" />)"},
      {"part out of range", R"(<userData code="rm:assembly" asset="a" inst="1" part="9001" />)"},
      {"bad offset", R"(<userData code="rm:assembly" asset="a" inst="1" part="0" dv="wat" />)"},
      {"negative part", R"(<userData code="rm:assembly" asset="a" inst="1" part="-1" />)"},
  };
  for (const auto& [label, record] : cases) {
    SCOPED_TRACE(label);
    const std::string xodr = std::string(R"(<?xml version="1.0"?>
<OpenDRIVE>
  <header revMajor="1" revMinor="8" name="t" />
  <road length="80" id="1" junction="-1">
    <planView><geometry s="0" x="0" y="0" hdg="0" length="80"><line /></geometry></planView>
    <lanes><laneSection s="0">
      <center><lane id="0" type="none" level="false" /></center>
      <right><lane id="-1" type="driving" level="false">
        <width sOffset="0" a="3.6" b="0" c="0" d="0" />
      </lane></right>
    </laneSection></lanes>
    <objects>
      <object type="pole" name="pole_signal" id="1" s="10" t="-6" zOffset="0" radius="0.14" height="6.47">
        )") + record + R"(
      </object>
    </objects>
  </road>
</OpenDRIVE>)";

    const auto parsed = parse_xodr(xodr);
    ASSERT_TRUE(parsed.has_value()) << parsed.error().message;
    EXPECT_EQ(parsed->network.object_count(), 1U) << "Layer 0 must survive a bad Layer-1 record";
    ObjectId only;
    parsed->network.for_each_object([&](ObjectId id, const Object&) { only = id; });
    EXPECT_FALSE(parsed->network.object(only)->assembly.has_value())
        << "a malformed record must be dropped, not half-believed";
    EXPECT_FALSE(parsed->diagnostics.empty()) << "and it must say so";
  }
}

TEST(AssemblyPersistence, AnUnknownAttributeWarnsWithoutCostingTheRecord) {
  const std::string xodr = R"(<?xml version="1.0"?>
<OpenDRIVE>
  <header revMajor="1" revMinor="8" name="t" />
  <road length="80" id="1" junction="-1">
    <planView><geometry s="0" x="0" y="0" hdg="0" length="80"><line /></geometry></planView>
    <lanes><laneSection s="0">
      <center><lane id="0" type="none" level="false" /></center>
      <right><lane id="-1" type="driving" level="false">
        <width sOffset="0" a="3.6" b="0" c="0" d="0" />
      </lane></right>
    </laneSection></lanes>
    <objects>
      <object type="pole" name="pole_signal" id="1" s="10" t="-6" zOffset="0" radius="0.14" height="6.47">
        <userData code="rm:assembly" asset="signal_mast" inst="1" part="0" futureField="9" />
      </object>
    </objects>
  </road>
</OpenDRIVE>)";

  const auto parsed = parse_xodr(xodr);
  ASSERT_TRUE(parsed.has_value()) << parsed.error().message;
  ObjectId only;
  parsed->network.for_each_object([&](ObjectId id, const Object&) { only = id; });
  ASSERT_TRUE(parsed->network.object(only)->assembly.has_value())
      << "a newer RoadMaker's extra field must not ungroup the assembly";
  EXPECT_EQ(parsed->network.object(only)->assembly->asset, "signal_mast");
  const bool warned = std::ranges::any_of(parsed->diagnostics, [](const Diagnostic& d) {
    return d.message.find("rm:assembly") != std::string::npos;
  });
  EXPECT_TRUE(warned) << "but it must still be reported";
}

// --------------------------------------------------------------------------- //
// The mesher: this sprint's claim is that it needed no change at all
// --------------------------------------------------------------------------- //

TEST(AssemblyMesh, EveryPartBecomesItsOwnInstanceAtItsOwnPose) {
  RoadNetwork network;
  const RoadId road = author_street(network);
  auto place = edit::place_assembly(network, road, 40.0, -6.0, 0.0, "signal_mast");
  ASSERT_TRUE(place->apply(network).has_value());

  const props::PropAssembly& definition = *props::assembly("signal_mast");
  const NetworkMesh mesh = build_network_mesh(network);
  ASSERT_EQ(mesh.objects.size(), definition.parts.size())
      << "the renderer needed no change BECAUSE the parts are ordinary objects";

  // ★ EVERY EXPECTATION HERE IS AGAINST THE CATALOGUE, never against the placed
  // object's own field. An earlier version compared the instance's z to
  // `part.z_offset` and PASSED with placement zeroing z_offset outright — both
  // sides of the comparison came from the same broken value. The catalogue's `dz`
  // is the independent authority, so the chain
  // definition -> object -> mesh instance is checked end to end.
  const std::vector<ObjectId> parts = only_assembly(network);
  ASSERT_EQ(parts.size(), definition.parts.size());
  for (std::size_t i = 0; i < parts.size(); ++i) {
    SCOPED_TRACE("part " + std::to_string(i) + " (" + definition.parts[i].model + ")");
    const props::AssemblyPart& spec = definition.parts[i];
    const Object& part = *network.object(parts[i]);
    EXPECT_EQ(part.name, spec.model);
    EXPECT_DOUBLE_EQ(part.z_offset, spec.dz) << "the authored dz must reach the object";

    const auto found = std::ranges::find_if(
        mesh.objects, [&](const ObjectInstance& instance) { return instance.object == parts[i]; });
    ASSERT_NE(found, mesh.objects.end());
    EXPECT_EQ(found->model_id, spec.model);
    EXPECT_NEAR(found->position[2], spec.dz, 1e-6)
        << "and reach the mesh instance, or the arm draws on the ground";
    // The heading likewise: without the arm's quarter turn its instance would be
    // aimed along the road, which is the whole bug the dyaw field exists to avoid.
    EXPECT_NEAR(std::remainder(found->heading - spec.dyaw, 2.0 * std::numbers::pi), 0.0, 1e-6)
        << "the part's dyaw must reach the instance heading";
  }
}

} // namespace
} // namespace roadmaker
