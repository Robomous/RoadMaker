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

// Authored surface boundaries (p5-s1, issue #231): the seed query, the two
// commands, the derived → authored detach and its reverse, the survival rules
// derive_surfaces owes an authored surface, the authored mesh, and the xodr
// round-trip in both directions.

#include "roadmaker/edit/operations.hpp"
#include "roadmaker/mesh/mesh_builder.hpp"
#include "roadmaker/mesh/surface_boundary.hpp"
#include "roadmaker/road/authoring.hpp"
#include "roadmaker/road/network.hpp"
#include "roadmaker/road/surface.hpp"
#include "roadmaker/road/surface_derivation.hpp"
#include "roadmaker/xodr/reader.hpp"
#include "roadmaker/xodr/writer.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <string>
#include <vector>

#include "support/network_compare.hpp"

using roadmaker::BoundarySource;
using roadmaker::derive_surfaces;
using roadmaker::LaneProfile;
using roadmaker::parse_xodr;
using roadmaker::RoadId;
using roadmaker::RoadNetwork;
using roadmaker::sample_surface_boundary;
using roadmaker::Surface;
using roadmaker::surface_boundary_nodes;
using roadmaker::surface_boundary_self_intersects;
using roadmaker::SurfaceId;
using roadmaker::SurfaceNode;
using roadmaker::Waypoint;
using roadmaker::write_xodr;
using roadmaker::edit::Command;
using roadmaker::edit::revert_surface_to_derived;
using roadmaker::edit::set_surface_boundary;
using roadmaker::test::expect_network_matches;
using roadmaker::test::snapshot_xodr;

namespace {

RoadId
segment(RoadNetwork& network, const char* odr_id, double x0, double y0, double x1, double y1) {
  const std::vector<Waypoint> waypoints{Waypoint{.x = x0, .y = y0}, Waypoint{.x = x1, .y = y1}};
  auto road = roadmaker::author_clothoid_road(
      network, waypoints, LaneProfile::two_lane_default(), "", odr_id);
  EXPECT_TRUE(road.has_value());
  return road.value_or(RoadId{});
}

/// A 60 m block of four roads — big enough that the enclosed hole survives the
/// footprint union with room for a harmonic interior.
void author_block(RoadNetwork& network) {
  segment(network, "a", 0.0, 0.0, 60.0, 0.0);
  segment(network, "b", 60.0, 0.0, 60.0, 60.0);
  segment(network, "c", 60.0, 60.0, 0.0, 60.0);
  segment(network, "d", 0.0, 60.0, 0.0, 0.0);
}

SurfaceId the_surface(const RoadNetwork& network) {
  SurfaceId id{};
  network.for_each_surface([&](SurfaceId sid, const Surface&) { id = sid; });
  return id;
}

std::string write(const RoadNetwork& network) {
  auto xml = write_xodr(network);
  EXPECT_TRUE(xml.has_value());
  return xml.value_or(std::string{});
}

/// The §8 command oracle: apply changes the doc, revert restores it
/// byte-identically, re-apply reproduces, final revert is pristine.
void expect_command_round_trip(RoadNetwork& network, Command& command) {
  const std::string before = snapshot_xodr(network);
  ASSERT_TRUE(command.apply(network).has_value());
  const std::string after = snapshot_xodr(network);
  EXPECT_NE(before, after);
  ASSERT_TRUE(command.revert(network).has_value());
  expect_network_matches(network, before);
  ASSERT_TRUE(command.apply(network).has_value());
  expect_network_matches(network, after);
  ASSERT_TRUE(command.revert(network).has_value());
  expect_network_matches(network, before);
}

/// A plain square loop with zero tangents — the simplest valid boundary.
std::vector<SurfaceNode> square(double half) {
  std::vector<SurfaceNode> nodes;
  for (const std::array<double, 2>& corner : {std::array<double, 2>{-half, -half},
                                              std::array<double, 2>{half, -half},
                                              std::array<double, 2>{half, half},
                                              std::array<double, 2>{-half, half}}) {
    nodes.push_back(SurfaceNode{.x = 30.0 + corner[0], .y = 30.0 + corner[1]});
  }
  return nodes;
}

} // namespace

