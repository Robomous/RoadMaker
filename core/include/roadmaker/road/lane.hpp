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
#include "roadmaker/road/defaults.hpp"
#include "roadmaker/road/id.hpp"
#include "roadmaker/road/traffic_rule.hpp"
#include "roadmaker/xodr/raw_xml.hpp"

#include <optional>
#include <string>
#include <vector>

namespace roadmaker {

/// OpenDRIVE lane types RoadMaker distinguishes in M1. Anything else parses
/// as `Other` (with a diagnostic), never dropped.
enum class LaneType {
  Driving,
  Stop,
  Shoulder,
  Biking,
  Sidewalk,
  Border,
  Restricted,
  Parking,
  Median,
  Curb,
  None,
  Other,
};

/// Lane travel direction relative to the reference-line-derived standard
/// (e_lane_direction). Introduced in OpenDRIVE 1.8.0, so it is legal under
/// both writer targets (1.8.1 and 1.9.0) with no version gating. The
/// enumeration is exactly standard | reversed | both — there is NO `same`
/// literal (1.8.1 §11 + Annex A.3.10 Table 173; 1.9.0 §11.2/§11.3.1 + Annex
/// A.3.11 Table 180). `Standard` means "determined by the <left>/<right>
/// grouping and the road @rule (LHT/RHT)"; the @direction attribute overrides
/// that. Written explicitly only when not Standard (byte-stable default).
enum class LaneDirection {
  Standard,
  Reversed,
  Both,
};

/// True when traffic on this lane travels toward increasing `s`.
///
/// **The one place the RHT/LHT flip is written down.** OpenDRIVE 1.9.0 §11
/// states the default driving direction normatively:
///
/// > For a road with the @rule="RHT" attribute, the default driving direction
/// > would be in positive direction of the road reference line for all
/// > `<right>` element lanes with negative @id attribute and against the road
/// > reference line for lanes in the `<left>` element with positive @id
/// > attribute. If the road has the @rule="LHT" attribute, the default driving
/// > direction would be in positive direction of the road reference line for
/// > all `<left>` element lanes with positive @id attribute and against the
/// > road reference line for all `<right>` element lanes with negative @id
/// > attribute.
///
/// So the rule flips the side test outright, and `@direction` then overrides
/// the result: `Reversed` inverts it, `Standard` leaves it alone (§11 — *"If
/// the @direction attribute is not specified or has a value of
/// @direction='standard', the default driving direction is not changed"*).
///
/// `Both` answers the grouping's default, because a bidirectional lane gives a
/// caller no reason to prefer either sense; callers that must distinguish a
/// contraflow-capable lane read `Lane::direction` themselves.
///
/// The center lane (`@id` 0) carries no traffic; it answers as a left lane and
/// callers are expected to have excluded it already.
[[nodiscard]] constexpr bool
lane_travels_with_s(int lane_odr_id, LaneDirection direction, TrafficRule rule) {
  const bool by_side = rule == TrafficRule::LeftHandTraffic ? lane_odr_id > 0 : lane_odr_id < 0;
  return direction == LaneDirection::Reversed ? !by_side : by_side;
}

/// One entry of a lane's `<link>` — a `<predecessor>` or `<successor>`
/// (§11.6, Table 40). Multiplicity is 0..*, so a lane holds a list of these.
struct LaneLink {
  /// @id — the preceding/succeeding lane's OpenDRIVE id. Required.
  int id = 0;

  /// @layer — `e_layerType`, `"permanent"` or `"temporary"`, kept verbatim.
  ///
  /// Empty when the attribute was absent, and §11.6 says *"omitting @layer
  /// shall default to @layer='permanent'"* — so absent and explicit-permanent
  /// mean the same thing, and keeping the spelling is what lets the writer
  /// re-emit each as it found it. The temporary lane layer is precisely what
  /// §11.6 gives as the reason a permanent lane ends up with more than one
  /// predecessor, so dropping it alongside the extra links would have left
  /// half the feature unreadable.
  std::string layer;

  friend bool operator==(const LaneLink&, const LaneLink&) = default;
};

/// Lane marking types RoadMaker renders in M1; exotic ones map to Other
/// (with a diagnostic).
enum class RoadMarkType {
  None,
  Solid,
  Broken,
  SolidSolid,
  SolidBroken,
  BrokenSolid,
  BrokenBroken,
  Other,
};

/// e_roadMarkColor (OpenDRIVE 1.9.0 §11.9, Table 48). Standard resolves to
/// white or yellow by mark type/region in the renderer (the kernel stays
/// render-free). Unknown spellings parse as Other with a diagnostic — never
/// dropped, mirroring RoadMarkType::Other.
enum class RoadMarkColor {
  Standard,
  White,
  Yellow,
  Red,
  Blue,
  Green,
  Orange,
  Other,
};

/// One painted stripe of a (possibly multi-line) road mark — a <line> inside
/// the <roadMark>'s <type> element (§11.9.1, Table 50). Populated only for
/// multi-line marks (solid_solid etc.); empty for the common single-stripe
/// case where RoadMark::width is authoritative (M2 behaviour, byte-stable).
struct RoadMarkLine {
  double width = defaults::kLineWidth; ///< stripe width [m]
  double length = 0.0;                 ///< painted length [m] (0 = continuous)
  double space = 0.0;                  ///< gap length [m] (0 = solid)
  double t_offset = 0.0;               ///< lateral offset of this stripe from the mark line [m]
  double s_offset = 0.0;               ///< longitudinal start offset within the mark [m]

