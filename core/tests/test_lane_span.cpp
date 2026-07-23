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

// Lane Add (add_lane_span) and Lane Form (form_lane) — the p2-s5 composite lane
// tools (issue #217). Both compose the p2-s1/s2 primitives (split_lane_section,
// add_lane, insert_lane, set_lane_width_profile) into one atomic, byte-identical
// undo step. The properties pinned here are what the viewport tools rely on:
//   - Lane Add is a self-contained POCKET (0 -> full -> 0) that stays interior,
//     so it needs no cross-section links and the carriageway stays continuous;
//   - Lane Form runs an interior lane to the road end, backward-unlinked, and
//     is CARRIED across every downstream lane-section seam as a matched-pair
//     linked carriageway (link_lane_across_seam), rather than refusing.

#include "roadmaker/edit/connection.hpp"
#include "roadmaker/edit/operations.hpp"
#include "roadmaker/geometry/poly3.hpp"
#include "roadmaker/road/authoring.hpp"
#include "roadmaker/road/network.hpp"
#include "roadmaker/tol.hpp"
#include "roadmaker/xodr/diagnostic.hpp"
#include "roadmaker/xodr/reader.hpp"
#include "roadmaker/xodr/writer.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <string>
#include <vector>

#include "support/network_compare.hpp"

using roadmaker::ContactPoint;
using roadmaker::JunctionId;
using roadmaker::Lane;
using roadmaker::LaneId;
using roadmaker::LaneProfile;
using roadmaker::LaneSection;
using roadmaker::LaneSectionId;
using roadmaker::LaneType;
using roadmaker::RoadEnd;
using roadmaker::RoadId;
using roadmaker::RoadNetwork;
using roadmaker::Waypoint;
using roadmaker::edit::Command;
using roadmaker::test::expect_network_matches;
using roadmaker::test::snapshot_xodr;

namespace {

/// A straight 120 m road on the default two-lane profile (lanes +1..-2, the
/// right side driving -1 with shoulder -2).
RoadId author_straight(RoadNetwork& network, const char* odr_id, double length = 120.0) {
  const std::vector<Waypoint> waypoints{Waypoint{.x = 0.0, .y = 0.0},
                                        Waypoint{.x = length, .y = 0.0}};
  auto road = roadmaker::author_clothoid_road(
      network, waypoints, LaneProfile::two_lane_default(), "", odr_id);
  if (!road.has_value()) {
    throw std::runtime_error("author_straight: " + road.error().message);
  }
  return *road;
}

/// The §8 oracle: apply mutates, revert restores byte-identically, re-apply
/// reproduces the applied state, a final revert leaves it pristine.
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

void expect_rejected(RoadNetwork& network, std::unique_ptr<Command> command) {
  ASSERT_NE(command, nullptr);
  const std::string before = snapshot_xodr(network);
  EXPECT_FALSE(command->apply(network).has_value());
  expect_network_matches(network, before); // a failed apply must not mutate
}

LaneId lane_id_by_odr(const RoadNetwork& network, LaneSectionId section_id, int odr_id) {
  for (const LaneId lane_id : network.lane_section(section_id)->lanes) {
    if (network.lane(lane_id)->odr_id == odr_id) {
      return lane_id;
    }
  }
  return LaneId{};
}

const Lane* lane_by_odr(const RoadNetwork& network, LaneSectionId section_id, int odr_id) {
  const LaneId lane_id = lane_id_by_odr(network, section_id, odr_id);
  return lane_id.is_valid() ? network.lane(lane_id) : nullptr;
}

/// Total width of one side at global station s, each lane evaluated in its own
/// section-local frame. Continuous across a seam iff no full-width lane appears
/// there unlinked.
double side_width_at(const RoadNetwork& network, RoadId road_id, double s, int sign) {
  const LaneSectionId section_id = section_at(network, road_id, s);
  const LaneSection& section = *network.lane_section(section_id);
  const double local = s - section.s0;
  double total = 0.0;
  for (const LaneId lane_id : section.lanes) {
    const Lane& lane = *network.lane(lane_id);
    if (lane.odr_id * sign > 0) {
      total += roadmaker::eval_profile(lane.widths, local);
    }
  }
  return total;
}

/// Three straight two-lane arms meeting near the origin (the golden T, all
/// contacting at End), with the junction generated.
struct TJunction {
  RoadId west;
  RoadId east;
  RoadId south;
  JunctionId junction;
};

RoadId author_arm(RoadNetwork& network, std::vector<Waypoint> waypoints, const char* id) {
  auto road = roadmaker::author_clothoid_road(
      network, std::move(waypoints), LaneProfile::two_lane_default(), "", id);
  if (!road.has_value()) {
    throw std::runtime_error("author_arm: " + road.error().message);
  }
  return *road;
}

TJunction make_t_junction(RoadNetwork& network) {
  const RoadId west = author_arm(network, {Waypoint{-40.0, 0.0}, Waypoint{-6.0, 0.0}}, "1");
  const RoadId east = author_arm(network, {Waypoint{40.0, 0.0}, Waypoint{6.0, 0.0}}, "2");
  const RoadId south = author_arm(network, {Waypoint{0.0, -40.0}, Waypoint{0.0, -6.0}}, "3");
  const std::array<RoadEnd, 3> ends{RoadEnd{.road = west, .contact = ContactPoint::End},
                                    RoadEnd{.road = east, .contact = ContactPoint::End},
                                    RoadEnd{.road = south, .contact = ContactPoint::End}};
  if (!roadmaker::edit::create_junction(network, ends)->apply(network).has_value()) {
    throw std::runtime_error("make_t_junction: create_junction failed");
  }
  return TJunction{
      .west = west, .east = east, .south = south, .junction = network.find_junction("1")};
}

} // namespace

