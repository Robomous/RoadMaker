#pragma once

#include <string_view>

/// ASAM OpenDRIVE checker-rule UIDs cited in Diagnostic::rule_id.
///
/// UID format (OpenDRIVE 1.8.1, Annex E "Checker rules (normative)"):
///   <emanating-entity>:<standard>:<definition-setting>:<rule_set.rule_name>
/// The version component is the spec version the rule FIRST appeared in,
/// not the version of the document being checked — a 1.4.0-stamped UID is
/// the current identifier even when reading 1.7 or 1.8 files.
///
/// Sources: .claude/references/asam/opendrive-1.8.1/16_annexes.md (Annex E)
/// and opendrive-1.9.0/16_annexes.md (Annex F). Descriptions quoted verbatim.
namespace roadmaker::rules {

/// "IDs shall be unique within a class."
inline constexpr std::string_view kIdUniqueInClass = "asam.net:xodr:1.4.0:ids.id_unique_in_class";

/// "Lane IDs shall be unique within a lane section."
inline constexpr std::string_view kIdUniqueInLaneSection =
    "asam.net:xodr:1.4.0:ids.id_unique_in_lane_section";

/// "Only defined IDs may be referenced."
inline constexpr std::string_view kOnlyRefDefinedIds =
    "asam.net:xodr:1.4.0:ids.only_ref_defined_ids";

/// "Each road shall have a road reference line."
inline constexpr std::string_view kReflineExists =
    "asam.net:xodr:1.4.0:road.geometry.refline_exists";

/// "A road reference line shall have no gaps."
inline constexpr std::string_view kReflineNoGaps =
    "asam.net:xodr:1.4.0:road.geometry.refline_no_gaps";

/// "The road length should be the sum of the lengths of all <geometry>
/// elements" — introduced in 1.9.0 (Annex F.6.1); cited for older files
/// too per the first-appearance UID convention above.
inline constexpr std::string_view kRoadLengthSumGeometries =
    "asam.net:xodr:1.9.0:road.length_sum_geometries";

/// "Each road shall have at least one lane section."
inline constexpr std::string_view kLaneSectionRequired =
    "asam.net:xodr:1.4.0:road.lane_section.lane_sect_req";

/// "The length of lane sections shall be greater than zero." (A laneSection
/// repeating a previous s value would create a zero-length section.)
inline constexpr std::string_view kLaneSectionValidLength =
    "asam.net:xodr:1.4.0:road.lane_section.valid_length";

/// "The width of the lane shall be defined for the full length of the lane
/// section. This means that there must be a <width> element for @s=\"0\"."
inline constexpr std::string_view kWidthDefinedWholeSection =
    "asam.net:xodr:1.7.0:road.lane.width.width_defined_whole_section";

/// "For a road as successor or predecessor the @elementType, @elementId and
/// @contactPoint attributes shall be used."
inline constexpr std::string_view kRoadLinkAttributeUsage =
    "asam.net:xodr:1.4.0:road.linkage.road_link_attribute_usage";

/// "Junctions should not be used when only two roads meet." Present only
/// in the 1.9.0 catalog (Annex F.4.5.3); 1.8.1's Annex E has no
/// equivalent, so validate_network cites it only when targeting 1.9.0.
inline constexpr std::string_view kJunctionNotOnlyTwo =
    "asam.net:xodr:1.9.0:junctions.common.not_only_two";

} // namespace roadmaker::rules
