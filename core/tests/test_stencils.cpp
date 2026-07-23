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

// Point-stencil arrow authoring + concave meshing (p3-s4): edit::
// apply_stencil_asset writes one closed cornerLocal arrow outline that the
// mesher tessellates with the in-tree CDT, and arrow_glyph_outline is the single
// source of the 6-arrow glyph set.

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
#include <cmath>
#include <string>
#include <vector>

using roadmaker::LaneProfile;
using roadmaker::Object;
using roadmaker::RoadId;
using roadmaker::RoadNetwork;
using roadmaker::Waypoint;

namespace {

const std::array<std::string, 6> kArrows{"arrowStraight",
                                         "arrowLeft",
                                         "arrowRight",
                                         "arrowLeftRight",
                                         "arrowStraightLeft",
                                         "arrowStraightRight"};

RoadId author_street(RoadNetwork& network) {
  const std::vector<Waypoint> waypoints{Waypoint{0.0, 0.0}, Waypoint{120.0, 0.0}};
  auto road =
      roadmaker::author_clothoid_road(network, waypoints, LaneProfile::two_lane_default(), "", "1");
  if (!road.has_value()) {
    throw std::runtime_error("author_street: " + road.error().message);
  }
  return *road;
}

double polygon_area(const std::vector<roadmaker::OutlineCorner>& corners) {
  double a = 0.0;
  for (std::size_t i = 0; i < corners.size(); ++i) {
    const auto& p = corners[i];
    const auto& q = corners[(i + 1) % corners.size()];
    a += (p.a * q.b) - (q.a * p.b);
  }
  return std::abs(a) / 2.0;
}

// XY area of a marking submesh (the road is flat at z=0, frame is orthonormal).
double mesh_area(const roadmaker::SubMesh& m) {
  double a = 0.0;
  for (std::size_t i = 0; i + 2 < m.indices.size(); i += 3) {
    const auto x = [&](std::uint32_t k) { return m.positions[3 * k]; };
    const auto y = [&](std::uint32_t k) { return m.positions[(3 * k) + 1]; };
    const std::uint32_t i0 = m.indices[i];
    const std::uint32_t i1 = m.indices[i + 1];
    const std::uint32_t i2 = m.indices[i + 2];
    a += std::abs(((x(i1) - x(i0)) * (y(i2) - y(i0))) - ((x(i2) - x(i0)) * (y(i1) - y(i0)))) / 2.0;
  }
  return a;
}

} // namespace

TEST(Stencils, ApplyAuthorsLocalOutlineMaterialAndUserData) {
  Object object;
  object.odr_id = "5";
  roadmaker::edit::StencilParams params;
  params.subtype = "arrowLeft";
  params.length_m = 4.0;
  params.width_m = 1.75;
  params.material = "material.paint_white";
  params.asset = "stencil.arrow_left";
  const auto ok = roadmaker::edit::apply_stencil_asset(object, params);
  ASSERT_TRUE(ok.has_value()) << (ok ? "" : ok.error().message);

  EXPECT_EQ(object.type_str, "roadMark");
  EXPECT_EQ(object.subtype, "arrowLeft");
  // ONE closed cornerLocal outline — no mixed corner kinds.
  ASSERT_EQ(object.outlines.size(), 1U);
  EXPECT_FALSE(object.outlines.front().road_coords);
  ASSERT_TRUE(object.outlines.front().closed.has_value());
  EXPECT_TRUE(*object.outlines.front().closed);
  EXPECT_GE(object.outlines.front().corners.size(), 3U);
  // rm:stencil keys the instance to its asset.
  ASSERT_TRUE(object.stencil.has_value());
  EXPECT_EQ(object.stencil->asset, "stencil.arrow_left");
  EXPECT_EQ(object.stencil->material, "material.paint_white");
  // <material roadMarkColor> preserved for foreign viewers.
  bool has_material = false;
  for (const std::string& child : object.preserved.children) {
    if (child.find("roadMarkColor") != std::string::npos) {
      has_material = true;
    }
  }
  EXPECT_TRUE(has_material);
}

