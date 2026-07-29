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

// The OSM tag mapping (p7-s4, #244) and its two gates.
//
// GATE 1 — the committed docs/domain/osm_mapping.md tables must be what the
// code table renders. The mapping is a POLICY: which real-world roads become
// which cross sections is a product decision a reviewer must be able to read,
// and a doc that has drifted from the code is worse than no doc.
//
// GATE 2 — every row of the code table must appear in the committed fixture.
// Without it the table can quietly grow past its own coverage: a new
// `highway=` row with no fixture way is a mapping nobody has ever executed.

#include "roadmaker/osm/graph.hpp"
#include "roadmaker/osm/tags.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

std::string committed_spec_doc() {
  const std::filesystem::path page =
      std::filesystem::path(RM_DOCS_DIR) / "domain" / "osm_mapping.md";
  std::ifstream file(page);
  EXPECT_TRUE(file.is_open()) << "missing " << page.string();
  std::stringstream buffer;
  buffer << file.rdbuf();
  return buffer.str();
}

std::filesystem::path fixture(std::string_view name) {
  return std::filesystem::path(RM_OSM_FIXTURES_DIR) / name;
}

} // namespace

// --- gate 1: doc <-> code ---------------------------------------------------

TEST(OsmMapping, DocHighwayTableMatchesTheCodeTable) {
  const std::string generated = roadmaker::osm::highway_mapping_markdown();
  EXPECT_TRUE(committed_spec_doc().find(generated) != std::string::npos)
      << "docs/domain/osm_mapping.md §1 is out of date with the code table in "
         "core/include/roadmaker/osm/tags.hpp.\nRegenerate the marked table from "
         "osm::highway_mapping_markdown():\n\n"
      << generated;
}

TEST(OsmMapping, DocTagTableMatchesTheCodeTable) {
  const std::string generated = roadmaker::osm::tag_mapping_markdown();
  EXPECT_TRUE(committed_spec_doc().find(generated) != std::string::npos)
      << "docs/domain/osm_mapping.md §2 is out of date with the code table.\n"
         "Regenerate the marked table from osm::tag_mapping_markdown():\n\n"
      << generated;
}

TEST(OsmMapping, TheDocListsEveryDroppedHighwayValue) {
  // Prose rather than a table, so it is checked by containment. A value the
  // code drops but the doc does not name is a refusal nobody can look up.
  const std::string doc = committed_spec_doc();
  for (const std::string_view value : roadmaker::osm::dropped_highway_values()) {
    EXPECT_NE(doc.find("`" + std::string(value) + "`"), std::string::npos)
        << value << " is dropped by the code table but not named in osm_mapping.md";
  }
}

TEST(OsmMapping, RenderersEmitTheirMarkers) {
  EXPECT_EQ(roadmaker::osm::highway_mapping_markdown().rfind("<!-- rm-osm: highway -->", 0), 0U);
  EXPECT_EQ(roadmaker::osm::tag_mapping_markdown().rfind("<!-- rm-osm: tags -->", 0), 0U);
}

// --- gate 2: code table <-> committed fixture -------------------------------

TEST(OsmMapping, EveryClassifiedHighwayValueAppearsInTheFixture) {
  const auto parsed =
      roadmaker::osm::load_osm(fixture("district.osm"), {.keep_non_highway_ways = true});
  ASSERT_TRUE(parsed.has_value()) << (parsed ? "" : parsed.error().message);

  const auto carries = [&parsed](std::string_view value) {
    return std::ranges::any_of(parsed->graph.ways, [value](const roadmaker::osm::OsmWay& way) {
      return way.tag("highway") == value;
    });
  };

  for (const auto& row : roadmaker::osm::highway_mappings()) {
    EXPECT_TRUE(carries(row.value))
        << row.value << " is in the mapping table but no fixture way carries it — "
        << "add one in scripts/gen_osm_fixtures.py and regenerate";
  }
  for (const std::string_view value : roadmaker::osm::dropped_highway_values()) {
    EXPECT_TRUE(carries(value)) << value << " is a dropped classification with no fixture way — "
                                << "the drop path for it has never been executed";
  }
}

TEST(OsmMapping, TheTableItselfIsWellFormed) {
  const auto rows = roadmaker::osm::highway_mappings();
  ASSERT_FALSE(rows.empty());

  // No value may be both classified and dropped, or the two answers disagree
  // and which one wins is an implementation accident.
  for (const auto& row : rows) {
    EXPECT_FALSE(roadmaker::osm::is_dropped_highway(row.value)) << row.value;
    EXPECT_NE(roadmaker::osm::highway_mapping(row.value), nullptr) << row.value;
  }
  // And no duplicates, or the lookup silently answers with the first.
  for (std::size_t i = 0; i < rows.size(); ++i) {
    for (std::size_t j = i + 1; j < rows.size(); ++j) {
      EXPECT_NE(rows[i].value, rows[j].value) << rows[i].value << " appears twice";
    }
  }
  EXPECT_EQ(roadmaker::osm::highway_mapping("no_such_highway_value"), nullptr);
}

