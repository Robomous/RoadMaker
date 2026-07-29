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

// The OSM network plan and the single-undo import command (p7-s4, #244).
//
// The assertions here lean on CONSERVATION LAWS rather than on spot checks,
// because the failure this suite most needs to catch is not a wrong road — it
// is an import that quietly produced nothing. A classifier regression that
// returned nullopt for every way would leave a suite full of happily passing
// "this was dropped" diagnostics.

#include "roadmaker/edit/edit_stack.hpp"
#include "roadmaker/gis/crs.hpp"
#include "roadmaker/osm/graph.hpp"
#include "roadmaker/osm/import.hpp"
#include "roadmaker/osm/network_plan.hpp"
#include "roadmaker/road/georeference.hpp"
#include "roadmaker/road/network.hpp"
#include "roadmaker/xodr/rules.hpp"
#include "roadmaker/xodr/writer.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <iterator>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using roadmaker::RoadNetwork;
using roadmaker::osm::JointKind;
using roadmaker::osm::NetworkPlan;
using roadmaker::osm::PlannedRoad;

namespace {

std::filesystem::path fixture(std::string_view name) {
  return std::filesystem::path(RM_OSM_FIXTURES_DIR) / name;
}

/// A network georeferenced near the fixtures (Amsterdam) but with its origin
/// deliberately a few kilometres away, so nothing under test sits at (0, 0)
/// where a false-easting or sign error would be invisible.
RoadNetwork amsterdam_network() {
  RoadNetwork network;
  roadmaker::GeoReference geo;
  auto projection = roadmaker::tmerc_projection(52.4000, 4.9500);
  EXPECT_TRUE(projection.has_value());
  geo.projection = projection.value_or(std::string{});
  network.set_georeference(geo);
  return network;
}

roadmaker::gis::CrsTransform transform_for(const RoadNetwork& network) {
  const auto made = roadmaker::gis::crs_transform(
      roadmaker::gis::parse_crs("EPSG:4326"), roadmaker::gis::scene_crs(network.georeference()));
  EXPECT_TRUE(made.has_value());
  return made.value_or(roadmaker::gis::CrsTransform{});
}

bool has_rule(const std::vector<roadmaker::Diagnostic>& diagnostics, std::string_view rule) {
  return std::ranges::any_of(diagnostics, [rule](const auto& d) { return d.rule_id == rule; });
}

std::size_t count_rule(const std::vector<roadmaker::Diagnostic>& diagnostics,
                       std::string_view rule) {
  return static_cast<std::size_t>(
      std::ranges::count_if(diagnostics, [rule](const auto& d) { return d.rule_id == rule; }));
}

bool mentions_location(const std::vector<roadmaker::Diagnostic>& diagnostics,
                       std::string_view text) {
  return std::ranges::any_of(
      diagnostics, [text](const auto& d) { return d.location.find(text) != std::string::npos; });
}

roadmaker::osm::NetworkPlanResult plan_fixture(const RoadNetwork& network,
                                               std::string_view name,
                                               roadmaker::osm::OsmBuildOptions options = {}) {
  const auto parsed = roadmaker::osm::load_osm(fixture(name));
  EXPECT_TRUE(parsed.has_value());
  auto planned = roadmaker::osm::plan_network(parsed->graph, transform_for(network), options);
  EXPECT_TRUE(planned.has_value());
  return planned.value_or(roadmaker::osm::NetworkPlanResult{});
}

} // namespace

// --- conservation: the anti-vacuity backbone --------------------------------

TEST(OsmPlan, EveryWayIsEitherPlannedOrCounted) {
  const RoadNetwork network = amsterdam_network();
  const auto parsed = roadmaker::osm::load_osm(fixture("district.osm"));
  ASSERT_TRUE(parsed.has_value());

  const auto planned = roadmaker::osm::plan_network(parsed->graph, transform_for(network), {});
  ASSERT_TRUE(planned.has_value());

  // The district's ways split at no shared nodes (each sits on its own row),
  // so planned roads and dropped ways must account for every way exactly once.
  // A classifier that returned nullopt for everything fails HERE, where every
  // per-case assertion below would still pass.
  EXPECT_EQ(planned->plan.roads.size() + planned->plan.dropped_ways, parsed->graph.ways.size());
  EXPECT_GT(planned->plan.roads.size(), 0U) << "nothing was imported at all";
  EXPECT_GT(planned->plan.dropped_ways, 0U) << "nothing was dropped at all";
}

