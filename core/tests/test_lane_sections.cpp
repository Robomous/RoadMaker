// Copyright 2026 Robomous
// SPDX-License-Identifier: Apache-2.0

// Lane-section authoring: split_lane_section, set_lane_width_profile, and the
// section_at/section_end helpers (P2 pillar, issue #262).
//
// The multi-section fixtures come first on purpose. The data model, reader,
// writer and mesher have all handled multi-section roads with cubic width
// records since M2, but nothing ever authored one: merge_roads was the only
// operation that produced a second section, and no test asserted the result.
// These paths were correct by inspection and unexercised by CI — so they are
// pinned here before anything is built on top of them.

#include "roadmaker/edit/operations.hpp"
#include "roadmaker/geometry/poly3.hpp"
#include "roadmaker/road/authoring.hpp"
#include "roadmaker/road/network.hpp"
#include "roadmaker/tol.hpp"
#include "roadmaker/xodr/reader.hpp"
#include "roadmaker/xodr/writer.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <cstddef>
#include <string>
#include <vector>

#include "support/network_compare.hpp"

using roadmaker::Lane;
using roadmaker::LaneDirection;
using roadmaker::LaneId;
using roadmaker::LaneProfile;
using roadmaker::LaneSection;
using roadmaker::LaneSectionId;
using roadmaker::LaneType;
using roadmaker::Poly3;
using roadmaker::RoadId;
using roadmaker::RoadMark;
using roadmaker::RoadMarkType;
using roadmaker::RoadNetwork;
using roadmaker::Waypoint;
using roadmaker::edit::Command;
using roadmaker::test::expect_network_matches;
using roadmaker::test::snapshot_xodr;

namespace {

/// A straight 120 m road on the default two-lane profile (lanes +2..-2).
RoadId author_straight(RoadNetwork& network, const char* odr_id) {
  const std::vector<Waypoint> waypoints{
      Waypoint{.x = 0.0, .y = 0.0}, Waypoint{.x = 60.0, .y = 0.0}, Waypoint{.x = 120.0, .y = 0.0}};
  auto road = roadmaker::author_clothoid_road(
      network, waypoints, LaneProfile::two_lane_default(), "", odr_id);
  if (!road.has_value()) {
    throw std::runtime_error("author_straight: " + road.error().message);
  }
  return *road;
}

/// The §8 oracle, mirrored from test_edit_operations: apply changes the
/// document, revert restores it byte-identically, re-apply reproduces the
/// applied state byte-identically, and a final revert leaves it pristine.
void expect_command_round_trip(RoadNetwork& network, Command& command) {
  const std::string before = snapshot_xodr(network);
  {
    SCOPED_TRACE("first apply");
    ASSERT_TRUE(command.apply(network).has_value());
  }
  const std::string after = snapshot_xodr(network);
  EXPECT_NE(before, after);
  {
    SCOPED_TRACE("revert");
    ASSERT_TRUE(command.revert(network).has_value());
    expect_network_matches(network, before);
  }
  {
    SCOPED_TRACE("re-apply");
    ASSERT_TRUE(command.apply(network).has_value());
    expect_network_matches(network, after);
  }
  {
    SCOPED_TRACE("final revert");
    ASSERT_TRUE(command.revert(network).has_value());
    expect_network_matches(network, before);
  }
}

void expect_rejected(RoadNetwork& network, std::unique_ptr<Command> command) {
  ASSERT_NE(command, nullptr);
  const std::string before = snapshot_xodr(network);
  EXPECT_FALSE(command->apply(network).has_value());
  expect_network_matches(network, before); // a failed apply must not mutate
}

/// Total width of one side at global station s, summing every lane's width
/// evaluated in its own section-local frame. The carriageway must be
/// continuous across a section boundary — a split only changes where the
/// kernel may vary the width, never the width itself.
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

const Lane* lane_by_odr(const RoadNetwork& network, LaneSectionId section_id, int odr_id) {
  for (const LaneId lane_id : network.lane_section(section_id)->lanes) {
    const Lane* lane = network.lane(lane_id);
    if (lane->odr_id == odr_id) {
      return lane;
    }
  }
  return nullptr;
}

} // namespace

// --- multi-section fixtures (pinned before anything builds on them) ---------

