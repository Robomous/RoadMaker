#pragma once

#include "roadmaker/edit/command.hpp"
#include "roadmaker/export.hpp"
#include "roadmaker/road/authoring.hpp"
#include "roadmaker/road/lane.hpp"
#include "roadmaker/road/object.hpp"
#include "roadmaker/road/road.hpp"

#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace roadmaker {
class RoadNetwork;
} // namespace roadmaker

/// Edit-operation factories (docs/m2/01_editing_framework.md §2.3). Each
/// returns a Command capturing value snapshots of exactly the objects it
/// touches; validation happens in the factory where possible, and a factory
/// given invalid input returns a command whose apply() reports the error
/// without touching the network. Create a command and push it immediately —
/// snapshots are taken against the network state at factory time.
namespace roadmaker::edit {

// --- geometry (re-fit clothoid through authoring waypoints, §2.5) ----------
// Roads loaded without rm:waypoints get them derived from geometry-record
// endpoints on their first node edit. That derivation re-fit interpolates
// the chain's headings (G1 Hermite), so pure line/arc/spiral chains are
// reproduced within rm::tol; paramPoly3 roads are re-fitted approximately
// (the editor's one-time notice). Once waypoints are recorded, later edits
// re-fit through positions alone — the authored-road reflow semantics.

/// The editing nodes of `road`: its recorded authoring waypoints, or — for
/// roads loaded without rm:waypoints metadata — the derived set (every
/// geometry-record start plus the final endpoint, §2.5). What the waypoint
/// command factories below edit, and what an editor renders as node handles.
[[nodiscard]] RM_API std::vector<Waypoint> effective_waypoints(const Road& road);

/// Stations [m] of effective_waypoints() along the road's CURRENT reference
/// line (record starts plus the total length). Fails when recorded authoring
/// waypoints are out of sync with the plan-view geometry (hand-edited
/// rm:waypoints metadata) — waypoint count must equal record count + 1.
[[nodiscard]] RM_API Expected<std::vector<double>> waypoint_stations(const Road& road);

[[nodiscard]] RM_API std::unique_ptr<Command>
move_waypoint(const RoadNetwork& network, RoadId road, std::size_t index, Waypoint to);

/// move_waypoint, plus in-place regeneration of every junction the road touches,
/// as ONE command — so a mid-drag preview can show the connecting roads
/// following the arm instead of snapping into place on release (#156).
///
/// Falls back to a plain move_waypoint when the road touches no junction that
/// can regenerate, so a free-road drag keeps exactly its previous shape and
/// cost. Junctions with no recorded arms (read from a foreign file) are skipped
/// rather than refused: regenerate_junction treats them as an error, but a drag
/// must not fail because some unrelated junction came from someone else's file.
///
/// Unlike the editor's commit-time regeneration, this is **atomic** — if a
/// regeneration fails the whole move is refused and the network is untouched.
/// That is the right trade mid-drag: the frame is simply rejected and the last
/// good preview stands. It is also close to unreachable, since regeneration
/// only fails when the turn set changes and a node drag moves geometry, not
/// lanes.
[[nodiscard]] RM_API std::unique_ptr<Command> move_waypoint_following_junctions(
    const RoadNetwork& network, RoadId road, std::size_t index, Waypoint to);

/// Inserts before `index` (index == waypoint count appends).
[[nodiscard]] RM_API std::unique_ptr<Command>
insert_waypoint(const RoadNetwork& network, RoadId road, std::size_t index, Waypoint at);

[[nodiscard]] RM_API std::unique_ptr<Command>
delete_waypoint(const RoadNetwork& network, RoadId road, std::size_t index);

/// Minimum spacing [m] between an inserted node and an existing one — a UX
/// constraint (matches the editor's node pick radius) so a bend point never
/// lands on top of another node.
inline constexpr double kMinNodeSpacingM = 2.0;

/// Inserts a bend node at station `s`, PINNING the heading at every node
/// (existing and new) from the current curve so the re-fit reproduces every
/// untouched record exactly (line/arc/spiral within rm::tol; a paramPoly3
/// covering record is re-fitted approximately with the one-time derivation
/// notice) and only the record covering `s` splits into two G1-Hermite halves.
/// This is why insert_waypoint is insufficient: it approximates the new node's
/// heading (foreign roads) or re-fits positions-only (authored roads), both of
/// which drift the shape. The new node's position is plan_view.evaluate(s).
/// Rejects a stale road, an `s` outside (0, length), or an `s` within
/// kMinNodeSpacingM of an existing node.
[[nodiscard]] RM_API std::unique_ptr<Command>
insert_node_at(const RoadNetwork& network, RoadId road, double s);

// --- topology ---------------------------------------------------------------

/// Authors a new clothoid road (auto-assigned OpenDRIVE id; an empty name
/// auto-names it "Road <odr id>"). `locked` end headings pin the fit for
/// tangent-snap chaining (02_editing_tools.md §2). Undo frees the id; redo
/// resurrects the identical road under the same ids.
[[nodiscard]] RM_API std::unique_ptr<Command> create_road(std::vector<Waypoint> waypoints,
                                                          LaneProfile profile,
                                                          std::string name,
                                                          EndpointHeadings locked = {});

/// Authors a new clothoid road AND welds its start to the free road end
/// `link_start` in one undoable command — the Create Road tangent-continuation
/// snap routed through the weld path (gate finding 3), so a snapped road is
/// genuinely linked, not merely adjacent. The road is created regardless; the
/// weld is skipped (a no-op stage) when `link_start` is not linkable
/// (edit::check_linkable), so the create never fails on the link.
[[nodiscard]] RM_API std::unique_ptr<Command> create_linked_road(const RoadNetwork& network,
                                                                 std::vector<Waypoint> waypoints,
                                                                 LaneProfile profile,
                                                                 std::string name,
                                                                 RoadEnd link_start,
                                                                 EndpointHeadings locked = {});

/// Extends a road past its END by fitting a forward clothoid from the end
/// pose through `to` and appending it to the plan view — the "keep drawing off
/// the end" gesture. The appended geometry is curvature-continuous with the old
/// end by construction (fit_forward_clothoid honours the end pose AND curvature),
/// so no kink and no curvature step appears at the join; elevation is continued
/// with matching z and grade. The last lane section (and its widths) simply
/// spans the new length. ONE command over the single road (apply→revert
/// byte-identical). START-end extension is out of scope.
/// Errors (invalid_command): a stale road, `end.contact == Start`, an END that
/// is already linked, a road with no authoring waypoints, or a `to` the forward
/// clothoid cannot reach (behind the end).
[[nodiscard]] RM_API std::unique_ptr<Command>
extend_road(const RoadNetwork& network, RoadEnd end, Waypoint to);

/// Authors a new clothoid road AND tees its `teed_end` into the SIDE of
/// `target` at station `s` in one undoable command — Create Road's side-snap
/// path (create + attach_t_junction, mirroring create_linked_road's create +
/// weld). Errors surface attach_t_junction's guards (target already in a
/// junction, a paramPoly3 record at a cut, the drop too near a road end) at
/// apply.
[[nodiscard]] RM_API std::unique_ptr<Command> create_teed_road(const RoadNetwork& network,
                                                               std::vector<Waypoint> waypoints,
                                                               LaneProfile profile,
                                                               std::string name,
                                                               RoadId target,
                                                               double s,
                                                               ContactPoint teed_end,
                                                               EndpointHeadings locked = {});

/// Authors a new clothoid road that crosses `target`, then forms a 4-way
/// junction at the crossing in one undoable command — Create Road's
/// draw-across path (create + assembly::cross_roads). Errors surface
/// cross_roads's guards (no interior crossing, a road already in a junction, a
/// paramPoly3 at a cut) at apply.
[[nodiscard]] RM_API std::unique_ptr<Command> create_crossing_road(const RoadNetwork& network,
                                                                   std::vector<Waypoint> waypoints,
                                                                   LaneProfile profile,
                                                                   std::string name,
                                                                   RoadId target,
                                                                   EndpointHeadings locked = {});

/// Translates whole roads by (dx, dy) [m] in the plan-view plane: shifts every
/// geometry-record start position and every authoring waypoint; headings,
/// lengths, s-values, lanes, elevation and marks are untouched, so undo is
/// byte-identical from the value snapshots. ONE command for N roads (not a
/// macro): links BETWEEN two roads in the set survive the move, while a pred/
/// succ link leaving the set is cleared on BOTH sides (break + move = one undo
/// step). Refuses (invalid_command, junction named) any road participating in a
/// junction — connecting road, arm, or junction back-reference — since its pose
/// is generated, not free. `road_ids` is de-duplicated; empty is an error.
[[nodiscard]] RM_API std::unique_ptr<Command>
translate_roads(const RoadNetwork& network, std::span<const RoadId> road_ids, double dx, double dy);

/// Single-road convenience over translate_roads (same rules and diagnostics).
[[nodiscard]] RM_API std::unique_ptr<Command>
translate_road(const RoadNetwork& network, RoadId road, double dx, double dy);

/// Rotates a single road about the world pivot (pivot_x, pivot_y) by `angle`
/// [rad] CCW: every geometry record's start position rotates about the pivot and
/// `angle` is added to its heading, and authoring waypoints rotate too.
/// Elevation (s-relative) and each record's shape coefficients (defined in the
/// record's local u/v frame, which follows the heading) are unchanged — the
/// rotation is rigid, so arcs/spirals/paramPoly3 need no coefficient edit. Same
/// connectivity policy as translate_road: refuses a junction road (invalid_command,
/// junction named), and a road-level link to a road that does not rotate with it
/// breaks on BOTH sides (break + rotate = one undo step). Undo is byte-identical.
[[nodiscard]] RM_API std::unique_ptr<Command>
rotate_road(const RoadNetwork& network, RoadId road, double angle, double pivot_x, double pivot_y);

/// Splits at station s: the original keeps [0, s), a new road (auto id)
/// gets [s, length). Sections, profiles, links and lane links are carried
/// over. M2 restriction: roads participating in a junction cannot be split,
/// and the split station must not fall inside a paramPoly3 record.
[[nodiscard]] RM_API std::unique_ptr<Command>
split_road(const RoadNetwork& network, RoadId road, double s);

/// Checks whether `a`'s END can be merged into `b`'s START — the editor's
/// enablement query and the merge factory's precondition. Ok() means merge_roads
/// will succeed; otherwise the error message is the verbatim reason (one per
/// refusal): stale/equal ids; either road in or touching a junction; a joining
/// end already linked to a third road; the roads meet the wrong way round
/// ("reverse one first (coming soon)"); the joining ends farther apart than
/// kMergePositionGap; headings off by more than kMergeHeading; or the seam lane
/// profiles / elevation not matching. reverse_road is deferred, so only the
/// End(a)→Start(b) orientation merges (the editor normalizes the argument order).
[[nodiscard]] RM_API Expected<void> check_mergeable(const RoadNetwork& network, RoadId a, RoadId b);

/// Merges `a`'s END with `b`'s START into one road that keeps `a`'s id (`b` is
/// erased). `b`'s geometry is re-anchored to `a`'s end pose (the weld absorbs
/// the ≤ kMergePositionGap / kMergeHeading residual — vertex-exact seam), the
/// profiles and lane sections concatenate (sections are NOT coalesced — the
/// seam boundary survives, so split→merge is geometry-identical, not
/// byte-identical; coalescing is a follow-up), `b`'s far-end links and any far
/// neighbor's back-link re-point onto the merged road. Preconditions are
/// check_mergeable; a failing factory returns invalid_command. Undo is
/// byte-identical via the value snapshots.
[[nodiscard]] RM_API std::unique_ptr<Command>
merge_roads(const RoadNetwork& network, RoadId a, RoadId b);

/// Deletes the road with its sections and lanes, taking the full referential
/// closure with it (02_editing_tools.md §7): junction connections referencing
/// the road are removed, and where the road was the INCOMING one their
/// connecting roads are deleted too; surviving roads' links into the deleted
/// set are cleared. Undo restores every removed object and link under its
/// original id, so references held elsewhere become valid again.
[[nodiscard]] RM_API std::unique_ptr<Command> delete_road(const RoadNetwork& network, RoadId road);

/// Tuning for the connecting-road generator (docs/design/m2/02 §6).
struct JunctionGenOptions {
  /// Two arm ends farther apart than this [m] make create_junction fail.
  double max_end_distance_m = 50.0;

