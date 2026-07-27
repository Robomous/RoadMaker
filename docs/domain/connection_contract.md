# The road connection contract

*The canonical spec for what RoadMaker guarantees where two roads meet. This
is the **only** place those guarantees are defined: issues and code comments
reference this document and never restate its numbers, and any change to a
guarantee is a PR against this document first. Enforcement is structural —
see [Enforcement](#enforcement) below. Sprint record:
[#403](https://github.com/Robomous/RoadMaker/issues/403) (`p2-s10`); roadmap
record: [2026-07 field triage, batch 2](../roadmap/updates/2026-07-field-triage-2.md).*

## Why this document exists

RoadMaker joins two roads in three different ways, and until this contract was
written **no document stated what any of them guaranteed**. That absence was
not cosmetic. Elevation continuity existed only where a connector road was
generated, and only at generation time: pure-link welds compared x and y only,
so two ends 5 cm apart in plan and 5 m apart in elevation linked silently into
a cliff; profile edits were strictly per-road; and the terrain skirt downstream
faithfully amplified any joint step into a full-height ground wall.

ASAM OpenDRIVE does not close this gap. §10.3 constrains how a link is
**declared** — `road.linkage.road_link_attribute_usage`, and 1.9.0's
`road.linkage.both_sides_consistency` — but **no ASAM rule requires two linked
road ends to coincide**, and there is no road-to-road elevation-continuity rule
at all. The nearest normative text is the junction should-rule
`asam.net:xodr:1.8.0:junctions.elevation_grid.entry_exit_smoothness`. A step at
a plain link is therefore a product-quality matter rather than a spec
violation, which is exactly why it has to be written down here and enforced by
us.

## The three kinds of join

| Kind | Kernel entry point | What it is |
|---|---|---|
| **Pure link** | `edit::close_gap`, coincident branch | Two free ends that already meet are recorded as each other's predecessor/successor. **No geometry is generated.** |
| **Connector** | `edit::close_gap`, gap branch → `edit::fit_connector` | A real gap is bridged by a new single-lane road fitted to both ends. |
| **Junction contact** | `edit::plan_junction` → `materialize_connection` | An arm meets a junction's connecting roads, anchored on the **linked lane's inner boundary** rather than the arm's reference line. |
| **Merge seam** | `edit::merge_roads` (`check_mergeable`) | Not a join but its strictest relative: two roads become one, so the seam must already be continuous. |

## Guarantees

<!-- rm-contract: guarantees -->

| Join kind | Position | Heading | Curvature | Elevation (z) | Grade (dz/ds) |
|---|---|---|---|---|---|
| Pure link | G0, ≤ `tol::kWeldPosition` | continuous by construction | **not asserted** | C0, ≤ `tol::kWeldElevation` | C1, ≤ `tol::kWeldGrade` |
| Connector | G0 | G1 | G2 (`ConnectorParams::g2`) | C0 | C1 (cubic Hermite) |
| Junction contact | G0 | G1 | G1 (`g2 = false`) | C0 | C1 |
| Merge seam | ≤ `tol::kMergePositionGap` | ≤ `tol::kMergeHeading` | not asserted | ≤ 1e-3 m | ≤ 1e-3 |

Two consequences worth stating outright:

- **A pure link asserts no curvature continuity.** Two roads may legitimately
  meet tangentially with different curvatures; only a G2 `close_gap` connector
  drives the curvature step below `tol::kWeldCurvature`. `WeldReport` therefore
  reports `max_curvature_gap` for information and never breaches on it.
- **Elevation and grade are NOT informational.** Every kind of join guarantees
  both, which is why `check_linkable` refuses a coincident pair that disagrees
  in either rather than welding it.

## Tolerances

<!-- rm-contract: tolerances -->

| Constant | Value | Unit | Meaning |
|---|---|---|---|
| `tol::kWeldPosition` | 1e-3 | m | the two ends are in the same place |
| `tol::kWeldHeading` | 1e-3 | rad | the two ends point the same way |
| `tol::kWeldCurvature` | 5e-3 | 1/m | informational; only a G2 weld reaches it |
| `tol::kWeldElevation` | 1e-3 | m | the two ends are at the same height |
| `tol::kWeldGrade` | 1e-3 | — | the two ends rise at the same rate |

These live in `core/include/roadmaker/tol.hpp` and are checked against this
table by CI — see [Enforcement](#enforcement).

## Verifying a joint

- `edit::verify_link_weld(network, end)` — the five gaps at one plain
  road-to-road link. Declines an unlinked end, a junction-owned end, and either
  side of a junction **connecting road**: those weld on the linked lane's inner
  boundary, metres off the reference line on a multilane arm, so only
  `verify_junction_welds` knows where to measure.
- `edit::verify_junction_welds(network, junction)` — the worst gaps between a
  junction's connecting roads and the arms they link, computed with the same
  anchor math `plan_junction` uses so checker and generator cannot drift.

Both return a `WeldReport` whose `breaches` flag covers position, heading,
elevation and grade.

## The grade sign trap

`ContactState` pre-flips `curvature` by contact point but leaves `grade` as
**dz/ds along the road's own +s**. Every consumer that compares two ends — or
hands a grade to a connector — must re-express it in the joint's frame first:

- `edit::grade_sign_into(contact)` — for travel **into** the joint at that end
  (the direction `ContactState::into_hdg` names): `+1` at an End, `−1` at a
  Start.
- `edit::grade_sign_out(contact)` — for travel **out of** it (`out_hdg`). The
  exact negation of the above.

Two linked ends are continuous when
`grade_sign_into(a)·grade_a + grade_sign_into(b)·grade_b == 0`. Picking the
wrong sign built inverted end grades — V-kink ramps — for **three of the four**
contact combinations, and only the common End→Start chain was correct, which is
why it survived testing for so long
([#398](https://github.com/Robomous/RoadMaker/issues/398)). Use the named
functions; never re-derive the ternary.

## What each later edit must preserve

A joint is made once and then survives, or fails to survive, every subsequent
edit. This is the current, deliberate policy per operation.

<!-- rm-contract: edits -->

| Operation | Link policy |
|---|---|
| `close_gap` | Coincidence is **3D**. Ends within `coincident_gap_m` in plan but disagreeing in z or grade are **refused**, not welded — a pure link generates nothing to reconcile them, and a vertical-only mismatch cannot be bridged by a connector either (the clothoid fit has no planar distance to work with). |
| Chain creation (`create_linked_road`, `assembly::create_road_with_interactions`) | The new road inherits the contact's z and grade and eases the inherited slope back to level over `edit::kGradeEaseLength` — see below. |
| `extend_road` | Refuses an end that is already linked, so it only ever continues. Pins both z and grade at the contact and extends at constant grade. |
| `translate_roads` | **Neighbours follow** — see below. Links between roads moving together are untouched by the shift; a link leaving the moved set drags its neighbour's contacting end along. **Junctions follow too** — see [junction regeneration](#junction-regeneration-on-move) — and so do the [derived layers](#derived-layer-recompute-on-move). |
| `rotate_roads` | **Neighbours follow.** Every road-level link swings its neighbour's contacting end round onto the rotated pose. Same junction and derived-layer policy as `translate_roads` — which is why it takes a road SET: rotating a junction rigidly means rotating every one of its arms in one gesture. |
| `move_waypoint`, `insert_waypoint`, `delete_waypoint`, `insert_node_at` | **Neighbours follow.** A junction at the moved end is marked dirty and regenerates, the derived layers are recomputed, and a plain road-to-road link is re-fit. Note that a re-fit changes the road's *length*, so an interior waypoint edit can move a joint at either end. |
| `set_elevation_profile`, `set_node_elevation`, the Z gizmo | Write exactly what the user asked for. A welded boundary that diverges is **reported, never pinned** — the Profile panel names it live and the validator reports it on save. This is deliberately NOT a follow: a profile edit is a statement about one road's height, and pinning it would fight the user. |
| `merge_roads` | Refuses unless the seam is already continuous in position, heading, lanes **and elevation**. |

### Neighbour follow on move

Every gesture above that moves a road end passes through one funnel. After the
edit lands, each joint it **disturbed** is measured with `verify_link_weld`, and
each breaching joint is either followed or severed:

- **Follow.** The neighbour's contacting end is re-fit onto the moved pose: its
  endpoint waypoint moves there, its reference-line heading is locked to
  `edit::joint_road_heading`, and its elevation boundary node takes the moved
  end's z and its sign-folded grade. The neighbour is **re-fit, not relocated** —
  it is never translated bodily.
- **Sever.** When the re-fit cannot be made — it would loop, collapse the road,
  or push a lane section past the road's own end — the link is cleared on
  **both** sides instead of being forced. A sever is never silent: every one is
  reported as an `edit::FollowRecord` with a reason, readable from the command
  through `Command::follow_records()`.

Two boundaries are as load-bearing as the rule itself:

**Depth is one hop, and that bound is a theorem, not a policy.** A pure link
asserts position, heading, elevation and grade — never curvature. The follower's
*far* end is held fixed in all four: its endpoint waypoint is never moved, its
heading is pinned into the fit whenever it carries a link of its own, and its
elevation boundary node is carried through verbatim. So the joint beyond the
follower cannot move, and there is nothing left for a second hop to fix.

**A joint that was already breaching before the gesture is left exactly as it
was.** A move restores what it disturbed. It does not repair a joint it did not
break — that is the validator's report to make — and, far more importantly, it
must not *destroy* one: a foreign file may legitimately declare a link between
ends that never met, and severing it because the user nudged a road nearby would
turn an ordinary edit into silent data loss.

What follow deliberately does **not** re-base: a re-fit changes the follower's
length, and its lane sections, objects, signals, superelevation and `laneOffset`
stay indexed against the old stations. That is exactly what `move_waypoint` has
always done to the road being dragged — the follow *is* a waypoint move on the
neighbour's contacting node — so it introduces no new semantics. Only elevation
is handled explicitly, because a drifting boundary node would breach this very
contract.

Joints where either side is a junction **connecting** road are skipped entirely:
they weld on the linked lane's inner boundary, so they belong to
`verify_junction_welds` and junction regeneration — the next section. The layers
derived from the roads are the section after that; props are sprint s4 of the
move-with-cascade epic
([#406](https://github.com/Robomous/RoadMaker/issues/406)).

### Junction regeneration on move

*Sprint record: [#462](https://github.com/Robomous/RoadMaker/issues/462)
(`cascade-s2`).*

The same funnel carries a third stage. After the follow stage settles, every
junction the gesture disturbed is classified **once**, so no gesture can hold its
own opinion about what a moved arm means — which is exactly how they came to
disagree: a node drag followed live while `translate_roads` and `rotate_road`
refused any road touching a junction, arm or not.

| Case | Behaviour |
|---|---|
| The moved road is a junction's **connecting** road | **Refused.** Its pose is generated from the arm poses, so the next regeneration would overwrite whatever it was dragged to. Move the arms instead. |
| The junction is **carried whole** — every one of its arms is in the moved set | **No regeneration.** The transform is a rigid body motion, so the junction's connecting roads (and its maneuvers' world-space control points) ride along and the output is the input transformed. Nothing hand-authored is replanned. |
| Some arms move | **Regenerated** from the new arm poses, exactly as a node drag has always done. The turn set is unchanged, so [#263](https://github.com/Robomous/RoadMaker/issues/263)'s keyed matching keeps every connecting road's identity. |
| The junction is **foreign** (no recorded arms) | **Left alone.** There is nothing to regenerate from, and a move must not fail because someone else's file is in the scene. |
| The junction is **locked** ([#319](https://github.com/Robomous/RoadMaker/issues/319)) | **Left alone.** The user asked the hand-tuned result to survive edits to the arms, and this stage is one of the automatic loops that lock binds. |
| Regeneration cannot be planned | **The whole gesture is refused**, the network is untouched, and the message names the junction. |

That last row is the sprint's decision and it is deliberate. The alternative —
dissolving the junction back to free ends — always succeeds, but it destroys
authored maneuvers, corners and stop lines to satisfy a gesture the user may not
have meant. Refusing loses nothing: the user can still delete the junction
explicitly, and a rigid move of the whole assembly is never refused. Leaving the
junction **stale** was the third option and is the state this sprint exists to
remove.

The regeneration's **turn-set policy** is `AllowChange` for a committed edit, and
`InPlaceOnly` for the editor's per-frame preview — the same trade a node drag has
always made. Mid-drag, a frame that would add or drop a turn is rejected and the
last good preview stands, so the drag simply stops following rather than
churning connecting roads at frame rate.

`edit::verify_junction_welds` is the oracle, and [#403](https://github.com/Robomous/RoadMaker/issues/403)
gave it the elevation and grade dimensions it lacked, so a junction that
regenerates into a vertical step now fails the check rather than passing it.

### Derived-layer recompute on move

*Sprint record: [#463](https://github.com/Robomous/RoadMaker/issues/463)
(`cascade-s3`).*

The funnel's fourth and last stage. Roads and junctions have settled; what is
left is the layers derived **from** them. There are two, and they fail in
opposite directions. An enclosed-area ground surface is a function of the road
ring, so it can simply be recomputed. A `<bridge>` span is anchored by `@s` in
its own road's station and records nothing whatever about the crossing it was
built for, so it cannot be recomputed — only re-anchored, against provenance read
**before** the gesture, the same trick `already_broken_ends` uses one layer down.

| Case | Behaviour |
|---|---|
| A move changes which areas the roads enclose | **The surface set is reconciled** — vanished blocks erased, new ones created — inside the move command, so undo restores each surface under its own id **with its material**. A re-derivation on undo would hand back the same loop repainted grass, and the saved file would differ. |
| A `Derived` boundary whose ring is unchanged | **Nothing to do.** The boundary *is* `bounding_roads`; the geometry follows the roads at mesh time and always has. |
| An `Authored` boundary | **Never re-derived, and reported.** The moment the user reshapes a surface it detaches to `Authored` and the loop becomes their own geometry; re-deriving it would destroy an edit to satisfy a drag. This is the report-don't-correct stance profile edits at a welded end already take. |
| A bridge span whose crossing survives the move | **Relocated** by the change in the crossing station, keeping `@id`, `@length`, `@type`, the deck material and any foreign children. |
| A bridge span whose crossing is gone — the roads no longer cross, a junction now connects them, the clearance closed, or the road is too short to reach the new crossing | **Reported orphaned and left exactly as authored.** A move that deleted authored data would be a far worse defect than one that leaves a deck in the air. `Edit ▸ Bridge ▸ Remove Orphaned Spans` is the explicit way to clear them. |
| A surface or span the gesture did not reach | **Untouched, bit for bit.** Re-derivation is scoped to the affected ids — `remesh_surfaces` rebuilds only what it is handed, and an empty span is a no-op rather than "all". |

Everything the stage could not fix is reported as an `edit::DerivedRecord`,
readable through `Command::derived_records()` and
`EditStack::last_derived_records()` — the shape `FollowRecord` established, and
for the same reason: the outcomes a move cannot repair must never be silent.

Two supporting rules make the above true rather than merely intended.
`plan_surface_reconciliation` is the read-only half of `derive_surfaces`, and an
authored surface never enters the set it offers for erasure — **that is where the
authored guarantee lives**, so the stage inherits it instead of restating it. And
`DirtySet::surfaces_are_current`, the sibling of `junctions_are_current`, tells
the editor that the move already reconciled the set: re-deriving over it would be
idempotent, but it would be a network mutation outside the command layer on the
one edit whose undo has to restore a material.

`bridge_covering` is the single definition of "is this stretch already carried".
Before this sprint the detection hint asked that question while `author_bridge`
asked a different one — exact `(s, length)` equality — so re-running Generate
after a move stacked a **second** deck beside the stale one instead of noticing.

### Chain creation and grade easing

A road chained off an existing end starts at that end's z with that end's grade
(mirrored when chaining off a Start — see the sign trap above), then eases the
inherited grade linearly to zero over `edit::kGradeEaseLength` = **20 m**, and
is level thereafter.

The taper is linear in *grade*, not merely smooth: the cubic Hermite through
`(0, z₀, g₀)` and `(L, z₀ + g₀·L/2, 0)` is exactly the quadratic
`z₀ + g₀·s − g₀·s²/(2L)`, whose derivative runs straight from `g₀` down to `0`.
So a chain climbs precisely half of what a constant-grade continuation would
give it, over a fixed and predictable distance — rather than running away to
the end of however long the road happened to be drawn.

When a flat road would already satisfy this contract at the joint, no
`<elevationProfile>` is written at all, preserving the convention every
authoring path follows.

**Ordering is load-bearing.** The seeding stage must run **before** the weld
stage, because `check_linkable` now refuses a joint whose elevations disagree
and both chain paths treat a refusal as a silent no-op. Reversed, a chain off
any elevated end would simply land unlinked with no error anywhere.

## Validator rules

Both are RoadMaker-authored (`robomous.ai:rm:`), because ASAM has no equivalent
— see [Why this document exists](#why-this-document-exists). Findings are
`Warning`: they never block a save.

<!-- rm-contract: rules -->

| Rule UID | Fires when |
|---|---|
| `robomous.ai:rm:1.0.0:roads.link_ends_coincide` | Two linked ends are further apart than `tol::kWeldPosition` — the link no longer describes the geometry. |
| `robomous.ai:rm:1.0.0:roads.link_elevation_continuity` | Two linked ends step in z beyond `tol::kWeldElevation` or break grade beyond `tol::kWeldGrade`. |

Each joint is reachable from both of its ends and is reported **once**, from
the end whose `(road id, contact)` sorts first — unless the neighbour does not
link back, in which case that end is the joint's only vantage point. A joint
that fails to coincide reports only `link_ends_coincide`: a joint whose ends
are not in the same place is not a joint, and reporting its elevation too would
be derivative noise.

## Out of scope

- **Prop obstruction** — a prop driven into another road by the move that
  carried it is sprint s4 of the move-with-cascade epic
  [#406](https://github.com/Robomous/RoadMaker/issues/406). Road-level
  neighbour-follow, junction regeneration and derived-layer recompute have all
  landed and are specified [above](#neighbour-follow-on-move).
- **Junction quality under curved approaches** —
  [#356](https://github.com/Robomous/RoadMaker/issues/356), a separate open bug.
  A regenerated junction is exactly as good as a freshly generated one; making
  that better is not this contract's business.
- **Terrain skirt behaviour** — the skirt faithfully amplifies joint steps into
  cliffs (`core/src/mesh/terrain_mesh.cpp`). Fixing the cause is this
  contract's job; the skirt needs no change.
- **Lane-level link continuity** — road-level only here.

## Enforcement

`core/tests/test_connection_contract.cpp` reads this committed document and
fails CI, not review, when it drifts from the code:

- every `tol::kWeld*` constant appears in the [tolerances](#tolerances) table
  with its exact value;
- `edit::kGradeEaseLength` appears in the [chain creation](#chain-creation-and-grade-easing) section with its exact value;
- every `robomous.ai:rm:` UID in the [validator rules](#validator-rules) table
  exists in `core/include/roadmaker/xodr/rules.hpp`, and both connection rules
  are cited here;
- every kernel operation that can outlive a joint has a row in the
  [edit-policy table](#what-each-later-edit-must-preserve) — searched **within
  that table**, not across the document, so a row cannot be satisfied by a
  mention somewhere else entirely.

The behaviour itself is covered by `core/tests/test_connection.cpp`,
`core/tests/test_link_follow.cpp`, `core/tests/test_junction_cascade.cpp`,
`core/tests/test_derived_cascade.cpp`, `core/tests/test_xodr_writer.cpp`,
`editor/tests/test_profile_panel.cpp` and `python/tests/test_edit.py`.