  friend bool operator==(const RoadMarkLine&, const RoadMarkLine&) = default;
};

/// One <roadMark> record on a lane boundary.
struct RoadMark {
  /// Start offset LOCAL to the owning lane section [m].
  double s_offset = 0.0;

  RoadMarkType type = RoadMarkType::None;

  /// @type exactly as spelled; empty for an authored mark (#476). RoadMaker
  /// models 7 of e_roadMarkType's values, so `curb`, `grass`, `botts dots`,
  /// `edge` and `custom` (§11.9, Table 48) all parse to `Other` — which used to
  /// be written back as `"solid"`, painting a kerb as a lane line. Stored for
  /// every mark, for the same reason as `Lane::type_str`.
  std::string type_str;

  /// Painted width [m]; the registry's normal-line width when absent (the
  /// OpenDRIVE @width attribute is optional with no normative default).
  double width = defaults::kLineWidth;

  /// e_roadMarkColor (§11.9). Written explicitly only when not Standard.
  RoadMarkColor color = RoadMarkColor::Standard;

  /// @color exactly as spelled; empty when the attribute was absent or the mark
  /// was authored (#476). An unmodeled colour used to be rewritten to
  /// `"standard"`. Holding the spelling also keeps an EXPLICIT `color="standard"`
  /// explicit, which the enum alone cannot distinguish from an absent attribute.
  std::string color_str;

  /// @material (§11.9, Table 47): "Identifiers to be defined by the user, use
  /// standard as default value." Held as an optional so the byte-stable
  /// default (unset) writes nothing; RoadMaker authors "rm:<id>" when a
  /// marking material is assigned (GW-2 step 15 plumbing).
  std::optional<std::string> material;

  /// Explicit multi-line geometry (<type>/<line>, §11.9.1). Empty for the
  /// simple single-stripe mark; populated (two stripes with symmetric
  /// t_offset) for solid_solid / solid_broken / broken_solid so the mark
  /// renders as true dual geometry instead of one strip.
  std::vector<RoadMarkLine> lines;

  friend bool operator==(const RoadMark&, const RoadMark&) = default;
};

/// One <material> record on a lane — ASAM OpenDRIVE 1.9.0 §11.8.2, Table 44
/// (identical in 1.8.1 §11.7.2; only the chapter number moved, so no version
/// gating). Promoted out of the Preserved tier so the parser stops silently
/// dropping it. `surface` is the standard's "Surface material code, depending
/// on application" — an application-defined string; RoadMaker writes "rm:<id>".
struct LaneMaterial {
  /// s-coordinate of start position, LOCAL to the owning lane section [m]
  /// (Table 44 @sOffset, required).
  double s_offset = 0.0;

  /// @friction (Table 44, required). Modeled optional so a foreign file that
  /// omits it round-trips byte-identically (parse warns; write omits when
  /// unset). RoadMaker-authored records set it from the catalog nominal value.
  std::optional<double> friction;

  /// @roughness (Table 44, optional) — "for example, for sound and motion
  /// systems". Written only when present.
  std::optional<double> roughness;

  /// @surface (Table 44, optional) — application-defined code. RoadMaker
  /// writes "rm:<id>"; foreign codes survive verbatim.
  std::optional<std::string> surface;

  /// Attributes we do not model, preserved verbatim (risk-3 mitigation: a
  /// foreign file's extra @attrs survive a round-trip).
  RawXml preserved;

  friend bool operator==(const LaneMaterial&, const LaneMaterial&) = default;
};

/// A single lane within a lane section.
///
/// OpenDRIVE lane numbering: 0 is the (width-less) center lane on the
/// reference line, positive ids grow to the LEFT of the travel direction,
/// negative ids to the RIGHT, ordered outward.
struct Lane {
  /// Owning section (back-reference).
  LaneSectionId section;

  /// Signed OpenDRIVE lane id within the section.
  int odr_id = 0;

  LaneType type = LaneType::None;