TEST(OsmPlan, EveryDroppedWayIsNamedByADiagnostic) {
  const RoadNetwork network = amsterdam_network();
  const auto result = plan_fixture(network, "district.osm");

  // One named diagnostic per dropped way — the district is far under the
  // aggregation cap, so nothing here is summarised away.
  EXPECT_GE(count_rule(result.diagnostics, roadmaker::rules::kOsmElementDropped),
            result.plan.dropped_ways);
  EXPECT_TRUE(mentions_location(result.diagnostics, "way/"));
}

TEST(OsmPlan, ALosslessWayProducesNoDiagnosticAtAll) {
  // THE INVERSE, and the one people forget. "Nothing is dropped silently" is
  // half a contract: a build that warns about every way is indistinguishable
  // from one that warns about none, and only this direction catches it.
  //
  // THREE collinear nodes, not two, and that is the whole strength of this
  // test. A two-node way cannot be simplified at all, so it passes on any
  // definition of "lossless" — including the one this originally had, which
  // compared NODE COUNTS and therefore called every straight way with a
  // redundant midpoint compromised. Running python/examples/osm_import.py is
  // what surfaced it: every road in the fixture district reported itself
  // compromised at a deviation of 0.000 m.
  //
  // The middle node here is exactly on the line between the other two, so the
  // simplifier drops it and the road's SHAPE is unchanged. That is lossless,
  // whatever the vertex count says.
  const RoadNetwork network = amsterdam_network();
  constexpr std::string_view kStraight = R"(<?xml version="1.0"?>
<osm version="0.6">
  <node id="1" lat="52.3700" lon="4.8900"/>
  <node id="3" lat="52.3700" lon="4.8915"/>
  <node id="2" lat="52.3700" lon="4.8930"/>
  <way id="10">
    <nd ref="1"/><nd ref="3"/><nd ref="2"/>
    <tag k="highway" v="residential"/>
  </way>
</osm>)";
  const auto parsed = roadmaker::osm::parse_osm(kStraight, "straight.osm");
  ASSERT_TRUE(parsed.has_value());

  const auto planned = roadmaker::osm::plan_network(parsed->graph, transform_for(network), {});
  ASSERT_TRUE(planned.has_value());
  ASSERT_EQ(planned->plan.roads.size(), 1U);

  // The midpoint really was dropped...
  EXPECT_EQ(planned->plan.roads[0].compromise.source_nodes, 3U);
  EXPECT_EQ(planned->plan.roads[0].compromise.kept_nodes, 2U);
  // ...and the shape did not meaningfully move. NOT zero: a constant-latitude
  // way is a curve once projected, so its own midpoint sits about a millimetre
  // off the chord. That is why the threshold is road-scale rather than
  // geometric — see kLosslessDeviationM.
  EXPECT_GT(planned->plan.roads[0].compromise.max_deviation_m, 0.0);
  EXPECT_LE(planned->plan.roads[0].compromise.max_deviation_m, roadmaker::osm::kLosslessDeviationM);
  EXPECT_TRUE(planned->plan.roads[0].compromise.lossless());
  EXPECT_FALSE(has_rule(planned->diagnostics, roadmaker::rules::kOsmFitApproximated))
      << "a straight way whose shape did not change was warned about";
}