TEST(Stencils, UnknownSubtypeRejected) {
  Object object;
  roadmaker::edit::StencilParams params;
  params.subtype = "arrowMergeLeft"; // outside the 6-arrow core set
  EXPECT_FALSE(roadmaker::edit::apply_stencil_asset(object, params).has_value());
}

TEST(Stencils, SixSubtypesGiveDistinctGlyphs) {
  std::vector<std::vector<roadmaker::OutlineCorner>> glyphs;
  for (const std::string& subtype : kArrows) {
    const auto outline = roadmaker::edit::arrow_glyph_outline(subtype, 4.0, 1.75);
    ASSERT_GE(outline.size(), 3U) << subtype;
    EXPECT_GT(polygon_area(outline), 0.0) << subtype;
    glyphs.push_back(outline);
  }
  for (std::size_t i = 0; i < glyphs.size(); ++i) {
    for (std::size_t j = i + 1; j < glyphs.size(); ++j) {
      EXPECT_NE(glyphs[i], glyphs[j]) << kArrows[i] << " vs " << kArrows[j];
    }
  }
}

TEST(Stencils, SubtypesMeshTessellatedAreaMatchesPolygon) {
  for (const std::string& subtype : kArrows) {
    RoadNetwork network;
    const RoadId road = author_street(network);
    Object object;
    object.odr_id = "5";
    roadmaker::edit::StencilParams params;
    params.subtype = subtype;
    params.length_m = 4.0;
    params.width_m = 1.75;
    ASSERT_TRUE(roadmaker::edit::apply_stencil_asset(object, params).has_value());
    object.s = 40.0;
    object.t = -1.75; // sit on the right driving lane
    ASSERT_TRUE(roadmaker::edit::add_object(network, road, object)->apply(network).has_value());

    const roadmaker::NetworkMesh mesh = roadmaker::build_network_mesh(network);
    const roadmaker::SubMesh* arrow = nullptr;
    for (const auto& r : mesh.roads) {
      for (const auto& marking : r.markings) {
        if (marking.name.find("arrow") != std::string::npos && !marking.indices.empty()) {
          arrow = &marking;
        }
      }
    }
    ASSERT_NE(arrow, nullptr) << subtype;
    const double expected = polygon_area(roadmaker::edit::arrow_glyph_outline(subtype, 4.0, 1.75));
    EXPECT_NEAR(mesh_area(*arrow), expected, expected * 0.02) << subtype; // CDT is exact-ish
  }
}

TEST(Stencils, CornerLocalRoundTripsByteIdenticalBothVersions) {
  RoadNetwork network;
  const RoadId road = author_street(network);
  Object object;
  object.odr_id = "5";
  roadmaker::edit::StencilParams params;
  params.subtype = "arrowStraightLeft";
  params.material = "material.paint_white";
  params.asset = "stencil.arrow_straight_left";
  ASSERT_TRUE(roadmaker::edit::apply_stencil_asset(object, params).has_value());
  object.s = 40.0;
  object.t = -1.75;
  ASSERT_TRUE(roadmaker::edit::add_object(network, road, object)->apply(network).has_value());

  for (const auto version : {roadmaker::XodrVersion::v1_9_0, roadmaker::XodrVersion::v1_8_1}) {
    const roadmaker::WriterOptions opts{.target_version = version};
    const auto first = roadmaker::write_xodr(network, "st", opts);
    ASSERT_TRUE(first.has_value());
    EXPECT_NE(first->find("rm:stencil"), std::string::npos);
    EXPECT_NE(first->find("cornerLocal"), std::string::npos);
    auto parsed = roadmaker::parse_xodr(*first, "st");
    ASSERT_TRUE(parsed.has_value());
    const auto second = roadmaker::write_xodr(parsed->network, "st", opts);
    ASSERT_TRUE(second.has_value());
    EXPECT_EQ(*first, *second) << "stencil round trip must be byte-identical";
  }
}