// --- the seed query ----------------------------------------------------------

TEST(SurfaceBoundary, DerivedSurfaceSeedsAManageableNodeGraph) {
  RoadNetwork network;
  author_block(network);
  derive_surfaces(network);
  ASSERT_EQ(network.surface_count(), 1U);

  const std::vector<SurfaceNode> seed = surface_boundary_nodes(network, the_surface(network));
  ASSERT_GE(seed.size(), 3U);
  EXPECT_LE(seed.size(), roadmaker::kMaxSeedBoundaryNodes);
  // Nothing is stored: the surface is still derived, with no nodes of its own.
  EXPECT_EQ(network.surface(the_surface(network))->source, BoundarySource::Derived);
  EXPECT_TRUE(network.surface(the_surface(network))->nodes.empty());
  // The seed traces the enclosed block, not the roads' outer extent.
  for (const SurfaceNode& node : seed) {
    EXPECT_GT(node.x, -1.0);
    EXPECT_LT(node.x, 61.0);
    EXPECT_GT(node.y, -1.0);
    EXPECT_LT(node.y, 61.0);
  }
}

TEST(SurfaceBoundary, SeedIsEmptyForAStaleId) {
  RoadNetwork network;
  EXPECT_TRUE(surface_boundary_nodes(network, SurfaceId{}).empty());
}

TEST(SurfaceBoundary, AuthoredSurfaceReturnsItsOwnNodes) {
  RoadNetwork network;
  const std::vector<SurfaceNode> nodes = square(20.0);
  const SurfaceId id = network.create_surface(
      Surface{.source = BoundarySource::Authored, .bounding_roads = {}, .nodes = nodes});
  EXPECT_EQ(surface_boundary_nodes(network, id), nodes);
}

// --- the Hermite tessellation ------------------------------------------------

TEST(SurfaceBoundary, SampledLoopPassesThroughEveryNode) {
  const std::vector<SurfaceNode> nodes = square(20.0);
  const std::vector<std::array<double, 2>> ring = sample_surface_boundary(nodes);
  ASSERT_GE(ring.size(), nodes.size());
  for (const SurfaceNode& node : nodes) {
    bool found = false;
    for (const std::array<double, 2>& p : ring) {
      if (std::hypot(p[0] - node.x, p[1] - node.y) < 1e-9) {
        found = true;
        break;
      }
    }
    EXPECT_TRUE(found) << "node (" << node.x << ", " << node.y << ") is not on the sampled loop";
  }
}

TEST(SurfaceBoundary, FewerThanThreeNodesTessellateToNothing) {
  EXPECT_TRUE(sample_surface_boundary({}).empty());
  EXPECT_TRUE(sample_surface_boundary({SurfaceNode{.x = 0.0, .y = 0.0}}).empty());
  EXPECT_TRUE(
      sample_surface_boundary({SurfaceNode{.x = 0.0, .y = 0.0}, SurfaceNode{.x = 1.0, .y = 0.0}})
          .empty());
}

TEST(SurfaceBoundary, SelfIntersectionIsDetected) {
  EXPECT_FALSE(surface_boundary_self_intersects(square(20.0)));
  // Swap two opposite corners to make a bow tie.
  std::vector<SurfaceNode> bowtie = square(20.0);
  std::swap(bowtie[2], bowtie[3]);
  EXPECT_TRUE(surface_boundary_self_intersects(bowtie));
}

// --- the commands ------------------------------------------------------------

TEST(SurfaceBoundary, EditingADerivedBoundaryDetachesItToAuthored) {
  RoadNetwork network;
  author_block(network);
  derive_surfaces(network);
  const SurfaceId id = the_surface(network);

  std::vector<SurfaceNode> nodes = surface_boundary_nodes(network, id);
  ASSERT_GE(nodes.size(), 3U);
  nodes.front().x += 3.0;

  auto command = set_surface_boundary(network, id, nodes);
  ASSERT_TRUE(command->apply(network).has_value());

  const Surface* surface = network.surface(id);
  ASSERT_NE(surface, nullptr);
  EXPECT_EQ(surface->source, BoundarySource::Authored);
  EXPECT_EQ(surface->nodes, nodes);
  // Provenance survives the detach — it is what the elevation comes from.
  EXPECT_EQ(surface->bounding_roads.size(), 4U);
}