TEST(OsmPlan, ACompromisedRoadIsNamedWithItsMEASUREDNumbers) {
  const RoadNetwork network = amsterdam_network();
  const auto result = plan_fixture(network, "deviations.osm");
  ASSERT_EQ(result.plan.roads.size(), 1U);

  const auto& compromise = result.plan.roads[0].compromise;
  EXPECT_EQ(compromise.source_nodes, 4U);
  EXPECT_LT(compromise.kept_nodes, compromise.source_nodes);
  EXPECT_FALSE(compromise.lossless());

  // The ACTUAL deviation, not the tolerance. The fixture's vertices sit at
  // 0.4999 m and 0.5001 m from the chord, so the 0.5001 one is retained and
  // the measured deviation is the 0.4999 one — a number that could only come
  // from measuring.
  EXPECT_GT(compromise.max_deviation_m, 0.0);
  EXPECT_LE(compromise.max_deviation_m, compromise.tolerance_used_m)
      << "RDP's own construction guarantee";
  EXPECT_NEAR(compromise.max_deviation_m, 0.4999, 0.05);

  ASSERT_TRUE(has_rule(result.diagnostics, roadmaker::rules::kOsmFitApproximated));
  const auto found = std::ranges::find_if(result.diagnostics, [](const auto& d) {
    return d.rule_id == roadmaker::rules::kOsmFitApproximated;
  });
  // The message must carry the numbers, not merely the fact.
  EXPECT_NE(found->message.find("greatest deviation"), std::string::npos) << found->message;
  EXPECT_NE(found->location.find("way/"), std::string::npos) << found->location;
}

// --- topology ---------------------------------------------------------------

TEST(OsmPlan, WaysAreSplitAtTheirSharedNodes) {
  const RoadNetwork network = amsterdam_network();
  const auto result = plan_fixture(network, "topology.osm");

  // east-west and north-south each split at their shared centre node (2 + 2);
  // the continuation ends there rather than passing through, so it does not
  // split (1).
  //
  // THE OVERPASS DOES NOT SPLIT EITHER, and that is the layer rule showing
  // its work: it passes through `north`, which north-south also touches — but
  // degree is counted per (node, LAYER), so at layer 1 that node is not shared
  // with anything and the bridge stays one road (1). 2 + 2 + 1 + 1 = 6.
  EXPECT_EQ(result.plan.roads.size(), 6U);

  const auto segments_of = [&result](roadmaker::osm::OsmId way) {
    return std::ranges::count(result.plan.roads, way, &PlannedRoad::way);
  };
  EXPECT_GE(segments_of(result.plan.roads.front().way), 1);

  // Every planned road's odr_id carries its provenance, and they are unique.
  std::vector<std::string> ids;
  for (const PlannedRoad& road : result.plan.roads) {
    EXPECT_EQ(road.odr_id.rfind("osm.", 0), 0U) << road.odr_id;
    ids.push_back(road.odr_id);
  }
  std::ranges::sort(ids);
  EXPECT_EQ(std::ranges::unique(ids).begin(), ids.end()) << "duplicate odr_id";
}

TEST(OsmPlan, DegreeDecidesBetweenALinkAndAJunction) {
  const RoadNetwork network = amsterdam_network();
  const auto result = plan_fixture(network, "topology.osm");

  std::size_t links = 0;
  std::size_t junctions = 0;
  for (const auto& joint : result.plan.joints) {
    (joint.kind == JointKind::Link ? links : junctions) += 1;
    EXPECT_GE(joint.ends.size(), 2U);
    if (joint.kind == JointKind::Link) {
      EXPECT_EQ(joint.ends.size(), 2U);
    } else {
      EXPECT_GE(joint.ends.size(), 3U);
    }
  }
  // The centre node has four ends (a junction); the east arm meets the
  // continuation end-to-end (a link).
  EXPECT_GE(junctions, 1U);
  EXPECT_GE(links, 1U);
}