/// Hand-builds a two-section road carrying a real cubic width taper and
/// checks the writer emits it and the reader reads it back. Neither had a
/// test: the only pre-P2 producer of a second section was merge_roads, whose
/// tests assert round-trip and byte-identity but never the section count.
TEST(LaneSections, HandBuiltMultiSectionRoadSurvivesXodrRoundTrip) {
  RoadNetwork network;
  const RoadId road_id = author_straight(network, "1");
  const LaneSectionId second = network.add_lane_section(road_id, 60.0);
  ASSERT_TRUE(second.is_valid());
  ASSERT_EQ(network.road(road_id)->sections.size(), 2U);
  // add_lane_section keeps Road::sections sorted ascending by s0.
  EXPECT_EQ(network.lane_section(network.road(road_id)->sections[0])->s0, 0.0);
  EXPECT_EQ(network.lane_section(network.road(road_id)->sections[1])->s0, 60.0);

  const LaneId center = network.add_lane(second, 0, LaneType::None);
  const LaneId driving = network.add_lane(second, -1, LaneType::Driving);
  ASSERT_TRUE(center.is_valid());
  ASSERT_TRUE(driving.is_valid());
  // A lane widening 3.0 -> 4.5 m over the section: b != 0 is the thing no
  // authored road had ever carried.
  network.lane(driving)->widths = {Poly3{.s = 0.0, .a = 3.0, .b = 0.025}};
  network.lane(driving)->predecessor = -1;
  // The first section's -1 continues into it, in both directions
  // (asam.net:xodr:1.4.0:road.lane.link.lanes_across_laneSections).
  const LaneSectionId first = network.road(road_id)->sections[0];
  for (const LaneId lane_id : network.lane_section(first)->lanes) {
    if (network.lane(lane_id)->odr_id == -1) {
      network.lane(lane_id)->successor = -1;
    }
  }

  const auto text = roadmaker::write_xodr(network, "multi");
  ASSERT_TRUE(text.has_value()) << text.error().message;

  const auto reloaded = roadmaker::parse_xodr(*text, "multi");
  ASSERT_TRUE(reloaded.has_value()) << reloaded.error().message;
  EXPECT_EQ(roadmaker::count_errors(reloaded->diagnostics), 0U);
  const RoadNetwork& back = reloaded->network;
  const RoadId reloaded_road = back.find_road("1");
  ASSERT_TRUE(reloaded_road.is_valid());
  ASSERT_EQ(back.road(reloaded_road)->sections.size(), 2U);

  const LaneSectionId reloaded_second = back.road(reloaded_road)->sections[1];
  EXPECT_NEAR(back.lane_section(reloaded_second)->s0, 60.0, roadmaker::tol::kLength);
  const Lane* reloaded_lane = lane_by_odr(back, reloaded_second, -1);
  ASSERT_NE(reloaded_lane, nullptr);
  ASSERT_EQ(reloaded_lane->widths.size(), 1U);
  EXPECT_NEAR(reloaded_lane->widths[0].a, 3.0, 1e-9);
  EXPECT_NEAR(reloaded_lane->widths[0].b, 0.025, 1e-9);
  EXPECT_EQ(reloaded_lane->predecessor, -1);

  // And the document is a fixed point: write -> parse -> write is stable.
  const auto rewritten = roadmaker::write_xodr(back, "multi");
  ASSERT_TRUE(rewritten.has_value()) << rewritten.error().message;
  EXPECT_EQ(*rewritten, *text);
}

/// The writer refuses a dangling predecessor exactly as it refuses a dangling
/// successor: a lane continuing across sections is linked in both directions
/// (asam.net:xodr:1.4.0:road.lane.link.lanes_across_laneSections), so a
/// half-checked seam let a bad split write a subtly wrong file.
TEST(LaneSections, WriterRejectsADanglingLaneLinkInEitherDirection) {
  RoadNetwork network;
  const RoadId road_id = author_straight(network, "1");
  auto split = roadmaker::edit::split_lane_section(network, road_id, 50.0);
  ASSERT_TRUE(split->apply(network).has_value());
  ASSERT_TRUE(roadmaker::write_xodr(network, "ok").has_value()) << "the split itself must be valid";

  const auto& sections = network.road(road_id)->sections;
  LaneId head_id{};
  LaneId tail_id{};
  for (const LaneId lane_id : network.lane_section(sections[0])->lanes) {
    if (network.lane(lane_id)->odr_id == -1) {
      head_id = lane_id;
    }
  }
  for (const LaneId lane_id : network.lane_section(sections[1])->lanes) {
    if (network.lane(lane_id)->odr_id == -1) {
      tail_id = lane_id;
    }
  }

  // Predecessor naming a lane that does not exist in the previous section.
  network.lane(tail_id)->predecessor = -99;
  EXPECT_FALSE(roadmaker::write_xodr(network, "bad").has_value())
      << "a dangling predecessor must be refused";
  network.lane(tail_id)->predecessor = -1;
  EXPECT_TRUE(roadmaker::write_xodr(network, "ok").has_value());

  // ...and the successor direction, which was already checked.
  network.lane(head_id)->successor = -99;
  EXPECT_FALSE(roadmaker::write_xodr(network, "bad").has_value())
      << "a dangling successor must be refused";
}

