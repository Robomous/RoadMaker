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

/// "The center lane shall have no material elements." (§11.8.2 rules.)
/// Identical in 1.8.1 §11.7.2 and 1.9.0 §11.8.2, so no version gating.
inline constexpr std::string_view kMaterialCenterLaneNone =
    "asam.net:xodr:1.4.0:road.lane.material.center_lane_no_material";

/// "<material> elements shall be defined in ascending order according to the
/// s-coordinate." (§11.8.2 rules.)
inline constexpr std::string_view kMaterialElemAscOrder =
    "asam.net:xodr:1.4.0:road.lane.material.elem_asc_order";

/// "For a road as successor or predecessor the @elementType, @elementId and
/// @contactPoint attributes shall be used."
inline constexpr std::string_view kRoadLinkAttributeUsage =
    "asam.net:xodr:1.4.0:road.linkage.road_link_attribute_usage";

/// "Junctions should not be used when only two roads meet." Present only
/// in the 1.9.0 catalog (Annex F.4.5.3); 1.8.1's Annex E has no
/// equivalent, so validate_network cites it only when targeting 1.9.0.
inline constexpr std::string_view kJunctionNotOnlyTwo =
    "asam.net:xodr:1.9.0:junctions.common.not_only_two";

/// "The @mainRoad, @orientation, @sStart and @sEnd attributes shall only be
/// specified for virtual junctions." (1.9.0 §12.7 rules; 1.8.1 §12.7 states the
/// same as prose — "The @mainRoad, @sStart, @sEnd, @orientation attributes
/// shall only be valid for junctions of type virtual".) All four are @required
/// on `<junction type="virtual">` (§12.7 Table 69 in both versions).
inline constexpr std::string_view kJunctionVirtualAttributes =
    "asam.net:xodr:1.5.0:junctions.common.virtual_junction_attributes";

/// "Junction reference lines shall be defined by one <geometry> element. This
/// <geometry> element shall have only one <line> element." (1.8.0, §12.9.)
inline constexpr std::string_view kJunctionOneLineElement =
    "asam.net:xodr:1.8.0:junctions.geometry.only_one_line_element";

/// "The <geometry> element of a junction reference line shall be defined in a
/// way that every point of the junction can be reached with a perpendicular
/// straight line." (1.8.0, §12.9.)
inline constexpr std::string_view kJunctionRefLineDefinition =
    "asam.net:xodr:1.8.0:junctions.geometry.ref_line_definition";

/// "A junction shall have only one elevation grid." (1.8.0, §12.11.)
inline constexpr std::string_view kJunctionOneElevGrid =
    "asam.net:xodr:1.8.0:junctions.elevation_grid.only_one_elev_grid";

/// "The elevation grid shall be defined with vectors perpendicular to the
/// junction reference line." (1.8.0, §12.11.)
inline constexpr std::string_view kJunctionElevGridPerpendicular =
    "asam.net:xodr:1.8.0:junctions.elevation_grid.perpendicular_vectors";

/// "The type of an object shall be given by the @type attribute." (§13.1.)
inline constexpr std::string_view kObjectTypeAttr = "asam.net:xodr:1.7.0:road.object.type_attr";

/// "The direction for which objects are valid shall be specified." (§13.1.)
inline constexpr std::string_view kObjectOrientation =
    "asam.net:xodr:1.7.0:road.object.orientation";

/// "The origin position of the object shall be described with s- and
/// t-coordinates along the road surface." (§13.1.)
inline constexpr std::string_view kObjectStTCoords = "asam.net:xodr:1.7.0:road.object.s_t_coords";

/// "Objects may be of circular or angular shape. The possibilities are
/// mutually exclusive. The shape is defined by the used attributes." (§13.1.)
inline constexpr std::string_view kObjectCircularVsAngular =
    "asam.net:xodr:1.7.0:road.object.circular_vs_angular";

/// "An <outline> element shall be followed by two or more <cornerRoad>
/// elements, by two or more <cornerLocal> elements, or by one or more
/// <curveLocal> elements." (1.9.0, §13.2.)
inline constexpr std::string_view kOutlineFollowedByCorner =
    "asam.net:xodr:1.9.0:road.object.outline.outline_followed_by_corner";

/// "<outlines> elements shall have exactly one <outline> element with
/// @outer=true." (1.9.0, §13.2.)
inline constexpr std::string_view kOutlineExactlyOneOuter =
    "asam.net:xodr:1.9.0:road.object.outline.exactly_one_outer";