// --- Lane Add (add_lane_span) -----------------------------------------------

TEST(LaneSpan, AddLaneSpanIsSingleUndoStep) {
  RoadNetwork network;
  const RoadId road = author_straight(network, "1");
  auto command = roadmaker::edit::add_lane_span(network, road, -1, 40.0, 90.0, LaneType::Driving);
  ASSERT_TRUE(command->apply(network).has_value());
  // One command, reverted whole — the composite is the single undo step.
  ASSERT_TRUE(command->revert(network).has_value());
  EXPECT_EQ(network.road(road)->sections.size(), 1U);
}

TEST(LaneSpan, AddLaneSpanRevertIsByteIdentical) {
  RoadNetwork network;
  const RoadId road = author_straight(network, "1");
  auto command = roadmaker::edit::add_lane_span(network, road, -1, 40.0, 90.0, LaneType::Driving);
  expect_command_round_trip(network, *command);
}

TEST(LaneSpan, AddLaneSpanRoundTrips) {
  RoadNetwork network;
  const RoadId road = author_straight(network, "1");
  ASSERT_EQ(network.road(road)->sections.size(), 1U);
  auto command = roadmaker::edit::add_lane_span(network, road, -1, 40.0, 90.0, LaneType::Driving);
  ASSERT_TRUE(command->apply(network).has_value());

  const auto& sections = network.road(road)->sections;
  ASSERT_EQ(sections.size(), 3U); // orig + 2 seams
  const LaneSectionId mid = section_at(network, road, 65.0);
  EXPECT_NEAR(network.lane_section(mid)->s0, 40.0, roadmaker::tol::kLength);

  // The middle section carries an extra outermost right lane (-3, beyond the
  // -2 shoulder) that is zero width at BOTH seams.
  const Lane* pocket = lane_by_odr(network, mid, -3);
  ASSERT_NE(pocket, nullptr);
  EXPECT_EQ(pocket->type, LaneType::Driving);
  const double L = *section_end(network, mid) - network.lane_section(mid)->s0;
  EXPECT_NEAR(roadmaker::eval_profile(pocket->widths, 0.0), 0.0, roadmaker::tol::kLength);
  EXPECT_NEAR(roadmaker::eval_profile(pocket->widths, L), 0.0, 1e-6);
  // ...and its full plateau width in the middle is a real driving-lane width
  // (the -1 driving lane's 3.5 m on two_lane_default), not the -2 shoulder.
  EXPECT_NEAR(roadmaker::eval_profile(pocket->widths, L / 2.0), 3.5, 1e-6);

  ASSERT_TRUE(roadmaker::write_xodr(network, "ok").has_value());
  EXPECT_EQ(roadmaker::count_errors(roadmaker::validate_network(network)), 0U);
}

TEST(LaneSpan, AddLaneSpanPocketHasNoCrossSectionLinks) {
  RoadNetwork network;
  const RoadId road = author_straight(network, "1");
  auto command = roadmaker::edit::add_lane_span(network, road, -1, 40.0, 90.0, LaneType::Driving);
  ASSERT_TRUE(command->apply(network).has_value());

  const LaneSectionId mid = section_at(network, road, 65.0);
  const Lane* pocket = lane_by_odr(network, mid, -3);
  ASSERT_NE(pocket, nullptr);
  EXPECT_FALSE(pocket->predecessor.has_value());
  EXPECT_FALSE(pocket->successor.has_value());

  // Nothing in the neighbouring sections links to -3 either.
  const auto& sections = network.road(road)->sections;
  for (const LaneSectionId section_id : sections) {
    if (section_id == mid) {
      continue;
    }
    for (const LaneId lane_id : network.lane_section(section_id)->lanes) {
      const Lane& lane = *network.lane(lane_id);
      EXPECT_NE(lane.predecessor, -3);
      EXPECT_NE(lane.successor, -3);
    }
  }
}