// --- section_at / section_end ----------------------------------------------

TEST(LaneSections, SectionAtResolvesTheGoverningSection) {
  RoadNetwork network;
  const RoadId road_id = author_straight(network, "1");
  const LaneSectionId first = network.road(road_id)->sections[0];
  const LaneSectionId second = network.add_lane_section(road_id, 60.0);

  EXPECT_EQ(section_at(network, road_id, 0.0), first);
  EXPECT_EQ(section_at(network, road_id, 59.9), first);
  EXPECT_EQ(section_at(network, road_id, 60.0), second); // the boundary belongs to the new section
  EXPECT_EQ(section_at(network, road_id, 119.0), second);
  // A station before the road start still resolves to the first section: a
  // section is valid from its s0 until the next, and there is nothing before.
  EXPECT_EQ(section_at(network, road_id, -5.0), first);
  EXPECT_FALSE(section_at(network, RoadId{}, 0.0).is_valid());
}

TEST(LaneSections, SectionEndComesFromTheNextSectionOrTheRoadLength) {
  RoadNetwork network;
  const RoadId road_id = author_straight(network, "1");
  const LaneSectionId first = network.road(road_id)->sections[0];

  auto only = section_end(network, first);
  ASSERT_TRUE(only.has_value());
  EXPECT_NEAR(*only, network.road(road_id)->length, roadmaker::tol::kLength);

  const LaneSectionId second = network.add_lane_section(road_id, 60.0);
  auto bounded = section_end(network, first);
  ASSERT_TRUE(bounded.has_value());
  EXPECT_NEAR(*bounded, 60.0, roadmaker::tol::kLength);
  auto last = section_end(network, second);
  ASSERT_TRUE(last.has_value());
  EXPECT_NEAR(*last, network.road(road_id)->length, roadmaker::tol::kLength);

  EXPECT_FALSE(section_end(network, LaneSectionId{}).has_value());
}

TEST(LaneSections, LaneBoundaryOffsetsAccumulateWidthsFromTheCenter) {
  RoadNetwork network;
  const RoadId road_id = author_straight(network, "1");
  // two_lane_default: +1 driving (3.5), centre 0, -1 driving (3.5), -2 shoulder
  // (1.0), lane offset 0 — four boundaries accumulating outward from the centre.
  const auto offsets = roadmaker::lane_boundary_offsets(network, road_id, 30.0);
  ASSERT_EQ(offsets.size(), 4U);
  EXPECT_NEAR(offsets[0], 3.5, 1e-9);  // outer edge of +1
  EXPECT_NEAR(offsets[1], 0.0, 1e-9);  // the centre boundary sits at laneOffset(s)
  EXPECT_NEAR(offsets[2], -3.5, 1e-9); // -1 | -2 boundary
  EXPECT_NEAR(offsets[3], -4.5, 1e-9); // outer edge of the -2 shoulder
  for (std::size_t i = 1; i < offsets.size(); ++i) {
    EXPECT_LT(offsets[i], offsets[i - 1]) << "boundaries must read left-to-right";
  }
  // A stale road yields an empty vector rather than a crash — the editor pick
  // calls this every mouse-move.
  EXPECT_TRUE(roadmaker::lane_boundary_offsets(network, RoadId{}, 30.0).empty());
}

// --- split_lane_section -----------------------------------------------------

TEST(LaneSections, SplitCreatesASecondSectionAndRoundTrips) {
  RoadNetwork network;
  const RoadId road_id = author_straight(network, "1");
  ASSERT_EQ(network.road(road_id)->sections.size(), 1U);
  const std::size_t lanes = network.lane_section(network.road(road_id)->sections[0])->lanes.size();

  auto command = roadmaker::edit::split_lane_section(network, road_id, 50.0);
  expect_command_round_trip(network, *command);

  ASSERT_TRUE(command->apply(network).has_value());
  const auto& sections = network.road(road_id)->sections;
  ASSERT_EQ(sections.size(), 2U);
  EXPECT_NEAR(network.lane_section(sections[0])->s0, 0.0, roadmaker::tol::kLength);
  EXPECT_NEAR(network.lane_section(sections[1])->s0, 50.0, roadmaker::tol::kLength);
  // The cross section is duplicated, not invented.
  EXPECT_EQ(network.lane_section(sections[1])->lanes.size(), lanes);
}

