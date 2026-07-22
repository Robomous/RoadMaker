// Copyright 2026 Robomous
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// Named tolerances used across the kernel. Never inline magic epsilons —
/// add a constant here instead. Units are noted per constant.
namespace roadmaker::tol {

/// Length comparisons and station deduplication [m].
inline constexpr double kLength = 1e-6;

/// Angle comparisons (heading continuity checks) [rad].
inline constexpr double kAngle = 1e-9;

/// Default chord-deviation tolerance for curvature-adaptive sampling [m].
inline constexpr double kChord = 0.01;

/// Below this |curvature| a primitive is numerically a straight line [1/m].
inline constexpr double kCurvatureEpsilon = 1e-12;

/// Round-trip geometric equality: position [m] and heading [rad].
inline constexpr double kRoundTripPosition = 1e-4;
inline constexpr double kRoundTripHeading = 1e-6;

/// Merge (edit::merge_roads) weld tolerances: the joining ends must already be
/// this close in position [m] and heading [rad] — the weld absorbs the residual
/// so the seam is vertex-exact.
inline constexpr double kMergePositionGap = 0.01;
inline constexpr double kMergeHeading = 1e-3;

/// Weld coincidence tolerances for the connection engine (edit::connection):
/// the maxima verify_junction_welds and the gap-closing weld assertions allow
/// between two ends that are meant to be continuous — position [m], heading
/// [rad], and plan-view curvature [1/m]. Curvature is looser: a G1 weld leaves
/// a curvature step, and only a G2 close_gap drives it below kWeldCurvature.
inline constexpr double kWeldPosition = 1e-3;
inline constexpr double kWeldHeading = 1e-3;
inline constexpr double kWeldCurvature = 5e-3;

} // namespace roadmaker::tol