TEST(LaneSpan, AddLaneSpanCarriagewayWidthContinuous) {
  RoadNetwork network;
  const RoadId road = author_straight(network, "1");
  auto command = roadmaker::edit::add_lane_span(network, road, -1, 40.0, 90.0, LaneType::Driving);
  ASSERT_TRUE(command->apply(network).has_value());

  // Densely sampling the right side, no consecutive pair jumps — an unlinked
  // full-width lane at a seam would jump by ~3.5 m. The pocket tapers, so the
  // largest legitimate step is the taper slope over one sample.
  constexpr int kSamples = 600;
  const double length = network.road(road)->length;
  double previous = side_width_at(network, road, 0.0, -1);
  for (int i = 1; i <= kSamples; ++i) {
    const double s = length * i / kSamples;
    const double here = side_width_at(network, road, s, -1);
    SCOPED_TRACE("s=" + std::to_string(s));
    EXPECT_LT(std::abs(here - previous), 1.0) << "carriageway width is discontinuous";
    previous = here;
  }
}

TEST(LaneSpan, AddLaneSpanIdempotentSplitAtExistingBoundary) {
  RoadNetwork network;
  const RoadId road = author_straight(network, "1");
  // A section already starts at 40; the span's low seam lands exactly there.
  ASSERT_TRUE(roadmaker::edit::split_lane_section(network, road, 40.0)->apply(network).has_value());
  ASSERT_EQ(network.road(road)->sections.size(), 2U);

  auto command = roadmaker::edit::add_lane_span(network, road, -1, 40.0, 90.0, LaneType::Driving);
  ASSERT_TRUE(command->apply(network).has_value());
  // Only the 90 seam is new — the 40 split was an idempotent no-op.
  EXPECT_EQ(network.road(road)->sections.size(), 3U);
  ASSERT_TRUE(roadmaker::write_xodr(network, "ok").has_value());
}

TEST(LaneSpan, AddLaneSpanRejectsDegenerateSpan) {
  RoadNetwork network;
  const RoadId road = author_straight(network, "1");
  expect_rejected(network,
                  roadmaker::edit::add_lane_span(network, road, -1, 60.0, 60.0, LaneType::Driving));
  expect_rejected(network,
                  roadmaker::edit::add_lane_span(network, road, -1, 90.0, 40.0, LaneType::Driving));
  expect_rejected(network,
                  roadmaker::edit::add_lane_span(network, road, 0, 40.0, 90.0, LaneType::Driving));
  expect_rejected(
      network,
      roadmaker::edit::add_lane_span(network, RoadId{}, -1, 40.0, 90.0, LaneType::Driving));
}

TEST(LaneSpan, AddLaneSpanClampsAtRoadEnds) {
  RoadNetwork network;
  const RoadId road = author_straight(network, "1");
  const double length = network.road(road)->length;
  // A span overhanging both ends clamps to [0.5, length-0.5] so both seams stay
  // strictly interior (and the pocket needs no cross-section links).
  auto command =
      roadmaker::edit::add_lane_span(network, road, -1, -10.0, length + 10.0, LaneType::Driving);
  ASSERT_TRUE(command->apply(network).has_value());

  const auto& sections = network.road(road)->sections;
  ASSERT_EQ(sections.size(), 3U);
  EXPECT_NEAR(network.lane_section(sections[1])->s0, 0.5, roadmaker::tol::kLength);
  EXPECT_NEAR(network.lane_section(sections[2])->s0, length - 0.5, roadmaker::tol::kLength);
  ASSERT_TRUE(roadmaker::write_xodr(network, "ok").has_value());
}

TEST(LaneSpan, AddLaneSpanShortSpanTapersAsATriangle) {
  RoadNetwork network;
  const RoadId road = author_straight(network, "1");
  // A span shorter than 2*kTaperLen (30 m) has no room for a plateau; the taper
  // length clamps to L/2 and the pocket is a triangle, still valid.
  auto command = roadmaker::edit::add_lane_span(network, road, -1, 50.0, 70.0, LaneType::Driving);
  expect_command_round_trip(network, *command);
  ASSERT_TRUE(command->apply(network).has_value());

  const LaneSectionId mid = section_at(network, road, 60.0);
  const Lane* pocket = lane_by_odr(network, mid, -3);
  ASSERT_NE(pocket, nullptr);
  const double L = *section_end(network, mid) - network.lane_section(mid)->s0;
  EXPECT_NEAR(roadmaker::eval_profile(pocket->widths, 0.0), 0.0, roadmaker::tol::kLength);
  EXPECT_NEAR(roadmaker::eval_profile(pocket->widths, L), 0.0, 1e-6);
  EXPECT_GT(roadmaker::eval_profile(pocket->widths, L / 2.0), 0.5); // triangle peak at the middle
  ASSERT_TRUE(roadmaker::write_xodr(network, "ok").has_value());
  EXPECT_EQ(roadmaker::count_errors(roadmaker::validate_network(network)), 0U);
}