/// split_lane_section copies lanes field-by-field, so a field it forgets is
/// silently reset on the new-section copy. Direction is such a field.
TEST(LaneSections, SplitPreservesLaneDirection) {
  RoadNetwork network;
  const RoadId road_id = author_straight(network, "1");
  const LaneSectionId first = network.road(road_id)->sections[0];

  LaneId reversed_id{};
  for (const LaneId lane_id : network.lane_section(first)->lanes) {
    if (network.lane(lane_id)->odr_id == -1) {
      reversed_id = lane_id;
    }
  }
  ASSERT_TRUE(reversed_id.is_valid());
  auto set_dir = roadmaker::edit::set_lane_direction(network, reversed_id, LaneDirection::Reversed);
  ASSERT_TRUE(set_dir->apply(network).has_value());

  auto command = roadmaker::edit::split_lane_section(network, road_id, 50.0);
  ASSERT_TRUE(command->apply(network).has_value());

  const auto& sections = network.road(road_id)->sections;
  ASSERT_EQ(sections.size(), 2U);
  // Both the truncated original and the fresh [s, end) copy keep Reversed.
  for (const LaneSectionId section_id : sections) {
    const Lane* lane = lane_by_odr(network, section_id, -1);
    ASSERT_NE(lane, nullptr);
    EXPECT_EQ(lane->direction, LaneDirection::Reversed);
  }
}

/// The split only changes where the kernel may vary the width — never the
/// width. Sampling across the seam is the property that catches a bad Taylor
/// shift, which a section-count assertion never would.
TEST(LaneSections, SplitLeavesTheCarriagewayWidthContinuous) {
  RoadNetwork network;
  const RoadId road_id = author_straight(network, "1");
  const LaneSectionId first = network.road(road_id)->sections[0];

  // Give -1 a cubic taper so the split has something non-trivial to partition.
  const Lane* driving = lane_by_odr(network, first, -1);
  ASSERT_NE(driving, nullptr);
  LaneId driving_id{};
  for (const LaneId lane_id : network.lane_section(first)->lanes) {
    if (network.lane(lane_id)->odr_id == -1) {
      driving_id = lane_id;
    }
  }
  auto taper = roadmaker::edit::set_lane_width_profile(
      network, driving_id, {Poly3{.s = 0.0, .a = 3.0, .b = 0.02, .c = -0.0001}});
  ASSERT_TRUE(taper->apply(network).has_value());

  constexpr int kSamples = 240;
  std::vector<double> expected;
  expected.reserve(kSamples + 1);
  for (int i = 0; i <= kSamples; ++i) {
    expected.push_back(side_width_at(network, road_id, 120.0 * i / kSamples, -1));
  }

  auto command = roadmaker::edit::split_lane_section(network, road_id, 50.0);
  ASSERT_TRUE(command->apply(network).has_value());
  ASSERT_EQ(network.road(road_id)->sections.size(), 2U);

  for (int i = 0; i <= kSamples; ++i) {
    const double s = 120.0 * i / kSamples;
    SCOPED_TRACE("s=" + std::to_string(s));
    EXPECT_NEAR(
        side_width_at(network, road_id, s, -1), expected[static_cast<std::size_t>(i)], 1e-9);
  }
}

/// asam.net:xodr:1.4.0:road.lane.link.lanes_across_laneSections — a lane that
/// continues across the seam is connected in BOTH directions.
TEST(LaneSections, SplitLinksContinuingLanesInBothDirections) {
  RoadNetwork network;
  const RoadId road_id = author_straight(network, "1");
  auto command = roadmaker::edit::split_lane_section(network, road_id, 50.0);
  ASSERT_TRUE(command->apply(network).has_value());

  const auto& sections = network.road(road_id)->sections;
  for (const LaneId lane_id : network.lane_section(sections[0])->lanes) {
    const Lane& lane = *network.lane(lane_id);
    SCOPED_TRACE("odr lane " + std::to_string(lane.odr_id));
    EXPECT_EQ(lane.successor, lane.odr_id); // identity across the seam
    const Lane* copy = lane_by_odr(network, sections[1], lane.odr_id);
    ASSERT_NE(copy, nullptr);
    EXPECT_EQ(copy->predecessor, lane.odr_id);
    EXPECT_EQ(copy->type, lane.type);
  }
}

