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

#include "roadmaker/error.hpp"
#include "roadmaker/export.hpp"
#include "roadmaker/gis/crs.hpp"
#include "roadmaker/osm/graph.hpp"
#include "roadmaker/road/authoring.hpp"
#include "roadmaker/road/road.hpp"
#include "roadmaker/road/road_type.hpp"
#include "roadmaker/xodr/diagnostic.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace roadmaker::osm {

// --- fitting guardrails, all reported when they bind ------------------------

/// Interior points closer than this to the previously retained point are
/// merged. Road-scale rather than a geometric epsilon: clothoid fitting
/// refuses coincident waypoints outright, but a fit through two points three
/// millimetres apart is numerically degenerate well before it refuses.
inline constexpr double kMinWaypointSpacingM = 1.0;

/// Ramer–Douglas–Peucker tolerance. **Below the source's own noise floor**:
/// OSM geometry is traced from imagery at roughly 1–3 m positional accuracy,
/// so simplification is not the dominant error term. It is also well under one
/// driving lane (3.0–3.6 m from the defaults registry), so a simplified
/// centreline still lies inside its own lane. A tolerance without a stated
/// derivation is the one that gets quietly re-tuned.
inline constexpr double kSimplifyToleranceM = 0.5;

/// `fit_clothoid_path` emits one clothoid record per waypoint PAIR, so this is
/// 63 `<planView>` records for one road — already generous. Over the cap the
/// tolerance doubles and retries, bounded, and the tolerance actually used is
/// reported.
inline constexpr std::size_t kMaxWaypointsPerRoad = 64;
inline constexpr int kMaxToleranceDoublings = 8;

/// A turn sharper than this makes the G1 clothoid fit loop or fail; the road is
/// split at that node instead. In OSM a hairpin is usually a switchback, which
/// is a genuine place to split.
inline constexpr double kHairpinTurnRad = 2.618; // 150°

/// How far each arm is pulled BACK from a junction node before it is authored.
///
/// **Without this the junction generator has nowhere to build.** OSM states
/// topology by SHARING A NODE, so every arm of a crossing runs exactly to the
/// same point — and a connecting road between two arms that meet at a point
/// has zero length and no room to curve. Measured before it was added: a 7x7
/// lattice planned 25 junctions and built ZERO, every turn reporting "clothoid
/// fit failed", and the import produced a district of disconnected streets
/// that still looked plausible in the road count.
///
/// 12 m clears `JunctionGenOptions::min_turn_radius_m` (6 m) with margin while
/// staying well inside its `max_end_distance_m` (50 m), which is what groups
/// the pulled-back ends back into one junction.
inline constexpr double kJunctionArmSetbackM = 12.0;

/// Below this deviation, a simplification is not a compromise anyone could act
/// on, and reporting it would bury the ways that really were bent.
///
/// **Not a geometric epsilon, and not a tolerance loosened to accommodate
/// noise** — the two things this project has been bitten by. It is a
/// road-scale answer to a road-scale question, resting on two facts:
///
///   * A way traced along a constant latitude is a CURVE in any projected
///     frame, not a line. Three such nodes 100 m apart deviate about a
///     MILLIMETRE from their own chord after projection, so a micron-scale
///     epsilon calls perfectly clean data compromised.
///   * OSM geometry is accurate to 1–3 m. A centimetre is two to three orders
///     of magnitude below the source's own error, so nothing under it tells a
///     user anything they could act on.
///
/// The fixture that exercises the reporting path deviates 0.4999 m — fifty
/// times this — so the compromise gate still bites hard.
inline constexpr double kLosslessDeviationM = 0.01;

/// A segment shorter than this is left untrimmed rather than trimmed to
/// nothing — better a coincident joint that reports its dropped turns than a
/// road that vanished.
inline constexpr double kMinTrimmedSegmentM = 30.0;

/// Junction generation produces a connecting road per permitted (incoming
/// lane, outgoing lane) pair, so arms grow the result quadratically. A node
/// with more arms than this is refused rather than generated: a twelve-arm OSM
/// node is a generated-road explosion, not an intersection anyone modelled.
inline constexpr std::size_t kMaxJunctionArms = 8;

inline constexpr std::size_t kMaxRoads = 32ULL * 1024ULL;
inline constexpr std::size_t kMaxJunctions = 4ULL * 1024ULL;

/// Individual diagnostics are emitted up to this many, then aggregated by
/// reason with counts. A 50 km² district can produce tens of thousands of
/// compromises, and a panel with thirty thousand rows communicates no better
/// than a panel with none.
inline constexpr std::size_t kMaxNamedDiagnostics = 2000;

// --- the plan ---------------------------------------------------------------

/// How a planned road was compromised on its way out of OSM.
///
/// **Every field is a number the diagnostic quotes.** That is what makes
/// "diagnostics-first fitting" a contract in the type system rather than a
/// promise in a comment: the compromise is a measured value attached to the
/// road, and the diagnostic is a rendering of it — so a build that stopped
/// warning would also have to stop being able to answer `max_deviation_m`,
/// which a test asks directly.
struct FitCompromise {
  std::size_t source_nodes = 0;
  std::size_t kept_nodes = 0;
  std::size_t merged_nodes = 0;

