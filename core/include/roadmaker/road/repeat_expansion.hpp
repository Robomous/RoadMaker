// Copyright 2026 Robomous
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "roadmaker/export.hpp"

#include <vector>

namespace roadmaker {

struct ObjectRepeat;

/// One placed instance produced by expanding a `<repeat>` section (§13.4).
/// Coordinates are road-relative: `s` is absolute arc length along the road
/// reference line, `t` the lateral offset, `z_offset` the height above the
/// reference-line elevation. The mesher lifts these into a world pose.
struct RepeatInstance {
  double s = 0.0;
  double t = 0.0;
  double z_offset = 0.0;
};

/// Expands one `<repeat>` element into the discrete object instances it places
/// (ASAM OpenDRIVE 1.9.0 §13.4 "Repeating objects", Table 95).
///
/// Normative rules honored:
///  - "If @distance is greater than 0, an instance of the object is repeatedly
///    placed along the specified path as many time as the specified parameters
///    of @distance and @length allow, rounded down." We emit instances at
///    ds = 0, distance, 2*distance, ... k*distance where k = floor(length /
///    distance), i.e. k+1 instances (the origin at ds=0 is inside the section).
///  - "it is not possible or allowed to create an 'incomplete' instance at the
///    end of the provided @length" — so an instance is emitted at ds only while
///    ds <= length (rounded down), never at a partial trailing step.
///  - "The origin of each instance placed in a section of repetition must be
///    inside that section (including its border)." — hence the inclusive
///    endpoint when length is an exact multiple of distance.
///  - "If @distance is set to 0, the object's profile is instead extruded ...
///    creating one continuous shape" — a continuous object is NOT a series of
///    instances, so distance <= 0 yields an empty vector here (the mesher keeps
///    the single-instance fallback for that case).
///
/// t interpolation (Table 95, §13.4): "using a parametric cubic polynomial
/// based on @tStart, @bT, @cT, and @dT if at least one of the coefficients bT,
/// cT, and dT are provided; using a linear interpolation based on @tStart and
/// @tEnd if none of the coefficients are provided." bT/cT/dT were INTRODUCED in
/// 1.9.0 — ASAM OpenDRIVE 1.8.1 §13.2 has no cubic and interpolates t linearly
/// between @tStart and @tEnd unconditionally. We therefore only take the cubic
/// branch when at least one coefficient is present; absent coefficients count
/// as zero. z_offset is always interpolated linearly between @zOffsetStart and
/// @zOffsetEnd (the spec provides no cubic for z).
///
/// The endpoint-survival slack tol::kLength is added before the floor so an
/// exact-fit @length (length == k*distance) keeps its final instance despite
/// binary floating-point error.
[[nodiscard]] RM_API std::vector<RepeatInstance> expand_repeat(const ObjectRepeat& repeat);

} // namespace roadmaker