TEST(LaneSections, SplitPartitionsTheWidthProfileAtTheCut) {
  RoadNetwork network;
  const RoadId road_id = author_straight(network, "1");
  const LaneSectionId first = network.road(road_id)->sections[0];
  LaneId driving_id{};
  for (const LaneId lane_id : network.lane_section(first)->lanes) {
    if (network.lane(lane_id)->odr_id == -1) {
      driving_id = lane_id;
    }
  }
  // Two records: 3.0 m from 0, then 3.5 m from 80.
  auto profile = roadmaker::edit::set_lane_width_profile(
      network, driving_id, {Poly3{.s = 0.0, .a = 3.0}, Poly3{.s = 80.0, .a = 3.5}});
  ASSERT_TRUE(profile->apply(network).has_value());

  auto command = roadmaker::edit::split_lane_section(network, road_id, 50.0);
  ASSERT_TRUE(command->apply(network).has_value());
  const auto& sections = network.road(road_id)->sections;

  // The original keeps only what starts before the cut...
  const Lane* head = lane_by_odr(network, sections[0], -1);
  ASSERT_NE(head, nullptr);
  ASSERT_EQ(head->widths.size(), 1U);
  EXPECT_NEAR(head->widths[0].a, 3.0, 1e-12);

  // ...and the copy takes the rest, re-expressed about its own origin: the
  // record covering the cut shifts to sOffset 0, the 80 m record to 30.
  const Lane* tail = lane_by_odr(network, sections[1], -1);
  ASSERT_NE(tail, nullptr);
  ASSERT_EQ(tail->widths.size(), 2U);
  EXPECT_NEAR(tail->widths[0].s, 0.0, 1e-12);
  EXPECT_NEAR(tail->widths[0].a, 3.0, 1e-12);
  EXPECT_NEAR(tail->widths[1].s, 30.0, 1e-12);
  EXPECT_NEAR(tail->widths[1].a, 3.5, 1e-12);
}

/// Carve cuts both ends of a span and must not special-case a taper that
/// starts exactly where a section already does.
TEST(LaneSections, SplitIsIdempotentAtAnExistingBoundary) {
  RoadNetwork network;
  const RoadId road_id = author_straight(network, "1");
  auto first = roadmaker::edit::split_lane_section(network, road_id, 50.0);
  ASSERT_TRUE(first->apply(network).has_value());
  const std::string after_first = snapshot_xodr(network);

  auto again = roadmaker::edit::split_lane_section(network, road_id, 50.0);
  ASSERT_TRUE(again->apply(network).has_value()) << "splitting at a boundary must succeed";
  expect_network_matches(network, after_first);
  EXPECT_EQ(network.road(road_id)->sections.size(), 2U);

  ASSERT_TRUE(again->revert(network).has_value());
  expect_network_matches(network, after_first);
}

TEST(LaneSections, SplitRejectsStationsOutsideTheRoad) {
  RoadNetwork network;
  const RoadId road_id = author_straight(network, "1");
  const double length = network.road(road_id)->length;

  expect_rejected(network, roadmaker::edit::split_lane_section(network, road_id, 0.0));
  expect_rejected(network, roadmaker::edit::split_lane_section(network, road_id, -1.0));
  expect_rejected(network, roadmaker::edit::split_lane_section(network, road_id, length));
  expect_rejected(network, roadmaker::edit::split_lane_section(network, road_id, length + 10.0));
  expect_rejected(network, roadmaker::edit::split_lane_section(network, RoadId{}, 50.0));
}

/// Splitting twice yields three sections with no zero-length section — the
/// shape Lane Carve produces for a taper span.
TEST(LaneSections, SplittingBothEndsOfASpanYieldsThreeSections) {
  RoadNetwork network;
  const RoadId road_id = author_straight(network, "1");
  auto start = roadmaker::edit::split_lane_section(network, road_id, 40.0);
  ASSERT_TRUE(start->apply(network).has_value());
  auto end = roadmaker::edit::split_lane_section(network, road_id, 90.0);
  ASSERT_TRUE(end->apply(network).has_value());

  const auto& sections = network.road(road_id)->sections;
  ASSERT_EQ(sections.size(), 3U);
  double previous = -1.0;
  for (const LaneSectionId section_id : sections) {
    const double s0 = network.lane_section(section_id)->s0;
    EXPECT_GT(s0, previous) << "sections must ascend and never be zero-length";
    previous = s0;
    auto end_s = section_end(network, section_id);
    ASSERT_TRUE(end_s.has_value());
    EXPECT_GT(*end_s - s0, roadmaker::tol::kLength);
  }
}

// --- discard: reserved-slot recycling (#271) --------------------------------

