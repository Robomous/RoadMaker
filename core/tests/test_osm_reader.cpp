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

// The .osm XML reader (p7-s4, #244; format scope in ADR-0012).

#include "roadmaker/osm/graph.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace {

std::filesystem::path fixture(std::string_view name) {
  return std::filesystem::path(RM_OSM_FIXTURES_DIR) / name;
}

bool mentions(const std::vector<roadmaker::Diagnostic>& diagnostics, std::string_view text) {
  return std::ranges::any_of(
      diagnostics, [text](const auto& d) { return d.message.find(text) != std::string::npos; });
}

const roadmaker::osm::OsmWay* named(const roadmaker::osm::OsmGraph& graph, std::string_view name) {
  const auto found = std::ranges::find_if(
      graph.ways, [name](const roadmaker::osm::OsmWay& way) { return way.tag("name") == name; });
  return found == graph.ways.end() ? nullptr : &*found;
}

} // namespace

TEST(OsmReader, EveryFixtureIsPresent) {
  // The guard the GIS and lidar suites open with. A missing fixture must fail
  // as itself rather than as a puzzling assertion twenty tests later.
  for (const std::string_view name :
       {"district.osm", "topology.osm", "deviations.osm", "refused.osm.pbf"}) {
    EXPECT_TRUE(std::filesystem::exists(fixture(name)))
        << name << " is missing; run scripts/gen_osm_fixtures.py";
  }
}

TEST(OsmReader, ReadsNodesWaysAndTags) {
  const auto parsed = roadmaker::osm::load_osm(fixture("topology.osm"));
  ASSERT_TRUE(parsed.has_value()) << (parsed ? "" : parsed.error().message);

  EXPECT_EQ(parsed->graph.ways.size(), 4U);
  EXPECT_FALSE(parsed->graph.nodes.empty());
  EXPECT_EQ(parsed->graph.crs, "EPSG:4326");

  const auto* east_west = named(parsed->graph, "east-west");
  ASSERT_NE(east_west, nullptr);
  EXPECT_EQ(east_west->refs.size(), 3U);
  EXPECT_EQ(east_west->tag("highway"), "secondary");
  EXPECT_TRUE(east_west->has_tag("name"));
  EXPECT_FALSE(east_west->has_tag("maxspeed"));
  EXPECT_TRUE(east_west->tag("maxspeed").empty());
}

TEST(OsmReader, WaysShareTheirNodeIdsSoTopologyIsExact) {
  // The whole reason the planner never needs geometric crossing detection: OSM
  // states its own topology, and a shared node is an identity rather than a
  // proximity.
  const auto parsed = roadmaker::osm::load_osm(fixture("topology.osm"));
  ASSERT_TRUE(parsed.has_value());

  const auto* east_west = named(parsed->graph, "east-west");
  const auto* north_south = named(parsed->graph, "north-south");
  ASSERT_NE(east_west, nullptr);
  ASSERT_NE(north_south, nullptr);

  EXPECT_EQ(east_west->refs[1], north_south->refs[1])
      << "the two ways must literally share their middle node id";
}

TEST(OsmReader, BoundsComeFromTheFilesOwnDeclaration) {
  const auto parsed = roadmaker::osm::load_osm(fixture("topology.osm"));
  ASSERT_TRUE(parsed.has_value());

  const auto& bounds = parsed->graph.bounds;
  EXPECT_LT(bounds[0], bounds[2]);
  EXPECT_LT(bounds[1], bounds[3]);
  // Amsterdam, and deliberately NOT (0, 0) — a fixture at the origin hides
  // every false-easting and central-meridian mistake there is.
  EXPECT_NEAR(bounds[0], 4.8952, 0.01);
  EXPECT_NEAR(bounds[1], 52.3702, 0.01);
}

TEST(OsmReader, NonHighwayWaysAreSkippedAndCounted) {
  const auto skipped = roadmaker::osm::load_osm(fixture("district.osm"));
  ASSERT_TRUE(skipped.has_value());
  EXPECT_TRUE(mentions(skipped->diagnostics, "carry no 'highway' tag"));
  EXPECT_TRUE(std::ranges::none_of(skipped->graph.ways, [](const roadmaker::osm::OsmWay& way) {
    return way.tag("name") == "warehouse";
  }));

  // ...but they ARE readable when a caller asks, which is what lets the
  // mapping-coverage gate count what a district really contains.
  const auto kept =
      roadmaker::osm::load_osm(fixture("district.osm"), {.keep_non_highway_ways = true});
  ASSERT_TRUE(kept.has_value());
  EXPECT_GT(kept->graph.ways.size(), skipped->graph.ways.size());
  EXPECT_NE(named(kept->graph, "warehouse"), nullptr);
}

