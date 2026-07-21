#pragma once

#include "roadmaker/road/id.hpp"
#include "roadmaker/road/road.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace roadmaker {

/// One allowed route through a junction: an incoming road continues onto a
/// connecting road, with per-lane linkage.
struct JunctionConnection {
  RoadId incoming_road;

  /// The connecting road; its Road::junction points back at this junction.
  RoadId connecting_road;

  /// Which end of the connecting road touches the incoming road.
  ContactPoint contact_point = ContactPoint::Start;

  /// Pairs of {incoming lane odr_id, connecting lane odr_id}.
  std::vector<std::pair<int, int>> lane_links;
};

/// An authored override for the pavement fillet at one junction corner — the
/// re-entrant corner between two angularly adjacent arms (p4-s1, issue #225).
///
/// Corners are otherwise DERIVED: the mesher fillets every adjacent arm pair
/// with a radius read off the crossing connecting road. An entry here overrides
/// that derivation for the named pair; entries whose arms are no longer
/// adjacent (or whose roads are gone) lie dormant and are ignored.
///
/// ASAM OpenDRIVE 1.9.0 §12.10 gives `<boundary>` no corner-radius or
/// corner-material carrier (its `<segment>`/`<cornerRoad>` children carry only
/// geometry), so these persist as `<userData code="rm:corners">` on
/// `<junction>` (the rm:arms pattern); the exported
/// `<boundary>`/`<elevationGrid>` stay fully derived. Junction-scope values
/// (`Junction::default_corner_radius`, `Junction::material`) ride a sibling
/// `<userData code="rm:junction">`.
struct JunctionCorner {
  /// The corner's identity: the ordered pair of CCW-adjacent arms it sits
  /// between (arm_a's right edge meeting arm_b's left edge, entering).
  RoadEnd arm_a;
  RoadEnd arm_b;

  /// Authored fillet radius [m]. Unset ⇒ the derived radius is used.
  std::optional<double> radius;

  /// Authored tangent-leg setbacks [m] from the edge-line intersection to each
  /// side's tangency point. Unset ⇒ symmetric legs from `radius`.
  std::optional<double> extent_a;
  std::optional<double> extent_b;

  /// Bare catalog material names (e.g. "concrete") for the corner's sidewalk
  /// wedge and the median nose of the arms meeting here (p4-s2, issue #226).
  /// Unset ⇒ the mesher emits no such overlay at all, so an unauthored
  /// junction meshes exactly as it did before the feature existed.
  ///
  /// Tokens are restricted to `[A-Za-z0-9_.-]+` because the persistence
  /// grammar joins fields with ':' and entries with ';' and does not escape.
  std::optional<std::string> sidewalk_material;
  std::optional<std::string> median_material;
};

/// An authored override for the stop line at one junction arm (p4-s3, issue
/// #318).
///
/// Stop lines are otherwise DERIVED: every arm whose junction-facing end has
/// driving lanes gets one, set back `kStopLineDefaultDistance` from the mouth
/// and spanning the approach lanes. An entry here overrides that derivation for
/// the named arm; entries whose arm is no longer part of the junction (or whose
/// road is gone) lie dormant and are ignored, exactly like JunctionCorner.
///
/// ASAM OpenDRIVE 1.9.0 §13.7 Table 117 gives the stop line a carrier — an
/// `<object type="roadMark" subtype="signalLines">` — but no way to say which
/// road end owns it or how far back it sits (the object is placed absolutely at
/// an s/t, and the subtype is only documented as "referenced by a signal").
/// The parametric record therefore rides a `<userData code="rm:stopline">` on
/// that object (§7.2): the object stays a valid, self-contained stop line for a
/// foreign reader (ADR-0008 Layer 0) while RoadMaker absorbs the userData back
/// into this record on load (Layer 1). The object is materialized on write and
/// is never a live arena Object.
struct StopLine {
  /// The stop line's identity: the junction-facing end of its arm road.
  RoadEnd arm;

