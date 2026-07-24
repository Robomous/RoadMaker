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

#include <string>

namespace roadmaker {

enum class LaneType;

/// The machine-readable realism-defaults registry (#413).
///
/// docs/domain/realism_defaults.md is the canonical spec for every value
/// here; this namespace is its single code mirror. The authoring templates
/// (LaneProfile), the Library road styles (RoadStyle), the per-lane-type
/// fallback widths, and the marking constants all derive from these
/// constants — nothing else in the tree restates them. The §1.2/§1.3 tables
/// in the spec doc are rendered by cross_section_markdown() /
/// markings_markdown(), and test_defaults_registry.cpp fails CI when the
/// committed doc and this registry disagree (the shortcut_registry
/// mechanism). Change a default by PRing the spec doc and this file
/// together, then regenerating the doc tables from the renderers.
///
/// Units are SI meters throughout (kernel frame).
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

} // namespace defaults
} // namespace roadmaker