TEST(LaneSpan, AddLaneSpanOnJunctionArmMarksJunctionDirty) {
  RoadNetwork network;
  const TJunction t = make_t_junction(network);
  ASSERT_TRUE(t.junction.is_valid());
  const double length = network.road(t.west)->length;

  // Add a pocket over the arm's far span. The command must NAME the junction
  // (so the editor's regen loop visits it) without claiming it is current.
  auto command = roadmaker::edit::add_lane_span(
      network, t.west, -1, length * 0.3, length * 0.7, LaneType::Driving);
  const roadmaker::edit::DirtySet dirty = command->dirty();
  ASSERT_EQ(dirty.junctions.size(), 1U);
  EXPECT_EQ(dirty.junctions[0], t.junction);
  EXPECT_FALSE(dirty.junctions_are_current);

  ASSERT_TRUE(command->apply(network).has_value());
  // The plan view is untouched, so the welds still hold.
  auto welds = roadmaker::edit::verify_junction_welds(network, t.junction);
  ASSERT_TRUE(welds.has_value());
  EXPECT_FALSE(welds->breaches);
}

// --- Lane Form (form_lane) --------------------------------------------------

TEST(LaneSpan, FormLaneIsSingleUndoStep) {
  RoadNetwork network;
  const RoadId road = author_straight(network, "1");
  auto command = roadmaker::edit::form_lane(network, road, -1, 60.0, -1, LaneType::Driving);
  ASSERT_TRUE(command->apply(network).has_value());
  ASSERT_TRUE(command->revert(network).has_value());
  EXPECT_EQ(network.road(road)->sections.size(), 1U);
}

TEST(LaneSpan, FormLaneRevertByteIdentical) {
  RoadNetwork network;
  const RoadId road = author_straight(network, "1");
  auto command = roadmaker::edit::form_lane(network, road, -1, 60.0, -1, LaneType::Driving);
  expect_command_round_trip(network, *command);
}

TEST(LaneSpan, FormLaneRoundTrips) {
  RoadNetwork network;
  const RoadId road = author_straight(network, "1");
  auto command = roadmaker::edit::form_lane(network, road, -1, 60.0, -1, LaneType::Driving);
  ASSERT_TRUE(command->apply(network).has_value());

  const auto& sections = network.road(road)->sections;
  ASSERT_EQ(sections.size(), 2U); // split at s_start only
  const LaneSectionId last = sections.back();
  EXPECT_NEAR(network.lane_section(last)->s0, 60.0, roadmaker::tol::kLength);

  // The formed lane took position -1; the old -1/-2 stepped out to -2/-3.
  const Lane* formed = lane_by_odr(network, last, -1);
  ASSERT_NE(formed, nullptr);
  EXPECT_EQ(formed->type, LaneType::Driving);
  const double L = *section_end(network, last) - network.lane_section(last)->s0;
  EXPECT_NEAR(roadmaker::eval_profile(formed->widths, 0.0), 0.0, roadmaker::tol::kLength);
  EXPECT_GT(roadmaker::eval_profile(formed->widths, L), 1.0); // full width at the terminus

  ASSERT_TRUE(roadmaker::write_xodr(network, "ok").has_value());
  EXPECT_EQ(roadmaker::count_errors(roadmaker::validate_network(network)), 0U);
}

TEST(LaneSpan, FormLaneBackwardUnlinked) {
  RoadNetwork network;
  const RoadId road = author_straight(network, "1");
  auto command = roadmaker::edit::form_lane(network, road, -1, 60.0, -1, LaneType::Driving);
  ASSERT_TRUE(command->apply(network).has_value());

  const LaneSectionId last = network.road(road)->sections.back();
  const Lane* formed = lane_by_odr(network, last, -1);
  ASSERT_NE(formed, nullptr);
  EXPECT_FALSE(formed->predecessor.has_value()) << "a formed lane appears mid-road, not linked";
  // The writer accepts it: the lane is zero width at the seam, so no link is due.
  ASSERT_TRUE(roadmaker::write_xodr(network, "ok").has_value());
}