  /// Authored setback [m] from the junction mouth. Unset ⇒
  /// `kStopLineDefaultDistance`. Stored UNCLAMPED (authored-like, as with
  /// JunctionCorner::radius) and clamped to the road at solve time, so an arm
  /// that later shortens never fails the mesh.
  std::optional<double> distance;

  /// Which travel direction the line spans. false ⇒ the approach (incoming)
  /// lanes, the normal case; true ⇒ the outgoing lanes.
  bool flipped = false;

  /// The odr id of the crosswalk object this line was placed alongside, empty
  /// when free-standing. Purely informational provenance — the link does not
  /// tie the two lifetimes together.
  std::string crosswalk_odr_id;
};

/// An authored override for ONE connecting road's contribution ("surface
/// span") to the junction floor (p4-s5, issue #320).
///
/// NAME COLLISION, read this before reaching for `SpanArm`: a SurfaceSpan is a
/// road's stretch of the FLOOR UNION, one per connecting road, and exists only
/// on ordinary arm junctions. `SpanArm` is something else entirely — the
/// s-interval of a VIRTUAL (span) junction (§12.7). A span junction has no
/// floor at all, so it never carries SurfaceSpan records.
///
/// Sparse and dormant-tolerant exactly like JunctionCorner and StopLine: a
/// connecting road with no entry contributes by pure derivation, and an entry
/// whose road was erased (its turn dropped by regeneration) is ignored and
/// never written. `regenerate_junction` leaves the list untouched — it matches
/// surviving turns by TurnKey and rewrites geometry onto the SAME RoadId, so a
/// record outlives a turn-set change.
///
/// ASAM OpenDRIVE 1.9.0 §12.10 gives `<junction>` no carrier for how its
/// pavement is triangulated (`<boundary>` and `<elevationGrid>` are pure
/// output geometry), so these ride `<userData code="rm:floor">` on the
/// junction — ADR-0008 Layer 1, the rm:corners pattern.
struct SurfaceSpan {
  /// The span's identity: the connecting road whose contribution it overrides.
  RoadId road;

  /// false ⇒ this road's SAMPLES leave the fill inputs: its border elevations
  /// stop being Dirichlet sources, its centerline stops constraining the
  /// harmonic solve, and its samples stop protecting boundary debris from the
  /// short-segment merge. Its FOOTPRINT stays in the union either way, so the
  /// floor's coverage and the exported `<boundary>` never change — this is an
  /// escape valve for interior triangulation artifacts, not a way to punch a
  /// hole in the pavement.
  bool included = true;

  /// Precedence where span footprints OVERLAP: the higher sort index supplies
  /// the elevation there. Free integer (the editor's Raise/Lower simply move it
  /// by one), so a record survives regeneration without any renumbering pass.
  int sort_index = 0;
};

/// Magnitude bound on `SurfaceSpan::sort_index`. The value is a free integer —
/// Raise/Lower simply move it by one — but a bound keeps the persistence
/// grammar's field short and lets the reader reject a corrupt value outright
/// instead of loading an absurd one. Far above any plausible junction's turn
/// count, so no author ever meets it.
inline constexpr int kMaxSurfaceSpanSortIndex = 1000;

/// How a maneuver (a connecting road) turns through the junction (p4-s6, issue
/// #227).
///
/// LAYER 1, RoadMaker-only: ASAM OpenDRIVE has NO turn-type element. §12.2
/// Table 56 gives `<connection>` exactly @connectingRoad, @contactPoint, @id
/// and @incomingRoad, and §12.4/§12.4.2 describe a connecting road purely by
/// its geometry and lane linkage — the turn a driver perceives is implicit in
/// the plan view. The type is therefore DERIVED from the arm-face headings and
/// only stored when the author overrides it.
enum class TurnType {
  Left,
  Straight,
  Right,
  UTurn,
};