  /// A turn whose fitted connecting-road length exceeds this multiple of the
  /// straight-line end distance is dropped (nearly-parallel arms whose
  /// clothoid would loop). k=4 per 02 §6.
  double max_loop_factor = 4.0;

  /// Tightest turn radius [m] generated turns should need. attach_t_junction
  /// derives its auto gap from it (gap ≥ r·tan(Δθ/2) per turn — the junction
  /// area must give every turn room to stay drivable; see
  /// docs/design/hardening/t_junction.md §gap auto-sizing). 6 m is the
  /// tightest urban curb-return radius the templates need.
  double min_turn_radius_m = 6.0;
};

/// Non-mutating summary of what create_junction would generate for `ends`:
/// the connection count (status-bar feedback) and any turns the generator
/// drops. Same planner create_junction runs, so the editor previews exactly
/// what generation will produce. Errors match create_junction's factory
/// errors (fewer than 2 ends, stale/duplicate ends, an occupied link slot,
/// or ends farther apart than max_end_distance_m).
struct JunctionPreview {
  int connection_count = 0;

  /// Human-readable "road A→road B (lane i→j)" for every dropped turn.
  std::vector<std::string> dropped_turns;
};

[[nodiscard]] RM_API Expected<JunctionPreview>
preview_junction(const RoadNetwork& network,
                 std::span<const RoadEnd> ends,
                 const JunctionGenOptions& options = {});

/// Creates a common junction from `ends` (auto id): links each incoming road
/// end to the junction and generates the connecting roads for every permitted
/// turn — one connecting road per (incoming lane, outgoing lane) pair, a G1
/// clothoid built in driving direction (contactPoint="start"), single lane
/// section, lane width blended linearly source→target (02 §6). The arm list
/// is recorded on the junction for deterministic regeneration. Errors: fewer
/// than 2 ends, duplicate ends, an end whose link slot is already occupied,
/// or two ends farther apart than options.max_end_distance_m. Turns whose fit
/// loops (see max_loop_factor) are dropped, not an error — use
/// preview_junction to surface them. Undo frees every created id; redo
/// resurrects them identically.
[[nodiscard]] RM_API std::unique_ptr<Command>
create_junction(const RoadNetwork& network,
                std::span<const RoadEnd> ends,
                const JunctionGenOptions& options = {});

/// Tuning for attach_t_junction (docs/design/hardening/t_junction.md).
struct TAttachOptions {
  /// Half-length of the junction area removed from the target around `s`
  /// [m]. 0 = auto: the largest of the width bound (max(target half-width at
  /// s, attaching road half-width at its end) + 1 m — the area must at least
  /// span the crossing road's body), the turning bound
  /// (generation.min_turn_radius_m · tan(Δθ/2) + 1 m over both generated
  /// turn directions, Δθ clamped to 150° — the turns must fit at drivable
  /// curvature), and the fillet bound (branch half-width + the 3 m corner
  /// fillet's tangent leg + 0.5 m — the junction surface's pavement-edge
  /// arcs must fit between the branch corners and the cut faces;
  /// docs/design/hardening/t_junction.md §gap auto-sizing).
  double gap_m = 0.0;