TEST(LaneSpan, FormLaneAcrossOneSeamLinksMatchedPair) {
  RoadNetwork network;
  const RoadId road = author_straight(network, "1");
  // A boundary at 90 puts a downstream seam ahead of a form at 60. The lane is
  // now carried across it; the writer (the dangling-link detector) accepting
  // the network is proof the predecessor/successor pair is matched.
  ASSERT_TRUE(roadmaker::edit::split_lane_section(network, road, 90.0)->apply(network).has_value());
  auto command = roadmaker::edit::form_lane(network, road, -1, 60.0, -1, LaneType::Driving);
  ASSERT_TRUE(command->apply(network).has_value());

  const auto& sections = network.road(road)->sections;
  ASSERT_EQ(sections.size(), 3U); // [0,60), [60,90), [90,120)
  const LaneSectionId upstream = section_at(network, road, 75.0);
  const LaneSectionId downstream = section_at(network, road, 105.0);
  const Lane* up = lane_by_odr(network, upstream, -1);
  const Lane* down = lane_by_odr(network, downstream, -1);
  ASSERT_NE(up, nullptr);
  ASSERT_NE(down, nullptr);
  EXPECT_EQ(up->successor, -1); // matched pair, both ways
  EXPECT_EQ(down->predecessor, -1);

  ASSERT_TRUE(roadmaker::write_xodr(network, "ok").has_value());
  EXPECT_EQ(roadmaker::count_errors(roadmaker::validate_network(network)), 0U);
}

TEST(LaneSpan, FormLaneAcrossManySeamsRunsToRoadEnd) {
  RoadNetwork network;
  const RoadId road = author_straight(network, "1");
  for (const double s : {40.0, 70.0, 100.0}) {
    ASSERT_TRUE(roadmaker::edit::split_lane_section(network, road, s)->apply(network).has_value());
  }
  // Form at 20 (START section [20,40) is longer than the taper), upstream of
  // THREE downstream seams (40/70/100).
  auto command = roadmaker::edit::form_lane(network, road, -1, 20.0, -1, LaneType::Driving);
  ASSERT_TRUE(command->apply(network).has_value());

  const auto& sections = network.road(road)->sections;
  ASSERT_EQ(sections.size(), 5U); // split at 20 adds one to the four boundaries
  // The formed lane is present and full width at the terminus in the LAST
  // section, and every seam from the start section on is a matched pair.
  const LaneSectionId last = sections.back();
  const Lane* formed_last = lane_by_odr(network, last, -1);
  ASSERT_NE(formed_last, nullptr);
  const double L = *section_end(network, last) - network.lane_section(last)->s0;
  EXPECT_NEAR(roadmaker::eval_profile(formed_last->widths, L), 3.5, 1e-6);
  EXPECT_FALSE(formed_last->successor.has_value()); // road end: unlinked, legal

  ASSERT_TRUE(roadmaker::write_xodr(network, "ok").has_value());
  EXPECT_EQ(roadmaker::count_errors(roadmaker::validate_network(network)), 0U);
}

TEST(LaneSpan, FormLaneAcrossSeamRevertByteIdentical) {
  RoadNetwork network;
  const RoadId road = author_straight(network, "1");
  ASSERT_TRUE(roadmaker::edit::split_lane_section(network, road, 90.0)->apply(network).has_value());
  auto command = roadmaker::edit::form_lane(network, road, -1, 60.0, -1, LaneType::Driving);
  expect_command_round_trip(network, *command);
}

TEST(LaneSpan, FormLaneAcrossSeamRoundTripsThroughXodr) {
  RoadNetwork network;
  const RoadId road = author_straight(network, "1");
  ASSERT_TRUE(roadmaker::edit::split_lane_section(network, road, 90.0)->apply(network).has_value());
  ASSERT_TRUE(roadmaker::edit::form_lane(network, road, -1, 60.0, -1, LaneType::Driving)
                  ->apply(network)
                  .has_value());

  const auto text = roadmaker::write_xodr(network, "ok");
  ASSERT_TRUE(text.has_value());
  auto reparsed = roadmaker::parse_xodr(*text);
  ASSERT_TRUE(reparsed.has_value());
  EXPECT_EQ(roadmaker::count_errors(reparsed->diagnostics), 0U);
  EXPECT_EQ(roadmaker::count_errors(roadmaker::validate_network(reparsed->network)), 0U);
  // Byte-stable through a write/parse/write cycle.
  const auto rewritten = roadmaker::write_xodr(reparsed->network, "ok");
  ASSERT_TRUE(rewritten.has_value());
  EXPECT_EQ(*rewritten, *text);
}

TEST(LaneSpan, FormLaneSeamCarriagewayWidthContinuous) {
  RoadNetwork network;
  const RoadId road = author_straight(network, "1");
  ASSERT_TRUE(roadmaker::edit::split_lane_section(network, road, 60.0)->apply(network).has_value());
  ASSERT_TRUE(roadmaker::edit::form_lane(network, road, -1, 30.0, -1, LaneType::Driving)
                  ->apply(network)
                  .has_value());

  // Across the carried seam at 60 the right-side width does not jump: the formed
  // lane holds full width into the seam and continues at full width past it.
  constexpr double kEps = 1e-3;
  const double before = side_width_at(network, road, 60.0 - kEps, -1);
  const double after = side_width_at(network, road, 60.0 + kEps, -1);
  EXPECT_NEAR(before, after, 1e-2) << "carriageway width is discontinuous at the carried seam";
}