TEST(SurfaceBoundary, SetBoundaryApplyRevertIsByteIdentical) {
  RoadNetwork network;
  author_block(network);
  derive_surfaces(network);
  const SurfaceId id = the_surface(network);

  std::vector<SurfaceNode> nodes = surface_boundary_nodes(network, id);
  ASSERT_GE(nodes.size(), 3U);
  nodes.front().x += 3.0;

  auto command = set_surface_boundary(network, id, nodes);
  expect_command_round_trip(network, *command);
  // Revert restored the SOURCE too, not just the node vector.
  EXPECT_EQ(network.surface(id)->source, BoundarySource::Derived);
}

TEST(SurfaceBoundary, SetBoundaryRejectsBadInput) {
  RoadNetwork network;
  author_block(network);
  derive_surfaces(network);
  const SurfaceId id = the_surface(network);

  // Too few nodes.
  EXPECT_FALSE(set_surface_boundary(network, id, {SurfaceNode{}, SurfaceNode{}})
                   ->apply(network)
                   .has_value());
  // Self-intersecting.
  std::vector<SurfaceNode> bowtie = square(20.0);
  std::swap(bowtie[2], bowtie[3]);
  EXPECT_FALSE(set_surface_boundary(network, id, bowtie)->apply(network).has_value());
  // Stale id.
  EXPECT_FALSE(
      set_surface_boundary(network, SurfaceId{}, square(20.0))->apply(network).has_value());
  // Nothing above touched the surface.
  EXPECT_EQ(network.surface(id)->source, BoundarySource::Derived);
}

TEST(SurfaceBoundary, SetBoundaryRejectsANoOpOnAnAuthoredSurface) {
  RoadNetwork network;
  const std::vector<SurfaceNode> nodes = square(20.0);
  const SurfaceId id =
      network.create_surface(Surface{.source = BoundarySource::Authored, .nodes = nodes});
  EXPECT_FALSE(set_surface_boundary(network, id, nodes)->apply(network).has_value());
}

TEST(SurfaceBoundary, RevertToDerivedClearsTheNodesAndRoundTrips) {
  RoadNetwork network;
  author_block(network);
  derive_surfaces(network);
  const SurfaceId id = the_surface(network);

  std::vector<SurfaceNode> nodes = surface_boundary_nodes(network, id);
  nodes.front().x += 3.0;
  ASSERT_TRUE(set_surface_boundary(network, id, nodes)->apply(network).has_value());

  auto command = revert_surface_to_derived(network, id);
  expect_command_round_trip(network, *command);

  ASSERT_TRUE(command->apply(network).has_value());
  EXPECT_EQ(network.surface(id)->source, BoundarySource::Derived);
  EXPECT_TRUE(network.surface(id)->nodes.empty());
}

TEST(SurfaceBoundary, RevertRejectsAnAlreadyDerivedSurface) {
  RoadNetwork network;
  author_block(network);
  derive_surfaces(network);
  EXPECT_FALSE(
      revert_surface_to_derived(network, the_surface(network))->apply(network).has_value());
}

// --- derive_surfaces and authored surfaces ----------------------------------

TEST(SurfaceBoundary, DeriveDoesNotDuplicateAnAuthoredSurfacesFace) {
  RoadNetwork network;
  author_block(network);
  derive_surfaces(network);
  const SurfaceId id = the_surface(network);

  std::vector<SurfaceNode> nodes = surface_boundary_nodes(network, id);
  nodes.front().x += 3.0;
  ASSERT_TRUE(set_surface_boundary(network, id, nodes)->apply(network).has_value());

  // The provenance ring still claims the face: re-deriving must neither erase
  // the authored surface nor lay a second derived one on top of it.
  derive_surfaces(network);
  EXPECT_EQ(network.surface_count(), 1U);
  ASSERT_NE(network.surface(id), nullptr);
  EXPECT_EQ(network.surface(id)->source, BoundarySource::Authored);
}