/// A command that CREATED objects reserves their arena slots when reverted
/// (erase_exact keeps them off the free list for a restore that may never
/// come). discard() — called when the reverted command is dropped rather than
/// reapplied — recycles those slots so a preview redoing the same edit every
/// frame does not leak them.
TEST(LaneSections, DiscardAfterRevertRecyclesCreatedSlots) {
  RoadNetwork network;
  const RoadId road_id = author_straight(network, "1");

  auto first = roadmaker::edit::split_lane_section(network, road_id, 50.0);
  ASSERT_TRUE(first->apply(network).has_value());
  // The split created one section and duplicated the cross-section's lanes.
  const std::size_t sections = network.lane_section_slot_count();
  const std::size_t lanes = network.lane_slot_count();

  ASSERT_TRUE(first->revert(network).has_value());
  // Revert erased the created objects with erase_exact — the slots are reserved
  // (still counted), awaiting a restore.
  EXPECT_EQ(network.lane_section_slot_count(), sections);
  EXPECT_EQ(network.lane_slot_count(), lanes);

  // Discard recycles those reserved slots. A second discard is a no-op (the
  // generation was already bumped) — the precondition is self-enforcing.
  first->discard(network);
  first->discard(network);
  EXPECT_EQ(network.lane_section_slot_count(), sections);
  EXPECT_EQ(network.lane_slot_count(), lanes);

  // An identical split now REUSES the recycled indices — slot counts hold.
  auto second = roadmaker::edit::split_lane_section(network, road_id, 50.0);
  ASSERT_TRUE(second->apply(network).has_value());
  EXPECT_EQ(network.lane_section_slot_count(), sections);
  EXPECT_EQ(network.lane_slot_count(), lanes);
}

/// The same guarantee through a CompositeCommand: add_lane_span composes two
/// splits plus the pocket lanes, so discard must forward to its children.
TEST(LaneSections, DiscardForwardsThroughACompositeCommand) {
  RoadNetwork network;
  const RoadId road_id = author_straight(network, "1");

  auto first = roadmaker::edit::add_lane_span(network, road_id, -1, 40.0, 90.0, LaneType::Driving);
  ASSERT_NE(first, nullptr);
  ASSERT_TRUE(first->apply(network).has_value());
  const std::size_t sections = network.lane_section_slot_count();
  const std::size_t lanes = network.lane_slot_count();

  ASSERT_TRUE(first->revert(network).has_value());
  first->discard(network); // CompositeCommand::discard fans out to children
  first->discard(network); // idempotent

  auto second = roadmaker::edit::add_lane_span(network, road_id, -1, 40.0, 90.0, LaneType::Driving);
  ASSERT_NE(second, nullptr);
  ASSERT_TRUE(second->apply(network).has_value());
  EXPECT_EQ(network.lane_section_slot_count(), sections);
  EXPECT_EQ(network.lane_slot_count(), lanes);
}

TEST(LaneSections, SplitSurvivesAnXodrRoundTrip) {
  RoadNetwork network;
  const RoadId road_id = author_straight(network, "1");
  auto command = roadmaker::edit::split_lane_section(network, road_id, 50.0);
  ASSERT_TRUE(command->apply(network).has_value());

  const auto text = roadmaker::write_xodr(network, "split");
  ASSERT_TRUE(text.has_value()) << text.error().message;
  const auto reloaded = roadmaker::parse_xodr(*text, "split");
  ASSERT_TRUE(reloaded.has_value()) << reloaded.error().message;
  EXPECT_EQ(roadmaker::count_errors(reloaded->diagnostics), 0U);

  const RoadNetwork& back = reloaded->network;
  const RoadId reloaded_road = back.find_road("1");
  ASSERT_TRUE(reloaded_road.is_valid());
  ASSERT_EQ(back.road(reloaded_road)->sections.size(), 2U);
  // Lane links and width partitioning survive the file boundary.
  const Lane* head = lane_by_odr(back, back.road(reloaded_road)->sections[0], -1);
  const Lane* tail = lane_by_odr(back, back.road(reloaded_road)->sections[1], -1);
  ASSERT_NE(head, nullptr);
  ASSERT_NE(tail, nullptr);
  EXPECT_EQ(head->successor, -1);
  EXPECT_EQ(tail->predecessor, -1);

  const auto rewritten = roadmaker::write_xodr(back, "split");
  ASSERT_TRUE(rewritten.has_value()) << rewritten.error().message;
  EXPECT_EQ(*rewritten, *text);
}

// --- set_lane_width_profile -------------------------------------------------

TEST(LaneSections, SetWidthProfileAuthorsATaperAndRoundTrips) {
  RoadNetwork network;
  const RoadId road_id = author_straight(network, "1");
  const LaneSectionId first = network.road(road_id)->sections[0];
  LaneId driving_id{};
  for (const LaneId lane_id : network.lane_section(first)->lanes) {
    if (network.lane(lane_id)->odr_id == -1) {
      driving_id = lane_id;
    }
  }

  auto command = roadmaker::edit::set_lane_width_profile(
      network, driving_id, {Poly3{.s = 0.0, .a = 3.5}, Poly3{.s = 60.0, .a = 3.5, .b = -0.05}});
  expect_command_round_trip(network, *command);

  ASSERT_TRUE(command->apply(network).has_value());
  ASSERT_EQ(network.lane(driving_id)->widths.size(), 2U);
  EXPECT_NEAR(roadmaker::eval_profile(network.lane(driving_id)->widths, 80.0), 2.5, 1e-9);
}