TEST(OsmReader, ARoundaboutRingIsReadAsAClosedWay) {
  const auto parsed =
      roadmaker::osm::load_osm(fixture("district.osm"), {.keep_non_highway_ways = true});
  ASSERT_TRUE(parsed.has_value());

  const auto ring = std::ranges::find_if(parsed->graph.ways, [](const roadmaker::osm::OsmWay& way) {
    return way.tag("junction") == "roundabout";
  });
  ASSERT_NE(ring, parsed->graph.ways.end());
  EXPECT_TRUE(ring->closed());
  EXPECT_FALSE(named(parsed->graph, "east-west") != nullptr &&
               named(parsed->graph, "east-west")->closed());
}

// --- refusals ---------------------------------------------------------------

TEST(OsmReader, RefusesPbfByNameAndCitesTheDependencyAndTheIssue) {
  const auto refused = roadmaker::osm::load_osm(fixture("refused.osm.pbf"));
  ASSERT_FALSE(refused.has_value());

  const std::string& message = refused.error().message;
  // The refusal must name the real reason. ADR-0012's whole point is that the
  // obstacle is zlib, not Protocol Buffers, and a message blaming protobuf
  // would send the next reader down the wrong path.
  EXPECT_NE(message.find("zlib"), std::string::npos) << message;
  EXPECT_NE(message.find("#494"), std::string::npos) << message;
  EXPECT_NE(message.find("ADR-0012"), std::string::npos) << message;
  // ...and it must offer the way out, since one command fixes it.
  EXPECT_NE(message.find("osmium"), std::string::npos) << message;
  // The sabotage half: a bare "unsupported format" would satisfy a weaker
  // assertion while telling the user nothing they can act on.
  EXPECT_EQ(message.find("unsupported format"), std::string::npos) << message;
}

TEST(OsmReader, PbfIsNotOfferedAsOpenable) {
  // A dialog filter that lists a file the reader refuses is worse than one
  // that does not list it, so the predicate and the reader must agree.
  EXPECT_TRUE(roadmaker::osm::is_osm_extension("district.osm"));
  EXPECT_TRUE(roadmaker::osm::is_osm_extension("DISTRICT.OSM"));
  EXPECT_TRUE(roadmaker::osm::is_osm_extension("district.osm.xml"));
  EXPECT_FALSE(roadmaker::osm::is_osm_extension("district.osm.pbf"));
  EXPECT_FALSE(roadmaker::osm::is_osm_extension("district.pbf"));
  EXPECT_FALSE(roadmaker::osm::is_osm_extension("district.o5m"));
  EXPECT_FALSE(roadmaker::osm::is_osm_extension("district.xodr"));
}

TEST(OsmReader, MalformedXmlIsRefusedWithoutThrowing) {
  const auto refused = roadmaker::osm::parse_osm("<osm><way><nd ref=", "broken.osm");
  ASSERT_FALSE(refused.has_value());
  EXPECT_EQ(refused.error().code, roadmaker::ErrorCode::MalformedXml);
}

TEST(OsmReader, ADocumentThatIsNotOsmIsRefused) {
  const auto refused =
      roadmaker::osm::parse_osm(R"(<?xml version="1.0"?><OpenDRIVE/>)", "confused.osm");
  ASSERT_FALSE(refused.has_value());
  EXPECT_NE(refused.error().message.find("<osm>"), std::string::npos);
}

TEST(OsmReader, ABudgetIsRefusedBeforeAnyCoordinateIsRead) {
  // The #243 discipline: a file too large to hold is too large to read and
  // then discard. Refusing names both numbers so the user can act.
  const auto refused =
      roadmaker::osm::load_osm(fixture("district.osm"), {.max_nodes = 4, .max_ways = 1000});
  ASSERT_FALSE(refused.has_value());
  EXPECT_NE(refused.error().message.find("over this build's 4 limit"), std::string::npos)
      << refused.error().message;

  const auto ways =
      roadmaker::osm::load_osm(fixture("district.osm"), {.max_nodes = 999999, .max_ways = 2});
  ASSERT_FALSE(ways.has_value());
  EXPECT_NE(ways.error().message.find("ways"), std::string::npos) << ways.error().message;
}