/// An authored override for ONE connecting road's path through a junction — its
/// "maneuver" (p4-s6, issue #227).
///
/// Sparse and dormant-tolerant exactly like SurfaceSpan, and keyed the same
/// way: by the connecting RoadId. `retarget_junction` matches surviving turns
/// by TurnKey and rewrites geometry onto the SAME RoadId, so a record outlives
/// a turn-set change; a record whose road was erased lies dormant and is never
/// written.
///
/// AUTHORS-NOTHING ⇒ ERASE: a record that is unlocked, has no turn-type
/// override, no endpoint offsets and no control points is dropped rather than
/// kept, so flipping an override twice (or locking and unlocking) produces a
/// file byte-identical to the one before the first edit — the StopLine /
/// SurfaceSpan idiom.
///
/// `locked` is the finer grain of `Junction::locked`: the junction lock gates
/// the AUTOMATIC regeneration loop for the whole junction, while this keeps ONE
/// hand-shaped connecting road's geometry across an explicit regeneration. It
/// is also the survival mechanism for an explicit U-turn, which the planner
/// never produces and would otherwise drop on the next regeneration.
struct Maneuver {
  /// The maneuver's identity: the connecting road whose path it overrides.
  RoadId road;

  /// Lock Geometry — regeneration keeps this road's plan view, length,
  /// elevation and lane width instead of replanning them, and keeps the road
  /// itself even when the plan no longer contains its turn. Set implicitly by
  /// any manual geometry edit (`edit::set_maneuver_path`).
  bool locked = false;

  /// Authored turn type. Unset ⇒ the type computed from the arm-face headings.
  /// Purely semantic (labels, later turn-lane arrows and signal phases); it
  /// never moves geometry, which is why an explicit REBUILD keeps it while
  /// clearing everything else here.
  std::optional<TurnType> turn_type;

  /// Endpoint slide [m] along the INCOMING arm's cross-section face, measured
  /// from the anchor lane's inner boundary along the arm's +t axis. Unset ⇒ 0
  /// (the derived anchor). Bounded by the anchor lane's own span.
  std::optional<double> start_offset;

  /// Endpoint slide [m] along the OUTGOING arm's face; same convention.
  std::optional<double> end_offset;

  /// Authored INTERIOR waypoints of the path, in driving direction. The
  /// endpoints are NEVER stored here — they are derived from the arm faces plus
  /// the offsets above, so a maneuver keeps meeting its arms when they move.
  std::vector<Waypoint> control_points;
};

/// Upper bound on `Maneuver::control_points`. A hand-shaped junction path needs
/// a handful of points; the bound keeps the persistence grammar's list short
/// and lets a reader reject a corrupt record outright.
inline constexpr std::size_t kMaxManeuverControlPoints = 64;

/// Membership span of a virtual (span) junction: a stretch [s_start, s_end] of
/// one road that belongs to the junction without cutting that road.
///
/// See ASAM OpenDRIVE 1.9.0 §12.7 (identical in 1.8.1 §12.7): a
/// `<junction type="virtual">` names one @mainRoad plus the @sStart/@sEnd
/// interval on it, so a mid-road crosswalk or a parking-lot entry needs no
/// break in the main road. RoadMaker generalizes that to a LIST so one span
/// junction can cover several parallel roads (a crosswalk across a divided
/// carriageway); Layer 0 exports spans[0] as the spec-mandated main road and
/// the full list rides `<userData code="rm:spans">` (ADR-0008 Layer 1).
struct SpanArm {
  /// The road the span lies on. `spans[0].road` is exported as @mainRoad.
  RoadId road;

  /// Start of the covered interval in the road's reference-line s [m].
  double s_start = 0.0;

  /// End of the covered interval in the road's reference-line s [m].
  /// `s_end >= s_start`; both are `t_grEqZero` per §12.7 Table 69.
  double s_end = 0.0;
};

struct Junction {
  /// OpenDRIVE junction id (string, unique within a network).
  std::string odr_id;

  std::string name;

  std::vector<JunctionConnection> connections;

  /// The road ends this junction was generated from, in selection order —
  /// the deterministic input the connecting-road generator re-runs from on
  /// regeneration (docs/design/m2/02_editing_tools.md §6). Persisted in
  /// .xodr as `<userData code="rm:arms">` on the junction so edit sessions
  /// survive save/load; junctions from foreign files load with an empty arm
  /// list and cannot regenerate until recreated.
  std::vector<RoadEnd> arms;