TEST(OsmPlan, AnOverpassIsNotWeldedToTheRoadBeneathIt) {
  // THE CORRECTNESS RULE. The overpass passes through a node north-south also
  // touches, but carries layer=1. Joining them is invisible in plan view and
  // wrong in every 3D consumer downstream, which is exactly the kind of defect
  // that ships.
  const RoadNetwork network = amsterdam_network();
  const auto result = plan_fixture(network, "topology.osm");

  const auto index_of = [&result](std::string_view name) {
    const auto found = std::ranges::find(result.plan.roads, name, &PlannedRoad::name);
    return static_cast<std::size_t>(std::distance(result.plan.roads.begin(), found));
  };
  const std::size_t overpass = index_of("overpass");
  ASSERT_LT(overpass, result.plan.roads.size());
  EXPECT_EQ(result.plan.roads[overpass].layer, 1);

  // No joint may mix layers...
  for (const auto& joint : result.plan.joints) {
    for (const auto& [index, contact] : joint.ends) {
      EXPECT_EQ(result.plan.roads[index].layer, joint.layer)
          << "joint at node " << joint.node << " mixes layers";
    }
  }

  // ...and specifically, the overpass must share no joint with any road on
  // the ground. Asserted over the joint set rather than over a diagnostic,
  // because the ABSENCE of a weld is the property that matters: a diagnostic
  // could be emitted by a build that welded them anyway.
  for (const auto& joint : result.plan.joints) {
    bool has_overpass = false;
    bool has_ground = false;
    for (const auto& [index, contact] : joint.ends) {
      (index == overpass ? has_overpass : has_ground) = true;
      if (index != overpass && result.plan.roads[index].layer != 0) {
        has_ground = false;
      }
    }
    EXPECT_FALSE(has_overpass && has_ground) << "the overpass was joined to a road at grade";
  }

  // The at-grade compromise is still REPORTED — the vertical separation is not
  // authored (#496), and silence about that would be the dishonest half.
  EXPECT_TRUE(has_rule(result.diagnostics, roadmaker::rules::kOsmAtGrade));
}

TEST(OsmPlan, AnArmCountPastTheBudgetIsRefusedAndNamed) {
  const RoadNetwork network = amsterdam_network();
  roadmaker::osm::OsmBuildOptions options;
  options.max_junction_arms = 2; // forces the 4-arm centre node over budget
  const auto result = plan_fixture(network, "topology.osm", options);

  EXPECT_TRUE(has_rule(result.diagnostics, roadmaker::rules::kOsmTopologyUnlinked));
  for (const auto& joint : result.plan.joints) {
    EXPECT_LE(joint.ends.size(), 2U);
  }
}

// --- tags reaching the plan -------------------------------------------------

TEST(OsmPlan, MaxspeedBecomesASpeedAndAnAbsentOneStaysAbsent) {
  const RoadNetwork network = amsterdam_network();
  const auto result = plan_fixture(network, "district.osm");

  const auto by_name = [&result](std::string_view name) -> const PlannedRoad* {
    const auto found = std::ranges::find(result.plan.roads, name, &PlannedRoad::name);
    return found == result.plan.roads.end() ? nullptr : &*found;
  };

  const PlannedRoad* metric = by_name("bare-kmh");
  ASSERT_NE(metric, nullptr);
  ASSERT_TRUE(metric->type.has_value());
  ASSERT_TRUE(metric->type->speed.has_value());
  EXPECT_EQ(metric->type->speed->max_str, "50");
  EXPECT_EQ(metric->type->speed->unit, "km/h");

  const PlannedRoad* autobahn = by_name("autobahn");
  ASSERT_NE(autobahn, nullptr);
  ASSERT_TRUE(autobahn->type->speed.has_value());
  EXPECT_EQ(autobahn->type->speed->max_str, "no limit");

  // A way whose source records NO usable limit gets a type and no <speed>.
  // Inventing one would be guessing where silence is the honest answer.
  const PlannedRoad* unparseable = by_name("unparseable");
  ASSERT_NE(unparseable, nullptr);
  ASSERT_TRUE(unparseable->type.has_value());
  EXPECT_FALSE(unparseable->type->speed.has_value());
  EXPECT_EQ(unparseable->type->type, "townLocal");
}

TEST(OsmPlan, EveryPlannedRoadNamesAStandardRoadType) {
  const RoadNetwork network = amsterdam_network();
  const auto result = plan_fixture(network, "district.osm");
  ASSERT_FALSE(result.plan.roads.empty());
  for (const PlannedRoad& road : result.plan.roads) {
    ASSERT_TRUE(road.type.has_value()) << road.odr_id;
    EXPECT_TRUE(roadmaker::is_known_road_type(road.type->type)) << road.type->type;
  }
}

