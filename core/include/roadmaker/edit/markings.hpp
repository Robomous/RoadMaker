#pragma once

// Marking authoring: derive road-marking <object>s (crosswalks, later stop
// lines / arrows) from network topology. Pure geometry — returns objects ready
// for edit::add_object; the editor groups the adds into one undo step. Grounded
// in the crosswalk mesh path (docs/design/m3a/02 road marks).

#include "roadmaker/edit/connection.hpp"
#include "roadmaker/export.hpp"
#include "roadmaker/road/id.hpp"
#include "roadmaker/road/lane.hpp"
#include "roadmaker/road/object.hpp"

#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace roadmaker {
class RoadNetwork;
}

namespace roadmaker::edit {

/// Parameters for junction-arm crosswalk authoring. Beyond placement (depth,
/// setback), these carry the parametric-asset fields materialized into the
/// authored crosswalk's outline + <markings> + rm:crosswalk userData (p3-s2):
/// stripe geometry, paint material/colour, and the source asset/category tags.
struct CrosswalkParams {
  double depth_m = 3.0;   ///< crosswalk extent ALONG the road (walking depth)
  double setback_m = 1.0; ///< gap from the junction edge to the near stripe

  double border_width_m = 0.0; ///< edge-line width [m]; 0 = no border lines
  double dash_length_m = 0.5;  ///< stripe length along the crossing [m]; 0 = solid
  double dash_gap_m = 0.5;     ///< gap between stripes [m]
  std::string material;        ///< paint material code (e.g. "material.paint_white")
  std::string color = "white"; ///< e_roadMarkColor for the <marking>s
  std::string asset;           ///< source Library asset key (rm:crosswalk)
  std::string category;        ///< segmentation category tag
};

/// A zebra crosswalk `Object` for each distinct arm road of `junction`, spanning
/// the arm's driving lanes just inside the junction (the "Add crosswalks to all
/// arms" action). Returns {owning road, object} pairs ready for
/// `edit::add_object`; the caller adds them (the editor as one undo macro). Each
/// object carries a unique `odr_id`. Empty when the junction is stale, foreign
/// (no resolvable arm ends), or has no driving lanes. Pure — no mutation, so it
/// is headless-testable and Python-bindable.
[[nodiscard]] RM_API std::vector<std::pair<RoadId, Object>> junction_crosswalks(
    const RoadNetwork& network, JunctionId junction, const CrosswalkParams& params = {});

/// Rebuilds `object`'s crosswalk outline + <markings> + rm:crosswalk userData
/// from `params`, in place — the single authoring path shared by
/// junction_crosswalks and the editor's asset re-materialization, so both
/// produce identical geometry. Reads the object's placement (@s = centre along
/// the road, @t = centre across, @length = crossing span across the road) and
/// writes the walking depth (params.depth_m) into @width; also sets the object
/// type/subtype to a zebra crosswalk when unset. A closed cornerRoad outline
/// (ids 0..3) plus a stripes marking (dash 0 ⇒ solid) and, when
/// border_width>0, two border markings.
RM_API void apply_crosswalk_asset(Object& object, const CrosswalkParams& params);

/// Parameters for junction-arm stop-line authoring.
struct StopLineParams {
  double thickness_m = 0.3; ///< line extent ALONG the road (§13 solid line)
  double setback_m = 4.0;   ///< gap from the junction edge — clears a crosswalk
};

/// A solid stop line (`<object type="roadMark" subtype="signalLines">`) across
/// the APPROACH lanes of each distinct arm road of `junction`, placed just
/// inside the junction (the "Add stop lines to all arms" action). Spans only the
/// lanes leading INTO the junction (one travel direction), so a two-way road
/// gets a stop line per side at its own approach. Returns {owning road, object}
/// pairs ready for `edit::add_object`, each with a unique `odr_id`. Empty when
/// the junction is stale/foreign or an arm has no approach lanes. Pure.
[[nodiscard]] RM_API std::vector<std::pair<RoadId, Object>> junction_stop_lines(
    const RoadNetwork& network, JunctionId junction, const StopLineParams& params = {});

/// Chooses the arrow glyph for one approach `lane` of arm road `arm`. Return an
/// OpenDRIVE roadMark arrow subtype (§13.14.8 Table 117); returning an empty
/// string keeps the `arrowStraight` default.
///
/// `arrowLeft` / `arrowStraight` / `arrowRight` are the three the mesh draws
/// (docs/design/m3a/02 §4). The combined subtypes (`arrowStraightLeft`,
/// `arrowStraightLeftRight`, `arrowMergeLeft`, …) are equally normative and
/// survive a round trip, but currently mesh as a plain straight glyph.
using LaneArrowGlyph = std::function<std::string(RoadId arm, const ContactLane& lane)>;

/// Parameters for junction-arm lane-arrow authoring.
struct LaneArrowParams {
  double length_m = 4.0;   ///< arrow glyph extent along travel
  double setback_m = 7.0;  ///< gap from the junction edge (behind the stop line)
  double width_frac = 0.5; ///< arrow width as a fraction of the lane width

  /// Per-approach-lane glyph choice. Unset (the default) gives every approach
  /// lane `arrowStraight`, so a turn-lane scene opts in and every other caller
  /// is unaffected.
  LaneArrowGlyph glyph;
};

/// A lane arrow (`<object type="roadMark" subtype="arrowStraight">` by default)
/// on each APPROACH lane of every arm of `junction`, pointing into the junction
/// and set back behind the stop line (the "Add lane arrows to all arms" action —
/// one per approach lane, so a two-lane approach gets two). `params.glyph`
/// picks the turn variant per lane. Returns {owning road, object} pairs ready
/// for `edit::add_object`, each with a unique `odr_id`. Empty for a
/// stale/foreign junction or arms with no approach lanes. Pure.
[[nodiscard]] RM_API std::vector<std::pair<RoadId, Object>> junction_lane_arrows(
    const RoadNetwork& network, JunctionId junction, const LaneArrowParams& params = {});

/// Parameters for junction-arm centre-line authoring.
struct CenterMarkParams {
  /// Mark type for lane 0. The solid_solid family renders as true dual
  /// geometry — see the note on `lines` below.
  RoadMarkType type = RoadMarkType::SolidSolid;

  /// e_roadMarkColor (§11.9). Yellow is the centre-line colour GS-1 specifies.
  RoadMarkColor color = RoadMarkColor::Yellow;

  /// Painted stripe width [m].
  double width = 0.12;
};

/// The centre-line `RoadMark` for lane 0 of every lane section of each distinct
/// arm road of `junction` — a double-yellow centre by default (the "Add centre
/// lines to all arms" action). Returns {lane, mark} pairs ready for
/// `edit::set_road_mark`; the caller pushes them (the editor as one undo
/// macro), replacing the single mark the road profile laid down at sOffset 0.
/// Empty when the junction is stale or has no arms. Pure — no mutation, so it
/// is headless-testable and Python-bindable.
///
/// `lines` is deliberately left empty: the writer then emits the compact
/// single-`@width` form and the mesh synthesizes the two stripes at ±`width`
/// (docs/design/m3a/02 §1/§4), which is the same picture with less file. A
/// caller wanting explicit `<line>` geometry builds its own `RoadMark` and
/// calls `set_road_mark` directly.
[[nodiscard]] RM_API std::vector<std::pair<LaneId, RoadMark>> junction_center_marks(
    const RoadNetwork& network, JunctionId junction, const CenterMarkParams& params = {});

} // namespace roadmaker::edit