/// "There shall be at least two <cornerRoad> elements inside an <outline>
/// element." (§13.2.1.)
inline constexpr std::string_view kCornerRoadMinAmount =
    "asam.net:xodr:1.7.0:road.corner_road.element_min_amount";

/// "There shall be at least two <cornerLocal> elements inside an <outline>
/// element." (§13.2.2.)
inline constexpr std::string_view kCornerLocalMinAmount =
    "asam.net:xodr:1.7.0:road.corner_local.element_min_amount";

/// "There shall be no mixture of <cornerRoad>, <cornerLocal>, and
/// <curveLocal> elements inside the same <outline> element." (1.9.0,
/// §13.2.1/§13.2.2.)
inline constexpr std::string_view kCornerRoadLocalExcl =
    "asam.net:xodr:1.9.0:road.corner_road.corner_road_local_exclusivity";

/// "The @id attribute of a <cornerRoad> element shall be mandatory when the
/// parent also has a <markings> element." (1.9.0, §13.2.1.)
inline constexpr std::string_view kCornerRoadIdWithMarkings =
    "asam.net:xodr:1.9.0:road.corner_road.mandatory_id_with_markings";

/// "The @id attribute of a <cornerLocal> element shall be mandatory when the
/// parent also has a <markings> element." (1.9.0, §13.2.2.)
inline constexpr std::string_view kCornerLocalIdWithMarkings =
    "asam.net:xodr:1.9.0:road.corner_local.mandatory_id_with_markings";

/// "The color of the marking shall be defined." (§13.8.)
inline constexpr std::string_view kObjectMarkingColour =
    "asam.net:xodr:1.7.0:road.object.marking.colour";

/// "If no outline is used, the @side attribute is mandatory." (§13.8.)
inline constexpr std::string_view kObjectMarkingNoOutlineSide =
    "asam.net:xodr:1.7.0:road.object.marking.no_outline_side_attr";

/// "Signals shall have a specific type and subtype." (§14.1.)
inline constexpr std::string_view kSignalType = "asam.net:xodr:1.7.0:road.signal.signal_type";

/// "A country code shall be added to refer to country-specific rules using
/// the @country attribute." (§14.1.)
inline constexpr std::string_view kSignalUseCountryCode =
    "asam.net:xodr:1.7.0:road.signal.use_country_code";

/// "If the existing roads are not sufficient to define a closed junction
/// boundary, additional roads shall be defined for the missing segments."
/// (1.8.0, §12.10.) M3a emits <boundary> for gap-free generated junctions;
/// this is cited only when a gap remains that would need auxiliary boundary
/// roads (docs/design/m3a/00 phase 2b, #62).
inline constexpr std::string_view kJunctionBoundaryCloseGap =
    "asam.net:xodr:1.8.0:junctions.boundary.close_gap_with_new_roads";

/// "Junction boundaries are currently only valid for common junctions."
/// (1.8.0, §12.10.)
inline constexpr std::string_view kJunctionBoundaryOnlyCommon =
    "asam.net:xodr:1.8.0:junctions.boundary.only_for_common_junctions";

/// "Segments shall be ordered counter clockwise." (1.8.0, §12.10.)
inline constexpr std::string_view kJunctionBoundaryCcwOrder =
    "asam.net:xodr:1.8.0:junctions.boundary.segments_counter_clockwise_order";

/// "Segments shall close the entire junction boundary." (1.8.0, §12.10.)
inline constexpr std::string_view kJunctionBoundaryClosed =
    "asam.net:xodr:1.8.0:junctions.boundary.segments_close_boundry";

/// "Segments shall be defined to reach the start or end of all roads connected
/// to the junction." (1.8.0, §12.10.)
inline constexpr std::string_view kJunctionBoundaryReachAllRoads =
    "asam.net:xodr:1.8.0:junctions.boundary.segments_for_each_conn_road";

/// RoadMaker-authored rule (NOT ASAM): "A road end may be an arm of at most one
/// junction." Two junctions claiming the same (road, contact) end produce
/// superimposed/ambiguous topology (gate finding 5). Uses the vendor namespace
/// `robomous.ai` per the maintainer-approved convention extension; the version
/// is RoadMaker's, not a spec version.
inline constexpr std::string_view kJunctionArmSingleOwner =
    "robomous.ai:rm:1.0.0:junctions.arm_single_owner";

} // namespace roadmaker::rules
