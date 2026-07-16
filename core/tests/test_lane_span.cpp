// Lane Add (add_lane_span) and Lane Form (form_lane) — the p2-s5 composite lane
// tools (issue #217). Both compose the p2-s1/s2 primitives (split_lane_section,
// add_lane, insert_lane, set_lane_width_profile) into one atomic, byte-identical
// undo step. The properties pinned here are what the viewport tools rely on:
//   - Lane Add is a self-contained POCKET (0 -> full -> 0) that stays interior,
//     so it needs no cross-section links and the carriageway stays continuous;
//   - Lane Form runs an interior lane to the road end, backward-unlinked, and
//     REFUSES rather than dangle a full-width lane at a downstream seam.

#include "roadmaker/edit/connection.hpp"
#include "roadmaker/edit/operations.hpp"
#include "roadmaker/geometry/poly3.hpp"
#include "roadmaker/road/authoring.hpp"
#include "roadmaker/road/network.hpp"
#include "roadmaker/tol.hpp"
#include "roadmaker/xodr/diagnostic.hpp"
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

const Lane* lane_by_odr(const RoadNetwork& network, LaneSectionId section_id, int odr_id) {
  for (const LaneId lane_id : network.lane_section(section_id)->lanes) {
    const Lane* lane = network.lane(lane_id);
    if (lane->odr_id == odr_id) {
      return lane;
    }
  }
  return nullptr;
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

TEST(LaneSpan, FormLaneRejectsDownstreamSeam) {
  RoadNetwork network;
  const RoadId road = author_straight(network, "1");
  // Put a section boundary at 90 so a form at 60 no longer lands in the last
  // section — forward-linking the formed lane is out of scope, so it refuses.
  ASSERT_TRUE(roadmaker::edit::split_lane_section(network, road, 90.0)->apply(network).has_value());
  expect_rejected(network,
                  roadmaker::edit::form_lane(network, road, -1, 60.0, -1, LaneType::Driving));
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