TEST(OsmPlan, EveryLaneWidthComesFromTheDefaultsRegistry) {
  // The mapping may choose a class and a lane COUNT; it may not invent a
  // width. A literal here would also fail test_defaults_registry, but this
  // asserts it at the point the OSM path actually produces a profile.
  const RoadNetwork network = amsterdam_network();
  const auto result = plan_fixture(network, "district.osm");

  const auto registry_width = [](roadmaker::LaneType type, double width) {
    if (type == roadmaker::LaneType::Driving) {
      using roadmaker::defaults::RoadClass;
      for (const RoadClass road_class :
           {RoadClass::Freeway, RoadClass::Arterial, RoadClass::Collector, RoadClass::Local}) {
        if (width == roadmaker::defaults::driving_lane_width(road_class)) {
          return true;
        }
      }
      return false;
    }
    if (type == roadmaker::LaneType::Shoulder) {
      // A freeway's shoulders are class-specific and wider than the shared
      // arterial/collector value — three registry entries, not one.
      return width == roadmaker::defaults::kShoulderWidth ||
             width == roadmaker::defaults::kFreewayLeftShoulderWidth ||
             width == roadmaker::defaults::kFreewayRightShoulderWidth;
    }
    return width == roadmaker::defaults::lane_width(type);
  };

  for (const PlannedRoad& road : result.plan.roads) {
    for (const auto& side : {road.profile.left, road.profile.right}) {
      for (const roadmaker::LaneSpec& lane : side) {
        EXPECT_TRUE(registry_width(lane.type, lane.width))
            << road.odr_id << " lane width " << lane.width << " is not a registry value";
      }
    }
  }
}

// --- the command ------------------------------------------------------------

TEST(OsmImport, TheWholeDistrictIsOneUndoUnitAndUndoRestoresTheFileExactly) {
  RoadNetwork network = amsterdam_network();
  const auto before = roadmaker::write_xodr(network, "before");
  ASSERT_TRUE(before.has_value());

  auto import = roadmaker::osm::prepare_osm_import(network, fixture("topology.osm"));
  ASSERT_TRUE(import.has_value()) << (import ? "" : import.error().message);
  ASSERT_NE(import->command, nullptr);

  roadmaker::edit::EditStack stack;
  const auto pushed = stack.push(network, std::move(import->command));
  ASSERT_TRUE(pushed.has_value()) << (pushed ? "" : pushed.error().message);

  // ONE entry, not one per road — EditStack's depth limit alone would
  // otherwise make the earliest roads of a district un-undoable.
  EXPECT_EQ(stack.size(), 1U);

  // ...and it really did the work. size()==1 is satisfied by a composite that
  // created three roads, so the count is asserted beside it.
  std::size_t roads = 0;
  network.for_each_road([&roads](roadmaker::RoadId, const roadmaker::Road&) { ++roads; });
  EXPECT_GE(roads, 7U);

  ASSERT_TRUE(stack.undo(network).has_value());
  const auto after = roadmaker::write_xodr(network, "before");
  ASSERT_TRUE(after.has_value());
  // Byte-identical, which is the contract Command actually promises and the
  // only assertion a partial unwind fails.
  EXPECT_EQ(*before, *after);
}