  /// Passed through to the M2 connecting-road generator.
  JunctionGenOptions generation;
};

/// The half-length [m] of the junction area attach_t_junction would remove
/// around `s` for these inputs: options.gap_m when positive, else the auto
/// formula (width bound vs turning bound — see TAttachOptions::gap_m).
/// The editor's tee preview highlights [s−gap, s+gap] with this so the user
/// sees exactly the span generation will replace. Returns 0 for stale ids.
[[nodiscard]] RM_API double t_attach_gap(const RoadNetwork& network,
                                         RoadEnd end,
                                         RoadId target,
                                         double s,
                                         const TAttachOptions& options = {});

/// Attaches `end` to the SIDE of `target` at station `s` — the T-junction
/// workflow (docs/design/hardening/t_junction.md): splits the target at
/// s±gap, deletes the middle stub (it becomes the junction area, and its
/// deletion closure frees the facing link slots), then creates a common
/// junction from the three resulting ends (head:End, tail:Start, `end`)
/// generating ALL legal turns — the documented default policy; permission
/// pruning is a later milestone. ONE command: apply→revert leaves
/// write_xodr byte-identical, and a failure anywhere in the chain unwinds
/// the already-applied prefix so the network is untouched. Errors: stale
/// ids, end.road == target, an occupied link slot on `end`, a target
/// participating in a junction, s±gap not strictly inside the target, or a
/// paramPoly3 record at a cut station (M2 split restrictions).
[[nodiscard]] RM_API std::unique_ptr<Command> attach_t_junction(const RoadNetwork& network,
                                                                RoadEnd end,
                                                                RoadId target,
                                                                double s,
                                                                const TAttachOptions& options = {});

/// Whether a regeneration may change the junction's turn set.
enum class TurnSetPolicy {
  /// Turns may be added and dropped: new connecting roads are created, ones
  /// whose turn disappeared are erased, and the connection table is rewritten.
  /// The turns that survive keep their connecting-road IDs.
  AllowChange,
  /// Only geometry and widths may change; a different turn set is an error.
  /// For the per-frame preview path ONLY — see regenerate_junction.
  InPlaceOnly,
};

/// Re-runs the generator from a junction's recorded arm list and replaces its
/// connecting-road geometry and lane widths in place (02 §6 "Dependency
/// tracking"). The editor triggers this after any edit to an incoming road
/// (via junctions_touching). An empty arm list (foreign junction) is an error.
/// A no-op regeneration writes byte-identical output.
///
/// A turn that survives keeps its connecting-road ID — matching is by the
/// (incoming road+contact+lane, outgoing road+contact+lane) key, not by order
/// — so held references and the undo stack stay valid across a regeneration.
///
/// Under AllowChange (the default) a lane added to, removed from, or retyped
/// on an incoming road regenerates the junction: turns that appeared get fresh
/// connecting roads, turns that vanished have theirs erased.
///
/// `policy` exists for ONE caller. A preview session reverts and DESTROYS its
/// command on every frame (Document::update_preview), and revert frees created
/// ids with erase_exact, which reserves the slot rather than recycling it — so
/// a discarded command's created slots can never be reused. A per-frame
/// regeneration that creates connecting roads therefore leaks slots for the
/// rest of the session, which is why move_waypoint_following_junctions asks
/// for InPlaceOnly and takes the stale junction (as it does today) instead.
[[nodiscard]] RM_API std::unique_ptr<Command>
regenerate_junction(const RoadNetwork& network,
                    JunctionId junction,
                    const JunctionGenOptions& options = {},
                    TurnSetPolicy policy = TurnSetPolicy::AllowChange);

/// Deletes the junction AND its connecting roads (the §7 closure); incoming
/// roads survive with their predecessor/successor links into the junction
/// cleared. Undo restores the junction, every connecting road, and every
/// cleared link exactly.
[[nodiscard]] RM_API std::unique_ptr<Command> delete_junction(const RoadNetwork& network,
                                                              JunctionId junction);

// --- lanes ------------------------------------------------------------------

/// Adds a new outermost lane on `side` (+1 left, -1 right), copying the
/// current outermost lane's width profile (3.5 m constant when the side is
/// empty).
[[nodiscard]] RM_API std::unique_ptr<Command>
add_lane(const RoadNetwork& network, LaneSectionId section, int side, LaneType type);

/// Removes a lane. M2 restriction: only the outermost lane of its side can
/// be removed (keeps OpenDRIVE lane numbering contiguous); the center lane
/// never can. Dangling links in adjacent sections and junction lane_links
/// referencing the lane are cleared (and restored exactly on undo).
[[nodiscard]] RM_API std::unique_ptr<Command> remove_lane(const RoadNetwork& network, LaneId lane);

/// Inserts a lane at `at_odr_id`, renumbering every lane already at or outside
/// that position one step further out — where add_lane only ever appends the
/// outermost. `at_odr_id` must name a lane that exists (numbering stays
/// contiguous) and share its sign with the side it lands on; the center lane
/// (0) cannot be displaced.
///
/// The inserted lane does NOT link to the neighbouring sections: a lane that
/// appears mid-road is a new lane, not a continuation
/// (asam.net:xodr:1.4.0:road.lane.link.new_lane_appear). The lanes it pushes
/// outward keep their own links, and everything that named them by id —
/// adjacent-section predecessor/successor and junction lane_links — is
/// remapped to the new numbering (restored exactly on undo).
[[nodiscard]] RM_API std::unique_ptr<Command>
insert_lane(const RoadNetwork& network, LaneSectionId section, int at_odr_id, LaneType type);

[[nodiscard]] RM_API std::unique_ptr<Command>
set_lane_type(const RoadNetwork& network, LaneId lane, LaneType type);

/// Sets a CONSTANT width, replacing the profile with a single record at
/// sOffset 0.
///
/// Refuses a lane whose width already varies along s (more than one record,
/// or one with a non-zero b/c/d): flattening an authored taper to a constant
/// is data loss, and such a caller wants set_lane_width_profile. Before P2
/// this op replaced the profile unconditionally, which silently destroyed
/// every width record of any lane it touched.
[[nodiscard]] RM_API std::unique_ptr<Command>
set_lane_width(const RoadNetwork& network, LaneId lane, double width_m);

/// Replaces the lane's width profile outright — the general form of
/// set_lane_width, and the only way to author width that varies along s
/// (w(ds) = a + b·ds + c·ds² + d·ds³, §11.7.1).
///
/// `widths` are section-LOCAL sOffsets, matching Lane::widths. Validated
/// against the normative width rules, identical in OpenDRIVE 1.8.1 §11.7.1
/// and 1.9.0 §11.7.1:
///   - a record at sOffset 0 must exist
///     (asam.net:xodr:1.7.0:road.lane.width.width_defined_whole_section);
///   - records ascend by sOffset
///     (asam.net:xodr:1.4.0:road.lane.width.elem_asc_order);
///   - every record starts inside the owning section;
///   - the center lane takes no width at all
///     (asam.net:xodr:1.4.0:road.lane.center_lane_no_width).
///
/// Zero width is LEGAL and deliberately allowed — the rule is only that
/// width be >= 0 (asam.net:xodr:1.4.0:road.lane.width.lane_width_validity),
/// and a lane tapering from 0 is exactly how a turn lane begins. Only the
/// record coefficients are checked for a negative `a`; a cubic that dips
/// below zero mid-record is not detected here.
[[nodiscard]] RM_API std::unique_ptr<Command>
set_lane_width_profile(const RoadNetwork& network, LaneId lane, std::vector<Poly3> widths);

/// Splits the lane section covering `s` into two at `s`, duplicating the
/// cross section: every lane is copied into the new section with the same
/// OpenDRIVE id and type, and its width profile and road marks are
/// PARTITIONED at the cut — the original keeps [s0, s), the copy takes
/// [s, end) re-expressed about its own origin. Nothing about the road's
/// shape changes; only where the kernel is allowed to vary it.
///
/// Lanes that continue across the seam are linked in both directions
/// (asam.net:xodr:1.4.0:road.lane.link.lanes_across_laneSections). A lane
/// whose width has already reached zero at the cut is NOT linked: linkage
/// requires a physical connection with non-zero width on both sides
/// (§11.6). The center lane always continues — it carries no width by rule.
///
/// Idempotent: splitting where a section already starts succeeds and changes
/// nothing, so a caller may cut both ends of a span without special-casing
/// the boundaries. `s` must lie strictly inside the road.
///
/// The foundation of the lane tools: Lane Add, Lane Form and Lane Carve all
/// cut sections through this one op rather than re-deriving the rebasing
/// rules (the rules are subtle — see rebase_profile/rebase_marks).
[[nodiscard]] RM_API std::unique_ptr<Command>
split_lane_section(const RoadNetwork& network, RoadId road, double s);

/// Edits the FIRST of the lane's outer-boundary marking records; later
/// records survive untouched (the M2 editor edits the sOffset-0 entry only)
/// but the edit must keep ascending sOffset order
/// (asam.net:xodr:1.4.0:road.lane.road_mark.elem_asc_order).
[[nodiscard]] RM_API std::unique_ptr<Command>
set_road_mark(const RoadNetwork& network, LaneId lane, RoadMark mark);

// --- profiles ---------------------------------------------------------------

/// Sets the elevation at one authoring waypoint; the road's elevation
/// profile becomes piecewise-linear through all waypoint elevations (an
/// all-zero profile is written as no profile).
[[nodiscard]] RM_API std::unique_ptr<Command>
set_node_elevation(const RoadNetwork& network, RoadId road, std::size_t waypoint_index, double z);

/// One editable node of a road's vertical profile (hardening sprint
/// workstream C — the profile-view panel's handles).
struct ElevationPoint {
  double s = 0.0; ///< station [m], within [0, road length]
  double z = 0.0; ///< elevation [m]

