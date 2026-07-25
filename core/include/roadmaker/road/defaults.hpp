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

#include <numbers>
#include <string>

namespace roadmaker {

enum class LaneType;

/// The machine-readable realism-defaults registry (#413).
///
/// docs/domain/realism_defaults.md is the canonical spec for every value
/// here; this namespace is its single code mirror. The authoring templates
/// (LaneProfile), the Library road styles (RoadStyle), the per-lane-type
/// fallback widths, and the marking constants all derive from these
/// constants — nothing else in the tree restates them. The prop meshes are
/// authored by scripts/gen_prop_meshes.py (stdlib-only, so it cannot include
/// this header) and are held to the §1.5/§1.6 constants by the same tests.
/// The §1.2/§1.3/§1.4/§1.5/§1.6 tables and the auto-orientation table in the
/// spec doc are rendered by cross_section_markdown() / markings_markdown() /
/// signs_markdown() / sign_mounting_markdown() / signals_lighting_markdown() /
/// trees_buildings_markdown() / orientation_markdown(), and
/// test_defaults_registry.cpp fails CI when the
/// committed doc and this registry disagree (the shortcut_registry
/// mechanism). Change a default by PRing the spec doc and this file
/// together, then regenerating the doc tables from the renderers.
///
/// Units are SI meters, except the auto-orientation angles, which are radians
/// (the kernel frame's angular unit).
namespace defaults {

/// The four default authoring presets (spec §1.2). Both create-road
/// templates and Library road styles derive per-class cross sections.
enum class RoadClass {
  Freeway,
  Arterial,
  Collector,
  Local,
};

// --- §1.2 Lane & cross-section defaults -----------------------------------

inline constexpr double kFreewayLaneWidth = 3.6;
inline constexpr double kArterialLaneWidth = 3.6;
inline constexpr double kCollectorLaneWidth = 3.3;
inline constexpr double kLocalLaneWidth = 3.0;
inline constexpr double kFreewayRightShoulderWidth = 3.0;
inline constexpr double kFreewayLeftShoulderWidth = 1.2;
inline constexpr double kShoulderWidth = 1.8; ///< arterial/collector shoulder
inline constexpr double kParkingLaneWidth = 2.4;
inline constexpr double kBikeLaneWidth = 1.5;
inline constexpr double kSidewalkWidth = 1.8;
inline constexpr double kCurbHeight = 0.15; ///< canonical; not yet extruded
inline constexpr double kMedianWidth = 1.2; ///< raised median, minimum
inline constexpr double kTwoWayLeftTurnLaneWidth = 3.6;

// --- §1.3 Markings (MUTCD Ch. 3) ------------------------------------------

inline constexpr double kLineWidth = 0.10;        ///< normal painted line
inline constexpr double kFreewayLineWidth = 0.15; ///< freeway wide-line option
inline constexpr double kDashLength = 3.0;        ///< broken lane line dash
inline constexpr double kDashGap = 9.0;           ///< broken lane line gap
/// Clear space between the two stripes of a double line.
inline constexpr double kDoubleLineSeparation = 0.10;
inline constexpr double kStopLineWidth = 0.60; ///< along-road extent
inline constexpr double kStopLineMinWidth = 0.30;
inline constexpr double kCrosswalkLineMinWidth = 0.15; ///< transverse lines
inline constexpr double kCrosswalkLineMaxWidth = 0.60;
inline constexpr double kCrosswalkWidth = 3.0; ///< default walking depth
inline constexpr double kCrosswalkMinWidth = 1.8;
inline constexpr double kCrosswalkStripeLength = 0.60; ///< zebra bar
inline constexpr double kCrosswalkStripeGap = 0.60;    ///< zebra gap

// --- §1.4 Signs (MUTCD, conventional-road sizes) --------------------------
//
// Face sizes are authored geometry. The US pack itself is a data table in
// roadmaker::signs (core/include/roadmaker/assets/sign_catalog.hpp) that takes
// every dimension from here, and the sign meshes in
// scripts/gen_prop_meshes.py are built to the same extents —
// test_defaults_registry.cpp asserts both against these constants, exactly as
// it does for the §1.5/§1.6 props.

inline constexpr double kSignStopFace = 0.75;            ///< R1-1 octagon, 30 in
inline constexpr double kSignStopFaceMultilane = 0.90;   ///< R1-1 multilane option, 36 in
inline constexpr double kSignYieldFace = 0.90;           ///< R1-2 triangle, 36 in
inline constexpr double kSignSpeedLimitWidth = 0.60;     ///< R2-1, 24 in
inline constexpr double kSignSpeedLimitHeight = 0.75;    ///< R2-1, 30 in
inline constexpr double kSignDoNotEnterFace = 0.75;      ///< R5-1, 30 in
inline constexpr double kSignOneWayWidth = 0.90;         ///< R6-1, 36 in
inline constexpr double kSignOneWayHeight = 0.30;        ///< R6-1, 12 in
inline constexpr double kSignTurnRestrictionFace = 0.60; ///< R3-1/R3-2, 24 in
inline constexpr double kSignKeepRightWidth = 0.60;      ///< R4-7, 24 in
inline constexpr double kSignKeepRightHeight = 0.75;     ///< R4-7, 30 in
inline constexpr double kSignWarningFace = 0.75;         ///< W-series diamond, 30 in
inline constexpr double kSignSchoolFace = 0.90;          ///< S1-1 pentagon, 36 in
inline constexpr double kSignStreetNameMinHeight = 0.25; ///< D3-1 blade, 10 in
inline constexpr double kSignStreetNameHeight = 0.30;    ///< D3-1 blade, 12 in
/// D3-1 legends are sized to this; the blade length follows the text, so a
/// street-name sign declares no @width (§1.4 "length fits text").
inline constexpr double kSignStreetNameMinLetterHeight = 0.15;

// §1.4 sign mounting. The mesh assembles post + face so that the face's bottom
// edge sits at kSignMountUrban above z=0; the lateral values are placement
// guidance that composes with #338's outermost-lane-edge soft-snap.

inline constexpr double kSignMountUrban = 2.1;      ///< bottom edge over pavement, 7 ft
inline constexpr double kSignMountRural = 1.5;      ///< rural option, 5 ft
inline constexpr double kSignLateralShoulder = 1.8; ///< min from shoulder edge, 6 ft
inline constexpr double kSignLateralCurb = 0.60;    ///< urban min from curb face, 2 ft
inline constexpr double kSignPostDiameter = 0.06;   ///< breakaway single post, visual

// --- Auto-orientation ------------------------------------------------------
//
// The spec's auto-orientation section. These are the registry's only ANGLES:
// stored in radians like every other angle in the kernel (Signal::h_offset,
// Object::hdg), but CONSTRUCTED from their degree measure so the derivation
// survives — a hand-rounded 0.052 would be 2.979°, a different angle from the
// one the doc specifies.

/// Cant away from perpendicular applied to an auto-oriented sign or signal
/// face, so approaching headlights do not retroreflect straight back (3°).
/// Consumed by auto_signal_facing() — see roadmaker/road/signal_facing.hpp.
inline constexpr double kSignToeOut = 3.0 * std::numbers::pi / 180.0;

/// Detent of the viewport's prop rotation ring (15°), suppressed while Shift
/// is held. Consumed by the editor's transform gizmo.
inline constexpr double kPropRotationSnap = 15.0 * std::numbers::pi / 180.0;

// --- §1.5 Signals, lighting, street furniture -----------------------------
//
// The signal rows are registry-rendered here so the whole §1.5 table is under
// the divergence gate; the signal *meshes* are retuned by #414. The
// streetlight and hydrant values are what the shipped props must measure —
// test_defaults_registry.cpp asserts props::model() against them.

inline constexpr double kSignalLensDiameter = 0.30;  ///< 12 in lens
inline constexpr double kSignalHousingHeight = 1.07; ///< 3-section, 42 in
inline constexpr double kSignalClearanceMin = 4.6;   ///< housing bottom over roadway
inline constexpr double kSignalClearanceMax = 5.8;
inline constexpr double kSignalClearance = 5.2; ///< default, mast-arm mounted
inline constexpr double kPedSignalMountMin = 2.1;
inline constexpr double kPedSignalMountMax = 3.0;
inline constexpr double kStreetlightMountingHeight = 9.0; ///< default luminaire height
inline constexpr double kStreetlightResidentialHeight = 7.6;
inline constexpr double kStreetlightArterialHeight = 12.0;
inline constexpr double kFireHydrantHeight = 0.75; ///< no asset ships (see spec doc)

// --- §1.6 Trees & buildings -----------------------------------------------

inline constexpr double kStreetTreeHeight = 10.0;        ///< the default street tree
inline constexpr double kStreetTreeCanopyDiameter = 6.0; ///< model radius = half this
inline constexpr double kStreetTreeTrunkDiameter = 0.40;
inline constexpr double kTreeClearTrunkSidewalk = 2.4; ///< crown bottom over a walk
inline constexpr double kTreeClearTrunkRoadway = 4.4;
inline constexpr double kOrnamentalTreeMinHeight = 4.0; ///< small-ornamental band
inline constexpr double kOrnamentalTreeMaxHeight = 6.0;
inline constexpr double kMatureTreeMinHeight = 15.0; ///< large-mature band
inline constexpr double kMatureTreeMaxHeight = 20.0;
inline constexpr double kHouse1StoryHeight = 5.0; ///< to the ridge
inline constexpr double kHouse2StoryHeight = 8.0;
inline constexpr double kHouse2StoryMinHeight = 7.0;
inline constexpr double kHouse2StoryMaxHeight = 9.0;
inline constexpr double kCommercial1StoryHeight = 5.5;
inline constexpr double kFloorHeight = 3.7;            ///< commercial/mid-rise floor
inline constexpr double kResidentialFloorHeight = 3.0; ///< residential floor
inline constexpr double kParapetHeight = 1.0;          ///< above the top floor
inline constexpr double kHouseFootprintLength = 10.0;  ///< footprint sanity, plan view
inline constexpr double kHouseFootprintWidth = 8.0;

/// Driving-lane default for the given road class (spec §1.2).
[[nodiscard]] RM_API double driving_lane_width(RoadClass road_class);

/// Default width for a lane of `type` in a classless context — the add-lane
/// and taper paths, which act on roads that carry no road class. Driving
/// lanes use the arterial value; unlisted types (None, Border, ...) also
/// fall back to it so a fresh lane is always drivable-sized.
[[nodiscard]] RM_API double lane_width(LaneType type);

/// Spec §1.2 class name as used by the Library manifest and the docs
/// ("freeway" | "arterial" | "collector" | "local").
[[nodiscard]] RM_API const char* road_class_name(RoadClass road_class);

/// Renders the spec doc's §1.2 table (marker comment + markdown table)
/// exactly as committed in docs/domain/realism_defaults.md.
[[nodiscard]] RM_API std::string cross_section_markdown();

/// Renders the spec doc's §1.3 table, same contract.
[[nodiscard]] RM_API std::string markings_markdown();

/// Renders the spec doc's §1.4 sign-face table, same contract.
[[nodiscard]] RM_API std::string signs_markdown();

/// Renders the spec doc's §1.4 sign-mounting table, same contract.
[[nodiscard]] RM_API std::string sign_mounting_markdown();

/// Renders the spec doc's §1.5 table, same contract.
[[nodiscard]] RM_API std::string signals_lighting_markdown();

/// Renders the spec doc's §1.6 table, same contract.
[[nodiscard]] RM_API std::string trees_buildings_markdown();

/// Renders the spec doc's auto-orientation table, same contract.
[[nodiscard]] RM_API std::string orientation_markdown();

} // namespace defaults
} // namespace roadmaker
