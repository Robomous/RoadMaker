// Copyright 2026 Robomous
// SPDX-License-Identifier: Apache-2.0

// Free-form marking-curve authoring (p3-s4): edit::apply_marking_curve_asset
// derives an Object band from an (s,t) centreline — outline + <markings> +
// rm:markingCurve userData — and the mesher walks that userData by arc length.

#include "roadmaker/edit/markings.hpp"
#include "roadmaker/edit/operations.hpp"
#include "roadmaker/mesh/mesh_builder.hpp"
#include "roadmaker/road/authoring.hpp"
#include "roadmaker/road/network.hpp"
#include "roadmaker/road/object.hpp"
#include "roadmaker/xodr/reader.hpp"
#include "roadmaker/xodr/writer.hpp"

#include <gtest/gtest.h>

#include <array>
#include <string>
#include <vector>

using roadmaker::LaneProfile;
using roadmaker::Object;
using roadmaker::RoadId;
using roadmaker::RoadNetwork;
using roadmaker::Waypoint;

namespace {

RoadId author_street(RoadNetwork& network) {
  const std::vector<Waypoint> waypoints{Waypoint{0.0, 0.0}, Waypoint{120.0, 0.0}};
  auto road =
      roadmaker::author_clothoid_road(network, waypoints, LaneProfile::two_lane_default(), "", "1");
  if (!road.has_value()) {
    throw std::runtime_error("author_street: " + road.error().message);
  }
  return *road;
}

// A gentle straight-ish centreline in the road's (s,t) frame.
std::vector<std::array<double, 2>> gentle_line() {
  return {{10.0, 0.0}, {12.0, 0.5}, {14.0, 1.0}, {16.0, 1.5}, {18.0, 2.0}};
}

} // namespace

TEST(MarkingCurves, AuthorsOutlineMarkingsAndUserData) {
  Object object;
  object.odr_id = "5";
  const auto samples = gentle_line();
  roadmaker::edit::MarkingCurveParams params;
  params.width_m = 0.2;
  params.material = "material.paint_white";
  params.asset = "marking.solid_white";
  const auto ok = roadmaker::edit::apply_marking_curve_asset(object, samples, params);
  ASSERT_TRUE(ok.has_value()) << (ok ? "" : ok.error().message);

  // A closed cornerRoad band outline with two corners per sample.
  ASSERT_EQ(object.outlines.size(), 1U);
  const roadmaker::ObjectOutline& outline = object.outlines.front();
  EXPECT_TRUE(outline.road_coords);
  ASSERT_TRUE(outline.closed.has_value());
  EXPECT_TRUE(*outline.closed);
  EXPECT_EQ(outline.corners.size(), 2U * samples.size());
  ASSERT_EQ(outline.markings.size(), 1U);
  EXPECT_EQ(outline.markings.front().corner_refs.size(), 2U * samples.size());

  // rm:markingCurve userData is the render-time source of truth.
  ASSERT_TRUE(object.marking_curve.has_value());
  EXPECT_EQ(object.marking_curve->asset, "marking.solid_white");
  EXPECT_DOUBLE_EQ(object.marking_curve->width, 0.2);
  EXPECT_FALSE(object.marking_curve->striped);
  ASSERT_EQ(object.marking_curve->samples.size(), samples.size());
  EXPECT_DOUBLE_EQ(object.marking_curve->samples.front()[0], 10.0);
  // Placement anchors at the first sample for foreign viewers.
  EXPECT_DOUBLE_EQ(object.s, 10.0);
  EXPECT_DOUBLE_EQ(object.t, 0.0);
  // A plain marking is an untyped roadMark; not a crosswalk.
  EXPECT_EQ(object.type_str, "roadMark");
  EXPECT_FALSE(object.crosswalk.has_value());
}

TEST(MarkingCurves, StripedCrosswalkAssetTypesAsCrosswalk) {
  Object object;
  object.odr_id = "6";
  roadmaker::edit::MarkingCurveParams params;
  params.width_m = 3.0;
  params.dash_length_m = 0.5;
  params.dash_gap_m = 0.5;
  params.striped = true;
  params.asset = "crosswalk.zebra";
  const auto ok = roadmaker::edit::apply_marking_curve_asset(object, gentle_line(), params);
  ASSERT_TRUE(ok.has_value());
  EXPECT_EQ(object.type, roadmaker::ObjectType::Crosswalk);
  ASSERT_TRUE(object.marking_curve.has_value());
  EXPECT_TRUE(object.marking_curve->striped);
  EXPECT_DOUBLE_EQ(object.marking_curve->dash_length, 0.5);
}

TEST(MarkingCurves, DegenerateAndTightCurvesAreRejected) {
  Object object;
  roadmaker::edit::MarkingCurveParams params;
  params.width_m = 0.2;
  // Fewer than two samples.
  std::vector<std::array<double, 2>> one{{10.0, 0.0}};
  EXPECT_FALSE(roadmaker::edit::apply_marking_curve_asset(object, one, params).has_value());

  // A hairpin far tighter than half the (wide) band would self-intersect.
  params.width_m = 4.0;
  std::vector<std::array<double, 2>> hairpin{{10.0, 0.0}, {10.5, 0.0}, {10.0, 0.1}};
  EXPECT_FALSE(roadmaker::edit::apply_marking_curve_asset(object, hairpin, params).has_value());
}

