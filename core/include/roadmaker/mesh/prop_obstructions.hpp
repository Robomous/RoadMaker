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

#pragma once

#include "roadmaker/export.hpp"
#include "roadmaker/geometry/reference_line.hpp"
#include "roadmaker/road/network.hpp"

#include <array>
#include <span>
#include <vector>

namespace roadmaker {

/// What a prop's bounding volume runs into (cascade-s4, issue #464).
enum class ObstructionKind {
  RoadSurface,   ///< another road's DRIVING band
  JunctionFloor, ///< a junction's floor
  Prop,          ///< another prop's bounding volume
};

/// One prop instance overlapping one thing it should not.
///
/// Plain data, no out-of-line members: a member defined in the .cpp would need
/// its own RM_API under RM_BUILD_SHARED=ON (see JunctionSurfaceSpanInfo).
struct PropObstruction {
  ObjectId object;          ///< the flagged prop
  std::size_t instance = 0; ///< index into its placements; 0 = the single one

  ObstructionKind kind = ObstructionKind::RoadSurface;

  RoadId road;         ///< RoadSurface: what it blocks. Invalid otherwise.
  JunctionId junction; ///< JunctionFloor: what it blocks. Invalid otherwise.
  ObjectId other;      ///< Prop: the other prop. Invalid otherwise.
  std::size_t other_instance = 0;

  /// World (x, y) [m] of a point proven to lie in BOTH shapes — the witness the
  /// editor can zoom to and the relocation searches away from. Not a centroid
  /// and not an average: an actual witness.
  std::array<double, 2> at{};

  friend bool operator==(const PropObstruction&, const PropObstruction&) = default;
};

/// Vertical slack [m] on the 2.5D gate. Non-positive disables the vertical gate
/// entirely, making the query pure plan-view.
inline constexpr double kPropVerticalClearance = 0.5;

struct PropObstructionOptions {
  double vertical_clearance = kPropVerticalClearance;
  /// Must match what the mesh build uses, or the bands tested against will not
  /// be the ones the user is looking at.
  SamplingOptions sampling{};
};

/// Every prop obstruction in the network, sorted by
/// (object slot, instance, kind, other id) so repeated calls compare equal.
///
/// WHAT COUNTS AS AN OBSTRUCTION. A prop's bounding volume overlapping, in plan
/// view and in height, another road's DRIVING band, a junction floor, or
/// another prop. The five exclusions are not incidental — without them the
/// query flags ordinary, correct scenes:
///
///  R1. Never against its OWN anchor road. A prop is placed in that road's
///      frame; sitting on, beside or inside it is the placement. This is what
///      silences every median tree, bollard and kerbside streetlight, and it is
///      why no separate "prop inside its own lanes" rule is needed — that case
///      falls out of R1 for free.
///  R2. Never against a road a junction connects to the anchor road (all three
///      slots of road_detail::touched_junctions). Without it, every corner
///      streetlight at a signalised junction is a false positive, because
///      connecting roads fan across the whole intersection.
///  R3. Never against a junction floor the anchor road is an arm of.
///  R4. The road side is the DRIVING band, not the full cross section. A prop
///      standing on a neighbouring road's verge or sidewalk is where props
///      belong.
///  R5. Never two instances of the SAME object: a repeat series tighter than
///      its own diameter is a hedge, authored that way on purpose. Across
///      objects a pair is reported once.
///
/// FOOTPRINT. Declared dimensions only, per instance: @radius (a circle) wins
/// over @length x @width (a rectangle along the instance's world heading, §13.1
/// Table 85 — @length along local u, @width along v). Carrying both is
/// spec-illegal (road.object.circular_vs_angular); the circle is chosen, by
/// decision rather than by accident, because @radius is the channel RoadMaker
/// itself writes. The origin point is taken as the volume's CENTRE in u/v:
/// §13.1 says only "the position of its origin point" and settles this in a
/// figure, so the assumption is named here and pinned by a test that derives
/// its expected numbers from it.
///
/// ABSENT DIMENSIONS ARE NEVER INVENTED. A prop with no usable bounding volume
/// — no @radius > 0 and not both @length > 0 and @width > 0 — is NOT
/// obstruction-checked, and this query writes nothing anywhere. An absent
/// @height is not a skip: the vertical span is a zero-thickness slab at the
/// base, so the prop is still checked in plan view.
///
/// PAINT IS SKIPPED WHOLESALE. An object carrying an <outline>, or RoadMaker
/// crosswalk / marking-curve / stencil data, or typed Crosswalk, is excluded:
/// §13.1 says an outline supersedes the bounding volume, and every such object
/// in this product is paint, coplanar with the road by construction. Testing
/// their bounding boxes would flag every zebra crossing on every save.
///
/// WHAT THIS DELIBERATELY DOES NOT CATCH: bridge SOLIDS (deck thickness and
/// piers are not modelled — only the carrying road's surface height is), the
/// terrain height field (P5's), <signal> posts (a separate arena; ObstructionKind
/// is the extension point), and pitch/roll tilt (the footprint is the untilted
/// plan projection, which is what the renderer draws).
[[nodiscard]] RM_API std::vector<PropObstruction>
find_prop_obstructions(const RoadNetwork& network, const PropObstructionOptions& options = {});

/// The same query narrowed to a move: an obstruction is reported when the
/// prop's ANCHOR road, the obstructed road, or the other prop's anchor road is
/// in `touching`. Both directions matter — a moved road can carry its own prop
/// into a stationary road AND sweep its surface under a stationary prop.
///
/// This is the per-frame form. It builds bands only for roads whose bounds
/// overlap the candidate props', so a drag pays for its neighbourhood rather
/// than for the network.
[[nodiscard]] RM_API std::vector<PropObstruction>
find_prop_obstructions(const RoadNetwork& network,
                       std::span<const RoadId> touching,
                       const PropObstructionOptions& options = {});

} // namespace roadmaker
