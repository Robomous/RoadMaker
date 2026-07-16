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
`two_lane_rural` — existing `two_lane_default()` renamed/aliased, `urban_sidewalk`,
`highway`; defined in `road/authoring.hpp` as named factory functions).
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
> [P2 discovery report](../../../roadmap/pillars/p2_discovery.md) §4.

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