TEST(SurfaceBoundary, RinglessAuthoredSurfaceSurvivesDerivation) {
  RoadNetwork network;
  const SurfaceId id = network.create_surface(
      Surface{.source = BoundarySource::Authored, .bounding_roads = {}, .nodes = square(20.0)});

  derive_surfaces(network); // no roads at all: the early-out path
  ASSERT_NE(network.surface(id), nullptr);

  author_block(network);
  derive_surfaces(network); // and the general path, which now finds a face
  EXPECT_NE(network.surface(id), nullptr);
  EXPECT_EQ(network.surface_count(), 2U) << "the block's own derived surface joins it";
}

TEST(SurfaceBoundary, RevertedSurfaceIsReclaimedByDerivation) {
  RoadNetwork network;
  author_block(network);
  derive_surfaces(network);
  const SurfaceId id = the_surface(network);

  std::vector<SurfaceNode> nodes = surface_boundary_nodes(network, id);
  nodes.front().x += 3.0;
  ASSERT_TRUE(set_surface_boundary(network, id, nodes)->apply(network).has_value());
  ASSERT_TRUE(revert_surface_to_derived(network, id)->apply(network).has_value());

  derive_surfaces(network);
  EXPECT_EQ(network.surface_count(), 1U);
  ASSERT_NE(network.surface(id), nullptr) << "the ring still matches, so the id survives";
  EXPECT_EQ(network.surface(id)->source, BoundarySource::Derived);
}

// --- the authored mesh -------------------------------------------------------

TEST(SurfaceBoundary, AuthoredBoundaryMeshesAndFollowsTheNodes) {
  RoadNetwork network;
  author_block(network);
  derive_surfaces(network);
  const SurfaceId id = the_surface(network);
  ASSERT_TRUE(set_surface_boundary(network, id, square(15.0))->apply(network).has_value());

  const roadmaker::NetworkMesh mesh = roadmaker::build_network_mesh(network);
  ASSERT_EQ(mesh.surfaces.size(), 1U);
  const roadmaker::SubMesh& fill = mesh.surfaces.front().mesh;
  ASSERT_FALSE(fill.indices.empty());

  // Every vertex lies inside the authored 30x30 square (plus the weld slack),
  // so the mesh followed the nodes rather than the roads' enclosed hole.
  for (std::size_t i = 0; i + 2 < fill.positions.size(); i += 3) {
    EXPECT_GE(fill.positions[i], 15.0 - 0.1);
    EXPECT_LE(fill.positions[i], 45.0 + 0.1);
    EXPECT_GE(fill.positions[i + 1], 15.0 - 0.1);
    EXPECT_LE(fill.positions[i + 1], 45.0 + 0.1);
  }
}

TEST(SurfaceBoundary, AuthoredMeshIsWatertight) {
  RoadNetwork network;
  author_block(network);
  derive_surfaces(network);
  const SurfaceId id = the_surface(network);
  ASSERT_TRUE(set_surface_boundary(network, id, square(15.0))->apply(network).has_value());

  const roadmaker::NetworkMesh mesh = roadmaker::build_network_mesh(network);
  ASSERT_EQ(mesh.surfaces.size(), 1U);
  const roadmaker::SubMesh& fill = mesh.surfaces.front().mesh;

  // Interior edges are shared by exactly two triangles; the seam is the only
  // boundary, and it is a single closed loop (edge count == vertex count on it).
  std::map<std::pair<std::uint32_t, std::uint32_t>, int> edges;
  for (std::size_t i = 0; i + 2 < fill.indices.size(); i += 3) {
    const std::array<std::uint32_t, 3> tri{
        fill.indices[i], fill.indices[i + 1], fill.indices[i + 2]};
    for (int k = 0; k < 3; ++k) {
      const std::uint32_t a = tri[static_cast<std::size_t>(k)];
      const std::uint32_t b = tri[static_cast<std::size_t>((k + 1) % 3)];
      ++edges[a < b ? std::pair{a, b} : std::pair{b, a}];
    }
  }
  std::size_t boundary_edges = 0;
  std::map<std::uint32_t, int> boundary_degree;
  for (const auto& [edge, count] : edges) {
    EXPECT_LE(count, 2) << "a non-manifold edge is shared by more than two triangles";
    if (count == 1) {
      ++boundary_edges;
      ++boundary_degree[edge.first];
      ++boundary_degree[edge.second];
    }
  }
  EXPECT_GT(boundary_edges, 0U);
  for (const auto& [vertex, degree] : boundary_degree) {
    EXPECT_EQ(degree, 2) << "boundary vertex " << vertex << " is not on a simple closed loop";
  }
}