TEST(LaneSpan, FormLaneIntoNarrowerDownstreamSection) {
  RoadNetwork network;
  const RoadId road = author_straight(network, "1"); // right side -1 driving, -2 shoulder
  ASSERT_TRUE(roadmaker::edit::split_lane_section(network, road, 60.0)->apply(network).has_value());
  // Make the downstream section NARROWER: drop its -2 shoulder so its outermost
  // right lane is -1. A form at position -2 must then APPEND (add_lane), not
  // insert, and link the ACTUAL freshly-appended lane id.
  const LaneSectionId last = network.road(road)->sections.back();
  const LaneId shoulder = lane_id_by_odr(network, last, -2);
  ASSERT_TRUE(shoulder.is_valid());
  ASSERT_TRUE(roadmaker::edit::remove_lane(network, shoulder)->apply(network).has_value());
  ASSERT_EQ(lane_by_odr(network, network.road(road)->sections.back(), -2), nullptr);

  auto command = roadmaker::edit::form_lane(network, road, -1, 30.0, -2, LaneType::Driving);
  ASSERT_TRUE(command->apply(network).has_value());

  const auto& sections = network.road(road)->sections;
  ASSERT_EQ(sections.size(), 3U); // [0,30), [30,60), [60,120)
  const LaneSectionId upstream = section_at(network, road, 45.0);
  const LaneSectionId downstream = section_at(network, road, 90.0);
  const LaneId up_id = lane_id_by_odr(network, upstream, -2);
  const LaneId down_id = lane_id_by_odr(network, downstream, -2);
  ASSERT_TRUE(up_id.is_valid());
  ASSERT_TRUE(down_id.is_valid());
  // Non-identity: the appended downstream lane is a DIFFERENT arena lane, linked
  // by odr to the upstream formed lane, not assumed to be the same object.
  EXPECT_NE(up_id, down_id);
  const Lane* up = network.lane(up_id);
  const Lane* down = network.lane(down_id);
  EXPECT_EQ(up->successor, -2);
  EXPECT_EQ(down->predecessor, -2);
  EXPECT_NEAR(roadmaker::eval_profile(down->widths, 0.0), 3.5, 1e-6); // appended at full width

  ASSERT_TRUE(roadmaker::write_xodr(network, "ok").has_value());
  EXPECT_EQ(roadmaker::count_errors(roadmaker::validate_network(network)), 0U);
}

TEST(LaneSpan, LinkLaneAcrossSeamRejectsNonAdjacentAndCenter) {
  RoadNetwork network;
  const RoadId road = author_straight(network, "1");
  ASSERT_TRUE(roadmaker::edit::split_lane_section(network, road, 60.0)->apply(network).has_value());
  const auto& sections = network.road(road)->sections;
  const LaneSectionId first = sections.front();
  const LaneSectionId last = sections.back();
  // The last section has no follower — nothing to link across (non-adjacent).
  expect_rejected(network, roadmaker::edit::link_lane_across_seam(network, last, -1, -1));
  // The center lane never carries a cross-section link.
  expect_rejected(network, roadmaker::edit::link_lane_across_seam(network, first, 0, -1));
  expect_rejected(network, roadmaker::edit::link_lane_across_seam(network, first, -1, 0));
  // A stale section id is rejected too.
  expect_rejected(network,
                  roadmaker::edit::link_lane_across_seam(network, LaneSectionId{}, -1, -1));
}

TEST(LaneSpan, FormLaneRejectsBadArguments) {
  RoadNetwork network;
  const RoadId road = author_straight(network, "1");
  expect_rejected(network,
                  roadmaker::edit::form_lane(network, road, 0, 60.0, -1, LaneType::Driving));
  expect_rejected(network,
                  roadmaker::edit::form_lane(network, road, -1, 60.0, 1, LaneType::Driving));
  expect_rejected(network,
                  roadmaker::edit::form_lane(network, road, -1, 0.0, -1, LaneType::Driving));
  expect_rejected(network,
                  roadmaker::edit::form_lane(network, RoadId{}, -1, 60.0, -1, LaneType::Driving));
}

// --- Lane Carve (carve_lane) ------------------------------------------------
// Like Lane Form, a carve runs an interior lane to the road terminus (where the
// junction absorbs it), but the width ramps 0 -> full over the DRAGGED span
// [s_start, s_end] rather than a fixed taper. Dragging to the terminus makes the
// whole lane one diagonal; a shorter drag tapers then holds full to the end.

