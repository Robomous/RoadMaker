#pragma once

#include "roadmaker/road/id.hpp"
#include "roadmaker/road/road.hpp"

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

  /// Bare catalog material name for the junction carriageway (the floor).
  /// Empty ⇒ the derived asphalt look, mirroring `Surface::material`.
  std::string material;
};

} // namespace roadmaker