/// asam.net:xodr:1.4.0:road.lane.width.lane_width_validity — width shall be
/// >= 0, so zero is legal. A lane tapering up from nothing is exactly how a
/// turn lane begins, and rejecting it would make Lane Carve unimplementable.
TEST(LaneSections, SetWidthProfileAllowsALaneThatStartsAtZeroWidth) {
  RoadNetwork network;
  const RoadId road_id = author_straight(network, "1");
  const LaneSectionId first = network.road(road_id)->sections[0];
  LaneId driving_id{};
  for (const LaneId lane_id : network.lane_section(first)->lanes) {
    if (network.lane(lane_id)->odr_id == -1) {
      driving_id = lane_id;
    }
  }
  auto command = roadmaker::edit::set_lane_width_profile(
      network, driving_id, {Poly3{.s = 0.0, .a = 0.0, .b = 0.1}});
  ASSERT_TRUE(command->apply(network).has_value());
  EXPECT_NEAR(roadmaker::eval_profile(network.lane(driving_id)->widths, 0.0), 0.0, 1e-12);
  EXPECT_NEAR(roadmaker::eval_profile(network.lane(driving_id)->widths, 35.0), 3.5, 1e-9);
}

TEST(LaneSections, SetWidthProfileRejectsNonConformantProfiles) {
  RoadNetwork network;
  const RoadId road_id = author_straight(network, "1");
  const LaneSectionId first = network.road(road_id)->sections[0];
  LaneId driving_id{};
  LaneId center_id{};
  for (const LaneId lane_id : network.lane_section(first)->lanes) {
    if (network.lane(lane_id)->odr_id == -1) {
      driving_id = lane_id;
    }
    if (network.lane(lane_id)->odr_id == 0) {
      center_id = lane_id;
    }
  }
  namespace edit = roadmaker::edit;

  // asam.net:xodr:1.4.0:road.lane.center_lane_no_width
  expect_rejected(network, edit::set_lane_width_profile(network, center_id, {Poly3{.a = 3.0}}));
  // Empty profile.
  expect_rejected(network, edit::set_lane_width_profile(network, driving_id, {}));
  // asam.net:xodr:1.7.0:road.lane.width.width_defined_whole_section — no
  // record at sOffset 0.
  expect_rejected(network,
                  edit::set_lane_width_profile(network, driving_id, {Poly3{.s = 10.0, .a = 3.0}}));
  // asam.net:xodr:1.4.0:road.lane.width.elem_asc_order
  expect_rejected(network,
                  edit::set_lane_width_profile(
                      network, driving_id, {Poly3{.s = 0.0, .a = 3.0}, Poly3{.s = 0.0, .a = 3.5}}));
  expect_rejected(
      network,
      edit::set_lane_width_profile(
          network, driving_id, {Poly3{.s = 0.0, .a = 3.0}, Poly3{.s = -5.0, .a = 3.5}}));
  // asam.net:xodr:1.4.0:road.lane.width.lane_width_validity
  expect_rejected(network,
                  edit::set_lane_width_profile(network, driving_id, {Poly3{.s = 0.0, .a = -1.0}}));
  // A record starting past the section end.
  expect_rejected(
      network,
      edit::set_lane_width_profile(
          network, driving_id, {Poly3{.s = 0.0, .a = 3.0}, Poly3{.s = 500.0, .a = 3.5}}));
  expect_rejected(network, edit::set_lane_width_profile(network, LaneId{}, {Poly3{.a = 3.0}}));
}

// --- set_lane_width: the data-loss regression -------------------------------

TEST(LaneSections, SetLaneWidthStillSetsAConstantWidth) {
  RoadNetwork network;
  const RoadId road_id = author_straight(network, "1");
  const LaneSectionId first = network.road(road_id)->sections[0];
  LaneId driving_id{};
  for (const LaneId lane_id : network.lane_section(first)->lanes) {
    if (network.lane(lane_id)->odr_id == -1) {
      driving_id = lane_id;
    }
  }
  auto command = roadmaker::edit::set_lane_width(network, driving_id, 4.25);
  expect_command_round_trip(network, *command);
  ASSERT_TRUE(command->apply(network).has_value());
  ASSERT_EQ(network.lane(driving_id)->widths.size(), 1U);
  EXPECT_NEAR(network.lane(driving_id)->widths[0].a, 4.25, 1e-12);
}

