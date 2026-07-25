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

#include "roadmaker/error.hpp"
#include "roadmaker/export.hpp"
#include "roadmaker/road/id.hpp"
#include "roadmaker/road/object.hpp" // ObjectOrientation

namespace roadmaker {

class RoadNetwork;

/// Which way traffic runs along a road's reference line, as a signed factor:
/// +1 travels toward increasing s, -1 toward decreasing s.
enum class TravelDirection {
  Forward,
  Backward,
};

/// The facing a sign or signal should carry: the OpenDRIVE @orientation whose
/// traffic it applies to, plus the @hOffset measured from THAT orientation's
/// datum.
///
/// The datum is the subtlety. ASAM OpenDRIVE 1.9.0 §14.1: a signal with
/// @orientation="+" applies to traffic travelling in the positive reference
/// line direction, and "any @hOffset given to this signal is applied
/// counter-clockwise from the NEGATIVE road reference line direction" — the
/// axis OPPOSITE the one the face looks along. So:
///
///   Plus  -> face datum = road heading + pi   (looks back down -s)
///   Minus -> face datum = road heading        (looks up +s)
///   None  -> face datum = road heading        (§14.1: relative to the
///                                              reference line)
///
/// `h_offset` is then a counter-clockwise rotation from that datum, exactly as
/// the mesher and the writer treat it.
struct SignalFacing {
  ObjectOrientation orientation = ObjectOrientation::None;

  /// Heading offset from the orientation's datum [rad], counter-clockwise.
  double h_offset = 0.0;

  friend constexpr bool operator==(const SignalFacing&, const SignalFacing&) = default;
};

/// The travel direction of the driving lane governing a sign placed at
/// (`road`, `s`, `t`), and the side of the reference line it sits on.
///
/// Exposed for tests and for callers that want the reasoning without the
/// angle. `side` is +1 left of the reference line, -1 right of it.
struct SignalApproach {
  TravelDirection travel = TravelDirection::Forward;
  double side = -1.0;

  /// True when the governing direction came from a junction connection rather
  /// than from a lane's own left/right grouping (see auto_signal_facing).
  bool from_junction_approach = false;

  /// False when the cross section had no driving lane at all and `travel` fell
  /// back to the side convention.
  bool has_driving_lane = true;
};

/// The travel direction governing a sign at (`road`, `s`, `t`), per the
/// right-hand-traffic convention: lanes right of the reference line
/// (odr_id < 0) run toward +s, lanes left of it run toward -s, and a lane's
/// @direction=reversed flips its own contribution.
///
/// A CONNECTING ROAD inside a junction is resolved from its junction
/// connection instead of from its own lanes: the interior turn lanes' left /
/// right grouping is an artifact of how the connector was generated, so
/// reading them would orient a sign off the junction interior rather than off
/// the approach it governs. Traffic enters a connecting road at the contact
/// point named by its JunctionConnection and travels away from it.
[[nodiscard]] RM_API Expected<SignalApproach>
signal_approach(const RoadNetwork& network, RoadId road, double s, double t);

/// Auto-orientation per docs/domain/realism_defaults.md (auto-orientation
/// section): the facing derived from the road heading at `s`, the side (sign
/// of `t`), and the travel direction of the nearest driving lane. The face
/// looks AGAINST approaching travel, canted away from the roadway by
/// defaults::kSignToeOut so approaching headlights do not retroreflect
/// straight back.
///
/// INVARIANT — a facing is computed when, and only when, a signal is being
/// PLACED or the user explicitly asks for one:
///   1. editor/src/document/signal_placement.cpp  make_signal()   (placement)
///   2. edit::signalize_junction()                (template placement)
///   3. edit::auto_orient_signal()                ("Auto facing" action)
/// The first two happen while the signal is being created, so there is no
/// authored heading to lose; the third is the user asking. EVERY other path
/// that relocates or rewrites a signal must leave `orientation` and
/// `h_offset` alone. That is what makes the spec's "a user-set heading is an
/// override: it is never re-auto-computed silently" true by CONSTRUCTION
/// rather than by discipline — no path that touches an EXISTING signal
/// derives a facing, so none can overwrite one. It binds #417's rotation ring
/// and #406's cascade relocation: both may write a heading the user asked
/// for, neither may derive one.
///
/// Fails on a stale `road`, an empty plan view, or an `s` outside
/// [0, length]. A cross section with no driving lane at all is NOT a failure:
/// the side alone decides (a sign on the right faces +s traffic), so a plate
/// on a sidewalk-only stretch still gets a sane facing.
[[nodiscard]] RM_API Expected<SignalFacing>
auto_signal_facing(const RoadNetwork& network, RoadId road, double s, double t);

} // namespace roadmaker
