// Copyright 2026 Robomous
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "roadmaker/export.hpp"
#include "roadmaker/road/id.hpp"
#include "roadmaker/road/network.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace roadmaker {

/// How far [m] from the junction mouth a `<signal>` on an arm still counts as
/// belonging to that approach.
///
/// Named rather than buried because three consumers must agree on it: the
/// query below, `edit::clear_signalization` (which erases exactly what the
/// query resolved), and the editor's Signalization panel. ASAM gives no
/// normative distance — §14.1 only requires that "signals shall be positioned
/// in such a way that it is clear to which road or lane they belong" — so this
/// is a RoadMaker authoring convention, sized to cover a stop line, its sign,
/// and an advance head on a city arm without reaching the next junction.
inline constexpr double kSignalApproachWindow = 30.0;

/// One APPROACH of a junction — an incoming arm, what it may do, and what
/// currently controls it (p4-s7, issue #228). The single source shared by the
/// editor's Signal tool and Signalization rows, the signalization commands'
/// validate-first path, the Python bindings, and (later) p4-s8's phase editor,
/// so none of them can disagree about what an approach is or which movements a
/// head gates.
///
/// Plain data, no out-of-line member functions: a member defined in the .cpp
/// would need its own RM_API under RM_BUILD_SHARED=ON, and exporting the struct
/// wholesale would trip MSVC C4251 on its std::string / std::vector members.
struct JunctionApproachInfo {
  /// Identity — the junction-facing end of the incoming arm, matching
  /// `JunctionManeuverInfo::from` and `JunctionStopLineInfo::arm`.
  RoadEnd arm;

  /// The travel heading [rad] of traffic reaching the junction on this arm —
  /// the tangent leaving the arm INTO the junction (`ContactState::into_hdg`).
  /// Opposite arms of one axis therefore differ by ~pi, which is what the
  /// signalization templates cluster on.
  double heading = 0.0;

  /// The placement anchor in the arm road's track coordinates: the station of
  /// the approach's stop line and its mid-span lateral offset. Taken from
  /// `junction_stoplines()` so a head is never placed at a different station
  /// than the line drivers stop at. When the arm solves no stop line (a one-way
  /// arm, a legacy line already painted in the file) the anchor falls back to
  /// `kStopLineDefaultDistance` from the mouth on the reference line.
  double s_stop = 0.0;
  double t_center = 0.0;

  /// The connecting roads this approach feeds — every `junction_maneuvers()`
  /// entry whose `from` is this arm, in connection order.
  ///
  /// DERIVED, never stored: a head gates whatever movements leave its arm, and
  /// nothing in the file says otherwise. p4-s8's per-phase highlighting is
  /// built on exactly this list.
  std::vector<RoadId> gated;

  /// The live signals resolved onto this approach: signals on `arm.road`
  /// within `kSignalApproachWindow` of the mouth whose @orientation admits
  /// traffic travelling toward the junction (the matching direction, or
  /// `None`, which is valid in both). In arena creation order.
  ///
  /// NOT named `signals`: Qt's <QObject> defines `signals` as a macro
  /// (`#define signals public`), so a member of that name makes this header
  /// impossible to include from ANY editor translation unit — the struct
  /// declaration itself fails to parse. The kernel never includes Qt, but its
  /// headers are consumed by code that does.
  std::vector<SignalId> signal_ids;

  /// The odr ids of the top-level `<controller>`s (§14.6) that name at least
  /// one of `signal_ids` in a `<control>`, in arena creation order, deduplicated.
  /// A `<control>` naming no live signal is simply never matched — the query
  /// is dormant-tolerant and never mutates.
  std::vector<std::string> controller_odr_ids;

  /// True when any resolved signal is `@dynamic="yes"` — the approach is
  /// light-controlled rather than sign-controlled.
  bool dynamic = false;
};

/// Every approach of `junction_id`, in the junction's CONNECTION order (first
/// appearance of each incoming arm).
///
/// Walks `Junction::connections` through `junction_maneuvers()` rather than the
/// arm list on purpose: a FOREIGN junction (read from someone else's file, so
/// it has no arm list and cannot be regenerated) still has connections, and its
/// approaches must still be READABLE — inspectable and labelled, just not
/// authorable. The signalization commands reject such a junction themselves;
/// this query does not.
///
/// Returns empty for a stale id and, naturally, for a SPAN (virtual) junction:
/// §12.7 junctions never cut the main road, so they carry no connections — and
/// they "shall not have controllers and therefore no traffic lights"
/// (asam.net:xodr:1.9.0:junctions.virtual.no_controllers) either.
[[nodiscard]] RM_API std::vector<JunctionApproachInfo> junction_signals(const RoadNetwork& network,
                                                                        JunctionId junction_id);

/// Half-width [rad] of the band in which two approach headings are treated as
/// OPPOSITE, and therefore as the two arms of one signal AXIS (30 degrees).
///
/// Deliberately independent of `kManeuverStraightThreshold`, which happens to
/// share the value: that one labels a MOVEMENT for the author, this one pairs
/// ARMS for a phase plan. Changing either must not change the other.
///
/// Lives here beside `cluster_signal_axes`, its only consumer, having been
/// promoted out of `edit::` so `mesh::junction_phases()` can derive the default
/// cycle without depending on the command layer (the `lane_boundary_offsets`
/// promotion precedent).
inline constexpr double kSignalizeAxisTolerance = 0.5235987755982988; // 30 deg

/// Groups the approaches of a junction into signal AXES by clustering their
/// travel headings (p4-s7, issue #228; promoted to `mesh::` for p4-s8's phase
/// derivation).
///
/// The rule, deliberately arm-count-agnostic: walk the approaches in the
/// junction's connection order; the first unassigned one opens an axis, and the
/// still-unassigned approach whose heading is CLOSEST to opposite it — within
/// `kSignalizeAxisTolerance` of exactly pi apart — joins it. An axis therefore
/// holds one or two arms, and an arm with no opposite partner (the stem of a T,
/// the fifth leg of a star) forms its own single-arm axis instead of being
/// forced into someone else's phase. Indices are into the input vector.
[[nodiscard]] RM_API std::vector<std::vector<std::size_t>>
cluster_signal_axes(const std::vector<JunctionApproachInfo>& approaches);

} // namespace roadmaker