/// The regression this pillar exists to prevent. set_lane_width used to do
/// `widths = {Poly3{.a = width_m}}` unconditionally, so one constant-width
/// edit silently destroyed every taper on the lane — including one the user
/// had just carved, and any authored by a foreign .xodr.
TEST(LaneSections, SetLaneWidthRefusesToFlattenAWidthThatVariesAlongS) {
  RoadNetwork network;
  const RoadId road_id = author_straight(network, "1");
  const LaneSectionId first = network.road(road_id)->sections[0];
  LaneId driving_id{};
  for (const LaneId lane_id : network.lane_section(first)->lanes) {
    if (network.lane(lane_id)->odr_id == -1) {
      driving_id = lane_id;
    }
  }

  // A lane whose width varies through a coefficient...
  auto taper = roadmaker::edit::set_lane_width_profile(
      network, driving_id, {Poly3{.s = 0.0, .a = 3.0, .b = 0.01}});
  ASSERT_TRUE(taper->apply(network).has_value());
  expect_rejected(network, roadmaker::edit::set_lane_width(network, driving_id, 4.0));
  EXPECT_NEAR(network.lane(driving_id)->widths[0].b, 0.01, 1e-12) << "the taper must survive";

  // ...and one whose width varies through a second record.
  auto stepped = roadmaker::edit::set_lane_width_profile(
      network, driving_id, {Poly3{.s = 0.0, .a = 3.0}, Poly3{.s = 60.0, .a = 3.5}});
  ASSERT_TRUE(stepped->apply(network).has_value());
  expect_rejected(network, roadmaker::edit::set_lane_width(network, driving_id, 4.0));
  EXPECT_EQ(network.lane(driving_id)->widths.size(), 2U) << "the second record must survive";
}

// --- insert_lane: interior insert with renumbering (P2 #263) ----------------

/// odr ids present on a section, in the section's stored (descending) order.
std::vector<int> odr_ids(const RoadNetwork& network, LaneSectionId section_id) {
  std::vector<int> ids;
  for (const LaneId lane_id : network.lane_section(section_id)->lanes) {
    ids.push_back(network.lane(lane_id)->odr_id);
  }
  return ids;
}

TEST(LaneSections, InsertLaneRenumbersTheOuterBlockAndRoundTrips) {
  RoadNetwork network;
  const RoadId road_id = author_straight(network, "1");
  const LaneSectionId first = network.road(road_id)->sections[0];
  ASSERT_EQ(odr_ids(network, first), (std::vector<int>{1, 0, -1, -2}));

  // Insert at -1: the old -1 and -2 step out to -2 and -3, and a fresh lane
  // takes -1. The left side and center are untouched.
  auto command = roadmaker::edit::insert_lane(network, first, -1, LaneType::Driving);
  expect_command_round_trip(network, *command);
  ASSERT_TRUE(command->apply(network).has_value());
  EXPECT_EQ(odr_ids(network, first), (std::vector<int>{1, 0, -1, -2, -3}));
}

TEST(LaneSections, InsertLaneLeavesTheNewLaneUnlinkedAndRemapsNeighbourLinks) {
  RoadNetwork network;
  const RoadId road_id = author_straight(network, "1");
  ASSERT_TRUE(
      roadmaker::edit::split_lane_section(network, road_id, 60.0)->apply(network).has_value());
  const LaneSectionId head = network.road(road_id)->sections[0];
  const LaneSectionId tail = network.road(road_id)->sections[1];

  // Before: the head's -1 continues into the tail's -1, both ways.
  ASSERT_EQ(lane_by_odr(network, head, -1)->successor, -1);
  ASSERT_EQ(lane_by_odr(network, tail, -1)->predecessor, -1);

  auto command = roadmaker::edit::insert_lane(network, head, -1, LaneType::Driving);
  expect_command_round_trip(network, *command);
  ASSERT_TRUE(command->apply(network).has_value());

  // The inserted lane appears mid-road: no continuation into either neighbour.
  const Lane* inserted = lane_by_odr(network, head, -1);
  EXPECT_FALSE(inserted->predecessor.has_value());
  EXPECT_FALSE(inserted->successor.has_value());

  // The lane that used to be -1 is now -2 and still continues into the tail's
  // -1 (the tail was not renumbered); the tail's predecessor link followed the
  // renumbering from -1 to -2.
  EXPECT_EQ(lane_by_odr(network, head, -2)->successor, -1);
  EXPECT_EQ(lane_by_odr(network, tail, -1)->predecessor, -2);
}

TEST(LaneSections, InsertLaneRejectsBadPositions) {
  RoadNetwork network;
  const RoadId road_id = author_straight(network, "1");
  const LaneSectionId first = network.road(road_id)->sections[0];

  expect_rejected(network, roadmaker::edit::insert_lane(network, first, 0, LaneType::Driving));
  // No lane at -5: appending past the outermost is add_lane's job.
  expect_rejected(network, roadmaker::edit::insert_lane(network, first, -5, LaneType::Driving));
  expect_rejected(network,
                  roadmaker::edit::insert_lane(network, LaneSectionId{}, -1, LaneType::Driving));
}
