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

// Phase 1 (WP1) — the Manifold spike, retiring the "first consumer, zero call
// sites" integration risk before any generator or UX work. Proves the whole
// handoff: a swept deck is a valid, positively-oriented manifold solid, and
// to_submesh turns it into a faceted, UV'd RoadMaker mesh, deterministically.

#include <manifold/manifold.h>

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <vector>

#include "../src/mesh/manifold_bridge.hpp"

namespace roadmaker::bridge {
namespace {

// A 12 m × 3 m deck rectangle, 0.8 m deep, in the section's (t, w) plane, wound
// CCW: bottom edge left→right, then up the right, across the top, down the left.
std::vector<SectionPoint> deck_section() {
  constexpr double kHalfWidth = 6.0;
  constexpr double kDepth = 0.8;
  return {{-kHalfWidth, -kDepth}, {kHalfWidth, -kDepth}, {kHalfWidth, 0.0}, {-kHalfWidth, 0.0}};
}

// A straight deck of `length` along +x at origin: u runs the span, t is +y, the
// deck top sits at z=0 with w the vertical offset below it.
manifold::Manifold straight_deck(double length, int n_div) {
  return sweep_section(deck_section(), n_div, [length](double t, double w, double u) {
    return std::array<double, 3>{u * length, t, w};
  });
}

TEST(ManifoldBridgeSpike, SweptDeckIsAPositiveVolumeManifold) {
  const manifold::Manifold deck = straight_deck(24.0, 8);
  EXPECT_EQ(deck.Status(), manifold::Manifold::Error::NoError);
  EXPECT_FALSE(deck.IsEmpty());
  EXPECT_GT(deck.NumTri(), 0u);
  // Positive volume ⇒ outward orientation (an inside-out solid reports negative
  // volume). A straight 24×12×0.8 deck is 230.4 m³.
  EXPECT_NEAR(deck.Volume(), 24.0 * 12.0 * 0.8, 1e-6);
  EXPECT_EQ(deck.Genus(), 0); // a solid slab, no handles
}

TEST(ManifoldBridgeSpike, ToSubmeshIsFacetedWithUnitNormalsAndPlanarUVs) {
  const SubMesh mesh = to_submesh(straight_deck(24.0, 8));
  ASSERT_FALSE(mesh.positions.empty());
  // Faceted: three unique vertices per triangle.
  EXPECT_EQ(mesh.positions.size() % 9, 0u);
  EXPECT_EQ(mesh.normals.size(), mesh.positions.size());
  const std::size_t vert_count = mesh.positions.size() / 3;
  EXPECT_EQ(mesh.uvs.size(), vert_count * 2);
  EXPECT_EQ(mesh.indices.size(), vert_count); // one index per emitted vertex
  // Every normal is unit length, and the planar UV is the world (x, y).
  for (std::size_t v = 0; v < vert_count; ++v) {
    const double nx = mesh.normals[(v * 3) + 0];
    const double ny = mesh.normals[(v * 3) + 1];
    const double nz = mesh.normals[(v * 3) + 2];
    EXPECT_NEAR(std::sqrt((nx * nx) + (ny * ny) + (nz * nz)), 1.0, 1e-9);
    EXPECT_DOUBLE_EQ(mesh.uvs[(v * 2) + 0], mesh.positions[(v * 3) + 0]);
    EXPECT_DOUBLE_EQ(mesh.uvs[(v * 2) + 1], mesh.positions[(v * 3) + 1]);
  }
}

TEST(ManifoldBridgeSpike, SweepIsDeterministic) {
  const SubMesh a = to_submesh(straight_deck(24.0, 8));
  const SubMesh b = to_submesh(straight_deck(24.0, 8));
  EXPECT_EQ(a.positions, b.positions);
  EXPECT_EQ(a.indices, b.indices);
  EXPECT_EQ(a.normals, b.normals);
}

TEST(ManifoldBridgeSpike, BoxIsAManifoldOfTheExpectedVolume) {
  const manifold::Manifold pier = box({10.0, 5.0, -3.0}, {1.2, 1.2, 6.0});
  EXPECT_EQ(pier.Status(), manifold::Manifold::Error::NoError);
  EXPECT_NEAR(pier.Volume(), 1.2 * 1.2 * 6.0, 1e-9);
  const manifold::Box bounds = pier.BoundingBox();
  EXPECT_NEAR(bounds.Center().z, -3.0, 1e-9); // centred where asked
}

TEST(ManifoldBridgeSpike, DeckAndPierUnionCleanly) {
  // The real reason Manifold is here: a deck unioned with a supporting pier must
  // stay a single watertight solid (Phase 4 does this for every pier/abutment).
  const manifold::Manifold deck = straight_deck(24.0, 8);
  const manifold::Manifold pier = box({12.0, 0.0, -2.0}, {1.2, 1.2, 4.0});
  const manifold::Manifold bridge = deck + pier;
  EXPECT_EQ(bridge.Status(), manifold::Manifold::Error::NoError);
  EXPECT_GT(bridge.Volume(), deck.Volume()); // the pier added material
  EXPECT_FALSE(to_submesh(bridge).positions.empty());
}

} // namespace
} // namespace roadmaker::bridge