  /// Node tangent dz/ds (the grade handle; 8 % = 0.08). nullopt = estimate
  /// by finite differences like the M2 node-elevation fit.
  std::optional<double> grade;
};

/// The road's current vertical profile as editable nodes: one per elevation
/// record start plus the road end, with z and grade evaluated there. An
/// empty (flat) profile yields the two end nodes at z = 0.
[[nodiscard]] RM_API std::vector<ElevationPoint> elevation_profile_points(const Road& road);

/// Replaces the road's elevation profile with a C1 piecewise-cubic Hermite
/// through `points` (sorted copy; explicit grades honored, missing ones
/// finite-difference estimated). An all-zero profile (every z and explicit
/// grade zero) is written as NO profile — the OpenDRIVE default. Errors:
/// stale road, empty points, stations outside [0, length], or duplicate
/// stations. The profile editor pushes one of these per drag commit.
[[nodiscard]] RM_API std::unique_ptr<Command>
set_elevation_profile(const RoadNetwork& network, RoadId road, std::vector<ElevationPoint> points);

// --- objects (road props: trees, vegetation) --------------------------------

/// Adds a road object (OpenDRIVE <object>, §13) to `road`; `object.road` is
/// set to `road`. The object is located by its road-relative (s, t) — callers
/// snap the drop point to a road first (the editor rejects an off-road drop
/// rather than invent a world-xy placement: OpenDRIVE objects live under a
/// <road>). Undo erases the created object and redo resurrects it under the
/// same ObjectId (restore-in-place), so held references survive. Fails
/// (invalid_command) for a stale road or an s outside [0, road length].
[[nodiscard]] RM_API std::unique_ptr<Command>
add_object(const RoadNetwork& network, RoadId road, Object object);

/// Removes an object; undo restores it exactly (same ObjectId). Fails for a
/// stale object id.
[[nodiscard]] RM_API std::unique_ptr<Command> delete_object(const RoadNetwork& network,
                                                            ObjectId object);

/// Re-locates an object to road-relative (s, t) [m], optionally setting its
/// heading `hdg` [rad] (left unchanged when nullopt) — the drag-move commit.
/// Undo is byte-identical from the value snapshot. Fails for a stale object id
/// or an s outside [0, owning-road length].
[[nodiscard]] RM_API std::unique_ptr<Command> move_object(const RoadNetwork& network,
                                                          ObjectId object,
                                                          double s,
                                                          double t,
                                                          std::optional<double> hdg = std::nullopt);

// --- signals (traffic control) ----------------------------------------------

/// Adds a traffic-control signal (OpenDRIVE <signal>, §14) to `road`;
/// `signal.road` is set to `road`. Located by road-relative (s, t); undo erases
/// the created signal and redo resurrects it under the same SignalId
/// (restore-in-place). Fails (invalid_command) for a stale road or an s outside
/// [0, road length].
[[nodiscard]] RM_API std::unique_ptr<Command>
add_signal(const RoadNetwork& network, RoadId road, Signal signal);

/// Removes a signal; undo restores it exactly (same SignalId). Fails for a stale
/// signal id.
[[nodiscard]] RM_API std::unique_ptr<Command> delete_signal(const RoadNetwork& network,
                                                            SignalId signal);

/// Re-locates a signal to road-relative (s, t) [m], optionally setting its
/// heading offset `h_offset` [rad] (left unchanged when nullopt). Undo is
/// byte-identical from the value snapshot. Fails for a stale signal id or an s
/// outside [0, owning-road length].
[[nodiscard]] RM_API std::unique_ptr<Command>
move_signal(const RoadNetwork& network,
            SignalId signal,
            double s,
            double t,
            std::optional<double> h_offset = std::nullopt);

/// Re-points a placed object at another prop model: sets Object::name (the
/// prop_library id the mesher renders — mesh.hpp ObjectInstance::model_id) and
/// refreshes the bounding radius/height from that model, so the object's
/// declared volume never describes the model it used to be. Undo is
/// byte-identical from the value snapshot. Fails for a stale object id, an
/// empty id, or an id no bundled model matches (assets/prop_library.hpp).
[[nodiscard]] RM_API std::unique_ptr<Command>
set_object_model(const RoadNetwork& network, ObjectId object, std::string model_id);

// --- document ---------------------------------------------------------------

[[nodiscard]] RM_API std::unique_ptr<Command>
rename_road(const RoadNetwork& network, RoadId road, std::string name);

} // namespace roadmaker::edit
