# M2 editing tools — per-tool spec sheets

Conventions for every tool:

- Tools are headless controllers (see `01` §4): specs below describe behavior in
  terms of `ToolEvent` sequences, and every test drives the tool with synthetic
  events — no widget instantiation.
- Every mutation goes through a kernel command factory (`01` §2.3) via
  `Document::push_command()` or the preview session (`01` §3). "Undo behavior"
  below names the command(s) and any preview-session use.
- **Standard gtest plan applied to every tool** (listed once, referenced as
  "standard tests"): (a) command apply/undo/redo round-trip — `write_xodr`
  byte-equality before/after undo; (b) idempotence — redo after undo equals
  first apply; (c) referential integrity — no dangling `RoadId/LaneId/JunctionId`
  after the operation or its undo (checked by a network validator walk);
  (d) Esc/cancel leaves the network byte-identical.
- Status-bar guidance: every tool emits `status_message` on activation and state
  change (e.g. "Click to place waypoint — Enter commits, Esc cancels").

---

## 1. Select / Move

**Interaction.** LMB click: pick lane/road (existing picking); modifiers:
Shift = add, Ctrl/Cmd = toggle. LMB drag on empty space: rubber-band
rectangle spanned on the ground plane (z = 0) between the press and cursor
world positions → `select_many` of roads whose mesh AABB intersects the
rectangle. (The band is evaluated in world space, not as a screen-space
frustum slab: tools receive only ground-plane world positions through
`ToolEvent` (`01` §4), which keeps the interaction fully headless-testable;
for the editor's near-top-down editing views the two agree.) LMB drag **on a
geometry node handle** (visible when a road is selected): move node with
live clothoid re-fit + re-mesh preview; release commits.

**Kernel API.** `pick()`, `compute_road_aabbs()` (existing);
`edit::move_waypoint`; `edit::snap_point` (node drags snap to grid/endpoints).

**Undo.** Selection changes are NOT undoable (selection is view state).
Node move = preview session + one `move_waypoint` command on release.

**Edge cases.** Rubber band of zero area degenerates to click-pick. Dragging a
node of a junction-incoming road regenerates the junction (dirty set, `01`
§2.4). Node handles occluded by geometry: handles pick with priority over lane
patches within a pixel radius. Multi-select then drag: M2 moves only the
grabbed node (multi-node move deferred; note in status bar).

**Tests.** Standard tests on `move_waypoint`; plus: event sequence
click→shift-click→rubber-band yields expected `SelectionModel::entries()`;
drag sequence emits ≥1 `preview_changed` and exactly 1 stack push; snap engages
within radius (assert final waypoint == snapped position).

---

## 2. Create Road

**Interaction.** Toolbar template dropdown (kernel `LaneProfile` presets:
`two_lane_rural` — existing `two_lane_default()` renamed/aliased, `urban_sidewalk`
(the default new-road template, #355), `highway`; defined in `road/authoring.hpp`
as named factory functions).
Click places waypoints (ghost polyline + fitted-clothoid preview after 2 points);
`Enter` or double-click commits; `Esc` cancels; `Backspace` removes the last
waypoint. Tangent snap: when the first click lands on an existing road end
(`SnapKind::TangentContinuation`), the new road starts at that end with its
heading locked; same for the last point (closing onto a road end).

**Kernel API.** `edit::create_road(waypoints, profile, name)` (wraps
`author_clothoid_road`); `edit::snap_point`; preview fit uses the same G1 fit
on the candidate waypoint list (exposed as pure
`fit_clothoid_path(span<Waypoint>) → Expected<ReferenceLine>` so the preview
and the command share one code path).

**Undo.** One `create_road` command on commit. No preview session (nothing is
in the network until commit; preview is tool-local geometry).

**Edge cases.** <2 waypoints on Enter: status message, no command. Duplicate
consecutive points (within `tol::kLength`): rejected at click time. G1 fit
failure (collinear degenerate, loop too tight): command factory returns error →
status bar + no push. Auto-generated unique `odr_id` (max existing numeric id
+ 1) and name (`Road N`).

**Tests.** Standard tests on `create_road`; template contents (lane counts,
types, widths per template); tangent-snap chain: created road's start heading
equals `evaluate(end).hdg` of the snapped road within `tol::kAngle`; fit-failure
event sequence pushes nothing.

---

## 3. Edit Nodes

**Interaction.** With a road selected: node handles + tangent visualization.
Click on segment midpoint marker: insert node at that station (on the curve,
so the shape is preserved until the node is dragged). Click node + `Delete`
key: delete node. Drag node: as Select/Move. Tangent visualization is
display-only in M2 (whiskers along the fitted heading at every node — the G1
fit derives all tangents from the waypoints; a draggable end-node heading
handle needs a heading field in the waypoint data model and is deferred with
it to M3).

**Kernel API.** `edit::insert_waypoint`, `edit::delete_waypoint`,
`edit::move_waypoint`. Waypoint derivation for foreign roads per `01` §2.5.

**Undo.** Insert/delete = one command each. Drags = preview session + one.

**Edge cases.** Deleting below 2 waypoints refused (factory error). Insert on a
paramPoly3 road triggers the one-time re-fit notice (`01` §2.5). Node index
stability: commands address waypoints by index captured at creation; a stale
index after an intervening topology undo fails validation (stale generation) —
never corrupts.

**Tests.** Standard tests on insert/delete; insert-then-delete round-trip;
re-fit preserves endpoints within `tol::kRoundTripPosition`; foreign-road
derivation golden test (load `curved_road.xodr` fixture, edit, save, geometry
within tolerance).

---

## 4. Lane Profile editor

**Interaction.** Panel-based (Properties panel section when a lane/road is
selected; the tool id exists for toolbar consistency and sets viewport lane
highlighting). Buttons: add lane left/right of selection; remove lane;
type combo (driving/shoulder/sidewalk/median/border — subset of `LaneType`);
width spin box (constant value, meters); road-mark combo per lane
(solid/broken/none) + width spin.

**Kernel API.** `edit::add_lane`, `edit::remove_lane`, `edit::set_lane_type`,
`edit::set_lane_width`, `edit::set_road_mark`.

**Undo.** One command per discrete panel action (spin boxes commit on
`editingFinished`, not per keystroke).

**Standards.** Lane ids re-number contiguously per side after add/remove
(negative right, positive left, 0 = center — kernel convention, spec §11.4
lane sections; `asam.net:xodr:1.4.0:road.lane.lanes_across_lane_sections`
context and `asam.net:xodr:1.9.0:road.lane.road_mark.only_outer` — the mark
belongs to the lane's outer border; center-line style comes from lane 0's
`RoadMark`). Road-mark width: the spec's `@width` is optional with no normative
numeric values (weight standard/bold is the spec's coarse axis) — the editor
offers defaults 0.12 m (standard) / 0.25 m (bold), documented as conventions,
kernel default stays 0.12. Multiple `<roadMark>` per lane remain supported in
data; the M2 editor edits the first (sOffset 0) entry only.

**Edge cases.** Removing the last driving lane on a side is allowed (a road may
have one-sided lanes); removing lane 0 is impossible (not in the lane list).
Width 0 allowed by data model but flagged by the validator
(`asam.net:xodr:1.9.0:road.lane.width.no_neg_values` context). Re-numbering
updates junction `lane_links` referencing this road's lanes (command captures
and restores them; integrity test).

**Tests.** Standard tests on all five factories; re-number preserves junction
lane-link validity; width edit reflected in mesh (lane patch width at station).

---

## 5. Elevation editor (minimal)

**Interaction.** With a road selected, an "Elevation" mode shows per-waypoint
elevation values (small overlay labels + a spin box in Properties for the
selected node). Set elevation at node → kernel re-fits the road's elevation
profile as a cubic interpolation (C1 monotone-in-s Hermite through node
(s, z) pairs; tangents by finite differences — visually smooth without
overshoot control complexity in M2).

**Kernel API.** `edit::set_node_elevation`; helper
`fit_elevation_profile(span<const double> s, span<const double> z) → std::vector<Poly3>`
(pure, tested directly).

> **As-built (#16, PR #57).** `set_node_elevation` previously wrote a
> piecewise-*linear* profile through the node heights; #16 swapped it to the
> cubic helper above. The helper is a plain **C1 cubic Hermite** — node
> tangents from finite differences (central interior, one-sided at the ends) —
> and is deliberately **not** overshoot/monotonicity-clamped (no
> Fritsch–Carlson); "monotone-in-s" above means only that the emitted
> `<elevation>` records ascend in s, not that z(s) is monotone. `eval_profile`
> reproduces the node heights exactly and an all-zero profile is written as no
> profile. Signature and rationale: `core/include/roadmaker/geometry/profile_fit.hpp`.

**Undo.** One command per value commit.

**Standards.** `<elevation>` elements ascending in s
(`asam.net:xodr:1.4.0:road.elevation.elem_asc_order`), elevation along the
reference line (`asam.net:xodr:1.4.0:road.elevation.elev_along_ref_line`).
**Superelevation editing is deferred to M3** (M2 neither edits nor tilts the
lateral frame — the descope in `00`).

**Edge cases.** Roads without authoring waypoints: nodes derived as in Edit
Nodes. Elevation at a junction-incoming end marks the junction dirty (grade
feeds the blended surface, `03` §4).

**Tests.** Standard tests; fit helper golden values (flat, ramp, crest);
written profile parses back with `eval_profile` equal at nodes within
`tol::kLength`; ascending-s invariant on output.

---

## 6. Create Junction

**Interaction.** Tool active: click-select 2+ road **ends** (end markers
highlight within proximity; status bar counts). `Enter` generates the junction;
`Esc` cancels. Generation: connecting roads for permitted turns + lane links +
flat placeholder surface (Phase 3); blended surface arrives in Phase 4 (`03`).

**Kernel API.** `edit::create_junction(span<const RoadEnd>)`. Generation logic
(kernel, pure): for every ordered pair of arms (A_in → B_out, A ≠ B; U-turns
omitted in M2), for every driving lane on the incoming side with a compatible
outgoing driving lane (matched curb-in order), fit a connecting road
(G1 clothoid between end poses; single lane section; lane width blends linearly
from source to target width along s).

**Standards (cited).**
- One `<connection>` per (incomingRoad, connectingRoad) pair, `<laneLink>` only
  for lanes leading into the junction —
  `asam.net:xodr:1.8.0:junctions.connection.one_link_to_incoming`.
- One connecting road per lane pair (no lane change inside the junction) —
  `asam.net:xodr:1.7.0:junctions.connection.no_lane_change_for_mult_con_roads`
  (M2 picks this form; the alternative
  `…lane_change_one_con_road` form is read-compatible but not generated).
- `contactPoint` semantics —
  `asam.net:xodr:1.7.0:junctions.connection.start_along_linkage` /
  `…connection.end_opposite_linkage`; generator always builds connecting-road
  reference lines in driving direction → `contactPoint="start"`.
- Linked lanes fit smoothly (position/heading continuity at both contact
  points) — `asam.net:xodr:1.9.0:junctions.connection.smooth_fit` (1.9.0-only
  rule; we satisfy it always, cite only when writing 1.9.0).
- Incoming-road lanes ending at the junction drop their `<link>` element —
  `asam.net:xodr:1.4.0:road.lane.link.no_link`; road-level linkage sets the
  junction as successor/predecessor
  (`asam.net:xodr:1.7.0:road.linkage.junc_link_attribute_usage`).
- Junctions only where linkage is ambiguous; not for two roads —
  `asam.net:xodr:1.4.0:junctions.common.when_to_use`,
  `asam.net:xodr:1.9.0:junctions.common.not_only_two` (editor warns but allows
  2-arm junctions — data repair use case — and the validator diagnoses).
- Connecting roads are never incoming roads —
  `asam.net:xodr:1.4.0:junctions.connection.connect_road_no_incoming_road`
  (generator invariant + validator rule).

**Dependency tracking.** Editing any incoming road (geometry, lanes, elevation)
regenerates the junction: `junctions_touching()` (`01` §2.4) feeds the dirty
set; regeneration = deterministic re-run of the generator with the junction's
recorded arm list (arms are `RoadEnd`s stored on the `Junction`; new field).
Regeneration replaces connecting roads in place (IDs preserved via
restore-in-place so undo stays valid).

> **As-built (#17, PR #58) — resolved open point: arm persistence.** This
> spec left unspecified how the recorded arm list survives save/load (the
> `.xodr` is the project file). Resolved per the `rm:waypoints` precedent
> (`01` §2.5): `Junction::arms` serializes to a `<userData code="rm:arms">`
> element on the junction and round-trips through the writer/reader. A
> junction loaded from a **foreign** file (no `rm:arms`) comes in with an
> empty arm list and **cannot regenerate until it is recreated** — M2 does
> *not* derive arms from the `<connection>` table. In-place regeneration
> covers the stable-topology case only: `regenerate_junction` reuses
> connecting-road IDs when the connection count is unchanged, but a change to
> the count (a lane added/removed on an arm) returns an error asking for a
> recreate rather than reallocating IDs. Kernel surface: `preview_junction`
> (non-mutating count + dropped-turn report) and `regenerate_junction`.
>
> **Amended (P2, #263 — maintainer decision 2026-07-15).** The count-must-be-
> unchanged restriction is lifted. `regenerate_junction` now takes a
> `TurnSetPolicy`: under `AllowChange` (default) a lane added to, removed from,
> or retyped on an arm grows or shrinks the turn set — new connecting roads are
> created, vanished ones erased, and the turns that survive keep their IDs
> (keyed matching). The old refusal remains under `InPlaceOnly`, which the
> per-frame node-drag preview uses because creating connecting roads on a
> command that is reverted and discarded every frame would leak arena slots.
> The dirty-set contract gained `junctions_are_current` to distinguish "this
> command already built its junctions" (create/delete/split) from "this
> command is topology **and** needs the junction regenerated" (a lane
> appearing) — a distinction `topology` alone could not express. Detail: the
> [P2 discovery report](../../roadmap/pillars/p2_discovery.md) §4.

**Edge cases.** Ends too far apart (> configurable 50 m): factory error. Arms
nearly parallel: connecting-road fit may loop — generator drops turn pairs whose
fitted length exceeds k·(end distance) (k=4) and reports which turns were
omitted (structured warning). One-way arms produce asymmetric turn sets.

**Tests.** Standard tests on `create_junction`; T-junction golden: expected
connection count (6 turns for 3 two-way single-lane arms), lane-link table
matches hand-derived expectation; validator passes generated output with zero
diagnostics; regeneration determinism (same input → byte-equal xodr);
incoming-road edit regenerates (connection endpoints track the moved end).

---

## 6b. Stop Line (p4-s3, issue #318)

**Interaction.** Stop Line tool (⇧O): hover highlights a junction arm's
painted band, a click makes it active, and dragging it along the arm sets the
setback from the junction mouth. `F` flips which travel direction the band
spans; `Esc` cancels a live drag byte-identically, or clears the selection.
The band is its own handle — it is thin and wide, so the cursor grabs its
CENTRELINE rather than a midpoint, which a wide arm would make unclickable.

**Derived, not created.** Every arm whose junction-facing end has driving
lanes ALREADY has a stop line: `mesh::junction_stoplines()` solves one per
arm (default setback `kStopLineDefaultDistance` = 4 m, thickness 0.3 m,
spanning the approach lanes) and that same query feeds the mesher, the .xodr
writer, the Attributes pane and the bindings, so none of them can disagree.
The tool therefore only ever EDITS; there is no create gesture, and the
retired "Add stop lines to all arms" context action would have been a no-op.

**Suppression.** An arm carrying a live, untagged
`<object subtype="signalLines">` on its junction-facing half is skipped, so a
legacy or foreign file that painted its own stop line keeps rendering that
object and is never double-drawn.

**Kernel API.** `edit::set_stopline_distance` (creates the record when the arm
is still derived; distance stored UNCLAMPED and clamped only at solve time),
`edit::flip_stopline` (refuses a direction with no lanes to span; a record
left back at its defaults is ERASED, so flip-twice is byte-identical to no
edit), `edit::reset_stopline` (drops the record; an error when nothing is
authored). All three are GenericCommand value edits on the `Junction`, with
dirty set `{roads = {arm.road}, junctions = {junction},
junctions_are_current = true}` — the band belongs to the arm road's mesh and
the turn set is untouched.

**Sub-selection.** `ActiveStopLine{JunctionId, RoadEnd}` is tool-local, not a
`SelectionModel` entry (there is no StopLineId) — the CornerTool precedent.
The owning junction IS mirrored into the SelectionModel so the pane and the
scene tree follow, and `stopline_selection_changed()` binds the pane's finer
state.

**Attributes pane.** A "Stop line" group: a read-only arm row (naming which
line the numbers describe), a scrubbable **Distance** spin bounded by the
arm's `max_distance`, **Flip direction**, and **Reset to default** enabled
only when something is authored. Distance 0 is a MEANINGFUL setback, so
unlike the junction radius default there is no special-value sentinel.

**Persistence.** `<userData code="rm:stopline">` on a materialized
`<object type="roadMark" subtype="signalLines">` — see ADR-0008's registry
for the payload grammar and the degradation rules.

**Tests.** `test_junction_stoplines` (derivation, overrides, suppression,
meshing), `test_stopline_operations` (every case through the command
round-trip oracle), `test_stopline_persistence` (round trip + degradation +
fuzz seeds), `test_stopline_tool` (gestures, one-undo-per-drag, F),
`test_panels` (the pane's three rows), plus the Python suite.

---

## 6c. Maneuver (p4-s6, issue #227)

**Interaction.** Maneuver tool (⇧M — plain `M` is Move): selecting a junction
draws every one of its turns, dashed; clicking one makes it active and solid.
The active turn shows knobs on its interior **control points**, **midpoint
markers** between them (pressing one inserts a point and drags it in the same
gesture), and **endpoint dots** at the two arm faces with their slide segments
drawn as guides. Dragging a control point reshapes the path; dragging an
endpoint slides it ACROSS the arm face, constrained to the anchor lane's
cross-section. `Del` removes the focused point, `Esc` cancels a live drag
byte-identically or clears the sub-selection. Every geometry gesture is one
preview session and exactly ONE `edit::set_maneuver_path` on release.

**Picking without a mesh.** Connecting-road surfaces are deliberately not
tessellated (#103), so there is no proxy to ray-cast. The tool resolves the
junction (from the selection, or from a floor pick) and then runs a
SCREEN-space minimum-distance test against each maneuver's sampled centerline
— `editor::screen_distance_to_polyline`, an 8 px constant tolerance at any
zoom. Headless callers with no `ScreenContext` fall back to a world-metre
radius, which is what the tests drive.

**Derived, not created.** A junction's turns already exist: the generator plans
one connecting road per permitted movement. `mesh::junction_maneuvers()` solves
them all — identity, the two arm faces and linked lane ids, the fixed end
headings, the endpoint slide constraints and the sampled path — and that one
query feeds the tool, the pane, the command layer's validate-first checks and
the bindings, so none of them can disagree. It walks `Junction::connections`,
not the arm list, so a FOREIGN junction (no arms, not regenerable) is still
readable and labelled, just not authorable. A SPAN junction has no connections
and so no maneuvers.

**Turn Type is ours, not ASAM's.** §12.2 Table 56 gives `<connection>` exactly
`@connectingRoad`, `@contactPoint`, `@id` and `@incomingRoad` — there is no
turn-type carrier anywhere in the standard. It is therefore DERIVED from the
arm-face headings (`maneuver_turn_type`: ±30° reads Straight,
beyond 150° reads U-turn, and a movement returning to its own arm is always a
U-turn) and user-overridable as a purely semantic label. The 30° band is
deliberately NOT the planner's 10° lane-discipline threshold: that one decides
which LANES a movement may use and errs tiny on purpose; this one is a
perceptual label shown to the author. Setting the override to the value the
derivation already reports CLEARS it rather than pinning it.

**Kernel API.** Six factories, all validating through `junction_maneuvers()`
and all erasing a record left authoring nothing (so edit-then-undo writes the
original bytes): `edit::set_maneuver_path` (interior points + the two endpoint
slides; refits a G1 clothoid chain with the END HEADINGS LOCKED to the arm
faces per §12.4.2, rewriting length, elevation and blended lane width
together, and LOCKS the maneuver in the same undo step — hand-shaped geometry
the next regeneration threw away would be a data-loss bug),
`edit::set_maneuver_locked` (the "Convert to explicit" verb),
`edit::set_maneuver_turn_type` (`nullopt` clears), `edit::reset_maneuver`
(per-maneuver back-to-derived; refuses a foreign junction and an explicit
U-turn, which has no derivation to fall back on), `edit::rebuild_maneuvers`
(junction-wide replan under `ManeuverPolicy::Rebuild` — every lock ignored,
turns the plan no longer contains dropped, but turn-type overrides SURVIVE
because they are semantic, not geometric) and `edit::add_uturn_maneuver` (the
one movement the planner never emits, created as connecting road + connection
entry + a LOCKED record together). Dirty sets are value-only
(`{junctions, junctions_are_current}`) except the two that move geometry,
which add the connecting road, and the U-turn, which also sets `topology`.

**The regeneration guard.** `retarget_junction` skips the rewrite for a locked
matched turn, and KEEPS an unclaimed LOCKED connecting road instead of dropping
it — that single rule is how both hand-shaped geometry and explicit U-turns
survive an arm being moved. `InPlaceOnly` consequently fails only on new or
dropped turns.

**Sub-selection.** `ActiveManeuver{JunctionId, RoadId}` is tool-local (there is
no ManeuverId) — the CornerTool / JunctionSurfaceTool precedent. The connecting
ROAD is mirrored into the `SelectionModel`, which is what makes a maneuver
selectable and highlightable everywhere else in the editor, and
`maneuver_selection_changed()` binds the pane's finer state.

**Attributes pane.** A "Maneuvers" group on a junction selection: one row per
turn (`Turn <id>  <from> → <to>`) carrying a Turn Type combo (Auto / Left /
Straight / Right / U-Turn), a **Lock** checkbox and a **Reset** button
(disabled with an explaining tooltip on an explicit U-turn), plus a
junction-wide **Rebuild Maneuvers** button.

**Context menu.** On a junction: *Rebuild maneuvers* and an *Add U-Turn…*
submenu listing the arms. On a connecting road: *Convert to explicit (lock
geometry)* / *Return maneuver to derived (unlock geometry)* and *Reset
maneuver*.

**Persistence.** `<userData code="rm:maneuver">` on `<junction>` — see
ADR-0008's registry for the payload grammar and degradation rules. Geometry
itself stays Layer 0 (real connecting roads with their `<connection>` and
`<laneLink>` rows); only the lock, the override, the offsets and the control
points ride the extension.

**Tests.** `test_junction_maneuvers` (derivation, classification, slides,
foreign/span junctions, and every command through the round-trip oracle
including the regeneration guard), `test_maneuver_persistence` (round trip +
degradation + bounds, with four new fuzz-corpus seeds),
`test_maneuver_tool` (gestures, one-undo-per-drag, insert, Del, Esc),
`test_picking` (screen-space polyline distance), `test_panels` (the rows),
plus the Python suite and three soak operations.

---

## 6d. Signal (p4-s7, issue #228)

**The controller layer is new.** `<controller>`/`<control>` (§14.6) and
`<junction><controller>` (§12.14) did not exist anywhere in the kernel before
this sprint — a top-level `<controller>` in an input file was warned about and
silently DROPPED, and a `<junction><controller>` child dropped without even a
warning. This sprint adds them as Layer 0 truth and, by construction, closes
both round-trip data-loss paths.

**Controller entity.** `Controller` (`core/include/roadmaker/road/controller.hpp`)
is a **top-level** element — a child of `<OpenDRIVE>`, owned by no road and no
junction, stored in its own `Arena<Controller, ControllerId>` on `RoadNetwork`
with the full signal-parallel accessor set (`add_controller` takes no owner,
plus `erase`/`restore`/`erase_exact`/`release_reserved`/`for_each_controller`),
and it participates in the generic command engine through a `Values::controllers`
snapshot. Its `<control>` children reference a signal by its **string
`@signalId`**, never by `SignalId`: that is what the standard stores, it stays
faithful to a dangling reference in third-party input, and it survives the
signal being erased and restored. Resolving it to a live signal is the query's
job. Erasing a `Signal` therefore does NOT cascade into controllers — a
`<control>` naming a gone signal becomes a dangling reference the validator
reports and `clear_signalization` cleans, which keeps `delete_signal` a leaf op.
The junction's `<controller>` synchronization group (§12.14) is a REFERENCE, not
a definition: `Junction::junction_controllers` is sparse, empty on every
unsignalized junction so pre-existing files re-export byte-identically.

**One query: `junction_signals()`.** `core/src/mesh/junction_signals.cpp` (the
`junction_stoplines` / `junction_maneuvers` template) is the single source shared
by the tool, the panel, the commands' validate-first path, Python, and p4-s8's
phase editor, so none can disagree about what an approach is. Each
`JunctionApproachInfo` carries the incoming arm, its travel heading, the stop
station and mid-span offset (taken from `junction_stoplines()` so a head never
sits at a different station than the line drivers stop at), its **gated**
maneuvers (DERIVED — exactly the `junction_maneuvers()` entries whose `from ==
arm`, the substrate p4-s8's per-phase highlighting needs), the `signal_ids`
resolved on the arm, and the `controller_odr_ids` those signals belong to.
Signal→approach resolution takes signals on the arm within `kSignalApproachWindow`
(30 m, named in the header — no normative distance exists) whose `@orientation`
admits travel toward the junction. It walks `Junction::connections` like
`junction_maneuvers`, so a FOREIGN junction (no arms) yields read-only approach
info rather than nothing; a SPAN/virtual junction has no connections and so no
approaches. The member is `signal_ids`, NOT `signals`: Qt's `<QObject>` defines
`signals` as a preprocessor macro, so a member of that name makes the header
impossible to include from any editor translation unit — the struct itself fails
to parse. The Python attribute is `signal_ids` too.

**Templates.** `edit::SignalizeTemplate` is two dynamic and two static — the
static pair is what keeps the engine from hard-coding "a signalized junction has
lights on four arms". **Axes are derived by clustering approach headings** (an
odd arm forms its own group), so all four work on a 3-arm T:

- `FourWayProtectedLeft` (dynamic) — one head per approach, plus a protected-left
  head on every approach that actually has a left-turn maneuver; per axis a
  through/right controller and, where the axis has any left, a protected-left
  controller. The protected-left head is the **same** `1000001/-1/OpenDRIVE`
  vehicle-light code, told apart only by its signal group.
- `TwoPhase` (dynamic) — one head per approach and one controller per axis
  (permissive lefts, no separate left group).
- `AllWayStop` (static) — a stop sign on every approach and **NO controllers**:
  an all-way stop has no phases, so no phase data is created (GW-4 step 3).
- `TwoWayStop` (static) — stop signs on the minor axis only (fewer incoming
  driving lanes; ties break toward the later axis); needs at least two axes.

**Catalog codes come from the reference, not memory.** The vehicle light is
`type=1000001 subtype=-1 country="OpenDRIVE"` — §14.1 (14_signals.md:61-65) says
traffic lights have no official type/subtype and are given a catalog value under
`country="OpenDRIVE"`, and the reference names 1000001/-1 at
06_general_architecture.md:216. **The local ASAM reference names no OpenDRIVE-
catalog stop or yield code**, so the stop sign reuses RoadMaker's existing
library-drop code, StVO **206 "Halt! Vorfahrt gewähren", `country="DE"`,
subtype=-1** — disclosed here rather than inventing an OpenDRIVE code.

**Kernel API.** `edit::signalize_junction(network, junction, SignalizeOptions{})`
is ONE compound command (ONE undo step), validate-first via `junction_signals()`
with before/after snapshots. It removes anything a previous signalization
authored on this junction FIRST (switching templates never leaves two
generations of heads behind), then places one `Signal` per approach (dynamic →
`@dynamic="yes"` light; static → stop sign) anchored at the stopline station,
groups the dynamic heads into top-level `<controller>`s, references them from the
junction's sync group, and records the applied template and any signal→prop
mounts as Layer-1 userData. `SignalizeOptions` carries the template, an optional
`mount_model` prop id placed alongside each head (the #323 assembly extension
point — the mount record already holds a LIST of object ids per signal), and a
non-persisted `lateral_offset`. It is `invalid_command` for a stale id, a
foreign junction, a SPAN/virtual junction (§12.14 —
`asam.net:xodr:1.9.0:junctions.virtual.no_controllers`), a junction with no
solvable approach, an unknown `mount_model`, `TwoWayStop` with fewer than two
axes, and — deliberately — a re-apply of the exact same template and mount
(a no-op, which the round-trip oracle forbids; the same wall p4-s6 hit with
`rebuild_maneuvers`). `edit::clear_signalization` is the exact inverse and is
invalid when nothing is signalized; what counts as "authored here" is DERIVED
(the sync-group controllers, the signals they reference, the mount pairs, and
the template's own catalog-code signals within the approach window), so a
hand-placed sign of another type is left alone.

**Validator rules** (`validate_network`): a `<controller>` with zero `<control>`
children fires `asam.net:xodr:1.7.0:road.signal.controller.valid_for_signals`; a
duplicate controller `@id` reuses `rules::kIdUniqueInClass`; a span/virtual
junction carrying `junction_controllers` fires
`asam.net:xodr:1.9.0:junctions.virtual.no_controllers`. A `<control signalId>`
naming no existing signal has **no normative rule id**, so it is a RoadMaker-
scoped diagnostic with no rule id — project convention cites a rule id only when
the spec has one.

**Persistence.** Two `<userData>` records on `<junction>`, after the
`rm:maneuver` block — see ADR-0008's registry for the grammar and degradation
rules. `rm:signal` (`template=<tok>[:mount=<modelId>]`, tokens
`protected_left|two_phase|all_way_stop|two_way_stop`) records WHICH template was
applied so the tool can show and re-apply it; `rm:signalmount`
(`signalOdrId=objOdrId[,...][;...]`) pairs each logical signal with its physical
prop object(s). Both follow the every-prior-rm:* carrier rules — skip stale ids,
omit fields at defaults, **emit no element when empty** so an unsignalized
junction round-trips byte-identically; a malformed known grammar drops the whole
value with one warning, an unknown field key warns and skips. The `<signal>` and
`<controller>` elements are Layer 0 truth and are never re-derived on load.

**Editor.** A **Signal tool** (`G`, toolbar group "Signals & Signs"): clicking a
junction makes it the tool's target and the Attributes pane shows the
Signalization group; clicking a signal selects it; clicking a road places one
signal from the current Library item through the extracted
`document/signal_placement.{hpp,cpp}` (shared with the library-drop path so both
snap and mint ids one way). The overlay draws each controller group in a distinct
colour with a dotted leader from each head to the `path` polylines of the
maneuvers it gates. The **Signalization** group in the junction properties
(dynamic-rows idiom, `setParent(nullptr)` + `deleteLater()` teardown) carries a
template combo, a mount-prop combo, **Auto Signalize** / **Clear Signalization**
buttons and one read-only row per approach. The junction context menu adds an
**Auto signalize ▸ <template>** submenu and **Clear signalization**, both gated
on the same preconditions the command validates. Two coverage-gate holes were
closed deliberately: `test_toolbar_registry` now asserts the populated "Signals &
Signs" group, and `test_help_registry` looped only to `ToolId::Corner` (leaving
StopLine, JunctionSpan, JunctionSurface and Maneuver with no help page and
uncaught) — the loop now runs to the real last enumerator and the missing
`kToolPages` rows, including `Signal`, were added.

**Tests.** `test_controller` and `test_signalization_persistence` (arena
add/erase/restore-in-place, third-party `<controller>` and junction sync-group
byte-identity, malformed rm:* dropped with one warning, unsignalized junction
emits no rm:signal element, every validator rule fires),
`test_signalization` (query derivation, each template's counts, static creates
zero controllers, T-junction tolerance, span/virtual rejected, re-apply
`invalid_command`, `expect_command_round_trip` on both commands, mount pairs),
`test_signal_tool` (placement, target, and the structural overlay assertion
`OverlayLinksEveryHeadToTheMovementsItGates`), `test_panels`, plus the Python
suite, two soak operations, and fuzz-corpus seeds exercising both records, a
top-level `<controller>` and a `<junction><controller>`.

---

## 7. Delete

**Interaction.** Delete tool: click deletes picked road (with confirmation in
status bar? No — undo is the confirmation). `Delete`/`Backspace` key in Select
tool deletes the current selection (roads and junctions; lanes are deleted via
the Lane Profile panel, nodes via Edit Nodes).

**Kernel API.** `edit::delete_road`, `edit::delete_junction`.

**Undo.** One command per deletion (multi-select delete = one QUndoStack macro
wrapping per-object commands, so one Ctrl+Z restores everything).

**Referential integrity.** Deleting a road that is an incoming road: junction
connections referencing it are removed (and their connecting roads deleted)
in the same command; deleting a connecting road removes its `<connection>`
entry; deleting a junction deletes its connecting roads and clears
predecessor/successor links of incoming roads that pointed at it. Undo restores
every removed object and link exactly (snapshot includes the full closure).

**Tests.** Standard tests; closure snapshots: delete road with junction →
undo → byte-equal; delete junction → incoming roads' links restored; macro
delete of multi-selection is a single undo step.

---

## 8. New / Save project

**Interaction.** `File → New` (prompts to save if the stack is dirty —
`QUndoStack::isClean` drives the modified flag and window title `*`).
`File → Save` / `Save As…` writes `.xodr` via `save_xodr` (the `.xodr` IS the
project file; authoring waypoints persist via `<userData>`, `01` §2.5).
Save runs the validator; diagnostics (with rule ids) land in the Diagnostics
panel; errors do not block saving (the user sees what a consumer would).

**Kernel API.** `save_xodr` (existing), `write_xodr` for dirty-check;
writer gains the version-explicit target (`WriterOptions{ target_version }`,
1.8.1 default — the slim-fidelity scope in `00`); `RoadNetwork` default
constructor (empty document) already exists.

**Undo.** New/Save are not undoable; New clears the stack (existing `load()`
behavior extended to a `reset()`).

**Edge cases.** Save with zero roads: valid empty document (header only).
Overwrite prompt handled by `QFileDialog`. `setClean()` on successful save.

**Tests.** New→author→save→reload→byte-equal (the Phase 1 gate, using
create_road); dirty flag lifecycle (clean after save, dirty after edit, clean
after undo to clean state); empty-document save/load.

---

## Cross-cutting acceptance (copied into every phase epic)

- A tool ships only with its undo round-trip + integrity tests green — scope is
  cut from the tool list, never from the test plan.
- Every new item model or model-like class ships its tester gtest in the same
  commit; every new public kernel API ships bindings + example in the same PR.
- All interactive features demonstrated with a GIF in the PR description.
