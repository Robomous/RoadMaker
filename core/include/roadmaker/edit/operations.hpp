#pragma once

#include "roadmaker/edit/command.hpp"
#include "roadmaker/export.hpp"
#include "roadmaker/road/authoring.hpp"
#include "roadmaker/road/junction.hpp"
#include "roadmaker/road/lane.hpp"
#include "roadmaker/road/object.hpp"
#include "roadmaker/road/road.hpp"
#include "roadmaker/road/road_style.hpp"

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
/// LOCKED junctions (Junction::locked, p4-s4 #319) are skipped for the same
/// reason with the opposite cause: the user asked for the hand-tuned result to
/// survive edits to its arms, so dragging an arm node is a plain move and the
/// connections stay exactly where they were put. The lock is a policy of the
/// AUTOMATIC loops only — this one and Document's post-command regeneration —
/// never of regenerate_junction itself, so an explicit "re-derive" action
/// works on a locked junction with no bypass flag. edit::set_junction_locked
/// toggles the flag.
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

/// Extends a road past `end` by fitting a forward clothoid from the contact
/// pose through `to` — the "keep drawing off an endpoint" gesture. The fit is
/// curvature-continuous with the old end by construction (fit_forward_clothoid
/// honours the pose AND curvature), so no kink and no curvature step appears at
/// the join; elevation is continued with matching z and grade.
///
/// An END extension appends the fit to the plan view; the last lane section (and
/// its widths) simply spans the new length, and nothing s-indexed moves. A START
/// extension prepends the REVERSED fit and re-bases everything by the extension
/// length L_ext: interior lane-section boundaries += L_ext while the first
/// section stays at s0 = 0 and spans the new head (widths ride along untouched),
/// elevation/superelevation/lane_offset shift forward (values at the old
/// stations preserved; the head holds the boundary value flat rather than
/// extrapolating), objects/signals s += L_ext (world position invariant), and
/// `to` becomes the first authoring waypoint. Either way it is ONE command over
/// the single road (apply→revert byte-identical).
/// Errors (invalid_command): a stale road, the extended end already linked (a
/// junction arm or connecting road), a road with no authoring waypoints, or a
/// `to` the forward clothoid cannot reach (behind the endpoint).
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

