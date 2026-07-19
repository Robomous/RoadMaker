// Marking-curve placement helper (p3-s4, issue #223): the pure geometry behind
// the Marking Curve tool. Builds a straight road and asserts anchor resolution,
// projection onto the anchor, the single-road constraint, and that the kernel's
// curvature refusal surfaces as an error.

#include "roadmaker/edit/markings.hpp"
#include "roadmaker/edit/operations.hpp"
#include "roadmaker/road/network.hpp"
#include "roadmaker/road/object.hpp"

#include <gtest/gtest.h>

#include <array>
#include <stdexcept>
#include <vector>

#include "document/library_manifest.hpp"
#include "document/marking_curve_placement.hpp"
#include "render/material_catalog.hpp"
#include "viewport/picking.hpp"

namespace roadmaker::editor {
namespace {

using roadmaker::RoadNetwork;
using roadmaker::Waypoint;

RoadNetwork straight_road() {
  RoadNetwork network;
  auto command = roadmaker::edit::create_road(
      {Waypoint{-40.0, 0.0}, Waypoint{40.0, 0.0}}, roadmaker::LaneProfile::two_lane_rural(), "");
  if (command == nullptr || !command->apply(network).has_value()) {
    throw std::runtime_error("road setup failed");
  }
  return network;
}

edit::MarkingCurveParams plain_line() {
  edit::MarkingCurveParams params;
  params.width_m = 0.12;
  params.striped = false;
  params.asset = "marking.solid_white";
  return params;
}

TEST(MarkingCurvePlacement, AnchorResolvesNearestRoad) {
  const RoadNetwork network = straight_road();
  const auto anchor = anchor_road_at(network, 0.0, 0.5, kObjectSnapThreshold);
  ASSERT_TRUE(anchor.has_value());
  EXPECT_EQ(*anchor, network.find_road("1"));
  // Far off any road: no anchor.
  EXPECT_FALSE(anchor_road_at(network, 0.0, 80.0, kObjectSnapThreshold).has_value());
}

TEST(MarkingCurvePlacement, BuildsCenterlineProjectedOntoAnchor) {
  const RoadNetwork network = straight_road();
  const RoadId anchor = network.find_road("1");
  const std::vector<Waypoint> points{{-10.0, 0.0}, {0.0, 0.0}, {10.0, 0.0}};
  const auto result = build_marking_curve(network, anchor, points, plain_line());
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->road, anchor);
  EXPECT_FALSE(result->object.odr_id.empty());
  ASSERT_TRUE(result->object.marking_curve.has_value());
  EXPECT_FALSE(result->object.marking_curve->striped);
  EXPECT_GE(result->object.marking_curve->samples.size(), 2U);
}

TEST(MarkingCurvePlacement, RejectsFewerThanTwoPoints) {
  const RoadNetwork network = straight_road();
  const std::vector<Waypoint> one{{0.0, 0.0}};
  EXPECT_FALSE(build_marking_curve(network, network.find_road("1"), one, plain_line()).has_value());
}

TEST(MarkingCurvePlacement, EnforcesSingleRoadConstraint) {
  const RoadNetwork network = straight_road();
  const RoadId anchor = network.find_road("1");
  // The second point is far off the anchor road — the curve may not leave it.
  const std::vector<Waypoint> points{{-10.0, 0.0}, {0.0, 60.0}};
  const auto result = build_marking_curve(network, anchor, points, plain_line());
  EXPECT_FALSE(result.has_value());
}

TEST(MarkingCurvePlacement, CurvatureTooTightSurfacesAsError) {
  const RoadNetwork network = straight_road();
  const RoadId anchor = network.find_road("1");
  // A wide band (width/2 = 3 m) over a tight bump: the +-width/2 offset band
  // would self-intersect, so apply_marking_curve_asset refuses it.
  edit::MarkingCurveParams wide = plain_line();
  wide.striped = true;
  wide.width_m = 6.0;
  const std::vector<Waypoint> bump{{0.0, 0.0}, {1.0, 1.2}, {2.0, 0.0}};
  const auto result = build_marking_curve(network, anchor, bump, wide);
  EXPECT_FALSE(result.has_value());
}

TEST(MarkingCurvePlacement, ParamsFromCrosswalkAssetAreStriped) {
  const MaterialCatalog materials;
  LibraryItem crosswalk;
  crosswalk.key = "crosswalk.zebra";
  crosswalk.kind = LibraryItem::Kind::Crosswalk;
  crosswalk.crosswalk_width = 3.0;
  crosswalk.crosswalk_dash = 0.5;
  crosswalk.crosswalk_gap = 0.5;
  crosswalk.crosswalk_material = "material.paint_white";
  const edit::MarkingCurveParams params = marking_curve_params_from_item(crosswalk, materials);
  EXPECT_TRUE(params.striped);
  EXPECT_DOUBLE_EQ(params.width_m, 3.0);
  EXPECT_DOUBLE_EQ(params.dash_length_m, 0.5);
  EXPECT_EQ(params.asset, "crosswalk.zebra");

  LibraryItem marking;
  marking.key = "marking.solid_white";
  marking.kind = LibraryItem::Kind::Marking;
  marking.mark_width = 0.15;
  const edit::MarkingCurveParams line = marking_curve_params_from_item(marking, materials);
  EXPECT_FALSE(line.striped);
  EXPECT_DOUBLE_EQ(line.width_m, 0.15);

  EXPECT_TRUE(is_marking_curve_asset(crosswalk));
  EXPECT_TRUE(is_marking_curve_asset(marking));
  LibraryItem tree;
  tree.kind = LibraryItem::Kind::Tree;
  EXPECT_FALSE(is_marking_curve_asset(tree));
}

} // namespace
} // namespace roadmaker::editor
