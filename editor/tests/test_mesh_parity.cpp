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

// Editor/export parity: the mesh the editor scene holds must be EXACTLY the
// mesh the kernel builds from the same network — one meshing path, no drift.
// Document keeps its NetworkMesh current through incremental remesh
// (remesh_roads/remesh_junctions per DirtySet); if that path ever diverges
// from a from-scratch build_network_mesh, the editor shows something the
// export does not (the class of bug behind the tee's phantom seams,
// follow-up to issue #103). Exercised across load, a topology-churning edit
// (attach_t_junction), and undo.

#include "roadmaker/edit/operations.hpp"
#include "roadmaker/mesh/mesh_builder.hpp"
#include "roadmaker/road/object.hpp"
#include "roadmaker/road/signal.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <vector>

#include "document/document.hpp"
#include "render/scene_builder.hpp"

using roadmaker::JunctionFloor;
using roadmaker::NetworkMesh;
using roadmaker::ObjectInstance;
using roadmaker::RoadId;
using roadmaker::RoadMesh;
using roadmaker::SignalInstance;
using roadmaker::editor::Document;

namespace {

std::filesystem::path tee_sample() {
  return std::filesystem::path(RM_SAMPLES_DIR) / "t_attach.xodr";
}

/// Buffers must match per road id / junction id (order may differ between
/// incremental and from-scratch builds).
void expect_mesh_parity(const NetworkMesh& editor_mesh, const NetworkMesh& fresh) {
  ASSERT_EQ(editor_mesh.roads.size(), fresh.roads.size());
  for (const RoadMesh& expected : fresh.roads) {
    const auto found = std::ranges::find(editor_mesh.roads, expected.road, &RoadMesh::road);
    ASSERT_NE(found, editor_mesh.roads.end()) << "road missing from the editor mesh";
    EXPECT_EQ(found->positions, expected.positions);
    EXPECT_EQ(found->normals, expected.normals);
    ASSERT_EQ(found->lanes.size(), expected.lanes.size());
    for (std::size_t i = 0; i < expected.lanes.size(); ++i) {
      EXPECT_EQ(found->lanes[i].indices, expected.lanes[i].indices);
      EXPECT_EQ(found->lanes[i].material, expected.lanes[i].material);
    }
    ASSERT_EQ(found->markings.size(), expected.markings.size());
    for (std::size_t i = 0; i < expected.markings.size(); ++i) {
      EXPECT_EQ(found->markings[i].positions, expected.markings[i].positions);
      EXPECT_EQ(found->markings[i].indices, expected.markings[i].indices);
    }
  }
  ASSERT_EQ(editor_mesh.junction_floors.size(), fresh.junction_floors.size());
  for (const JunctionFloor& expected : fresh.junction_floors) {
    const auto found =
        std::ranges::find(editor_mesh.junction_floors, expected.junction, &JunctionFloor::junction);
    ASSERT_NE(found, editor_mesh.junction_floors.end()) << "floor missing from the editor mesh";
    EXPECT_EQ(found->mesh.positions, expected.mesh.positions);
    EXPECT_EQ(found->mesh.normals, expected.mesh.normals);
    EXPECT_EQ(found->mesh.indices, expected.mesh.indices);
    EXPECT_EQ(found->mesh.material, expected.mesh.material);
  }
  // The derived placement layer (#400): a prop stores no world pose, so any edit
  // that moves its road must re-derive it. Comparing it here makes that a
  // standing guarantee — the next op that moves a road without re-anchoring its
  // props fails this, not a hand-run.
  // Compared BY INDEX, not by id lookup: re-deriving a road's placements
  // appends them, and the channel's order reaches exported files (USD prim
  // names, glTF node sequence), so the incremental path restores arena order
  // and parity has to include it. An id-keyed comparison would pass on a
  // silently renumbered channel.
  ASSERT_EQ(editor_mesh.objects.size(), fresh.objects.size()) << "prop instance count drifted";
  for (std::size_t i = 0; i < fresh.objects.size(); ++i) {
    const ObjectInstance& expected = fresh.objects[i];
    const ObjectInstance& actual = editor_mesh.objects[i];
    EXPECT_EQ(actual.object, expected.object) << "prop instance order drifted at " << i;
    EXPECT_EQ(actual.position, expected.position);
    EXPECT_EQ(actual.heading, expected.heading);
    EXPECT_EQ(actual.scale, expected.scale);
    EXPECT_EQ(actual.road, expected.road);
  }
  ASSERT_EQ(editor_mesh.signal_instances.size(), fresh.signal_instances.size())
      << "signal instance count drifted";
  for (std::size_t i = 0; i < fresh.signal_instances.size(); ++i) {
    const SignalInstance& expected = fresh.signal_instances[i];
    const SignalInstance& actual = editor_mesh.signal_instances[i];
    EXPECT_EQ(actual.signal, expected.signal) << "signal instance order drifted at " << i;
    EXPECT_EQ(actual.position, expected.position);
    EXPECT_EQ(actual.heading, expected.heading);
    EXPECT_EQ(actual.road, expected.road);
  }
}

} // namespace

TEST(MeshParity, LoadedDocumentMatchesKernelBuild) {
  Document document;
  ASSERT_TRUE(document.load(tee_sample()).has_value());
  expect_mesh_parity(document.mesh(), roadmaker::build_network_mesh(document.network()));
}

