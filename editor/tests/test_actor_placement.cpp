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

// Resolving a world cursor to a lane anchor (p8-s2, issue #246).
//
// THE PROPERTY THAT MATTERS MOST HERE IS THE ROUND TRIP: a point picked on a
// lane must resolve to that lane, and projecting the resolved anchor back must
// land on the point. Both directions go through actor_world_pose, which is also
// what draws the ghost — so a drift between them would show up to the user as
// "the preview lied about where the actor would go".

#include "roadmaker/edit/operations.hpp"
#include "roadmaker/road/lane.hpp"
#include "roadmaker/road/lane_section.hpp"
#include "roadmaker/road/network.hpp"
#include "roadmaker/road/road.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <numbers>
#include <string>
#include <vector>

#include "document/actor_placement.hpp"
#include "document/document.hpp"

namespace roadmaker::editor {
namespace {

/// A straight two-lane road along +x from the origin, 100 m long.
///
/// Two-lane default = one driving lane each side of the centre, so lane -1 is
/// at negative t (right of +s) and lane +1 at positive t.
///
/// Populates in place rather than returning: `Document` is a QObject and is
/// deliberately non-copyable.
void author_straight_road(Document& document) {
  auto command = edit::create_road({Waypoint{.x = 0.0, .y = 0.0}, Waypoint{.x = 100.0, .y = 0.0}},
                                   LaneProfile::two_lane_default(),
                                   "main");
  ASSERT_NE(command, nullptr);
  ASSERT_TRUE(document.push_command(std::move(command)).has_value());
}

/// Every lane of every section, since RoadNetwork has no for_each_lane.
std::vector<LaneId> all_lanes(const RoadNetwork& network) {
  std::vector<LaneId> lanes;
  network.for_each_road([&](RoadId, const Road& road) {
    for (const LaneSectionId section_id : road.sections) {
      const LaneSection* section = network.lane_section(section_id);
      if (section == nullptr) {
        continue;
      }
      lanes.insert(lanes.end(), section->lanes.begin(), section->lanes.end());
    }
  });
  return lanes;
}

} // namespace

TEST(ActorPlacement, APointOverALaneResolvesToThatLane) {
  Document document;
  author_straight_road(document);

  // Right of the reference line (negative y here, since +s is +x) is lane -1.
  const auto right = nearest_lane_anchor(document.network(), 50.0, -1.75);
  ASSERT_TRUE(right.has_value());
  EXPECT_EQ(right->lane_odr_id, "-1");
  EXPECT_NEAR(right->s, 50.0, 1e-6);
  EXPECT_DOUBLE_EQ(right->offset, 0.0) << "an actor belongs in the middle of its lane";

  const auto left = nearest_lane_anchor(document.network(), 50.0, 1.75);
  ASSERT_TRUE(left.has_value());
  EXPECT_EQ(left->lane_odr_id, "1");
}

TEST(ActorPlacement, TheAnchorCarriesOpenDriveIdStringsNotArenaHandles) {
  // ADR-0014 §5: only the odr_id may cross into the .xosc. A RoadId in the file
  // is a runtime handle that is invalid the moment the scene reloads.
  Document document;
  author_straight_road(document);
  const auto anchor = nearest_lane_anchor(document.network(), 50.0, -1.75);
  ASSERT_TRUE(anchor.has_value());

  const Road* road = document.network().road(anchor->road);
  ASSERT_NE(road, nullptr);
  EXPECT_EQ(anchor->road_odr_id, road->odr_id);
  EXPECT_FALSE(anchor->road_odr_id.empty());

  const osc::LanePosition position = to_lane_position(*anchor);
  EXPECT_EQ(position.road_id, road->odr_id);
  EXPECT_EQ(position.lane_id, anchor->lane_odr_id);
}

TEST(ActorPlacement, ADropWithNoRoadInReachIsRefused) {
  Document document;
  author_straight_road(document);
  EXPECT_FALSE(nearest_lane_anchor(document.network(), 5000.0, 5000.0).has_value());
  EXPECT_FALSE(actor_drop_hint(document.network(), 5000.0, 5000.0).empty())
      << "a refusal must come with a hint, not silence";
}

TEST(ActorPlacement, ADropPastTheEndOfTheRoadIsRefused) {
  // find_station EXTRAPOLATES past the ends, so without the explicit bound an
  // actor could be placed at s = 400 on a 100 m road — which exports an s the
  // simulator silently truncates back to the road end.
  Document document;
  author_straight_road(document);
  EXPECT_FALSE(nearest_lane_anchor(document.network(), 400.0, 0.0).has_value());
  EXPECT_NE(actor_drop_hint(document.network(), 400.0, 0.0).find("end of the road"),
            std::string::npos);
}

TEST(ActorPlacement, ADropOverANonDrivingLaneSaysSoRatherThanClaimingNoRoad) {
  // ★ TWO DIFFERENT REFUSALS. Collapsing them into "no road in reach" would
  // send the user looking for a road that is right there.
  Document document;
  author_straight_road(document);
  // Retype both driving lanes to sidewalk: the road stays, its lanes stop
  // being places an actor can stand.
  std::vector<LaneId> lanes;
  for (const LaneId id : all_lanes(document.network())) {
    const Lane* lane = document.network().lane(id);
    if (lane != nullptr && lane->odr_id != 0) {
      lanes.push_back(id);
    }
  }
  ASSERT_FALSE(lanes.empty());
  for (const LaneId lane : lanes) {
    auto command = edit::set_lane_type(document.network(), lane, LaneType::Sidewalk);
    ASSERT_NE(command, nullptr);
    ASSERT_TRUE(document.push_command(std::move(command)).has_value());
  }

  EXPECT_FALSE(nearest_lane_anchor(document.network(), 50.0, -1.75).has_value());
  const std::string hint = actor_drop_hint(document.network(), 50.0, -1.75);
  EXPECT_NE(hint.find("driving lane"), std::string::npos) << hint;
  EXPECT_EQ(hint.find("No road within reach"), std::string::npos)
      << "the road is right there — the hint blamed the wrong thing: " << hint;
}

TEST(ActorPlacement, ALegalDropHasNoHint) {
  Document document;
  author_straight_road(document);
  EXPECT_TRUE(actor_drop_hint(document.network(), 50.0, -1.75).empty());
}

// --- the round trip ---------------------------------------------------------

TEST(ActorPlacement, ProjectingAnAnchorBackLandsOnTheLane) {
  // ★ THE GHOST-MATCHES-COMMIT PROPERTY. Both go through actor_world_pose, so
  // this is what stops the preview from lying.
  Document document;
  author_straight_road(document);
  const auto anchor = nearest_lane_anchor(document.network(), 50.0, -1.75);
  ASSERT_TRUE(anchor.has_value());

  const auto pose = actor_world_pose(document.network(), *anchor);
  ASSERT_TRUE(pose.has_value());
  EXPECT_NEAR(pose->position[0], 50.0, 1e-6) << "the station moved";
  // The y lands on the LANE CENTRE, which is the point of snapping — not back
  // on the cursor.
  EXPECT_LT(pose->position[1], 0.0) << "lane -1 is right of the reference line";
  EXPECT_GT(pose->position[1], -7.0) << "the actor left the carriageway";

  // Re-resolving the projected point must find the same lane: the round trip
  // is a fixed point, not merely reversible-looking.
  const auto again = nearest_lane_anchor(document.network(), pose->position[0], pose->position[1]);
  ASSERT_TRUE(again.has_value());
  EXPECT_EQ(again->lane_odr_id, anchor->lane_odr_id);
  EXPECT_NEAR(again->s, anchor->s, 1e-6);
}

TEST(ActorPlacement, HeadingFollowsTheLanesTravelDirectionNotTheReferenceLine) {
  // ★ An actor facing backwards down its lane renders convincingly and
  // simulates absurdly, and nothing downstream flags it.
  Document document;
  author_straight_road(document);

  const auto right = nearest_lane_anchor(document.network(), 50.0, -1.75);
  ASSERT_TRUE(right.has_value());
  const auto right_pose = actor_world_pose(document.network(), *right);
  ASSERT_TRUE(right_pose.has_value());
  // The road runs +x, so a right-hand lane travels +x: heading 0.
  EXPECT_NEAR(std::cos(right_pose->heading), 1.0, 1e-9);

  const auto left = nearest_lane_anchor(document.network(), 50.0, 1.75);
  ASSERT_TRUE(left.has_value());
  const auto left_pose = actor_world_pose(document.network(), *left);
  ASSERT_TRUE(left_pose.has_value());
  // A left-hand lane travels the other way: heading pi.
  EXPECT_NEAR(std::cos(left_pose->heading), -1.0, 1e-9)
      << "a left-lane actor faces down its own lane the wrong way";
}

TEST(ActorPlacement, APositionNamingAMissingRoadHasNoPose) {
  // The .xosc holds strings, so a scenario can outlive the road it references.
  // That must read as "cannot be drawn", never as "drawn at the origin".
  Document document;
  author_straight_road(document);
  osc::LanePosition nowhere;
  nowhere.road_id = "999";
  nowhere.lane_id = "-1";
  nowhere.s = 10.0;
  EXPECT_FALSE(actor_world_pose(document.network(), nowhere).has_value());
}

TEST(ActorPlacement, APositionNamingAMissingLaneHasNoPose) {
  Document document;
  author_straight_road(document);
  const auto anchor = nearest_lane_anchor(document.network(), 50.0, -1.75);
  ASSERT_TRUE(anchor.has_value());

  osc::LanePosition position = to_lane_position(*anchor);
  position.lane_id = "-9";
  EXPECT_FALSE(actor_world_pose(document.network(), position).has_value());
}

TEST(ActorPlacement, ANonNumericLaneIdIsUnresolvableRatherThanUndefined) {
  // OpenSCENARIO types laneId as a STRING and admits a temporary-layer id. Not
  // resolvable here, and it must not be a crash or a stoi throw either.
  Document document;
  author_straight_road(document);
  osc::LanePosition position;
  position.road_id = "1";
  position.lane_id = "tmp-3";
  position.s = 10.0;
  EXPECT_FALSE(actor_world_pose(document.network(), position).has_value());
}

// --- names ------------------------------------------------------------------

TEST(ActorPlacement, NamesAreMintedLowestUnusedWithACapitalisedStem) {
  osc::Scenario scenario;
  EXPECT_EQ(next_actor_name(scenario, "car"), "Car1");

  osc::ScenarioObject first;
  first.name = "Car1";
  scenario.entities.scenario_objects.push_back(first);
  EXPECT_EQ(next_actor_name(scenario, "car"), "Car2");

  // A gap is filled rather than skipped — the counter is the scenario's
  // contents, not a running total.
  osc::ScenarioObject third;
  third.name = "Car3";
  scenario.entities.scenario_objects.push_back(third);
  EXPECT_EQ(next_actor_name(scenario, "car"), "Car2");

  EXPECT_EQ(next_actor_name(scenario, "pedestrian"), "Pedestrian1")
      << "each archetype counts independently";
}

TEST(ActorPlacement, AMintedNameIsAlwaysUniqueBecauseTheWriterRefusesADuplicate) {
  osc::Scenario scenario;
  for (int i = 0; i < 25; ++i) {
    osc::ScenarioObject object;
    object.name = next_actor_name(scenario, "car");
    for (const osc::ScenarioObject& existing : scenario.entities.scenario_objects) {
      ASSERT_NE(existing.name, object.name) << "a minted name collided";
    }
    scenario.entities.scenario_objects.push_back(object);
  }
  EXPECT_EQ(scenario.entities.scenario_objects.size(), 25U);
}

} // namespace roadmaker::editor