TEST(OsmReader, AMissingFileIsFileNotFoundNotAParseFailure) {
  const auto refused = roadmaker::osm::load_osm(fixture("no_such_district.osm"));
  ASSERT_FALSE(refused.has_value());
  EXPECT_EQ(refused.error().code, roadmaker::ErrorCode::FileNotFound);
}

// --- the things a real extract does that a hand-written one does not --------

TEST(OsmReader, ANodeReferenceOutsideTheExtractTrimsTheWayRatherThanDroppingIt) {
  constexpr std::string_view kCropped = R"(<?xml version="1.0"?>
<osm version="0.6">
  <node id="1" lat="52.37" lon="4.89"/>
  <node id="2" lat="52.371" lon="4.891"/>
  <way id="10">
    <nd ref="1"/><nd ref="2"/><nd ref="999"/>
    <tag k="highway" v="residential"/>
  </way>
</osm>)";
  const auto parsed = roadmaker::osm::parse_osm(kCropped, "cropped.osm");
  ASSERT_TRUE(parsed.has_value());

  // A way running off the edge of a cropped bounding box is normal. Shortening
  // it is right; dropping it would lose a road that is genuinely there.
  ASSERT_EQ(parsed->graph.ways.size(), 1U);
  EXPECT_EQ(parsed->graph.ways[0].refs.size(), 2U);
  EXPECT_TRUE(mentions(parsed->diagnostics, "fell outside the extract"));
}

TEST(OsmReader, ANodeWithAnUnreadableCoordinateIsSkippedNotPlacedAtZero) {
  // pugixml's as_double returns 0.0 for a malformed value, and (0, 0) is a
  // real place in the Gulf of Guinea. A node that silently lands there drags
  // its whole way across the planet.
  constexpr std::string_view kBroken = R"(<?xml version="1.0"?>
<osm version="0.6">
  <node id="1" lat="52.37" lon="4.89"/>
  <node id="2" lat="not-a-number" lon="4.891"/>
  <way id="10"><nd ref="1"/><nd ref="2"/><tag k="highway" v="residential"/></way>
</osm>)";
  const auto parsed = roadmaker::osm::parse_osm(kBroken, "broken_node.osm");
  ASSERT_TRUE(parsed.has_value());

  EXPECT_EQ(parsed->graph.nodes.size(), 1U);
  EXPECT_FALSE(parsed->graph.nodes.contains(2));
  EXPECT_TRUE(mentions(parsed->diagnostics, "no usable id/lat/lon"));
}

TEST(OsmReader, RelationsAreCountedAndReportedRatherThanIgnored) {
  constexpr std::string_view kWithRelations = R"(<?xml version="1.0"?>
<osm version="0.6">
  <node id="1" lat="52.37" lon="4.89"/>
  <node id="2" lat="52.371" lon="4.891"/>
  <way id="10"><nd ref="1"/><nd ref="2"/><tag k="highway" v="residential"/></way>
  <relation id="20"><tag k="type" v="restriction"/></relation>
  <relation id="21"><tag k="type" v="route"/></relation>
</osm>)";
  const auto parsed = roadmaker::osm::parse_osm(kWithRelations, "relations.osm");
  ASSERT_TRUE(parsed.has_value());

  EXPECT_EQ(parsed->graph.relation_count, 2U);
  EXPECT_EQ(parsed->graph.turn_restriction_count, 1U);
  EXPECT_TRUE(mentions(parsed->diagnostics, "relation(s) were not imported"));
  // Turn restrictions get their own line: someone took the trouble to map
  // them, and should be told directly rather than left to notice.
  EXPECT_TRUE(mentions(parsed->diagnostics, "turn restrictions"));
}

TEST(OsmReader, SourceCountsReportWhatTheFileHeldNotWhatSurvived) {
  const auto parsed = roadmaker::osm::load_osm(fixture("district.osm"));
  ASSERT_TRUE(parsed.has_value());

  // ways holds only what passed the filter; source_way_count says how much did
  // not, so "we imported 40 of 300 ways" is answerable.
  EXPECT_GT(parsed->graph.source_way_count, parsed->graph.ways.size());
  EXPECT_EQ(parsed->graph.source_node_count, parsed->graph.nodes.size());
}

TEST(OsmReader, AnExtractWithNoRoadsSaysSo) {
  constexpr std::string_view kEmpty = R"(<?xml version="1.0"?><osm version="0.6"/>)";
  const auto parsed = roadmaker::osm::parse_osm(kEmpty, "empty.osm");
  ASSERT_TRUE(parsed.has_value());
  EXPECT_TRUE(parsed->graph.empty());
  EXPECT_TRUE(mentions(parsed->diagnostics, "no road ways"));
}