TEST(MeshParity, AttachEditKeepsIncrementalMeshIdenticalToScratchBuild) {
  Document document;
  document.reset();
  ASSERT_TRUE(
      document
          .push_command(roadmaker::edit::create_road(
              {roadmaker::Waypoint{.x = -60.0, .y = 0.0}, roadmaker::Waypoint{.x = 60.0, .y = 0.0}},
              roadmaker::LaneProfile::two_lane_default(),
              ""))
          .has_value());
  ASSERT_TRUE(
      document
          .push_command(roadmaker::edit::create_road({roadmaker::Waypoint{.x = 0.0, .y = -50.0},
                                                      roadmaker::Waypoint{.x = 0.0, .y = -10.0}},
                                                     roadmaker::LaneProfile::two_lane_default(),
                                                     ""))
          .has_value());
  RoadId target;
  RoadId branch;
  document.network().for_each_road(
      [&](RoadId id, const roadmaker::Road& road) { (road.odr_id == "1" ? target : branch) = id; });
  ASSERT_TRUE(target.is_valid());
  ASSERT_TRUE(branch.is_valid());

  ASSERT_TRUE(document
                  .push_command(roadmaker::edit::attach_t_junction(
                      document.network(),
                      roadmaker::RoadEnd{branch, roadmaker::ContactPoint::End},
                      target,
                      60.0))
                  .has_value());
  expect_mesh_parity(document.mesh(), roadmaker::build_network_mesh(document.network()));

  // Undo runs the incremental path in reverse — parity must survive it too.
  document.undo_stack()->undo();
  expect_mesh_parity(document.mesh(), roadmaker::build_network_mesh(document.network()));
}

// #400 in its most general form: an edit that moves a road must leave the
// incremental mesh indistinguishable from a rebuild — props included. Before the
// fix the prop instances kept their pre-move transform and this diverged.
TEST(MeshParity, MovingARoadKeepsItsPropsAndSignsInParity) {
  Document document;
  document.reset();
  ASSERT_TRUE(
      document
          .push_command(roadmaker::edit::create_road(
              {roadmaker::Waypoint{.x = 0.0, .y = 0.0}, roadmaker::Waypoint{.x = 120.0, .y = 0.0}},
              roadmaker::LaneProfile::two_lane_default(),
              ""))
          .has_value());
  RoadId road;
  document.network().for_each_road([&](RoadId id, const roadmaker::Road&) { road = id; });
  ASSERT_TRUE(road.is_valid());

  roadmaker::Object tree;
  tree.odr_id = "1";
  tree.name = "tree_pine";
  tree.type = roadmaker::ObjectType::Tree;
  tree.s = 40.0;
  tree.t = 6.0;
  ASSERT_TRUE(document.push_command(roadmaker::edit::add_object(document.network(), road, tree))
                  .has_value());

  roadmaker::Signal sign;
  sign.odr_id = "1";
  sign.type = "R1-1";
  sign.country = "US";
  sign.s = 60.0;
  sign.t = -5.0;
  ASSERT_TRUE(document.push_command(roadmaker::edit::add_signal(document.network(), road, sign))
                  .has_value());
  ASSERT_FALSE(document.mesh().objects.empty()) << "the prop must mesh, or this proves nothing";
  ASSERT_FALSE(document.mesh().signal_instances.empty());

  ASSERT_TRUE(
      document.push_command(roadmaker::edit::translate_road(document.network(), road, 15.0, -8.0))
          .has_value());
  expect_mesh_parity(document.mesh(), roadmaker::build_network_mesh(document.network()));

  ASSERT_TRUE(
      document.push_command(roadmaker::edit::rotate_road(document.network(), road, 0.4, 10.0, 10.0))
          .has_value());
  expect_mesh_parity(document.mesh(), roadmaker::build_network_mesh(document.network()));

  // Undo walks the same incremental path backwards.
  document.undo_stack()->undo();
  expect_mesh_parity(document.mesh(), roadmaker::build_network_mesh(document.network()));
  document.undo_stack()->undo();
  expect_mesh_parity(document.mesh(), roadmaker::build_network_mesh(document.network()));
}

TEST(MeshParity, SceneItemsPassKernelBuffersThroughUnchanged) {
  Document document;
  ASSERT_TRUE(document.load(tee_sample()).has_value());
  const NetworkMesh& mesh = document.mesh();
  const roadmaker::editor::Scene scene = roadmaker::editor::build_scene(mesh);

  // Every scene item's float buffer is the narrowed kernel buffer — the
  // renderer draws exactly what the kernel meshed.
  std::size_t item = 0;
  for (const RoadMesh& road : mesh.roads) {
    for (const RoadMesh::LanePatch& patch : road.lanes) {
      ASSERT_LT(item, scene.items.size());
      const roadmaker::editor::RenderMeshData& data = scene.items[item++].data;
      ASSERT_EQ(data.positions.size(), road.positions.size());
      for (std::size_t i = 0; i < road.positions.size(); ++i) {
        EXPECT_EQ(data.positions[i], static_cast<float>(road.positions[i]));
      }
      EXPECT_EQ(data.indices, patch.indices);
    }
    item += road.markings.size();
  }
}
