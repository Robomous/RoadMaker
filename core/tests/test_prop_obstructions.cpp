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

// cascade-s4 (#464) — the prop-obstruction query. A prop obstructs when its
// DECLARED bounding volume overlaps another road's driving band, a junction
// floor, or another prop, in plan view and in height. What it must NOT flag is
// most of the file: a median tree, a kerbside streetlight, a corner pole at a
// signalised junction and a zebra crossing are all correct scenes.

#include "roadmaker/mesh/prop_obstructions.hpp"

#include "roadmaker/assets/prop_library.hpp"
#include "roadmaker/edit/operations.hpp"
#include "roadmaker/mesh/mesh_builder.hpp"
#include "roadmaker/road/authoring.hpp"
#include "roadmaker/road/network.hpp"
#include "roadmaker/road/object.hpp"
#include "roadmaker/road/road.hpp"

#include "support/network_compare.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <numbers>
#include <string>
#include <vector>

namespace roadmaker {
namespace {

using edit::create_junction;
using edit::JunctionGenOptions;

constexpr double kPi = std::numbers::pi;

/// A straight road from `a` to `b`. `collector()` is the default cross section:
/// one 3.3 m driving lane each way, plus a 1.8 m shoulder on the right — so the
/// DRIVING band is |t| <= 3.3 and the full cross section reaches t = -5.1.
RoadId straight_road(RoadNetwork& network,
                     Waypoint a,
                     Waypoint b,
                     const char* id,
                     LaneProfile profile = LaneProfile::collector()) {
  const std::array<Waypoint, 2> waypoints{a, b};
  return author_clothoid_road(network, waypoints, std::move(profile), "", id).value();
}

/// A box-shaped prop anchored at (s, t) on `road`, `length` along its own u
/// axis and `width` across it. Added through the arena so a test can also
/// author the states an import can produce.
ObjectId box_prop(RoadNetwork& network,
                  RoadId road,
                  double s,
                  double t,
                  double length,
                  double width,
                  double hdg = 0.0,
                  const char* odr_id = "1") {
  Object object;
  object.road = road;
  object.odr_id = odr_id;
  object.name = "barrier";
  object.type = ObjectType::Barrier;
  object.s = s;
  object.t = t;
  object.hdg = hdg;
  object.length = length;
  object.width = width;
  object.height = 1.0;
  return network.add_object(road, std::move(object));
}

/// A cylindrical prop — the shape RoadMaker itself authors, since
/// edit::set_object_model seeds @radius and @height from the prop library.
ObjectId round_prop(RoadNetwork& network,
                    RoadId road,
                    double s,
                    double t,
                    double radius,
                    const char* odr_id = "1") {
  Object object;
  object.road = road;
  object.odr_id = odr_id;
  object.name = "tree_pine";
  object.type = ObjectType::Tree;
  object.s = s;
  object.t = t;
  object.radius = radius;
  object.height = 4.0;
  return network.add_object(road, std::move(object));
}

// -----------------------------------------------------------------------------
// T2 — the one a naive implementation passes
// -----------------------------------------------------------------------------

/// THE test to write first. "Is the prop's origin inside the road band?" passes
/// every obvious case — a prop in the middle of a lane, a prop far away, and
/// even the #338 rotation case, whose arc lands the origin on the road. It
/// fails exactly where the feature earns its keep: a long prop lying ACROSS a
/// carriageway has no corner inside the band, and no band vertex inside it.
/// Only the edge-vs-edge pass finds it.
TEST(PropObstructions, ALongPropStraddlingARoadIsReportedThoughNoVertexLiesInside) {
  RoadNetwork network;
  const RoadId anchor = straight_road(network, {-50, 20}, {50, 20}, "anchor");
  straight_road(network, {-50, 0}, {50, 0}, "crossed");

  // 12 m x 0.4 m, laid across the crossed road's 6.6 m driving band: it spans
  // y in [-6, +6] while the band is |y| <= 3.3, so every corner is outside it.
  // The band's own vertices sit at the road ends, 50 m away in x, so none of
  // them is inside a box 0.4 m wide.
  const ObjectId prop = box_prop(network, anchor, 50.0, -20.0, 12.0, 0.4, kPi / 2.0);

  const std::vector<PropObstruction> found = find_prop_obstructions(network);
  ASSERT_EQ(found.size(), 1U) << "a prop lying across a carriageway must be flagged";
  EXPECT_EQ(found[0].object, prop);
  EXPECT_EQ(found[0].kind, ObstructionKind::RoadSurface);
  EXPECT_NEAR(found[0].at[1], 3.3, 0.5) << "the witness lies where the shapes actually meet";

  // The control: the same prop clear of the band reports nothing, so an
  // implementation that always reports does not pass either.
  network.object(prop)->t = -40.0; // 20 m clear of the crossed road
  EXPECT_TRUE(find_prop_obstructions(network).empty());
}

// -----------------------------------------------------------------------------
// The legitimacy rules — what must NOT be flagged
// -----------------------------------------------------------------------------

/// R1. A prop is placed in its anchor road's frame; sitting on, beside or
/// inside it IS the placement. This is what makes a median tree, a bollard and
/// a kerbside streetlight legal, and it is why no separate "prop inside its own
/// lanes" rule exists.
TEST(PropObstructions, APropIsNeverFlaggedAgainstItsOwnAnchorRoad) {
  RoadNetwork network;
  const RoadId road = straight_road(network, {-50, 0}, {50, 0}, "own");
  round_prop(network, road, 50.0, 0.0, 0.6); // dead centre of its own carriageway
  EXPECT_TRUE(find_prop_obstructions(network).empty());
}

/// R2/R3. Two roads a junction connects meet by design. Without this, every
/// corner streetlight at a signalised junction is a false positive, because the
/// connecting roads fan across the whole intersection.
TEST(PropObstructions, APropOnAnApproachArmIsNotFlaggedAgainstTheJunctionItServes) {
  RoadNetwork network;
  const RoadId west = straight_road(network, {-60, 0}, {-10, 0}, "west");
  const RoadId east = straight_road(network, {10, 0}, {60, 0}, "east");
  const RoadId north = straight_road(network, {0, 10}, {0, 60}, "north");
  const std::array<RoadEnd, 3> ends{RoadEnd{.road = west, .contact = ContactPoint::End},
                                    RoadEnd{.road = east, .contact = ContactPoint::Start},
                                    RoadEnd{.road = north, .contact = ContactPoint::Start}};
  ASSERT_TRUE(create_junction(network, ends, JunctionGenOptions{})->apply(network).has_value());

  // A pole at the west arm's kerb, right at the intersection mouth.
  round_prop(network, west, 49.0, -4.0, 0.6, "pole");
  EXPECT_TRUE(find_prop_obstructions(network).empty())
      << "an arm's own kerbside prop is not obstructing the junction it serves";

  // The second half is what stops "always return empty" from passing: the SAME
  // prop against an unconnected road running through the same place is flagged.
  const RoadId stranger = straight_road(network, {-60, -4}, {60, -4}, "stranger");
  ASSERT_TRUE(network.road(stranger) != nullptr);
  const std::vector<PropObstruction> found = find_prop_obstructions(network);
  ASSERT_FALSE(found.empty()) << "an unconnected road running under the prop IS an obstruction";
  EXPECT_EQ(found[0].kind, ObstructionKind::RoadSurface);
  EXPECT_EQ(found[0].road, stranger);
}

/// R4. The road side is the DRIVING band, not the full cross section. The edge
/// cursor is the trap: lane_boundary_offsets covers only the width-bearing
/// lanes, because the centre lane 0 is a boundary and not a span. Advancing it
/// for lane 0 too puts the band edge one lane out — onto the sidewalk — on
/// every road with a centre lane, which is every road this editor authors.
TEST(PropObstructions, DrivingBandExcludesTheSidewalkOnARoadWithACentreLane) {
  RoadNetwork network;
  const RoadId anchor = straight_road(network, {-50, 30}, {50, 30}, "anchor");
  straight_road(network, {-50, 0}, {50, 0}, "street", LaneProfile::local_road());

  const double lane = defaults::driving_lane_width(defaults::RoadClass::Local);
  // Centred on the sidewalk band, which starts where the driving lane ends.
  const double sidewalk_centre = lane + (defaults::kSidewalkWidth * 0.5);
  const ObjectId prop = round_prop(network, anchor, 50.0, -(30.0 - sidewalk_centre), 0.4);
  EXPECT_TRUE(find_prop_obstructions(network).empty())
      << "a prop standing on a neighbouring road's sidewalk is where props belong";

  // One lane inboard, onto the carriageway.
  network.object(prop)->t = -(30.0 - (lane * 0.5));
  const std::vector<PropObstruction> found = find_prop_obstructions(network);
  ASSERT_EQ(found.size(), 1U);
  EXPECT_EQ(found[0].kind, ObstructionKind::RoadSurface);
}

/// Paint is skipped wholesale: §13.1 says an <outline> supersedes the bounding
/// volume, and every object carrying one in this product is a crosswalk,
/// marking curve or stencil — coplanar with the road by construction. Without
/// this, every zebra crossing is an obstruction on every save.
TEST(PropObstructions, AZebraCrossingIsNeverFlagged) {
  RoadNetwork network;
  const RoadId anchor = straight_road(network, {-50, 20}, {50, 20}, "anchor");
  straight_road(network, {-50, 0}, {50, 0}, "crossed");

  // A crosswalk's bounding box genuinely covers the carriageway — that is what
  // a crosswalk is. Only the outline/type filter keeps it out of the report.
  const ObjectId paint = box_prop(network, anchor, 50.0, -20.0, 8.0, 4.0, kPi / 2.0);
  network.object(paint)->type = ObjectType::Crosswalk;
  EXPECT_TRUE(find_prop_obstructions(network).empty());

  // The same volume typed as a barrier, carrying an outline instead, is still
  // skipped — the outline is what supersedes the volume.
  network.object(paint)->type = ObjectType::Barrier;
  ObjectOutline outline;
  outline.road_coords = true;
  outline.closed = true;
  network.object(paint)->outlines.push_back(std::move(outline));
  EXPECT_TRUE(find_prop_obstructions(network).empty());

  // Control: strip both and the very same object IS reported, so the filter is
  // doing the work rather than the geometry missing.
  network.object(paint)->outlines.clear();
  EXPECT_EQ(find_prop_obstructions(network).size(), 1U);
}

/// R5. A repeat series tighter than its own diameter is a hedge, authored that
/// way on purpose; two instances of the same object never flag each other.
TEST(PropObstructions, AHedgeDoesNotObstructItself) {
  RoadNetwork network;
  const RoadId road = straight_road(network, {-50, 0}, {50, 0}, "own");
  Object object;
  object.road = road;
  object.odr_id = "hedge";
  object.name = "shrub";
  object.type = ObjectType::Vegetation;
  object.s = 10.0;
  object.t = 8.0; // clear of its own driving band, and of every other road
  object.radius = 1.0;
  object.height = 1.2;
  ObjectRepeat repeat;
  repeat.s = 10.0;
  repeat.length = 40.0;
  repeat.distance = 0.5; // far tighter than the 2 m diameter
  repeat.t_start = 8.0;
  repeat.t_end = 8.0;
  object.repeats.push_back(repeat);
  network.add_object(road, std::move(object));

  EXPECT_TRUE(find_prop_obstructions(network).empty());
}

// -----------------------------------------------------------------------------
// The footprint model
// -----------------------------------------------------------------------------

/// §13.1 Table 85 fixes @length along local u and @width along v, but the spec
/// settles the ORIGIN in a figure rather than in text. The implementation takes
/// the origin as the centre in u/v; this test derives its numbers from that
/// assumption explicitly, so a wrong assumption fails here instead of sitting
/// every rectangle length/2 off.
TEST(PropObstructions, ARectangularPropIsCentredOnItsOrigin) {
  RoadNetwork network;
  const RoadId anchor = straight_road(network, {-50, 20}, {50, 20}, "anchor");
  straight_road(network, {-50, 0}, {50, 0}, "crossed");

  // Origin 9 m from the crossed road's centreline, long axis pointing at it.
  // Centred: the box reaches 9 - 5 = 4 m short of the band edge at 3.3 -> clear.
  // Corner-anchored: it would reach the centreline and be flagged.
  const ObjectId prop = box_prop(network, anchor, 50.0, -11.0, 10.0, 1.0, kPi / 2.0);
  EXPECT_TRUE(find_prop_obstructions(network).empty())
      << "a centred 10 m box at 9 m reaches only to 4 m; a corner-anchored one would not";

  // Lengthening it to 12 m reaches 9 - 6 = 3 m, just inside the 3.3 m edge.
  network.object(prop)->length = 12.0;
  EXPECT_EQ(find_prop_obstructions(network).size(), 1U);
}

/// @radius wins over @length x @width when an object illegally carries both
/// (road.object.circular_vs_angular). Chosen by decision, not by accident:
/// @radius is the channel RoadMaker itself writes.
TEST(PropObstructions, ARadiusWinsOverAnIllegalSecondBoundingVolume) {
  RoadNetwork network;
  const RoadId anchor = straight_road(network, {-50, 20}, {50, 20}, "anchor");
  straight_road(network, {-50, 0}, {50, 0}, "crossed");

  // A small circle that misses, and a long box that would hit.
  const ObjectId prop = box_prop(network, anchor, 50.0, -11.0, 20.0, 1.0, kPi / 2.0);
  network.object(prop)->radius = 0.5;
  EXPECT_TRUE(find_prop_obstructions(network).empty()) << "the circle is the volume used";

  network.object(prop)->radius.reset();
  EXPECT_EQ(find_prop_obstructions(network).size(), 1U) << "and the box is what it fell back to";
}

/// The absent-dimension policy: no usable bounding volume means NOT CHECKED,
/// and nothing is ever invented. The byte-identity assertion is the part that
/// bites — a query that helpfully back-filled @radius from the prop library
/// would still return empty and pass a weaker version of this test.
TEST(PropObstructions, APropWithNoBoundingVolumeIsNotCheckedAndNothingIsInvented) {
  RoadNetwork network;
  const RoadId anchor = straight_road(network, {-50, 20}, {50, 20}, "anchor");
  straight_road(network, {-50, 0}, {50, 0}, "crossed");
  const ObjectId prop = round_prop(network, anchor, 50.0, -20.0, 2.0);

  ASSERT_EQ(find_prop_obstructions(network).size(), 1U)
      << "with a radius it IS flagged — otherwise the empty cases below prove nothing";

  const auto clear_all = [&] {
    Object* object = network.object(prop);
    object->radius.reset();
    object->length.reset();
    object->width.reset();
    object->height.reset();
  };

  clear_all();
  const std::string before = test::snapshot_xodr(network);
  EXPECT_TRUE(find_prop_obstructions(network).empty());
  EXPECT_EQ(test::snapshot_xodr(network), before)
      << "the query must not write a dimension it was not given";

  network.object(prop)->radius = 0.0; // present but not a volume
  EXPECT_TRUE(find_prop_obstructions(network).empty());

  network.object(prop)->radius.reset();
  network.object(prop)->length = 4.0; // length without width is not a box
  EXPECT_TRUE(find_prop_obstructions(network).empty());
}

/// An absent @height is NOT a skip: the vertical span collapses to a slab at
/// the base and the prop is still checked in plan view. The conservative
/// reading of "no invented dimensions" is a zero-thickness prop, not an
/// unchecked one.
TEST(PropObstructions, APropWithNoHeightIsStillCheckedInPlanView) {
  RoadNetwork network;
  const RoadId anchor = straight_road(network, {-50, 20}, {50, 20}, "anchor");
  straight_road(network, {-50, 0}, {50, 0}, "crossed");
  const ObjectId prop = round_prop(network, anchor, 50.0, -20.0, 2.0);
  network.object(prop)->height.reset();
  EXPECT_EQ(find_prop_obstructions(network).size(), 1U);
}

/// The declared dimensions are the OpenDRIVE truth a validator rule speaks, and
/// they work for a foreign prop the library has never heard of. They are NOT
/// the rendered size: props::instance_scale scales the whole model by
/// @height / model.height and its own comment says @radius rides along. Editing
/// @height therefore grows the drawn crown past the declared circle. The fix
/// belongs wherever @height is authored, not here — named so this decision
/// cannot quietly become a bug report.
TEST(PropObstructions, UsesDeclaredDimensionsNotTheRenderScale) {
  const props::PropModel* pine = props::model("tree_pine");
  ASSERT_NE(pine, nullptr);

  RoadNetwork network;
  const RoadId anchor = straight_road(network, {-50, 20}, {50, 20}, "anchor");
  straight_road(network, {-50, 0}, {50, 0}, "crossed");

  // Declared radius reaches to 20 - 2 = 18 m from the anchor: nowhere near the
  // band edge at 3.3 m. Scaled 8x by the height edit it would sweep 16 m and
  // hit. The declared circle is what the query uses.
  const ObjectId prop = round_prop(network, anchor, 50.0, -20.0, 2.0);
  network.object(prop)->t = -14.0; // gap of 14 - 3.3 - 2 = 8.7 m
  network.object(prop)->height = pine->height * 8.0;
  EXPECT_TRUE(find_prop_obstructions(network).empty());
}

// -----------------------------------------------------------------------------
// Agreement with what the renderer draws
// -----------------------------------------------------------------------------

/// A <repeat> with @distance > 0 SUPPRESSES the object's base instance (§13.4).
/// The base here sits squarely on the crossed road while every series instance
/// is clear of it, so an implementation that forgets the suppression reports
/// exactly one obstruction that the user cannot see.
TEST(PropObstructions, ARepeatSeriesSuppressesTheBaseInstanceItReplaces) {
  RoadNetwork network;
  const RoadId anchor = straight_road(network, {-50, 20}, {50, 20}, "anchor");
  straight_road(network, {-50, 0}, {50, 0}, "crossed");

  Object object;
  object.road = anchor;
  object.odr_id = "line";
  object.name = "tree_pine";
  object.type = ObjectType::Tree;
  object.s = 50.0;
  object.t = -20.0; // the base would sit on the crossed road's centreline
  object.radius = 1.0;
  object.height = 4.0;
  ObjectRepeat repeat;
  repeat.s = 5.0;
  repeat.length = 30.0;
  repeat.distance = 6.0;
  repeat.t_start = 6.0; // the series runs along the anchor road, well clear
  repeat.t_end = 6.0;
  object.repeats.push_back(repeat);
  network.add_object(anchor, std::move(object));

  EXPECT_TRUE(find_prop_obstructions(network).empty())
      << "the base instance the series replaces is not drawn, so it cannot obstruct";
}

/// The query and the mesher must agree instance for instance — they share
/// object_placement::placed_instances, and this is what proves the sharing is
/// load-bearing rather than incidental.
TEST(PropObstructions, RepeatSeriesAgreesInstanceForInstanceWithTheMeshedProps) {
  RoadNetwork network;
  const RoadId anchor = straight_road(network, {-50, 20}, {50, 20}, "anchor");
  straight_road(network, {-50, 0}, {50, 0}, "crossed");

  Object object;
  object.road = anchor;
  object.odr_id = "line";
  object.name = "tree_pine";
  object.type = ObjectType::Tree;
  object.s = 0.0;
  object.t = 0.0;
  object.radius = 1.0;
  object.height = 4.0;
  ObjectRepeat repeat;
  repeat.s = 40.0;
  repeat.length = 20.0;
  repeat.distance = 5.0;
  repeat.t_start = -20.0; // the whole series sits on the crossed road
  repeat.t_end = -20.0;
  object.repeats.push_back(repeat);
  const ObjectId prop = network.add_object(anchor, std::move(object));

  const NetworkMesh mesh = build_network_mesh(network);
  std::size_t drawn = 0;
  for (const ObjectInstance& instance : mesh.objects) {
    if (instance.object == prop) {
      ++drawn;
    }
  }
  ASSERT_GT(drawn, 1U) << "fixture must produce a real series";

  const std::vector<PropObstruction> found = find_prop_obstructions(network);
  ASSERT_EQ(found.size(), drawn) << "every drawn instance is checked, and only those";
  for (std::size_t i = 0; i < found.size(); ++i) {
    EXPECT_EQ(found[i].instance, i) << "instance indices are the mesher's own";
  }
}

/// ReferenceLine::evaluate CLAMPS s to [0, length], so a prop authored past a
/// road's end — which an import can hand us, and which a shortening re-fit
/// produces — piles onto the end cap. The mesher clamps identically, so the
/// query must report it as DRAWN. "Fixing" this by skipping s > length makes
/// the flag disagree with the picture.
TEST(PropObstructions, PropsPastTheRoadEndClampAndCollideAsRendered) {
  RoadNetwork network;
  const RoadId anchor = straight_road(network, {-50, 20}, {0, 20}, "anchor");
  straight_road(network, {-50, 0}, {50, 0}, "crossed");

  const double length = network.road(anchor)->plan_view.length();
  // s far past the end: the pose clamps to the anchor's end at (0, 20), and t
  // carries it down onto the crossed road.
  round_prop(network, anchor, length + 500.0, -20.0, 1.5);

  const std::vector<PropObstruction> found = find_prop_obstructions(network);
  ASSERT_EQ(found.size(), 1U);
  EXPECT_NEAR(found[0].at[0], 0.0, 2.0) << "flagged where the clamp actually draws it";
}

// -----------------------------------------------------------------------------
// The other surfaces
// -----------------------------------------------------------------------------

TEST(PropObstructions, APropStandingInAJunctionFloorIsFlaggedAsTheFloor) {
  RoadNetwork network;
  const RoadId west = straight_road(network, {-60, 0}, {-10, 0}, "west");
  const RoadId east = straight_road(network, {10, 0}, {60, 0}, "east");
  const RoadId north = straight_road(network, {0, 10}, {0, 60}, "north");
  const std::array<RoadEnd, 3> ends{RoadEnd{.road = west, .contact = ContactPoint::End},
                                    RoadEnd{.road = east, .contact = ContactPoint::Start},
                                    RoadEnd{.road = north, .contact = ContactPoint::Start}};
  ASSERT_TRUE(create_junction(network, ends, JunctionGenOptions{})->apply(network).has_value());

  // Anchored to an unrelated road that passes well north of the intersection,
  // with a t that reaches all the way down into the floor.
  const RoadId stranger = straight_road(network, {-40, 40}, {40, 40}, "stranger");
  round_prop(network, stranger, 40.0, -40.0, 1.5); // world (0, 0): the floor's middle

  const std::vector<PropObstruction> found = find_prop_obstructions(network);
  ASSERT_FALSE(found.empty());
  EXPECT_EQ(found[0].kind, ObstructionKind::JunctionFloor)
      << "the floor is named, not each of the turns that tile it";
  EXPECT_TRUE(found[0].junction.is_valid());
}

TEST(PropObstructions, TwoPropsSharingAPlaceAreFlaggedAgainstEachOther) {
  RoadNetwork network;
  const RoadId a = straight_road(network, {-50, 0}, {50, 0}, "a");
  const RoadId b = straight_road(network, {-50, 30}, {50, 30}, "b");

  round_prop(network, a, 50.0, 12.0, 1.5, "one");  // world (0, 12)
  round_prop(network, b, 50.0, -17.0, 1.5, "two"); // world (0, 13)

  const std::vector<PropObstruction> found = find_prop_obstructions(network);
  ASSERT_EQ(found.size(), 1U) << "reported once, not once from each side";
  EXPECT_EQ(found[0].kind, ObstructionKind::Prop);
  EXPECT_TRUE(found[0].other.is_valid());
}

/// The gate is 2.5D, not 2D: pure plan view flags every prop beside an
/// overpass, and #233/#463 made this a grade-separated world.
TEST(PropObstructions, APropOnADeckAboveARoadIsNotObstructingIt) {
  RoadNetwork network;
  const RoadId anchor = straight_road(network, {-50, 20}, {50, 20}, "anchor");
  straight_road(network, {-50, 0}, {50, 0}, "below");

  const ObjectId prop = round_prop(network, anchor, 50.0, -20.0, 2.0);
  ASSERT_EQ(find_prop_obstructions(network).size(), 1U) << "at grade it IS an obstruction";

  // Raised onto a deck 8 m up: the road below is far outside its vertical span.
  network.object(prop)->z_offset = 8.0;
  EXPECT_TRUE(find_prop_obstructions(network).empty());

  // But a tall prop at grade still grows through a deck the same distance away.
  network.object(prop)->z_offset = 0.0;
  network.object(prop)->height = 12.0;
  EXPECT_EQ(find_prop_obstructions(network).size(), 1U);
}

// -----------------------------------------------------------------------------
// Numerics and robustness
// -----------------------------------------------------------------------------

/// Clipper2Lib::PointInPolygon is unsound on a double path — its
/// CrossProductSign casts the edge deltas to __int128_t, so sub-metre
/// differences truncate to zero and interior points report "on the edge"
/// (#442). Far from the origin is where a reimplementation reaching for it
/// breaks: it is the coordinate DELTAS that truncate.
TEST(PropObstructions, PropInsideARoadBandIsReportedAtUtmScale) {
  RoadNetwork network;
  const RoadId anchor =
      straight_road(network, {500000, 4500020}, {500100, 4500020}, "anchor");
  straight_road(network, {500000, 4500000}, {500100, 4500000}, "crossed");

  round_prop(network, anchor, 50.0, -20.0, 0.3); // 0.3 m radius, dead centre
  EXPECT_EQ(find_prop_obstructions(network).size(), 1U);
}

/// The ring is built left-forward then right-reversed, which winds one way for
/// a road authored west-to-east and the other for the same road authored
/// east-to-west. The even-odd ray cast is winding-agnostic; an implementation
/// that reached for a NonZero fill rule instead would pass one and fail the
/// other.
TEST(PropObstructions, PropInsideARoadBuiltInEitherDirection) {
  for (const bool reversed : {false, true}) {
    SCOPED_TRACE(reversed ? "authored east to west" : "authored west to east");
    RoadNetwork network;
    const RoadId anchor = straight_road(network, {-50, 20}, {50, 20}, "anchor");
    if (reversed) {
      straight_road(network, {50, 0}, {-50, 0}, "crossed");
    } else {
      straight_road(network, {-50, 0}, {50, 0}, "crossed");
    }
    round_prop(network, anchor, 50.0, -20.0, 1.0);
    EXPECT_EQ(find_prop_obstructions(network).size(), 1U);
  }
}

TEST(PropObstructions, DegenerateRoadsAndLanelessSectionsAreSkippedNotCrashed) {
  RoadNetwork network;
  const RoadId anchor = straight_road(network, {-50, 20}, {50, 20}, "anchor");
  round_prop(network, anchor, 50.0, -20.0, 1.0);

  // A road with no plan view at all — the state an in-progress authoring
  // session and a malformed import can both produce.
  (void)network.create_road("bare", "bare");

  EXPECT_NO_THROW({ (void)find_prop_obstructions(network); });
  EXPECT_TRUE(find_prop_obstructions(network).empty());
}

/// Traversal order must never leak into the result: the cascade stage diffs two
/// runs, and an unstable order would show it phantom changes every frame.
/// Without the explicit sort this test is FLAKY rather than failing, which is
/// the worse outcome.
TEST(PropObstructions, DeterministicAcrossRepeatedQueries) {
  RoadNetwork network;
  const RoadId anchor = straight_road(network, {-50, 20}, {50, 20}, "anchor");
  straight_road(network, {-50, 0}, {50, 0}, "crossed");
  straight_road(network, {-50, -20}, {50, -20}, "further");
  round_prop(network, anchor, 30.0, -20.0, 1.5, "one");
  round_prop(network, anchor, 60.0, -40.0, 1.5, "two");

  const std::vector<PropObstruction> first = find_prop_obstructions(network);
  ASSERT_GE(first.size(), 2U);
  EXPECT_EQ(first, find_prop_obstructions(network));
}

// -----------------------------------------------------------------------------
// #338 — the risk this sprint owns by name
// -----------------------------------------------------------------------------

/// A prop anchored at large |t| sweeps a large ARC when its anchor road is
/// rotated: the transform is derived from the road frame, so the prop travels
/// with the heading rather than with the road body. #338 accepted this and
/// deferred the mitigation here by name.
TEST(PropObstructions, APropAtLargeTIsFlaggedWhenItsAnchorRoadIsRotated) {
  RoadNetwork network;
  const RoadId anchor = straight_road(network, {-50, 40}, {50, 40}, "anchor");
  straight_road(network, {-50, 15}, {50, 15}, "crossed");

  // 25 m out on the FAR side of its anchor, at world (0, 65) — 50 m clear of
  // the crossed road, with the anchor road itself in between.
  round_prop(network, anchor, 50.0, 25.0, 1.5);
  ASSERT_TRUE(find_prop_obstructions(network).empty()) << "nothing wrong before the gesture";

  // The arc a rotation sweeps a prop through is a CIRCLE about the pivot with
  // radius |prop - pivot| — 25 m about the road's own midpoint (0, 40) here.
  // Half a turn carries the prop from (0, 65) to (0, 15), straight onto the
  // crossed road, while the anchor road (a line through the pivot) maps onto
  // itself and never goes near it. Nothing about the prop's own data changed.
  ASSERT_TRUE(edit::rotate_road(network, anchor, kPi, 0.0, 40.0)
                  ->apply(network)
                  .has_value());

  const std::vector<PropObstruction> found = find_prop_obstructions(network);
  ASSERT_EQ(found.size(), 1U) << "#338: the rotation arc drove the prop into another road";
  EXPECT_EQ(found[0].kind, ObstructionKind::RoadSurface);
}

// -----------------------------------------------------------------------------
// The narrowed (per-frame) overload
// -----------------------------------------------------------------------------

TEST(PropObstructions, TheNarrowedOverloadSeesBothDirectionsOfAMove) {
  RoadNetwork network;
  const RoadId anchor = straight_road(network, {-50, 20}, {50, 20}, "anchor");
  const RoadId crossed = straight_road(network, {-50, 0}, {50, 0}, "crossed");
  const RoadId elsewhere = straight_road(network, {-50, 200}, {50, 200}, "elsewhere");
  round_prop(network, anchor, 50.0, -20.0, 1.5);

  const std::vector<PropObstruction> all = find_prop_obstructions(network);
  ASSERT_EQ(all.size(), 1U);

  // Named through the prop's own anchor road...
  const std::array<RoadId, 1> moved_anchor{anchor};
  EXPECT_EQ(find_prop_obstructions(network, moved_anchor), all);
  // ...and through the road it was driven into, which carries no prop at all.
  const std::array<RoadId, 1> moved_crossed{crossed};
  EXPECT_EQ(find_prop_obstructions(network, moved_crossed), all);
  // A road that has nothing to do with it narrows the answer to nothing.
  const std::array<RoadId, 1> moved_elsewhere{elsewhere};
  EXPECT_TRUE(find_prop_obstructions(network, moved_elsewhere).empty());
}

} // namespace
} // namespace roadmaker