TEST(OsmMapping, EveryLinkRampSharesItsParentsClass) {
  // A ramp is narrowed, never reclassified: an off-ramp from a motorway is
  // still motorway-grade geometry, and giving it a different class would
  // change its lane WIDTHS, which come from the class.
  using roadmaker::osm::highway_mapping;
  for (const auto& [ramp, parent] :
       std::initializer_list<std::pair<const char*, const char*>>{{"motorway_link", "motorway"},
                                                                  {"trunk_link", "trunk"},
                                                                  {"primary_link", "primary"},
                                                                  {"secondary_link", "secondary"},
                                                                  {"tertiary_link", "tertiary"}}) {
    const auto* ramp_row = highway_mapping(ramp);
    const auto* parent_row = highway_mapping(parent);
    ASSERT_NE(ramp_row, nullptr) << ramp;
    ASSERT_NE(parent_row, nullptr) << parent;
    EXPECT_TRUE(ramp_row->link) << ramp << " is a *_link but not flagged as one";
    EXPECT_EQ(ramp_row->road_class, parent_row->road_class) << ramp;
  }
}

// --- the individual tag parsers ---------------------------------------------

TEST(OsmTags, OneWayReadsOsmsThreeAffirmativeSpellings) {
  using roadmaker::osm::parse_oneway;
  for (const std::string_view yes : {"yes", "true", "1"}) {
    EXPECT_TRUE(parse_oneway(yes).one_way) << yes;
    EXPECT_FALSE(parse_oneway(yes).reversed) << yes;
  }
  for (const std::string_view no : {"no", "false", "0", ""}) {
    EXPECT_FALSE(parse_oneway(no).one_way) << no;
  }
  // -1 is one-way AND reversed: the polyline gets flipped so the reference
  // line runs with traffic (osm_mapping.md §2).
  EXPECT_TRUE(parse_oneway("-1").one_way);
  EXPECT_TRUE(parse_oneway("-1").reversed);
}

TEST(OsmTags, LaneCountRefusesAValueThatIsNotWhollyANumber) {
  using roadmaker::osm::parse_lane_count;
  EXPECT_EQ(parse_lane_count("4"), 4);
  EXPECT_EQ(parse_lane_count("1"), 1);
  // "1;2" and "2 lanes" are OSM's way of saying "it is complicated". Reading
  // the leading digit would invent a certainty the source declined to state.
  EXPECT_FALSE(parse_lane_count("1;2").has_value());
  EXPECT_FALSE(parse_lane_count("2 lanes").has_value());
  EXPECT_FALSE(parse_lane_count("").has_value());
  EXPECT_FALSE(parse_lane_count("0").has_value());
  EXPECT_FALSE(parse_lane_count("-2").has_value());
  // Clamped, so a typo cannot ask for a thousand-lane road.
  EXPECT_EQ(parse_lane_count("9999"), roadmaker::osm::kMaxLanesPerSide * 2);
}

TEST(OsmTags, MaxSpeedFollowsOsmsConventionAndCarriesTheAutobahnLiteral) {
  using roadmaker::osm::parse_maxspeed;

  // A bare number is km/h — OSM's convention, not a guess.
  const auto bare = parse_maxspeed("50");
  ASSERT_TRUE(bare.has_value());
  EXPECT_EQ(bare->max, "50");
  EXPECT_EQ(bare->unit, "km/h");

  const auto imperial = parse_maxspeed("30 mph");
  ASSERT_TRUE(imperial.has_value());
  EXPECT_EQ(imperial->max, "30");
  EXPECT_EQ(imperial->unit, "mph");

  // THE ONE THAT MATTERS. `none` is the German autobahn, and it becomes
  // t_maxSpeed's string literal — never a number, and never dropped.
  const auto autobahn = parse_maxspeed("none");
  ASSERT_TRUE(autobahn.has_value());
  EXPECT_EQ(autobahn->max, "no limit");
  EXPECT_TRUE(autobahn->unit.empty());

  // Everything else is refused rather than approximated: "walk" is a real
  // legal state, but it is not a number and inventing one would be a lie.
  for (const std::string_view unparseable : {"walk", "signals", "variable", "RO:urban", ""}) {
    EXPECT_FALSE(parse_maxspeed(unparseable).has_value()) << unparseable;
  }
}

TEST(OsmTags, LayerIsReadAsASignedIndexAndDefaultsToZero) {
  using roadmaker::osm::parse_layer;
  EXPECT_EQ(parse_layer("1"), 1);
  EXPECT_EQ(parse_layer("-2"), -2);
  EXPECT_EQ(parse_layer(""), 0);
  EXPECT_EQ(parse_layer("ground"), 0);
}