TEST(OsmImport, CreatedIdsMatchPlanOrderEvenWithANonTrivialFreeList) {
  // THE MOST DANGEROUS ASSUMPTION IN THE DESIGN. Joint stages recover the ids
  // the road stages created by diffing the arena against a pre-import
  // snapshot, which rests on for_each_road yielding CREATION ORDER. If that
  // ever breaks, the import welds the WRONG ends together — silently,
  // producing a network that looks plausible and is wrong.
  //
  // THIS TEST WAS VACUOUS WHEN FIRST WRITTEN. It asserted only that every
  // planned odr_id was findable, which a PERMUTATION preserves perfectly:
  // reversing the recovered order left it green. A pairing has to be checked
  // as a pairing — presence of all the values says nothing about which value
  // went where. Found by the sabotage run.
  RoadNetwork network = amsterdam_network();

  // Churn the arena first so the free list is not trivial: created ids will be
  // reused slots with bumped generations rather than a clean ascending run.
  const auto scratch_a = network.create_road("scratch a", "scratch_a");
  const auto scratch_b = network.create_road("scratch b", "scratch_b");
  network.erase_road(scratch_a);
  network.erase_road(scratch_b);

  auto import = roadmaker::osm::prepare_osm_import(network, fixture("topology.osm"));
  ASSERT_TRUE(import.has_value());
  const NetworkPlan plan = import->plan;
  ASSERT_GE(plan.roads.size(), 4U) << "the fixture must have enough roads to permute";

  roadmaker::edit::EditStack stack;
  ASSERT_TRUE(stack.push(network, std::move(import->command)).has_value());

  // The road carrying a planned odr_id must be the road built from THAT plan
  // entry — checked through geometry, which a permutation cannot preserve.
  for (const PlannedRoad& planned : plan.roads) {
    const roadmaker::RoadId found = network.find_road(planned.odr_id);
    ASSERT_TRUE(found.is_valid()) << planned.odr_id << " is not in the network";

    const roadmaker::Road* road = network.road(found);
    ASSERT_NE(road, nullptr);
    ASSERT_FALSE(planned.waypoints.empty());
    ASSERT_FALSE(road->plan_view.records().empty());

    const auto& first = road->plan_view.records().front();
    EXPECT_NEAR(first.x, planned.waypoints.front().x, 1e-6)
        << planned.odr_id << " carries another plan entry\'s geometry";
    EXPECT_NEAR(first.y, planned.waypoints.front().y, 1e-6) << planned.odr_id;
  }
}

TEST(OsmImport, ReImportingTheSameExtractIsANoOpThatSaysSo) {
  RoadNetwork network = amsterdam_network();

  auto first = roadmaker::osm::prepare_osm_import(network, fixture("topology.osm"));
  ASSERT_TRUE(first.has_value());
  roadmaker::edit::EditStack stack;
  ASSERT_TRUE(stack.push(network, std::move(first->command)).has_value());

  std::size_t after_first = 0;
  network.for_each_road(
      [&after_first](roadmaker::RoadId, const roadmaker::Road&) { ++after_first; });

  const auto second = roadmaker::osm::prepare_osm_import(network, fixture("topology.osm"));
  ASSERT_TRUE(second.has_value());
  EXPECT_EQ(second->plan.roads.size(), 0U) << "every road was already present";
  EXPECT_GT(second->plan.skipped_existing, 0U) << "and it must SAY how many it skipped";
}

TEST(OsmImport, RefusesASceneWithNoGeoreferenceAndDoesNotSetOneItself) {
  // Silently georeferencing a scene gives it a projection the user never
  // chose. The refusal is gis::crs_transform's own, verbatim and shared with
  // the GIS and lidar importers.
  RoadNetwork network; // no georeference
  auto refused = roadmaker::osm::prepare_osm_import(network, fixture("topology.osm"));
  ASSERT_FALSE(refused.has_value());
  EXPECT_FALSE(refused.error().message.empty());
  EXPECT_TRUE(network.georeference().projection.empty())
      << "the importer set an origin the user never chose";
}

TEST(OsmImport, DroppedJunctionTurnsReachTheDiagnostics) {
  RoadNetwork network = amsterdam_network();
  auto import = roadmaker::osm::prepare_osm_import(network, fixture("topology.osm"));
  ASSERT_TRUE(import.has_value());
  ASSERT_NE(import->apply_diagnostics, nullptr);

  roadmaker::edit::EditStack stack;
  ASSERT_TRUE(stack.push(network, std::move(import->command)).has_value());

  // The channel must exist and be readable after the push. Whether this
  // particular fixture drops a turn is the generator's business; what this
  // pins is that the sink is wired at all — a district that dropped forty
  // turns in silence would look identical to one that dropped none.
  for (const auto& diagnostic : *import->apply_diagnostics) {
    EXPECT_FALSE(diagnostic.rule_id.empty()) << diagnostic.message;
  }
  SUCCEED();
}
