// Copyright 2026 Robomous
// SPDX-License-Identifier: Apache-2.0

// Stencil placement helper (p3-s4, issue #223): the pure geometry shared by the
// Marking Point tool and the Library drag-drop path. Builds a straight two-lane
// road and asserts lane/s/t/hdg resolution and off-road rejection headless.

#include "roadmaker/edit/operations.hpp"
#include "roadmaker/road/network.hpp"
#include "roadmaker/road/object.hpp"

#include <gtest/gtest.h>

#include <numbers>
#include <stdexcept>

#include "document/library_manifest.hpp"
#include "document/stencil_placement.hpp"
#include "render/material_catalog.hpp"

namespace roadmaker::editor {
namespace {

using roadmaker::RoadNetwork;
using roadmaker::Waypoint;

/// One straight two-lane road along +x from (-40, 0) to (40, 0): x=0 is s=40,
/// t = +y (left of travel), so a point at y=-1.75 sits in the right driving lane.
RoadNetwork straight_road() {
  RoadNetwork network;
  auto command = roadmaker::edit::create_road(
      {Waypoint{-40.0, 0.0}, Waypoint{40.0, 0.0}}, roadmaker::LaneProfile::two_lane_rural(), "");
  if (command == nullptr || !command->apply(network).has_value()) {
    throw std::runtime_error("road setup failed");
  }
  return network;
}

LibraryItem straight_arrow_item() {
  LibraryItem item;
  item.key = "stencil.arrow_straight";
  item.kind = LibraryItem::Kind::Stencil;
  item.stencil_subtype = "arrowStraight";
  item.stencil_length = 4.0;
  item.stencil_width_frac = 0.5;
  item.stencil_material = "material.paint_white";
  item.stencil_segmentation = "road_marking";
  return item;
}

TEST(StencilPlacement, PoseResolvesRightLaneTravelDirection) {
  const RoadNetwork network = straight_road();
  const auto pose = stencil_pose_for_point(network, 0.0, -1.75);
  ASSERT_TRUE(pose.has_value());
  EXPECT_NEAR(pose->s, 40.0, 1e-3);  // x=0 is 40 m from the -40 start
  EXPECT_LT(pose->t, 0.0);           // right of travel
  EXPECT_NEAR(pose->hdg, 0.0, 1e-9); // right lanes travel +s
  EXPECT_GT(pose->lane_width_m, 0.0);
}

TEST(StencilPlacement, PoseResolvesLeftLaneAsReverseTravel) {
  const RoadNetwork network = straight_road();
  const auto pose = stencil_pose_for_point(network, 0.0, 1.75);
  ASSERT_TRUE(pose.has_value());
  EXPECT_GT(pose->t, 0.0);                        // left of travel
  EXPECT_NEAR(pose->hdg, std::numbers::pi, 1e-9); // left lanes travel -s
}

TEST(StencilPlacement, PoseRejectsOpenSpace) {
  const RoadNetwork network = straight_road();
  EXPECT_FALSE(stencil_pose_for_point(network, 0.0, 50.0).has_value());
}

TEST(StencilPlacement, ForPointAuthorsGlyphOnLane) {
  const RoadNetwork network = straight_road();
  const MaterialCatalog materials;
  const auto placed = stencil_for_point(network, 0.0, -1.75, straight_arrow_item(), materials);
  ASSERT_TRUE(placed.has_value());
  const Object& object = placed->second;
  EXPECT_FALSE(object.odr_id.empty());
  EXPECT_EQ(object.type_str, "roadMark");
  EXPECT_EQ(object.subtype, "arrowStraight");
  ASSERT_TRUE(object.stencil.has_value());
  EXPECT_EQ(object.stencil->asset, "stencil.arrow_straight");
  ASSERT_EQ(object.outlines.size(), 1U);
  EXPECT_FALSE(object.outlines.front().road_coords); // cornerLocal glyph
  EXPECT_NEAR(object.hdg, 0.0, 1e-9);                // right-lane travel direction
}

TEST(StencilPlacement, ForPointRejectsNonStencilAsset) {
  const RoadNetwork network = straight_road();
  const MaterialCatalog materials;
  LibraryItem tree;
  tree.kind = LibraryItem::Kind::Tree;
  EXPECT_FALSE(stencil_for_point(network, 0.0, -1.75, tree, materials).has_value());
}

TEST(StencilPlacement, ForPointRejectsOpenSpace) {
  const RoadNetwork network = straight_road();
  const MaterialCatalog materials;
  EXPECT_FALSE(stencil_for_point(network, 0.0, 50.0, straight_arrow_item(), materials).has_value());
}

} // namespace
} // namespace roadmaker::editor
