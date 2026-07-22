// Copyright 2026 Robomous
// SPDX-License-Identifier: Apache-2.0

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

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <vector>

#include "document/document.hpp"
#include "render/scene_builder.hpp"

using roadmaker::JunctionFloor;
using roadmaker::NetworkMesh;
using roadmaker::RoadId;
using roadmaker::RoadMesh;
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