  /// The ACTUAL greatest distance [m] from a dropped vertex to the retained
  /// line — never the tolerance. Quoting the tolerance reports the request
  /// rather than the result: two ways simplified at the same tolerance can
  /// deviate by 0.02 m and 0.49 m, and only one deserves attention.
  double max_deviation_m = 0.0;

  /// The tolerance that ended up being used, which exceeds
  /// `kSimplifyToleranceM` only when the waypoint cap forced doublings.
  double tolerance_used_m = 0.0;

  bool split_at_hairpin = false;

  /// Whether the road's SHAPE survived, which is not the same as whether its
  /// vertex count did.
  ///
  /// A straight way traced with a redundant midpoint simplifies from three
  /// nodes to two with a deviation of exactly zero: nothing about the road
  /// changed, and warning about it would bury the ways that really were bent.
  /// Found by running python/examples/osm_import.py, where EVERY road in the
  /// fixture district reported itself compromised — the earlier definition
  /// compared node counts, and the test guarding it used a two-node way that
  /// could not be simplified at all.
  [[nodiscard]] bool lossless() const {
    return merged_nodes == 0 && !split_at_hairpin && max_deviation_m <= kLosslessDeviationM;
  }
};

/// One road the import will author. Everything fallible has already happened.
struct PlannedRoad {
  OsmId way = 0;
  std::size_t segment = 0;

  /// `osm.<way>.<segment>` — provenance for free, and the re-import
  /// idempotency key. Deliberately NOT a new `rm:` userData code: the
  /// OpenDRIVE id already travels, and every `rm:` code owes a parser, a
  /// fuzz-corpus sample, a round-trip test and an ADR-0008 registry row.
  std::string odr_id;
  std::string name;

  /// Scene metres, ready for `edit::create_road`.
  std::vector<Waypoint> waypoints;
  LaneProfile profile;

  /// The `<type>` record, from `highway=*` and (when the source states one)
  /// `maxspeed`. A way whose source records no limit gets a type and **no
  /// `<speed>`** — inventing one would be guessing where silence is honest.
  std::optional<RoadTypeRecord> type;

  /// Vertical separation is not authored (#496), but the tag is carried so the
  /// follow-up has it and so the diagnostic can name it.
  int layer = 0;
  bool bridge = false;
  bool tunnel = false;

  FitCompromise compromise;
  OsmId start_node = 0;
  OsmId end_node = 0;
};

enum class JointKind : std::uint8_t {
  /// Two road ends meet: a plain link.
  Link,
  /// Three or more: a generated junction.
  Junction,
};

struct PlannedJoint {
  OsmId node = 0;
  int layer = 0;
  JointKind kind = JointKind::Link;

  /// Index into `NetworkPlan::roads`, and which end of it meets here.
  std::vector<std::pair<std::size_t, ContactPoint>> ends;
};

struct NetworkPlan {
  std::vector<PlannedRoad> roads;
  std::vector<PlannedJoint> joints;

  /// {min_x, min_y, max_x, max_y} in scene metres.
  std::array<double, 4> bounds{};

  std::size_t dropped_ways = 0;
  /// Roads whose `odr_id` the network already carries — a re-import of the
  /// same extract is a no-op that says how much it skipped.
  std::size_t skipped_existing = 0;

  [[nodiscard]] bool empty() const { return roads.empty(); }

  /// Plan-view extent [km²], for the import summary.
  [[nodiscard]] RM_API double area_km2() const;
};

struct NetworkPlanResult {
  NetworkPlan plan;
  std::vector<Diagnostic> diagnostics;
};

struct OsmBuildOptions {
  double simplify_tolerance_m = kSimplifyToleranceM;
  double min_waypoint_spacing_m = kMinWaypointSpacingM;
  std::size_t max_waypoints_per_road = kMaxWaypointsPerRoad;
  std::size_t max_roads = kMaxRoads;
  std::size_t max_junction_arms = kMaxJunctionArms;

  /// `highway=service` covers everything from a signposted access road to a
  /// supermarket parking aisle. Off by default: at district scale they
  /// outnumber the road network and turn a legible import into a hairball.
  bool include_service_roads = false;
};

/// Turns an OSM graph into a plan of roads and joints, in scene metres.
///
/// **Pure**: reads a graph, writes a plan, touches no `RoadNetwork`. That is
/// not tidiness — it is what makes diagnostics-first fitting possible at all.
/// `edit::CompositeCommand` unwinds its whole prefix when a child fails, which
/// is right for a four-arm intersection and exactly wrong for a district
/// import where one unfittable way must be *dropped with a warning* rather
/// than abort sixteen hundred good roads. So every fallible decision happens
/// here, before any command exists, and the command carries only work already
/// proven buildable.
///
/// `existing_odr_ids` makes a re-import idempotent: a planned road whose id is
/// already present is skipped and counted.
[[nodiscard]] RM_API Expected<NetworkPlanResult>
plan_network(const OsmGraph& graph,
             const gis::CrsTransform& transform,
             const OsmBuildOptions& options = {},
             std::span<const std::string> existing_odr_ids = {});

} // namespace roadmaker::osm