/// What a regeneration does with the junction's authored maneuvers (p4-s6,
/// issue #227).
enum class ManeuverPolicy {
  /// The default everywhere: a LOCKED maneuver keeps its plan view, length,
  /// elevation and lane width, and its connecting road is KEPT even when the
  /// plan no longer contains its turn — which is what makes hand-shaped
  /// geometry, and an explicit U-turn, survive a turn-set change. Unlocked
  /// maneuvers are replanned exactly as before the feature existed.
  Respect,
  /// The explicit rebuild (`rebuild_maneuvers`): locks are ignored, so every
  /// connecting road is replanned and every unclaimed one is dropped. The
  /// records' geometric fields (lock, offsets, control points) are cleared and
  /// records left authoring nothing are erased; a `turn_type` override SURVIVES,
  /// because it is semantic rather than geometric.
  Rebuild,
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
/// command on every frame (Document::update_preview); revert frees created ids
/// with erase_exact, which reserves the slot rather than recycling it. Those
/// reserved slots are now recycled when the dropped command is discarded
/// (Command::discard, #271), so a per-frame regeneration no longer leaks —
/// but move_waypoint_following_junctions still asks for InPlaceOnly and takes
/// the stale junction, because regenerating a junction mid-drag is expensive
/// and the arm poses have not settled until the drag commits.
[[nodiscard]] RM_API std::unique_ptr<Command>
regenerate_junction(const RoadNetwork& network,
                    JunctionId junction,
                    const JunctionGenOptions& options = {},
                    TurnSetPolicy policy = TurnSetPolicy::AllowChange);

/// Toggles `Junction::locked` — explicit user control over the AUTOMATIC
/// regeneration loops (p4-s4, issue #319). A locked junction keeps its
/// hand-tuned connections, corners and stop lines when a neighbouring road is
/// edited; regenerate_junction still re-derives it on demand, since the lock
/// guards the automatic pass and does not freeze the junction.
///
/// LOCK is a pure value edit (`junctions_are_current = true`).
///
/// UNLOCK hands the junction back to the automatic loop, so it must be
/// re-derived against whatever changed while it was locked. When the arms still
/// plan, that is a value edit with `junctions_are_current = false`, so the
/// editor's regeneration runs inside the same undo macro. When they no longer
/// plan (an arm was moved out of reach, or its road is gone) there is no
/// automatic state to hand back to, so the command instead performs the full
/// §7 junction removal delete_junction performs — connecting roads included.
///
/// Errors (invalid_command, so apply() reports them): a stale junction id; no
/// state change (locking a locked or unlocking an unlocked junction — the
/// round-trip oracle forbids a no-op command); a FOREIGN junction (no arms and
/// no spans, read from someone else's file — there is no automatic derivation
/// to guard); and unlocking a SPAN junction, whose lock is structural (§12.7
/// virtual junctions are never derived, so they are always locked).
[[nodiscard]] RM_API std::unique_ptr<Command>
set_junction_locked(const RoadNetwork& network, JunctionId junction, bool locked);

/// Adds `end` to a LOCKED junction's arm list and retargets it: the arm's link
/// slot points at the junction, the turns it opens get fresh connecting roads,
/// and every turn that survives keeps its connecting-road id (p4-s4, issue
/// #319).
///
/// The lock is a precondition, not a side effect. An AUTOMATIC junction is
/// defined by its derivation — the next regeneration would re-derive the arm
/// list from the roads that meet it and undo the edit — so hand-editing
/// membership is only meaningful once the user has taken the junction out of
/// the automatic loop with set_junction_locked.
///
/// Errors (invalid_command, network untouched): a stale junction id; a SPAN
/// junction (§12.7 virtual junctions cover a road, they never cut it, so they
/// have no arms); a FOREIGN junction (no arms, read from someone else's file);
/// an UNLOCKED junction; `end` already an arm of this junction; `end` already
/// owned by another junction (the single-owner rule create_junction enforces);
/// an occupied link slot at `end`; and anything the planner refuses for the
/// resulting arm list — notably two arm ends farther apart than
/// options.max_end_distance_m.
[[nodiscard]] RM_API std::unique_ptr<Command>
add_junction_arm(const RoadNetwork& network,
                 JunctionId junction,
                 RoadEnd end,
                 const JunctionGenOptions& options = {});

/// Removes `end` from a LOCKED junction's arm list and retargets it: the arm's
/// link slot is freed, every connecting road serving a turn through it is
/// erased, and the turns that remain keep their connecting-road ids (p4-s4,
/// issue #319).
///
/// DORMANCY: authored corners and stop lines naming the removed arm STAY on the
/// junction record. They go dormant exactly as they do across a turn-set change
/// (JunctionCorner / StopLine doc comments) and reactivate if the arm is added
/// back, so removing an arm by mistake costs no authored work.
///
/// Errors (invalid_command, network untouched): a stale junction id; a SPAN or
/// FOREIGN junction; an UNLOCKED junction; `end` not an arm of this junction;
/// fewer than 2 arms left afterwards (a junction needs two — unlock it to
/// re-derive, or delete_junction it); and anything the planner refuses for the
/// remaining arm list.
[[nodiscard]] RM_API std::unique_ptr<Command>
remove_junction_arm(const RoadNetwork& network,
                    JunctionId junction,
                    RoadEnd end,
                    const JunctionGenOptions& options = {});

/// Folds `absorbed` into `survivor` — ONE junction over the union of both arm
/// lists (p4-s4, issue #319). The survivor is the FIRST argument and keeps its
/// odr id, name, default corner radius and material; `absorbed` is erased in
/// place (erase_exact, no generation bump, so undo restores it under its own
/// id).
///
/// The absorbed junction's arm-road links and its connecting roads'
/// back-references are re-pointed at the survivor BEFORE the turns are matched,
/// so no reference into the erased junction outlives the command (#311) and an
/// absorbed turn that still plans keeps its connecting road. The absorbed
/// junction's authored corners and stop lines are appended verbatim — their
/// RoadEnd keys stay valid because the arms themselves survive.
///
/// The result is LOCKED: a hand-authored merge is not something the automatic
/// loop should re-derive away.
///
/// Errors (invalid_command, network untouched): a stale id on either side; the
/// same junction twice; a SPAN or FOREIGN junction on either side; either side
/// with fewer than 2 arms; and anything the planner refuses for the union —
/// notably two arm ends farther apart than options.max_end_distance_m, which is
/// what "neighbouring junctions" means here.
[[nodiscard]] RM_API std::unique_ptr<Command>
merge_junctions(const RoadNetwork& network,
                JunctionId survivor,
                JunctionId absorbed,
                const JunctionGenOptions& options = {});

/// Creates a SPAN (virtual) junction covering `spans` (p4-s4, issue #319) — the
/// mid-road crosswalk and the parallel-road span. ASAM OpenDRIVE 1.9.0 §12.7
/// (identical in 1.8.1 §12.7): a virtual junction marks a stretch of an
/// UNINTERRUPTED road, so the command creates nothing but the junction record
/// itself — no arms, no connecting roads, no connection table, and no link on
/// any road. The result is LOCKED, structurally: a span junction is never
/// derived, so the automatic loop has nothing to re-derive it from
/// (set_junction_locked refuses to unlock one).
///
/// The junction takes the next free numeric odr id and an empty name, exactly
/// as create_junction does.
///
/// One span is a single-road span (a crosswalk across one carriageway); two
/// spans cover the same crossing over two parallel roads. Errors
/// (invalid_command, network untouched): no spans or more than two; a stale
/// road id; the same road in both spans; a CONNECTING road (one that belongs to
/// a junction — a span covers a through road, not junction internals); and a
/// span that is not a real interval inside its road, i.e. `s_start < 0`,
/// `s_end > road length`, or a length of at most tol::kLength.
[[nodiscard]] RM_API std::unique_ptr<Command> create_span_junction(const RoadNetwork& network,
                                                                   std::span<const SpanArm> spans);

/// Authors the fillet radius of ONE junction corner, named by its adjacent arm
/// pair (p4-s1, issue #225). `radius <= 0` removes the override and returns the
/// corner to its derived radius. Either way any per-side extent override on
/// that corner is cleared — a radius is symmetric by definition.
///
/// The pair must currently be an adjacent corner of `junction` (validated with
/// mesh::junction_corners); a non-adjacent or stale pair is an error. The value
/// is stored as authored and clamped only at mesh time, so a later arm move
/// that shrinks the corner never fails the mesh.
///
/// Dirty set: `{junctions = {junction}, junctions_are_current = true}` — the
/// turn set is untouched, so the editor re-meshes the floor without
/// regenerating connecting roads.
[[nodiscard]] RM_API std::unique_ptr<Command> set_corner_radius(
    const RoadNetwork& network, JunctionId junction, RoadEnd arm_a, RoadEnd arm_b, double radius);

/// Authors the two tangent-leg setbacks [m] of ONE junction corner — the reach
/// of the fillet along each arm's edge, independently. The corner curve stays
/// G1-tangent to both edges (rational quadratic Bezier), so unequal legs remain
/// watertight. Same validation and dirty set as set_corner_radius; a
/// non-positive extent on either side is an error (use set_corner_radius with
/// a non-positive radius to clear the whole override).
[[nodiscard]] RM_API std::unique_ptr<Command> set_corner_extents(const RoadNetwork& network,
                                                                 JunctionId junction,
                                                                 RoadEnd arm_a,
                                                                 RoadEnd arm_b,
                                                                 double extent_a,
                                                                 double extent_b);

/// Authors the sidewalk (resp. median) overlay material of ONE junction corner
/// — a bare catalog name such as "concrete" (p4-s2, issue #226). An empty
/// `material` clears the slot; clearing a corner that authors nothing is an
/// error, as is a name outside `[A-Za-z0-9_.-]+` (the persistence grammar
/// joins on ':' and ';' and does not escape).
///
/// Same validation and dirty set as set_corner_radius. Materials are pure
/// pass-through: the corner geometry is untouched, but the mesher emits the
/// matching overlay submesh only while a material is authored.
[[nodiscard]] RM_API std::unique_ptr<Command>
set_corner_sidewalk_material(const RoadNetwork& network,
                             JunctionId junction,
                             RoadEnd arm_a,
                             RoadEnd arm_b,
                             std::string material);

/// See set_corner_sidewalk_material — the median-nose counterpart. An arm's
/// nose takes the material of the corner where that arm is `arm_a`, falling
/// back to the corner where it is `arm_b`.
[[nodiscard]] RM_API std::unique_ptr<Command> set_corner_median_material(const RoadNetwork& network,
                                                                         JunctionId junction,
                                                                         RoadEnd arm_a,
                                                                         RoadEnd arm_b,
                                                                         std::string material);

/// Authors the junction-wide fillet radius [m] — the fallback every corner
/// without its own `radius` uses (p4-s2, issue #226). Resolution order is
/// per-corner override > this default > derived. `radius <= 0` clears the
/// default (an error when none is set). Like a per-corner radius the value is
/// stored uncapped and clamped to the corner geometry only at mesh time.
[[nodiscard]] RM_API std::unique_ptr<Command>
set_junction_default_corner_radius(const RoadNetwork& network, JunctionId junction, double radius);

/// Authors the junction carriageway material — a bare catalog name, empty to
/// clear (an error when already empty). Same token rule as the corner
/// materials; same dirty set (floor re-mesh only, no turn-set change).
[[nodiscard]] RM_API std::unique_ptr<Command>
set_junction_material(const RoadNetwork& network, JunctionId junction, std::string material);

/// Authors the setback [m] of ONE junction arm's stop line — the distance from
/// the junction mouth back to the near face of the painted band (p4-s3, issue
/// #318). Creates the StopLine record when the arm is still fully derived.
///
/// `arm` must name a stop line the junction currently HAS: solvability comes
/// from mesh::junction_stoplines(), the same query the mesher and the panel
/// read, so tool, panel and command can never disagree about what a stop line
/// is. A negative or non-finite distance is an error. Like a corner radius the
/// value is stored UNCLAMPED and clamped to the road only when solved, so an
/// arm that later shortens never fails the mesh.
///
/// `crosswalk_link` records the odr id of a crosswalk this line was placed
/// alongside (the Crosswalk tool passes it inside its macro); nullopt leaves
/// any existing link alone.
///
/// Dirty set: `{roads = {arm.road}, junctions = {junction},
/// junctions_are_current = true}` — the band is part of the arm road's mesh and
/// the turn set is untouched.
[[nodiscard]] RM_API std::unique_ptr<Command>
set_stopline_distance(const RoadNetwork& network,
                      JunctionId junction,
                      RoadEnd arm,
                      double distance,
                      std::optional<std::string> crosswalk_link = std::nullopt);

/// Toggles which travel direction ONE arm's stop line spans — the approach
/// (incoming) lanes by default, the outgoing lanes when flipped. The direction
/// being toggled INTO must have driving lanes, else there would be nothing to
/// span; that is an error, not an empty band.
///
/// A record left authoring nothing at all by the toggle (default distance, not
/// flipped, no crosswalk link) is erased rather than kept, so the file a
/// flip-twice produces is byte-identical to the one before it. Same validation
/// and dirty set as set_stopline_distance.
[[nodiscard]] RM_API std::unique_ptr<Command>
flip_stopline(const RoadNetwork& network, JunctionId junction, RoadEnd arm);

/// Drops ONE arm's authored stop-line record, returning it to the derived
/// default. An arm that authors nothing has nothing to reset — that is a clean
/// error (the command layer rejects no-ops). Same dirty set as
/// set_stopline_distance.
[[nodiscard]] RM_API std::unique_ptr<Command>
reset_stopline(const RoadNetwork& network, JunctionId junction, RoadEnd arm);

/// Authors whether ONE connecting road's samples take part in the junction
/// floor's elevation and triangulation (p4-s5, issue #320). Creates the
/// SurfaceSpan record when the span is still fully derived.
///
/// Excluding a span is SAMPLES-ONLY: its footprint stays in the floor union, so
/// the pavement's coverage and the exported `<boundary>` never change. What
/// drops out is its border elevations as Dirichlet sources, its centerline as a
/// soft constraint, and its samples' protection of nearby boundary debris from
/// the short-segment merge — the escape valve for an interior triangulation
/// artifact one overlapping ribbon is causing.
///
/// `road` must name a span the junction currently HAS: solvability comes from
/// mesh::junction_surface_spans(), the same query the mesher and the panel read.
/// A span (virtual) junction has no floor, so it has no spans and every edit on
/// one is an error.
///
/// Dirty set: `{junctions = {junction}, junctions_are_current = true}` — only
/// the floor re-meshes and the turn set is untouched.
[[nodiscard]] RM_API std::unique_ptr<Command> set_surface_span_included(const RoadNetwork& network,
                                                                        JunctionId junction,
                                                                        RoadId road,
                                                                        bool included);

/// Authors ONE connecting road's precedence where span footprints OVERLAP:
/// the higher sort index supplies the elevation there ("higher wins").
///
/// A free integer bounded by `kMaxSurfaceSpanSortIndex` — there is no
/// renumbering pass, so a record survives regeneration untouched. The editor's
/// Raise/Lower are just this factory with `current ± 1`.
///
/// A record left authoring nothing (included, index back to zero) is erased
/// rather than kept, so raise-then-lower produces a file byte-identical to the
/// one before the first raise. Same validation and dirty set as
/// set_surface_span_included; the command layer rejects a no-op against the
/// EFFECTIVE value, including "included = true with no record at all".
[[nodiscard]] RM_API std::unique_ptr<Command> set_surface_span_sort_index(
    const RoadNetwork& network, JunctionId junction, RoadId road, int sort_index);

// --- maneuvers (p4-s6, #227) -------------------------------------------------
//
// A maneuver is ONE connecting road's path through a junction. Every one of
// these factories validates through mesh::junction_maneuvers(), the same query
// the tool, the panel and the Python bindings read, so none of them can
// disagree about what a maneuver is; and every one erases a record left
// authoring nothing, so an edit-and-undo pair writes the original bytes.

/// Locks or unlocks ONE maneuver's geometry against regeneration — the
/// finer-grained sibling of `Junction::locked`, and the "Convert to Explicit"
/// verb on a derived maneuver.
///
/// A locked maneuver keeps its plan view, length, elevation and lane width
/// through an explicit `regenerate_junction`, and its connecting road is kept
/// even when the plan no longer contains its turn. `rebuild_maneuvers` is the
/// way back to derivation for the whole junction.
///
/// Errors (invalid_command, network untouched): a stale junction id; `road` not
/// a maneuver of this junction; the maneuver already being in that lock state
/// (the round-trip oracle forbids a no-op command).
///
/// Dirty set: `{junctions = {junction}, junctions_are_current = true}` — pure
/// value edit, no geometry moves.
[[nodiscard]] RM_API std::unique_ptr<Command>
set_maneuver_locked(const RoadNetwork& network, JunctionId junction, RoadId road, bool locked);

/// Overrides ONE maneuver's turn type, or clears the override with `nullopt`.
///
/// The type is otherwise DERIVED from the arm-face headings (there is no ASAM
/// carrier for it — §12.2 Table 56 has no such attribute), so the override is
/// purely semantic and never moves geometry. Storing a value equal to the
/// computed type CLEARS the override rather than pinning it, which keeps the
/// record from authoring something the derivation already says.
///
/// Errors (invalid_command): a stale junction id; `road` not a maneuver of this
/// junction; clearing an override that does not exist; and setting the type the
/// maneuver already reports.
///
/// Same dirty set as set_maneuver_locked.
[[nodiscard]] RM_API std::unique_ptr<Command> set_maneuver_turn_type(const RoadNetwork& network,
                                                                     JunctionId junction,
                                                                     RoadId road,
                                                                     std::optional<TurnType> type);

/// Reshapes ONE maneuver: its INTERIOR control points plus the endpoint slides
/// along the two arm faces. THE geometry command — the editor's add point, move
/// point, insert point and endpoint drag all compose into this one factory, so
/// a drag is one preview session and one command on release.
///
/// The path is refitted as a G1 clothoid chain through
/// `[start anchor + start_offset, control_points…, end anchor + end_offset]`
/// with the END HEADINGS LOCKED to the arm faces, so a reshaped maneuver still
/// meets its arms tangentially (§12.4.2). The refit rewrites the road's length,
/// elevation profile and blended drive-lane width together — a stale domain
/// there exports invalid OpenDRIVE.
///
/// Applying this LOCKS the maneuver in the same undo step: hand-shaped geometry
/// that the next regeneration threw away would be a data-loss bug, and an
/// implicit lock keeps it one undo away rather than two.
///
/// Offsets are `nullopt` to leave the current slide alone. Errors
/// (invalid_command, network untouched): a stale junction id; `road` not a
/// maneuver of this junction; more than `kMaxManeuverControlPoints` points; a
/// non-finite coordinate or offset; an offset outside the anchor lane's span
/// (see `ManeuverSlide`); an anchor lane that no longer exists; a failed refit;
/// and a request that changes nothing.
///
/// Dirty set: `{roads = {road}, junctions = {junction},
/// junctions_are_current = true}` — the connecting road re-meshes, the turn set
/// is untouched.
[[nodiscard]] RM_API std::unique_ptr<Command>
set_maneuver_path(const RoadNetwork& network,
                  JunctionId junction,
                  RoadId road,
                  std::span<const Waypoint> control_points,
                  std::optional<double> start_offset = std::nullopt,
                  std::optional<double> end_offset = std::nullopt);

/// Drops ONE maneuver's authored record and replans its connecting road from
/// the junction's arms — the per-maneuver "back to derived".
///
/// Errors (invalid_command): a stale junction id; `road` not a maneuver of this
/// junction; a maneuver with nothing authored to reset; a FOREIGN junction (no
/// arm list, so there is nothing to replan from); and an EXPLICIT U-turn, which
/// the planner never emits and therefore has no derived geometry to fall back
/// on — delete its connecting road instead.
///
/// Same dirty set as set_maneuver_path.
[[nodiscard]] RM_API std::unique_ptr<Command>
reset_maneuver(const RoadNetwork& network,
               JunctionId junction,
               RoadId road,
               const JunctionGenOptions& options = {});

/// Replans the WHOLE junction ignoring every maneuver lock
/// (`ManeuverPolicy::Rebuild`): hand-shaped geometry is replaced by the
/// derivation, connecting roads the plan no longer contains are dropped
/// (explicit U-turns included), and the records' geometric fields are cleared.
/// `turn_type` overrides SURVIVE — they are semantic, not geometric.
///
/// Errors (invalid_command): a stale junction id; a FOREIGN or SPAN junction;
/// a junction with no locked or hand-shaped maneuver to rebuild.
[[nodiscard]] RM_API std::unique_ptr<Command> rebuild_maneuvers(
    const RoadNetwork& network, JunctionId junction, const JunctionGenOptions& options = {});

/// Adds the one maneuver the planner never emits: a U-turn on `arm`, from its
/// innermost incoming driving lane back to its innermost outgoing one.
///
/// The generator skips the same-arm pair on purpose (a U-turn is a policy
/// decision, not a derivable movement), so this creates the connecting road,
/// the connection-table entry and a LOCKED Maneuver record together — the lock
/// is what keeps the next regeneration from dropping a turn no plan contains.
///
/// Errors (invalid_command, network untouched): a stale junction id; a FOREIGN
/// or SPAN junction; `arm` not an arm of this junction; no driving lane in one
/// of the two directions; a same-arm connection already existing; and a failed
/// connector fit, whose message is surfaced verbatim — a U-turn between two
/// adjacent lanes can legitimately be too tight to fit.
///
/// Dirty set: `{roads = {arm.road}, junctions = {junction}, topology = true,
/// junctions_are_current = true}`.
[[nodiscard]] RM_API std::unique_ptr<Command>
add_uturn_maneuver(const RoadNetwork& network,
                   JunctionId junction,
                   RoadEnd arm,
                   const JunctionGenOptions& options = {});

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

/// Sets the lane's travel direction (e_lane_direction, §11 — introduced in
/// OpenDRIVE 1.8.0, legal under both writer targets). Refuses the center lane
/// (odr id 0): a width-less separator has no travel of its own. Marks only the
/// owning road dirty — the connection engine does not read @direction, so no
/// junction regeneration is needed.
[[nodiscard]] RM_API std::unique_ptr<Command>
set_lane_direction(const RoadNetwork& network, LaneId lane, LaneDirection direction);

/// Replaces the lane's <material> records outright (§11.8.2) — an empty vector
/// clears them. `records` carry section-LOCAL sOffsets, matching
/// Lane::materials. Validated against the normative material rules, identical
/// in OpenDRIVE 1.8.1 §11.7.2 and 1.9.0 §11.8.2:
///   - the center lane takes no material at all
///     (asam.net:xodr:1.4.0:road.lane.material.center_lane_no_material);
///   - records ascend by sOffset
///     (asam.net:xodr:1.4.0:road.lane.material.elem_asc_order);
///   - every record starts inside the owning section;
///   - friction/roughness, when present, are >= 0 (t_grEqZero, Table 44).
///
/// Marks only the owning road dirty (re-mesh feeds the surface code to the
/// renderer); the connection engine does not read material, so no junction
/// regeneration is needed. Feeds GW-3 (lane-level material application).
[[nodiscard]] RM_API std::unique_ptr<Command>
set_lane_material(const RoadNetwork& network, LaneId lane, std::vector<LaneMaterial> records);

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

/// Lane Add (p2-s5): adds a self-contained POCKET lane that tapers 0 -> full
/// -> 0 within the station span [s0, s1] on `side` (+1 left, -1 right). The
/// span is clamped inward to [0.5, length-0.5] so both seams stay strictly
/// interior — the pocket therefore needs NO cross-section links (it is zero
/// width at both section boundaries) and is a single undo step.
///
/// Composes the p2-s1 lane primitives: split_lane_section at each end
/// (idempotent at an existing boundary), add_lane on the middle section, then
/// set_lane_width_profile with the taper. The plateau width mirrors add_lane
/// (the current outermost lane's width, else 3.5 m). Fails (invalid_command)
/// for a stale road, a bad side, or a degenerate span (s0 >= s1).
[[nodiscard]] RM_API std::unique_ptr<Command> add_lane_span(
    const RoadNetwork& network, RoadId road, int side, double s0, double s1, LaneType type);

/// Lane Form (p2-s5, p2-s8 seam-linking): forms an interior lane that starts at
/// zero width at `s_start` and tapers up to full width, holding it to the road
/// terminus. `side` is +1/-1 and `at_odr_id` (whose sign must match `side`)
/// names the numbering position the formed lane takes — every lane already at
/// or outside it steps one further out (insert_lane).
///
/// Composes split_lane_section at `s_start`, insert_lane on the START section,
/// then set_lane_width_profile with an up-only taper. The formed lane is
/// backward-unlinked (it appears mid-road, a new lane — not a continuation).
///
/// When `s_start` is upstream of one or more lane-section boundaries, the lane
/// is CARRIED across every downstream seam so it runs to the road end as a
/// properly linked carriageway: in each downstream section it is inserted where
/// the position already exists, else appended one lane outward (add_lane), and
/// the two sides of each seam are joined with link_lane_across_seam — the
/// matched predecessor/successor pair the writer requires
/// (asam.net:xodr:1.4.0:road.lane.link.lanes_across_laneSections, §11.6). If a
/// downstream section is too narrow to host the position (more than one lane
/// short of it) the chain stops there and the lane ends unlinked at that seam,
/// which is legal. A terminus welded to another road likewise ends unlinked
/// across the ROAD boundary — the writer validates intra-road links only.
///
/// Fails (invalid_command) for a stale road, a bad side, an `s_start` outside
/// the road, or a sign-mismatched `at_odr_id`.
[[nodiscard]] RM_API std::unique_ptr<Command> form_lane(const RoadNetwork& network,
                                                        RoadId road,
                                                        int side,
                                                        double s_start,
                                                        int at_odr_id,
                                                        LaneType type);

/// Joins one lane across a single lane-section seam by setting the matched
/// predecessor/successor pair the writer requires
/// (asam.net:xodr:1.4.0:road.lane.link.lanes_across_laneSections, §11.6): the
/// upstream lane's successor becomes `downstream_odr` and the downstream lane's
/// predecessor becomes `upstream_odr`. `upstream_section` is the section on the
/// low-`s` side of the seam; the downstream section is the one immediately
/// after it in road order (so the two are always CONSECUTIVE on ONE road).
///
/// Fails (invalid_command) for a stale `upstream_section`, a center-lane odr
/// (`upstream_odr` or `downstream_odr` == 0), an `upstream_section` that has no
/// following section (nothing to link across — a non-adjacent request), or a
/// named lane that does not exist on its side of the seam.
[[nodiscard]] RM_API std::unique_ptr<Command> link_lane_across_seam(const RoadNetwork& network,
                                                                    LaneSectionId upstream_section,
                                                                    int upstream_odr,
                                                                    int downstream_odr);

/// Lane Carve (p2-s6): carves a turn lane approaching a junction. Like Lane
/// Form it inserts an interior lane that starts at zero width at `s_start` and
/// runs to the road terminus, but the width ramps 0 -> full over the DRAGGED
/// span `[s_start, s_end]` (the taper the user pulled) and then holds full to
/// the terminus, where the junction absorbs it. Drag the whole way to the
/// junction end and `s_end` reaches the terminus: the ramp spans the entire
/// lane and there is no plateau — a single diagonal, GW-2 step 12.
///
/// Composes split_lane_section at `s_start` then insert_lane on the road's
/// FINAL section then set_lane_width_profile with the span-length up-taper.
/// `side` is +1/-1 and `at_odr_id` (sign must match `side`) is the numbering
/// position the carved lane takes; the width is the nearest driving lane's
/// (3.5 m default). Carving away from the final section would strand a
/// full-width lane at a downstream seam (forward-linking is out of scope,
/// p2-s5), so — exactly like form_lane — the op GUARDS: `s_start` must land in
/// the final lane section. Also fails (invalid_command) for a stale road, a bad
/// side, a sign-mismatched `at_odr_id`, an `s_start` outside the road, or a
/// non-positive dragged span (`s_end <= s_start`).
[[nodiscard]] RM_API std::unique_ptr<Command> carve_lane(const RoadNetwork& network,
                                                         RoadId road,
                                                         int side,
                                                         double s_start,
                                                         double s_end,
                                                         int at_odr_id,
                                                         LaneType type);

/// Road styles (p2-s8): replaces `road`'s entire lane profile and boundary
/// road marks with `style`, flattening the road to a SINGLE lane section at
/// s=0 (a prior carve/split/form taper is intentionally replaced — the contract
/// lists lane count, types, and widths as replaced). Everything orthogonal to
/// the cross section is preserved: reference-line geometry, elevation and
/// superelevation profiles, lane offset, name and odr id, the road's
/// predecessor/successor links, and any placed objects and signals.
///
/// The command names the junctions the road feeds (junctions_touching) with
/// junctions_are_current=false, so the editor regenerates them against the new
/// lane count — a styled arm reaches its junction the same way a carved lane
/// does. Undo restores the previous sections and lanes in place, so junction
/// lane links snap back exactly (byte-identical round trip).
///
/// Fails (invalid_command, network untouched) for a stale road, a CONNECTING
/// road (one owned by a junction — connection.cpp assumes a single section
/// there), a style with no lanes, or a non-positive lane width.
[[nodiscard]] RM_API std::unique_ptr<Command>
apply_road_style(const RoadNetwork& network, RoadId road, const RoadStyle& style);

/// Edits the FIRST of the lane's outer-boundary marking records; later
/// records survive untouched (the M2 editor edits the sOffset-0 entry only)
/// but the edit must keep ascending sOffset order
/// (asam.net:xodr:1.4.0:road.lane.road_mark.elem_asc_order).
[[nodiscard]] RM_API std::unique_ptr<Command>
set_road_mark(const RoadNetwork& network, LaneId lane, RoadMark mark);

/// Sets a derived ground surface's material name ("" clears it back to the
/// default grass; "asphalt"/"concrete" pick a paved look). A stale SurfaceId
/// yields an invalid command. The surface geometry is untouched — only the
/// stored material and its render class change (p6-s2).
[[nodiscard]] RM_API std::unique_ptr<Command>
set_surface_material(const RoadNetwork& network, SurfaceId surface, std::string material);

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

/// Sets a signal's free-text face (ASAM OpenDRIVE 1.9.0 §14, Table 122: @text —
/// "Additional text associated with the signal", the carrier for editable sign
/// text like a town-entrance plate; multi-line text uses literal `\n`). Undo is
/// byte-identical from the value snapshot. Fails for a stale signal id or a
/// stale road back-reference, and rejects a no-op (`text` equal to the current
/// value) so the round-trip harness never sees an empty command.
[[nodiscard]] RM_API std::unique_ptr<Command>
set_signal_text(const RoadNetwork& network, SignalId signal, std::string text);

// --- signalization (p4-s7, issue #228) ---------------------------------------

// `kSignalizeAxisTolerance` and `cluster_signal_axes` were promoted to
// mesh/junction_signals.hpp for p4-s8 (so the phase-derivation query can share
// them without depending on the command layer). Include that header for them.

/// The auto-signalization templates (`signalize_junction`).
///
/// Two dynamic, two static — the static pair is what keeps the engine from
/// hard-coding "a signalized junction has traffic lights on four arms". None of
/// them assumes four arms: axes are derived by clustering approach headings, so
/// a 3-arm T yields one two-arm axis and one single-arm axis.
enum class SignalizeTemplate {
  /// Dynamic. One head per approach, plus a protected-left head on every
  /// approach that actually has a left-turn maneuver. Per axis: one controller
  /// for the through/right heads and, when the axis has any, one for the
  /// protected-left heads.
  FourWayProtectedLeft,
  /// Dynamic. One head per approach and ONE controller per axis — permissive
  /// lefts, no separate left group.
  TwoPhase,
  /// Static. A stop sign on every approach and NO controllers at all: an
  /// all-way stop has no phases, so no phase data is created.
  AllWayStop,
  /// Static. Stop signs on the MINOR axis only (the axis carrying fewer
  /// incoming driving lanes; ties break toward the later axis), and no
  /// controllers. Needs at least two axes.
  TwoWayStop,
};

/// Options for `signalize_junction`.
struct SignalizeOptions {
  SignalizeTemplate tmpl = SignalizeTemplate::FourWayProtectedLeft;