  /// Authored corner-fillet overrides, keyed by adjacent arm pair. Sparse: a
  /// corner with no entry keeps the derived fillet. `regenerate_junction`
  /// leaves this untouched — an override outlives a turn-set change and simply
  /// goes dormant if its pair stops being adjacent.
  std::vector<JunctionCorner> corners;

  /// Junction-wide fillet radius [m] applied to every corner that carries no
  /// per-corner `radius` (p4-s2, issue #226). Resolution order at solve time:
  /// per-corner override > this default > derived. Authored-like: uncapped
  /// here, clamped only to the geometric `max_radius` when solved.
  std::optional<double> default_corner_radius;

  /// Authored stop-line overrides, keyed by arm. Sparse in exactly the sense
  /// `corners` is: an arm with no entry still gets the derived default line, and
  /// `regenerate_junction` leaves this untouched so an override outlives a
  /// turn-set change and simply goes dormant if its arm leaves the junction.
  std::vector<StopLine> stoplines;

  /// Authored floor-contribution overrides, keyed by connecting road (p4-s5,
  /// issue #320). Sparse in exactly the sense `corners` and `stoplines` are: a
  /// connecting road with no entry contributes by pure derivation, and
  /// `regenerate_junction` leaves this untouched so an override outlives a
  /// turn-set change and simply goes dormant if its turn is dropped.
  ///
  /// Always empty on a span (virtual) junction — it has no floor to control.
  std::vector<SurfaceSpan> surface_spans;

  /// Authored maneuver overrides, keyed by connecting road (p4-s6, issue #227).
  /// Sparse in exactly the sense `surface_spans` is: a connecting road with no
  /// entry is fully derived, and `retarget_junction` under the default
  /// `ManeuverPolicy::Respect` leaves the list untouched — a record outlives a
  /// turn-set change and simply goes dormant if its turn is dropped.
  ///
  /// A LOCKED record additionally survives regeneration structurally: its
  /// connecting road is kept even when the plan no longer contains its turn,
  /// which is what keeps an explicit U-turn (a turn the planner never emits)
  /// alive. `edit::rebuild_maneuvers` is the explicit way back to derivation.
  ///
  /// Always empty on a span (virtual) junction — it has no connections at all.
  std::vector<Maneuver> maneuvers;

  /// Bare catalog material name for the junction carriageway (the floor).
  /// Empty ⇒ the derived asphalt look, mirroring `Surface::material`.
  std::string material;

  /// Explicit user control over automatic regeneration (p4-s4, issue #319).
  /// Automatic regeneration loops (the ones that re-run the connecting-road
  /// generator after a neighbouring edit) SKIP a locked junction, so hand-tuned
  /// connections, corners and stop lines survive edits to the arms. An explicit
  /// `regenerate_junction` is still allowed — locking guards the automatic
  /// pass, it does not freeze the junction.
  ///
  /// Persisted as `locked=1` inside `<userData code="rm:junction">`; omitted
  /// when false so an unlocked junction keeps exactly its pre-#319 bytes.
  bool locked = false;

  /// Membership spans of a VIRTUAL (span) junction — ASAM OpenDRIVE 1.9.0
  /// §12.7 (identical in 1.8.1 §12.7). Non-empty ⇔ this is a span junction.
  ///
  /// arms-xor-spans: a span junction has an EMPTY `arms` list and NO
  /// `connections` — the main road is never cut, so there is nothing to
  /// generate and nothing to fillet. It is also always `locked` (there is no
  /// automatic derivation that could regenerate it). Conversely an arm-based
  /// junction has an empty `spans` list. Nothing in the kernel produces a
  /// junction with both.
  ///
  /// State is DERIVED from these fields, never stored as an enum:
  ///   foreign   = arms.empty() && spans.empty()
  ///   automatic = !arms.empty() && !locked
  ///   locked    = !arms.empty() && locked
  ///   span      = !spans.empty()   (always locked)
  std::vector<SpanArm> spans;
};

} // namespace roadmaker
