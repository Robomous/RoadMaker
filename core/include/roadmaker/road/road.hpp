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

#include "roadmaker/geometry/poly3.hpp"
#include "roadmaker/geometry/reference_line.hpp"
#include "roadmaker/road/bridge.hpp"
#include "roadmaker/road/id.hpp"

#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace roadmaker {

/// Which end of the linked element the link attaches to.
enum class ContactPoint {
  Start,
  End,
};

/// Resolved predecessor/successor link of a road: either another road (with
/// the end we attach to) or a junction.
struct RoadLink {
  std::variant<RoadId, JunctionId> target;
  ContactPoint contact = ContactPoint::Start;
};

/// Plan-view waypoint [m] for clothoid path fitting (authoring API and the
/// M2 node-editing tools).
struct Waypoint {
  double x = 0.0;
  double y = 0.0;

  friend constexpr bool operator==(const Waypoint&, const Waypoint&) = default;
};

/// One specific end of a road — how callers name junction arms and
/// tangent-continuation anchors (docs/m2/01_editing_framework.md §2.3).
struct RoadEnd {
  RoadId road;
  ContactPoint contact = ContactPoint::Start;

  friend constexpr bool operator==(const RoadEnd&, const RoadEnd&) = default;
};

/// A road: one reference line plus lane sections and vertical profiles.
///
/// All `s` coordinates are arc length along the reference line, in meters,
/// starting at 0 at the road start. Frames: right-handed, Z-up.
struct Road {
  std::string name;

  /// OpenDRIVE road id (xodr ids are strings; unique within a network).
  std::string odr_id;

  /// Total reference-line length [m]. Kept consistent with the plan-view
  /// geometry by the authoring API / parser.
  double length = 0.0;

  /// Plan-view reference line (the <planView> geometry records).
  ReferenceLine plan_view;

  /// Set iff this is a connecting road inside a junction.
  JunctionId junction;

  /// Lane sections sorted ascending by s0. Maintained by
  /// RoadNetwork::add_lane_section — do not reorder by hand.
  std::vector<LaneSectionId> sections;

  /// Lateral shift of lane 0 from the reference line. Poly3::s is GLOBAL
  /// road s, sorted ascending. Empty means no offset.
  std::vector<Poly3> lane_offset;

  /// Elevation z(s) [m]. Poly3::s is global road s, sorted ascending.
  std::vector<Poly3> elevation;

  /// Superelevation roll(s) [rad] about the reference-line tangent.
  /// Poly3::s is global road s, sorted ascending.
  std::vector<Poly3> superelevation;

  std::optional<RoadLink> predecessor;
  std::optional<RoadLink> successor;

  /// The waypoints the reference line was fitted through. Set by the
  /// authoring API and node-edit commands; persisted in .xodr as
  /// `<userData code="rm:waypoints">` (spec-sanctioned extension element,
  /// OpenDRIVE 1.9.0 §7.2) so edit sessions survive save/load. Roads from
  /// foreign files load without it — Edit Nodes derives waypoints lazily
  /// (docs/m2/01_editing_framework.md §2.5).
  std::optional<std::vector<Waypoint>> authoring_waypoints;

  /// `<bridge>` spans over this road (ASAM 1.9.0 §13.12) — p5-s3, #233. Modeled
  /// as first-class records so a raised span can carry generated deck/pier/
  /// abutment/guardrail solids; the span serializes, the solids do not. Empty on
  /// a road with no bridge. The reader routes `<bridge>` here; `<tunnel>` and
  /// `<objectReference>` still fall through to `object_extras`.
  std::vector<Bridge> bridges;

  /// Non-<object> children of this road's <objects> container
  /// (<objectReference>, <tunnel> — §13.10–§13.11), preserved as verbatim XML
  /// fragments in document order. RoadMaker does not model them; the writer
  /// re-emits them inside <objects> so round-trip loses nothing. `<bridge>` USED
  /// to live here too but is now the typed `bridges` list above.
  std::vector<std::string> object_extras;

  /// Non-<signal> children of this road's <signals> container
  /// (<signalReference> — §14.5, multiplicity 0..*), preserved as verbatim
  /// XML fragments in document order. M3a does not model them; the writer
  /// re-emits them inside <signals> so round-trip loses nothing.
  std::vector<std::string> signal_extras;
};

} // namespace roadmaker