TEST(LaneSpan, CarveLaneIsSingleUndoStep) {
  RoadNetwork network;
  const RoadId road = author_straight(network, "1");
  auto command = roadmaker::edit::carve_lane(network, road, -1, 60.0, 90.0, -1, LaneType::Driving);
  ASSERT_TRUE(command->apply(network).has_value());
  ASSERT_TRUE(command->revert(network).has_value());
  EXPECT_EQ(network.road(road)->sections.size(), 1U);
}

TEST(LaneSpan, CarveLaneRevertByteIdentical) {
  RoadNetwork network;
  const RoadId road = author_straight(network, "1");
  auto command = roadmaker::edit::carve_lane(network, road, -1, 60.0, 90.0, -1, LaneType::Driving);
  expect_command_round_trip(network, *command);
}

TEST(LaneSpan, CarveLaneTapersOverTheDraggedSpanThenHoldsFull) {
  RoadNetwork network;
  const RoadId road = author_straight(network, "1");
  // Drag [60, 90] on a 120 m road: the taper occupies exactly the dragged span,
  // then the lane holds full width to the terminus.
  auto command = roadmaker::edit::carve_lane(network, road, -1, 60.0, 90.0, -1, LaneType::Driving);
  ASSERT_TRUE(command->apply(network).has_value());

  const auto& sections = network.road(road)->sections;
  ASSERT_EQ(sections.size(), 2U); // one split at s_start
  const LaneSectionId last = sections.back();
  EXPECT_NEAR(network.lane_section(last)->s0, 60.0, roadmaker::tol::kLength);

  const Lane* carved = lane_by_odr(network, last, -1);
  ASSERT_NE(carved, nullptr);
  EXPECT_EQ(carved->type, LaneType::Driving);
  // section-local: 0 at s_start (60), full at the end of the dragged span (90 ->
  // local 30), and still full at the terminus (120 -> local 60).
  EXPECT_NEAR(roadmaker::eval_profile(carved->widths, 0.0), 0.0, roadmaker::tol::kLength);
  EXPECT_NEAR(roadmaker::eval_profile(carved->widths, 30.0), 3.5, 1e-6);
  EXPECT_NEAR(roadmaker::eval_profile(carved->widths, 60.0), 3.5, 1e-6);
  // Half-way up the taper the lane is roughly half width — it is a real ramp.
  EXPECT_NEAR(roadmaker::eval_profile(carved->widths, 15.0), 1.75, 1e-6);

  ASSERT_TRUE(roadmaker::write_xodr(network, "ok").has_value());
  EXPECT_EQ(roadmaker::count_errors(roadmaker::validate_network(network)), 0U);
}

TEST(LaneSpan, CarveLaneDraggedToTerminusIsASingleDiagonal) {
  RoadNetwork network;
  const RoadId road = author_straight(network, "1");
  const double length = network.road(road)->length;
  // Drag the whole way to the junction end: the ramp spans the entire lane, so
  // half-way along it is still climbing (~half width), not yet full.
  auto command =
      roadmaker::edit::carve_lane(network, road, -1, 60.0, length, -1, LaneType::Driving);
  ASSERT_TRUE(command->apply(network).has_value());

  const LaneSectionId last = network.road(road)->sections.back();
  const Lane* carved = lane_by_odr(network, last, -1);
  ASSERT_NE(carved, nullptr);
  const double L = *section_end(network, last) - network.lane_section(last)->s0;
  EXPECT_NEAR(roadmaker::eval_profile(carved->widths, 0.0), 0.0, roadmaker::tol::kLength);
  EXPECT_NEAR(roadmaker::eval_profile(carved->widths, L / 2.0), 1.75, 1e-2); // still ramping
  EXPECT_NEAR(roadmaker::eval_profile(carved->widths, L), 3.5, 1e-2);        // full at the end
  ASSERT_TRUE(roadmaker::write_xodr(network, "ok").has_value());
  EXPECT_EQ(roadmaker::count_errors(roadmaker::validate_network(network)), 0U);
}

TEST(LaneSpan, CarveLaneCarriagewayWidthContinuousAndNoDanglingLink) {
  RoadNetwork network;
  const RoadId road = author_straight(network, "1");
  auto command = roadmaker::edit::carve_lane(network, road, -1, 60.0, 90.0, -1, LaneType::Driving);
  ASSERT_TRUE(command->apply(network).has_value());

  // Densely sample the right side: an unlinked full-width lane at the s_start
  // seam would jump ~3.5 m. The carve tapers up from zero there, so no jump.
  constexpr int kSamples = 600;
  const double length = network.road(road)->length;
  double previous = side_width_at(network, road, 0.0, -1);
  for (int i = 1; i <= kSamples; ++i) {
    const double s = length * i / kSamples;
    const double here = side_width_at(network, road, s, -1);
    SCOPED_TRACE("s=" + std::to_string(s));
    EXPECT_LT(std::abs(here - previous), 1.0) << "carriageway width is discontinuous";
    previous = here;
  }

  // The carved lane runs to the terminus, so it carries no successor and nothing
  // downstream references it (there is no downstream section). The writer's
  // link validator — which checks the successor direction — accepts it.
  const LaneSectionId last = network.road(road)->sections.back();
  const Lane* carved = lane_by_odr(network, last, -1);
  ASSERT_NE(carved, nullptr);
  EXPECT_FALSE(carved->successor.has_value());
  EXPECT_FALSE(carved->predecessor.has_value()) << "a carved lane appears mid-road, not linked";
}