  /// @type exactly as spelled in the file; empty for an authored lane, which
  /// lets the writer derive the spelling from `type` (#476).
  ///
  /// ★ WITHOUT THIS THE WRITER MAKES AN AFFIRMATIVE WRONG CLAIM, which is worse
  /// than dropping the value. RoadMaker models 11 of e_laneType's values, so a
  /// perfectly ordinary `onRamp` / `offRamp` / `entry` / `exit` /
  /// `connectingRamp` / `slipLane` lane parses to `Other` — and `Other` used to
  /// be written back as `"none"`, i.e. a drivable ramp re-exported as *not
  /// usable*. The spelling is stored for EVERY lane, not only the unmodeled
  /// ones, because two modeled spellings also used to change on save:
  /// §11.8.1 deprecates `sidewalk` in favour of `walking` (1.8.1 §11 note under
  /// the e_laneType list, identical in 1.9.0), and both parse to
  /// `LaneType::Sidewalk` — so a conformant `walking` came back as the
  /// deprecated `sidewalk`.
  ///
  /// ★ EVERY COMMAND THAT CHANGES `type` MUST CLEAR THIS, or the file keeps the
  /// spelling of a type the lane no longer has. `edit::set_lane_type` does.
  std::string type_str;

  /// Travel direction (e_lane_direction, §11). Defaults to Standard, which
  /// emits nothing on write; only Reversed/Both are serialized.
  LaneDirection direction = LaneDirection::Standard;

  /// @direction exactly as spelled; empty when the attribute was absent or the
  /// lane was authored (#476).
  ///
  /// Same contract as `type_str`, and the old behaviour here was a *deletion*
  /// rather than a rewrite: an unknown spelling parsed to Standard, and the
  /// writer omits @direction for Standard — so the attribute vanished, and its
  /// absence means "standard" per §11, which is the same wrong claim by another
  /// route. Cleared by `edit::set_lane_direction`.
  std::string direction_str;

  /// @level (§11.8.1) as read: "lane keeps the same level as the reference
  /// line", i.e. it is not affected by superelevation or crossfall. Unmodeled
  /// otherwise — RoadMaker's mesher does not implement level lanes — but the
  /// value is carried so it is re-emitted verbatim instead of being flattened
  /// (the writer used to hardcode `"false"`, silently discarding a `"true"`).
  /// nullopt = absent in the source; an authored lane writes the `"false"` the
  /// existing fixtures expect.
  std::optional<bool> level;

  /// Width polynomials. Poly3::s is the sOffset LOCAL to the owning lane
  /// section's start (per OpenDRIVE), sorted ascending. Empty for lane 0.
  std::vector<Poly3> widths;

  /// Markings on this lane's OUTER boundary, sorted by s_offset (section-
  /// local). On lane 0 this is the center line marking.
  std::vector<RoadMark> road_marks;

  /// <material> records (§11.8.2), sorted ascending by s_offset (section-
  /// local). Empty for lane 0 (the center lane shall have no material,
  /// asam.net:xodr:1.4.0:road.lane.material.center_lane_no_material).
  std::vector<LaneMaterial> materials;

  /// `<link><predecessor>` / `<link><successor>` — the linked lanes in the
  /// previous/next lane section, or across a road boundary (§11.6).
  ///
  /// **Lists, because the multiplicity is 0..\*** and
  /// `asam.net:xodr:1.4.0:road.lane.link.multiple_connections` *mandates*
  /// several entries where a lane splits or merges abruptly. Before #536 these
  /// were scalar `optional<int>`, the reader took `link.child("predecessor")`
  /// — the first element only — and the rest vanished with no diagnostic: a
  /// spec-mandated split was silently halved on every round trip.
  ///
  /// Order is document order, which is the order the writer re-emits.
  std::vector<LaneLink> predecessors;
  std::vector<LaneLink> successors;

  /// The first predecessor/successor id, or nullopt when the lane has none.
  ///
  /// Exactly what the scalar fields used to hold, so every consumer that only
  /// wants "the lane that continues" reads unchanged. Callers that must handle
  /// a split — the writer's dangling-link check and the route resolver — walk
  /// the vectors instead, and are the reason this is spelled `first_` rather
  /// than `the_`.
  [[nodiscard]] std::optional<int> first_predecessor() const {
    return predecessors.empty() ? std::nullopt : std::optional<int>{predecessors.front().id};
  }

  [[nodiscard]] std::optional<int> first_successor() const {
    return successors.empty() ? std::nullopt : std::optional<int>{successors.front().id};
  }

  /// Replace the link list with a single permanent-layer link. What every
  /// AUTHORED link is: RoadMaker's own commands create one lane continuing into
  /// one lane, so a multi-link list only ever arrives from a file.
  void set_predecessor(int id) { predecessors.assign(1, LaneLink{.id = id}); }

  void set_successor(int id) { successors.assign(1, LaneLink{.id = id}); }

  /// Preserved tier: unmodeled lane children (<speed>/<access>/<height>/
  /// <rule>/<userData>/…) captured verbatim in document order and re-emitted
  /// after the modeled content, so the parser never silently drops input
  /// (the Object precedent; docs/design/m3a/01 §5). Also carries centre-lane
  /// surface <userData> for P3/P4.
  RawXml preserved;
};

} // namespace roadmaker
