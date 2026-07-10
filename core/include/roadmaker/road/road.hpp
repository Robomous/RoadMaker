#pragma once

#include "roadmaker/geometry/poly3.hpp"
#include "roadmaker/geometry/reference_line.hpp"
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
};

} // namespace roadmaker