TEST(LaneSpan, CarveLaneIsIdempotentAtAnExistingBoundary) {
  RoadNetwork network;
  const RoadId road = author_straight(network, "1");
  // A section already starts at 60; carving there reuses it instead of cutting a
  // duplicate seam (split_lane_section is idempotent at a boundary).
  ASSERT_TRUE(roadmaker::edit::split_lane_section(network, road, 60.0)->apply(network).has_value());
  ASSERT_EQ(network.road(road)->sections.size(), 2U);

  auto command = roadmaker::edit::carve_lane(network, road, -1, 60.0, 90.0, -1, LaneType::Driving);
  ASSERT_TRUE(command->apply(network).has_value());
  EXPECT_EQ(network.road(road)->sections.size(), 2U); // no new seam
  ASSERT_TRUE(roadmaker::write_xodr(network, "ok").has_value());
  EXPECT_EQ(roadmaker::count_errors(roadmaker::validate_network(network)), 0U);
}

TEST(LaneSpan, CarveLaneRejectsDownstreamSeam) {
  RoadNetwork network;
  const RoadId road = author_straight(network, "1");
  // A boundary at 90 means a carve at 60 no longer lands in the last section —
  // forward-linking a carved lane across a downstream seam is out of scope.
  ASSERT_TRUE(roadmaker::edit::split_lane_section(network, road, 90.0)->apply(network).has_value());
  expect_rejected(
      network, roadmaker::edit::carve_lane(network, road, -1, 60.0, 90.0, -1, LaneType::Driving));
}

TEST(LaneSpan, CarveLaneRejectsBadArguments) {
  RoadNetwork network;
  const RoadId road = author_straight(network, "1");
  expect_rejected(network,
                  roadmaker::edit::carve_lane(network, road, 0, 60.0, 90.0, -1, LaneType::Driving));
  expect_rejected(network,
                  roadmaker::edit::carve_lane(network, road, -1, 60.0, 90.0, 1, LaneType::Driving));
  expect_rejected(network,
                  roadmaker::edit::carve_lane(network, road, -1, 0.0, 90.0, -1, LaneType::Driving));
  expect_rejected( // non-positive dragged span
      network,
      roadmaker::edit::carve_lane(network, road, -1, 90.0, 60.0, -1, LaneType::Driving));
  expect_rejected(
      network,
      roadmaker::edit::carve_lane(network, RoadId{}, -1, 60.0, 90.0, -1, LaneType::Driving));
}

TEST(LaneSpan, CarveLaneOnJunctionArmRegeneratesTheJunction) {
  RoadNetwork network;
  const TJunction t = make_t_junction(network);
  ASSERT_TRUE(t.junction.is_valid());
  const double length = network.road(t.west)->length;

  // Carve a turn lane approaching the junction along the west arm. The command
  // must NAME the junction (so the editor's regen loop visits it) without
  // claiming the junction structure is already current.
  auto command =
      roadmaker::edit::carve_lane(network, t.west, -1, length * 0.4, length, -1, LaneType::Driving);
  const roadmaker::edit::DirtySet dirty = command->dirty();
  ASSERT_EQ(dirty.junctions.size(), 1U);
  EXPECT_EQ(dirty.junctions[0], t.junction);
  EXPECT_FALSE(dirty.junctions_are_current);

  const auto applied = command->apply(network);
  ASSERT_TRUE(applied.has_value()) << applied.error().message;

  // The carve widens the arm to full width AT the junction boundary, so the
  // stale connecting roads no longer weld. Regeneration — taught turn-set
  // changes in p2-s2 — rebuilds them, which is how a carved turn lane reaches
  // the junction (the editor runs exactly this on commit).
  auto regen = roadmaker::edit::regenerate_junction(network, t.junction);
  const auto regenerated = regen->apply(network);
  ASSERT_TRUE(regenerated.has_value()) << regenerated.error().message;
  auto welds = roadmaker::edit::verify_junction_welds(network, t.junction);
  ASSERT_TRUE(welds.has_value());
  EXPECT_FALSE(welds->breaches);
}
