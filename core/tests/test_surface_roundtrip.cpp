// Copyright 2026 Robomous
// SPDX-License-Identifier: Apache-2.0

// rm:surface <userData> round-trip (#215, p2-s7). A derived Surface serializes
// only its bounding-road ids; geometry is re-derived on load. These tests pin
// the two contracts the soak driver also enforces: write->parse->write is
// byte-identical, and a surface reconstructs from its marker.

#include "roadmaker/edit/operations.hpp"
#include "roadmaker/road/authoring.hpp"
#include "roadmaker/road/network.hpp"
#include "roadmaker/road/surface.hpp"
#include "roadmaker/road/surface_derivation.hpp"
#include "roadmaker/xodr/reader.hpp"
#include "roadmaker/xodr/writer.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

using roadmaker::derive_surfaces;
using roadmaker::LaneProfile;
using roadmaker::parse_xodr;
using roadmaker::RoadId;
using roadmaker::RoadNetwork;
using roadmaker::Surface;
using roadmaker::SurfaceId;
using roadmaker::Waypoint;
using roadmaker::write_xodr;

namespace {

RoadId
segment(RoadNetwork& network, const char* odr_id, double x0, double y0, double x1, double y1) {
  const std::vector<Waypoint> waypoints{Waypoint{.x = x0, .y = y0}, Waypoint{.x = x1, .y = y1}};
  auto road = roadmaker::author_clothoid_road(
      network, waypoints, LaneProfile::two_lane_default(), "", odr_id);
  EXPECT_TRUE(road.has_value());
  return road.value_or(RoadId{});
}

void author_square(RoadNetwork& network) {
  segment(network, "a", 0.0, 0.0, 10.0, 0.0);
  segment(network, "b", 10.0, 0.0, 10.0, 10.0);
  segment(network, "c", 10.0, 10.0, 0.0, 10.0);
  segment(network, "d", 0.0, 10.0, 0.0, 0.0);
}

std::string write(const RoadNetwork& network) {
  auto xml = write_xodr(network);
  EXPECT_TRUE(xml.has_value());
  return xml.value_or(std::string{});
}

} // namespace

TEST(SurfaceRoundTrip, SurfaceMarkerRoundTripsByteIdentical) {
  RoadNetwork network;
  author_square(network);
  derive_surfaces(network);
  ASSERT_EQ(network.surface_count(), 1U);

  const std::string first = write(network);
  EXPECT_NE(first.find("rm:surface"), std::string::npos);

  auto reparsed = parse_xodr(first);
  ASSERT_TRUE(reparsed.has_value());
  const std::string second = write(reparsed->network);

  EXPECT_EQ(first, second);
}

TEST(SurfaceRoundTrip, ReaderReconstructsSurfaceFromMarker) {
  RoadNetwork network;
  author_square(network);
  derive_surfaces(network);
  const std::string xml = write(network);

  auto reparsed = parse_xodr(xml);
  ASSERT_TRUE(reparsed.has_value());
  const RoadNetwork& loaded = reparsed->network;
  ASSERT_EQ(loaded.surface_count(), 1U);

  SurfaceId id{};
  loaded.for_each_surface([&](SurfaceId sid, const Surface&) { id = sid; });
  const Surface* surface = loaded.surface(id);
  ASSERT_NE(surface, nullptr);
  ASSERT_EQ(surface->bounding_roads.size(), 4U);

  std::vector<std::string> odr_ids;
  for (const RoadId road_id : surface->bounding_roads) {
    const roadmaker::Road* road = loaded.road(road_id);
    ASSERT_NE(road, nullptr);
    odr_ids.push_back(road->odr_id);
  }
  std::ranges::sort(odr_ids);
  EXPECT_EQ(odr_ids, (std::vector<std::string>{"a", "b", "c", "d"}));
}

TEST(SurfaceRoundTrip, NoSurfaceNoMarker) {
  RoadNetwork network;
  // An open chain encloses nothing.
  segment(network, "a", 0.0, 0.0, 10.0, 0.0);
  segment(network, "b", 10.0, 0.0, 20.0, 0.0);
  derive_surfaces(network);
  ASSERT_EQ(network.surface_count(), 0U);

  const std::string xml = write(network);
  EXPECT_EQ(xml.find("rm:surface"), std::string::npos);
}

// --- surface material (p6-s2) ------------------------------------------------

namespace {

SurfaceId the_surface(const RoadNetwork& network) {
  SurfaceId id{};
  network.for_each_surface([&](SurfaceId sid, const Surface&) { id = sid; });
  return id;
}

} // namespace

TEST(SurfaceMaterial, SetSurfaceMaterialApplyRevertIsByteIdentical) {
  RoadNetwork network;
  author_square(network);
  derive_surfaces(network);
  ASSERT_EQ(network.surface_count(), 1U);
  const std::string baseline = write(network);

  auto command = roadmaker::edit::set_surface_material(network, the_surface(network), "asphalt");
  ASSERT_NE(command, nullptr);
  ASSERT_TRUE(command->apply(network).has_value());
  const std::string with_material = write(network);
  EXPECT_NE(with_material.find("material=\"asphalt\""), std::string::npos);

  ASSERT_TRUE(command->revert(network).has_value());
  EXPECT_EQ(write(network), baseline); // apply -> revert restores byte-for-byte
}

TEST(SurfaceMaterial, SetSurfaceMaterialStaleIdIsInvalid) {
  RoadNetwork network;
  author_square(network);
  derive_surfaces(network);
  auto command = roadmaker::edit::set_surface_material(network, SurfaceId{}, "asphalt");
  ASSERT_NE(command, nullptr);
  EXPECT_FALSE(command->apply(network).has_value()); // a stale id fails apply
}

TEST(SurfaceMaterial, XodrRoundTripsSurfaceMaterial) {
  RoadNetwork network;
  author_square(network);
  derive_surfaces(network);
  auto command = roadmaker::edit::set_surface_material(network, the_surface(network), "concrete");
  ASSERT_TRUE(command->apply(network).has_value());

  const std::string first = write(network);
  auto reparsed = parse_xodr(first);
  ASSERT_TRUE(reparsed.has_value());
  const RoadNetwork& loaded = reparsed->network;
  ASSERT_EQ(loaded.surface_count(), 1U);
  const Surface* surface = loaded.surface(the_surface(loaded));
  ASSERT_NE(surface, nullptr);
  EXPECT_EQ(surface->material, "concrete");
  EXPECT_EQ(write(loaded), first); // stable across the round trip
}

TEST(SurfaceMaterial, DeriveSurfacesPreservesMaterialOnSurvivingRing) {
  RoadNetwork network;
  author_square(network);
  derive_surfaces(network);
  auto command = roadmaker::edit::set_surface_material(network, the_surface(network), "asphalt");
  ASSERT_TRUE(command->apply(network).has_value());

  // Re-deriving with no topology change keeps the surface id-stable AND its
  // material — a survivor's Surface object is left untouched.
  derive_surfaces(network);
  ASSERT_EQ(network.surface_count(), 1U);
  const Surface* surface = network.surface(the_surface(network));
  ASSERT_NE(surface, nullptr);
  EXPECT_EQ(surface->material, "asphalt");
}