  /// Optional prop model id (assets/prop_library.hpp) placed as an `<object>`
  /// alongside each authored signal and recorded in the junction's
  /// `signal_mounts`; empty places nothing. This is the #323 extension point —
  /// an assembly id slots straight in and needs no schema change, because the
  /// mount record already holds a LIST of object ids per signal.
  std::string mount_model;

  /// Lateral clearance [m] between the outboard edge of the approach's stop
  /// band and the head, on the driver's right. Not persisted: the `rm:signal`
  /// record carries the template and the mount model only, so re-applying the
  /// same template with a different offset is still rejected as a no-op.
  double lateral_offset = 0.5;
};

/// Auto-signalizes `junction` from a template: places the `<signal>`s, groups
/// the dynamic ones into top-level `<controller>`s (§14.6), references those
/// controllers from the junction's synchronization group (§12.14), and records
/// the applied template plus any signal→prop mounts as Layer-1 userData. ONE
/// compound command, so it is ONE undo step.
///
/// Anything a previous signalization authored on this junction is removed
/// first, so switching templates never leaves two generations of heads behind.
///
/// Fails (invalid_command) for a stale id, for a junction with no recorded arms
/// (foreign — recreate it to edit), for a SPAN/virtual junction (which "shall
/// not have controllers and therefore no traffic lights",
/// asam.net:xodr:1.9.0:junctions.virtual.no_controllers), for a junction with
/// no solvable approach, for an unknown `mount_model`, for `TwoWayStop` on a
/// junction with fewer than two axes, and — deliberately — when the junction
/// ALREADY carries exactly this template and mount model: that would be a
/// no-op, and the command layer's round-trip oracle forbids no-op commands.
[[nodiscard]] RM_API std::unique_ptr<Command> signalize_junction(
    const RoadNetwork& network, JunctionId junction, const SignalizeOptions& options = {});

/// The exact inverse: erases the signals, controllers and mount props a
/// signalization authored on `junction` and clears its Layer-1 records.
///
/// What counts as "authored here" is DERIVED, never a hidden flag: the
/// controllers the junction's synchronization group names (and every signal
/// their `<control>` children reference), every signal and object named in
/// `signal_mounts`, and — for a junction carrying an `rm:signal` template — the
/// signals within `kSignalApproachWindow` of its approaches whose catalog code
/// is the one that template places. A hand-placed sign of another type is left
/// alone.
///
/// Fails (invalid_command) for a stale id and for a junction with nothing
/// signalized (a no-op, which the round-trip oracle forbids).
[[nodiscard]] RM_API std::unique_ptr<Command> clear_signalization(const RoadNetwork& network,
                                                                  JunctionId junction);

// --- signal phases (p4-s8, issue #229) ---------------------------------------
//
// The six factories that edit a junction's signal CYCLE (`Junction::phases`).
// All are pure junction-value edits — no geometry moves, so the turn set is
// untouched (corner_value_command's DirtySet) — and all share `phase_edit_context`:
// they reject a stale id, a SPAN (virtual) junction and an EMPTY plan
// ("signalize the junction first"), and on the FIRST edit they MATERIALIZE the
// derived cycle sparsely into `Junction::phases` (storing only the non-Red
// states) so `mesh::junction_phases().authored` flips true while the derived
// shape is preserved. The round-trip oracle forbids no-op commands, so every
// factory rejects an edit that would change nothing (a duration equal to the
// effective value, a state equal to the effective state, ...). Constants and
// the derived-cycle shape live in `mesh/junction_phases.hpp`.

/// Sets phase `phase_index`'s duration [s]. Rejects a non-finite, `<= 0` or
/// `> kMaxSignalPhaseDuration` value, an out-of-range index, and a value equal
/// to the phase's effective (possibly derived) duration.
[[nodiscard]] RM_API std::unique_ptr<Command> set_phase_duration(const RoadNetwork& network,
                                                                 JunctionId junction,
                                                                 std::size_t phase_index,
                                                                 double duration);

/// Sets one controller's state within phase `phase_index`. Setting it to Red
/// ERASES the controller's sparse `PhaseState` pair (Red is the omitted
/// default). Rejects an out-of-range index, a `controller_odr_id` that is not a
/// live member of the junction's sync group or fails the `[A-Za-z0-9_.-]+`
/// token alphabet, and a state equal to the controller's effective state.
[[nodiscard]] RM_API std::unique_ptr<Command> set_phase_state(const RoadNetwork& network,
                                                              JunctionId junction,
                                                              std::size_t phase_index,
                                                              std::string controller_odr_id,
                                                              SignalState state);

/// Inserts an all-red phase (`kDefaultAddedPhaseSeconds`, empty state list) at
/// `phase_index` in `0..count` (`count` appends). Rejects an out-of-range index
/// and a resulting count exceeding `kMaxSignalPhases`.
[[nodiscard]] RM_API std::unique_ptr<Command>
add_signal_phase(const RoadNetwork& network, JunctionId junction, std::size_t phase_index);

/// Deep-copies phase `phase_index` to `phase_index + 1`. Rejects an
/// out-of-range index and a resulting count exceeding `kMaxSignalPhases`.
[[nodiscard]] RM_API std::unique_ptr<Command>
duplicate_signal_phase(const RoadNetwork& network, JunctionId junction, std::size_t phase_index);

/// Removes phase `phase_index`. Rejects an out-of-range index and — because a
/// zero-phase authored cycle is unrepresentable — removing the LAST remaining
/// phase (use `clear_signal_phases` to return to the derived cycle instead).
[[nodiscard]] RM_API std::unique_ptr<Command>
remove_signal_phase(const RoadNetwork& network, JunctionId junction, std::size_t phase_index);

/// Clears the authored cycle (`Junction::phases.clear()`), returning the
/// junction to its DERIVED cycle (AUTHORS-NOTHING ⇒ ERASE). Unlike the other
/// factories this BYPASSES the empty-plan rejection — a de-signalized junction
/// carrying only dormant phases must stay clearable — and rejects only when
/// `Junction::phases` is already empty (nothing to clear).
[[nodiscard]] RM_API std::unique_ptr<Command> clear_signal_phases(const RoadNetwork& network,
                                                                  JunctionId junction);

/// Re-points a placed object at another prop model: sets Object::name (the
/// prop_library id the mesher renders — mesh.hpp ObjectInstance::model_id) and
/// refreshes the bounding radius/height from that model, so the object's
/// declared volume never describes the model it used to be. Undo is
/// byte-identical from the value snapshot. Fails for a stale object id, an
/// empty id, or an id no bundled model matches (assets/prop_library.hpp).
[[nodiscard]] RM_API std::unique_ptr<Command>
set_object_model(const RoadNetwork& network, ObjectId object, std::string model_id);

/// Replaces each listed object with its new value in one undoable command — the
/// batch behind an asset edit that re-materializes every instance following the
/// changed asset (p3-s2 crosswalk assets). `updates` pairs each ObjectId with
/// its full replacement Object; ids keep their generation (in-place value
/// assignment, so held references survive) and undo is byte-identical from the
/// before-snapshot. Refuses (invalid_command) a stale id or an update that
/// changes an object's owning road (`Object::road`); an empty `updates` is a
/// valid no-op. DirtySet::objects lists the deduped owning roads. `name` is the
/// undo-menu label (defaults to "Update Objects" when empty).
[[nodiscard]] RM_API std::unique_ptr<Command>
update_objects(const RoadNetwork& network,
               std::vector<std::pair<ObjectId, Object>> updates,
               std::string name = {});

// --- document ---------------------------------------------------------------

[[nodiscard]] RM_API std::unique_ptr<Command>
rename_road(const RoadNetwork& network, RoadId road, std::string name);

} // namespace roadmaker::edit