TEST(MarkingCurves, AddedCurveMeshesFromUserData) {
  RoadNetwork network;
  const RoadId road = author_street(network);
  Object object;
  object.odr_id = "5";
  roadmaker::edit::MarkingCurveParams params;
  params.width_m = 0.3;
  ASSERT_TRUE(
      roadmaker::edit::apply_marking_curve_asset(object, gentle_line(), params).has_value());
  ASSERT_TRUE(roadmaker::edit::add_object(network, road, object)->apply(network).has_value());

  const roadmaker::NetworkMesh mesh = roadmaker::build_network_mesh(network);
  int curve_meshes = 0;
  for (const auto& r : mesh.roads) {
    for (const auto& marking : r.markings) {
      if (marking.name.find("marking curve") != std::string::npos && !marking.indices.empty()) {
        ++curve_meshes;
      }
    }
  }
  EXPECT_EQ(curve_meshes, 1);
}

TEST(MarkingCurves, ExportValidatesBothVersions) {
  RoadNetwork network;
  const RoadId road = author_street(network);
  Object object;
  object.odr_id = "5";
  roadmaker::edit::MarkingCurveParams params;
  params.width_m = 0.3;
  params.asset = "marking.solid_white";
  ASSERT_TRUE(
      roadmaker::edit::apply_marking_curve_asset(object, gentle_line(), params).has_value());
  ASSERT_TRUE(roadmaker::edit::add_object(network, road, object)->apply(network).has_value());

  for (const auto version : {roadmaker::XodrVersion::v1_9_0, roadmaker::XodrVersion::v1_8_1}) {
    const auto written = roadmaker::write_xodr(network, "mc", {.target_version = version});
    ASSERT_TRUE(written.has_value());
    EXPECT_NE(written->find("rm:markingCurve"), std::string::npos);
    EXPECT_NE(written->find("samples="), std::string::npos);
  }
}

TEST(MarkingCurves, RoundTripsByteIdenticalBothVersions) {
  RoadNetwork network;
  const RoadId road = author_street(network);
  Object object;
  object.odr_id = "5";
  roadmaker::edit::MarkingCurveParams params;
  params.width_m = 0.3;
  params.dash_length_m = 0.4;
  params.dash_gap_m = 0.2;
  params.asset = "marking.broken_white";
  ASSERT_TRUE(
      roadmaker::edit::apply_marking_curve_asset(object, gentle_line(), params).has_value());
  ASSERT_TRUE(roadmaker::edit::add_object(network, road, object)->apply(network).has_value());

  for (const auto version : {roadmaker::XodrVersion::v1_9_0, roadmaker::XodrVersion::v1_8_1}) {
    const roadmaker::WriterOptions opts{.target_version = version};
    const auto first = roadmaker::write_xodr(network, "mc", opts);
    ASSERT_TRUE(first.has_value());
    auto parsed = roadmaker::parse_xodr(*first, "mc");
    ASSERT_TRUE(parsed.has_value());
    const auto second = roadmaker::write_xodr(parsed->network, "mc", opts);
    ASSERT_TRUE(second.has_value());
    EXPECT_EQ(*first, *second) << "marking-curve round trip must be byte-identical";
  }
}

TEST(MarkingCurves, MalformedUserDataWarnsAndIsIgnored) {
  const std::string xml = R"(<?xml version="1.0"?>
<OpenDRIVE>
  <header revMajor="1" revMinor="9" name="" version="1.00" date="" north="0" south="0" east="0" west="0"/>
  <road name="r" length="120" id="1" junction="-1">
    <planView><geometry s="0" x="0" y="0" hdg="0" length="120"><line/></geometry></planView>
    <lanes><laneSection s="0"><center><lane id="0" type="driving" level="false"/></center>
      <right><lane id="-1" type="driving" level="false"><width sOffset="0" a="3.5" b="0" c="0" d="0"/></lane></right>
    </laneSection></lanes>
    <objects>
      <object type="roadMark" id="5" s="10" t="0" zOffset="0" orientation="none">
        <userData code="rm:markingCurve" asset="x" width="0.2" samples="oops"/>
      </object>
    </objects>
  </road>
</OpenDRIVE>
)";
  auto parsed = roadmaker::parse_xodr(xml, "bad");
  ASSERT_TRUE(parsed.has_value());
  bool warned = false;
  for (const roadmaker::Diagnostic& d : parsed->diagnostics) {
    if (d.message.find("rm:markingCurve") != std::string::npos) {
      warned = true;
    }
  }
  EXPECT_TRUE(warned);
  parsed->network.for_each_object(
      [](roadmaker::ObjectId, const Object& o) { EXPECT_FALSE(o.marking_curve.has_value()); });
}