// --- persistence -------------------------------------------------------------

TEST(SurfaceBoundary, AuthoredSurfaceRoundTripsByteIdentical) {
  RoadNetwork network;
  author_block(network);
  derive_surfaces(network);
  const SurfaceId id = the_surface(network);
  ASSERT_TRUE(set_surface_boundary(network, id, square(15.0))->apply(network).has_value());

  const std::string first = write(network);
  EXPECT_NE(first.find("nodes="), std::string::npos);

  auto reparsed = parse_xodr(first);
  ASSERT_TRUE(reparsed.has_value());
  EXPECT_EQ(write(reparsed->network), first);

  ASSERT_EQ(reparsed->network.surface_count(), 1U);
  const Surface* loaded = reparsed->network.surface(the_surface(reparsed->network));
  ASSERT_NE(loaded, nullptr);
  EXPECT_EQ(loaded->source, BoundarySource::Authored);
  EXPECT_EQ(loaded->nodes, square(15.0));
  EXPECT_EQ(loaded->bounding_roads.size(), 4U) << "provenance is preserved";
}

TEST(SurfaceBoundary, DerivedSurfaceStillWritesNoNodesAttribute) {
  RoadNetwork network;
  author_block(network);
  derive_surfaces(network);
  const std::string xml = write(network);
  EXPECT_NE(xml.find("rm:surface"), std::string::npos);
  EXPECT_EQ(xml.find("nodes="), std::string::npos)
      << "a derived surface must write exactly what it wrote before p5-s1";
}

TEST(SurfaceBoundary, RinglessAuthoredSurfaceRoundTrips) {
  RoadNetwork network;
  network.create_surface(
      Surface{.source = BoundarySource::Authored, .bounding_roads = {}, .nodes = square(20.0)});

  const std::string first = write(network);
  auto reparsed = parse_xodr(first);
  ASSERT_TRUE(reparsed.has_value());
  ASSERT_EQ(reparsed->network.surface_count(), 1U)
      << "an authored surface is written even without a provenance ring";
  EXPECT_EQ(write(reparsed->network), first);
  EXPECT_EQ(reparsed->network.surface(the_surface(reparsed->network))->nodes, square(20.0));
}

TEST(SurfaceBoundary, MalformedNodesAreWarnedNotSilentlyDropped) {
  RoadNetwork network;
  author_block(network);
  derive_surfaces(network);
  std::string xml = write(network);
  // Splice a broken nodes attribute onto the surface marker.
  const std::size_t marker = xml.find("code=\"rm:surface\"");
  ASSERT_NE(marker, std::string::npos);
  xml.insert(marker + std::string("code=\"rm:surface\"").size(), " nodes=\"1,2,3\"");

  auto reparsed = parse_xodr(xml);
  ASSERT_TRUE(reparsed.has_value());
  EXPECT_EQ(reparsed->network.surface_count(), 0U);
  bool warned = false;
  for (const roadmaker::Diagnostic& diagnostic : reparsed->diagnostics) {
    if (diagnostic.message.find("rm:surface nodes") != std::string::npos) {
      warned = true;
    }
  }
  EXPECT_TRUE(warned) << "the reader must never drop input silently";
}
