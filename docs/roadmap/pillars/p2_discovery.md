# P2 discovery — Roads & Lanes

*What the code actually looks like today, per P2 scope item, and what that
means for the sprint cut. Written 2026-07-15, before p2-s1 landed. Roadmap:
[Road to Parity](../README.md) · Acceptance:
[GW-2](../golden_workflows/gw2_simple_scene.md).*

## Why this document exists

The P2 sprint bodies were written from the roadmap, not from the code. Read
against the code, the pillar is **inverted**: the refactor everyone expected
(lane-level picking) is already done, and the work that looked incremental
(lane authoring) is the bulk of the pillar. P2 is roughly **65% kernel, 35%
editor**. This records the ground truth each sprint builds on.

## 1. Lane picking — already done, end to end

The planning assumption was that a road renders as one merged mesh, so
resolving a lane would mean a picking refactor ahead of every lane tool. It
does not.

A road is **one shared vertex grid plus one index array per lane**
(`core/include/roadmaker/mesh/mesh.hpp` — `RoadMesh::LanePatch` carries
`LaneId`), watertight by construction because adjacent lanes reuse boundary
vertices. Lane identity survives all the way to the GPU: `scene_builder.cpp`
emits one draw item per lane patch, and `SceneItem` / `UploadedItem` both
carry a `LaneId`.

| Capability | State today |
|---|---|
| `PickHit::lane` | **populated** (`viewport/picking.cpp`) — CPU Möller–Trumbore raycast, per-road AABB prefilter |
| `SelectionModel` holds a lane | **yes** — `SelectionEntry{road, lane, …}`; `LaneProfileTool` already does it |
| Framing a single lane | **yes** — `framing.cpp` isolates a lane's vertices via `grow_indexed` |
| Lane-granular hover/highlight | **yes** — `hovered_lane_`; the readout formats `"road 5 / lane -1"` |
| `(road, lane, s)` | ~10 lines of composition — `find_station(road->plan_view, x, y)` exists; `build_menu_context` already pairs a `PickHit` with a station |
| Lane **boundary** picking | **missing** — nothing resolves "which lane edge is nearest". ~60 lines of pure code, derived from `(lane, s, t)` + cumulative widths |

**Consequence: the picking sprint the plan sketch proposed was never needed**
— it is not created. The finding also de-risks the Lane tool sprint, which had
been sequenced behind a picking prerequisite that turns out not to exist. The
sprint numbering shifts because p2-s1/s2 are inserted ahead of the existing
issues, not because any of them lose scope.

## 2. The kernel data model — faithful, and unexercised

The model is already fully OpenDRIVE-conformant, and **ahead of the
authoring layer**:

- `Road::sections` is a **sequence** of `LaneSectionId`, sorted by s0.
- `Lane::widths` is a `std::vector<Poly3>` — real cubic width records with
  section-local `sOffset`.
- Lane links are per-section `std::optional<int>` (raw OpenDRIVE ids).
- `LaneSectionId` exists; the arena API already has the `erase_exact` /
  `restore` pair the command layer needs.

The **reader, writer and mesher all handle multi-section roads correctly**.
The writer emits multiple `<laneSection>`, `<width>` with a/b/c/d, and lane
`<link>`; the mesher builds a separate vertex grid per section and evaluates
widths section-locally, so a tapering carve lane will mesh with **zero mesher
changes**.

**But almost nothing authors it, and nothing tests it.** `merge_roads` is the
only operation that ever produces a two-section road, and no test asserts the
resulting section count. Three tests actively assert the opposite
(`test_xodr_reader.cpp`, `test_edit_operations.cpp`,
`test_create_road_tool.cpp` each assert `sections.size() == 1`). The
multi-section paths look correct on reading but are effectively **untested** —
p2-s1 will be the first thing to run them in anger, and writes its fixtures
first for that reason.

## 3. Lane authoring — three verified blockers

The entire existing lane suite is five ops: `add_lane`, `remove_lane`,
`set_lane_type`, `set_lane_width`, `set_road_mark`.

1. **`set_lane_width` destroys the width profile.** It does
   `after.widths = {Poly3{.a = width_m}}` — replacing every record with one
   constant at sOffset 0. The header admits it: *"Constant width in M2"*.
   This is a **live data-loss bug** the moment tapers exist: carve a lane,
   then edit its width, and the taper is silently gone. It must be fixed
   before Lane Carve ships, not after.
2. **Nothing splits a lane section.** `RoadNetwork::add_lane_section` exists
   but has no `edit::` factory. The good news: `split_road` already contains
   the exact width-rebasing and road-mark sOffset-rebasing logic a
   `split_lane_section` needs, and `merge_roads` has the seam-link pattern.
   **Lift, don't invent.**
3. **`add_lane` is outermost-only**, deliberately — the comment says it
   "keeps OpenDRIVE lane numbering contiguous". Interior insert (Lane Form,
   Lane Carve) means renumbering, which cascades into `Lane::predecessor` /
   `successor` in **both** neighbouring sections and into
   `JunctionConnection::lane_links`. `remove_lane` already *clears* those;
   P2 needs to *remap* them.

Also worth tightening: the writer's pre-write link validator only checks the
**successor** direction. A bad split can set predecessors inconsistently, pass
the validator, and write a subtly wrong file.

## 4. Junction regeneration vs. lane-count change — the hard collision

`regenerate_junction` **refuses** any turn-set change, twice:

> `"regeneration changed the connection count; delete and recreate the junction"`
> `"regeneration changed the turn set; delete and recreate the junction"`

and the editor swallows the failure into a warning toast rather than failing
the user's edit (`document.cpp`).

So today: **add a lane to a road feeding a junction → the junction does not
update, and stays stale until the user deletes and recreates it.** GW-2 step
12 carves a turn lane *"along a lane approaching the junction"* — precisely
the refused case. The restriction is not incidental: regen's ability to
preserve connecting-road ids (and therefore keep undo exact) *depends* on a
fixed connection count.

It also collides with the editor's dirty-set contract, which deliberately
skips regen entirely for topology commands — so a Lane Carve would need to be
a topology command that *also* regenerates, a combination the contract
currently forbids.

**Decision (Armando, 2026-07-15): teach regen turn-set changes**, as its own
sprint before Carve. A turn lane that cannot reach a junction is a hollow
feature. Turn matching is already by key rather than by order, so that half is
done.

## 5. Ground surfaces — nothing exists, but the pipeline does

The "grass ground" is a **render trick**: a camera-following quad synthesized
from `gl_VertexID` with no vertex buffer, coloured by a value-noise fragment
shader. The whole API is `set_ground(bool, float base_z)`. There is no
`Surface` entity, no id type, no arena — the kernel has exactly six arenas and
none is ground-like. There is also **no cycle/loop detection** anywhere in the
network graph.

What *does* exist is the hard part. `core/src/mesh/junction_surface.cpp` runs
exactly the pipeline enclosed-area ground needs, and it is proven, pure and
deterministic: **Clipper2 union of plan-view footprints → CDT with interior
Steiner refinement → harmonic (Laplacian) elevation field with Dirichlet
boundary → boundary snapped to road-mesh vertices for a watertight seam.**
Its `RoadContribution` is already precisely "a road's outer boundary as a
Clipper2 path plus an exact 3D ring" — just private to that translation unit.
Promote it rather than rewriting it.

Dependency state, checked rather than assumed: **Clipper2 and CDT are live**
in production. **Manifold is linked but used only in a smoke test. libigl is
declared and never linked or included at all** — both cost build time for
nothing (worth a cleanup issue outside P2). Note `CLIPPER2_USINGZ` is **OFF**
("plan-view 2D only") — Clipper2 carries no Z, so elevation is reconstructed
separately, as the junction surface does.

**Decision (Armando, 2026-07-15): a real `Surface` entity with a derived
boundary** — `{BoundarySource source; std::vector<RoadId> bounding_roads;}`,
re-derived when a bounding road changes. P5's node-graph Surface tool extends
the same entity with authored nodes rather than replacing it. The alternative
(a pure derived function, per the junction-surface precedent) was cheaper but
would have forced P5 to introduce the entity and migrate.

## 6. Road styles — `LaneProfile` is the wrong type

`LaneProfile` is a kernel struct (`road/authoring.hpp`) of `LaneSpec{type,
width, outer_marking}`. The **entire** "road style asset" mechanism today is a
string→function switch in `library_drop.cpp` mapping a name to
`LaneProfile::two_lane_rural() / urban_sidewalk() / highway()`.

Three gaps make it unfit for P2:

- **Not serializable.** No JSON/XML, no manifest entry.
- **Carries almost no marking data.** `outer_marking` is a **bool → solid,
  hardcoded**; `center_marking` a bool → broken. No type, colour, or width —
  even though the `RoadMark` model supports all of it. GW-2 step 13 needs
  real marks.
- **`LaneSpec::width` is a constant `double`** — a style can never round-trip
  a carved lane.

And it is semantically a *creation blueprint*, consumed only by
`author_clothoid_road`; nothing applies a profile to an **existing** road.
P2 needs a *style delta applied to existing state*. Those are different types:
introduce a serializable, mark-carrying, poly-width-capable `RoadStyle`, and
keep `LaneProfile` as the legacy creation path.

Editor side is close to ready: a "Road style" `SlotWidget` is a near-verbatim
copy of the existing Model slot, and the Library MIME contract is settled. The
one real gap is that **`resolve_library_drop` discards the pick** and resolves
world x/y to the nearest road by reference-line proximity — "drop onto *this*
road" needs the target entity, so widen that signature early.

## 7. Extend-from-endpoint — new solver math, good scaffolding

Every fit available today is **pose-to-pose with both ends pinned**:
`fit_clothoid_path` needs the full waypoint list; `fit_g2_three_arc` requires
position + heading + curvature at *both* ends. **"Fixed start pose and
curvature, free end" does not exist** and is genuinely new kernel math.

The scaffolding is all there: `contact_state()` already returns pose,
curvature, z and grade at a road end; `G2solve3arc` is already linked and
wrapped; `create_linked_road` is a near-perfect template (it authors a road
*and* welds it to a source end in one undoable command). Copy
`build_g1_path`'s exception boundary — `G2solve3arc` throws on degenerate
input and the kernel API is exception-free. Investigate `insert_waypoint` at
`index == count` first: it appends, and may cover most of the case.

## 8. Editor frameworks from P1 — ready, and better than expected

- **`LaneProfileTool` is a 28-line stub**, already bound to `L` and already
  lane-picking. Its header says the tool exists "for toolbar consistency and
  lane-granular viewport highlighting". **This is the P2 Lane tool, waiting
  to be filled** — not a new tool.
- **`Editor2DPage`** is purpose-built for a width-along-s editor; its header
  already names the cross-section as a planned page. `ProfilePanel` (a 2D
  `z(s)` curve editor with draggable nodes and preview sessions) is the
  structural template for `width(s)`.
- **Scrub + slot** are ready; the lane-width scrub binding is already the
  exact template. Note the `ScrubBinding` table is deliberately local to
  `PropertiesPanel` "until a second one appears" — **P2 is that second
  consumer**.
- **Preview sessions** are mature: every P2 tool is `begin_preview` /
  `update_preview(factory)` / `commit_preview`, with `EditNodesTool` as the
  canonical reference. No new command infrastructure.

Constraints P2 must design around:

- **`⌥` is unavailable to tools.** `NavController` claims any `⌥`, any RMB
  and any MMB before the tool; a tool only ever sees plain LMB press/release.
  Lane Carve/Form chords must use `⇧` / `Ctrl`.
- **Adding a new selectable kind costs ~17 code sites + 7 test files.** Lane
  already exists, so only the Surface entity (p2-s7) pays it.
  `selected_roads()` is the classic miss — forget it and the new kind
  silently puts a road in play for every tool.
- **Shortcut namespace is crowded** — `Q M C N L E J K X F V P O` plus
  numpads are taken. Reuse `L` for Lane.
- **Perf:** `pick()` is brute-force over every triangle of every
  AABB-passing road, and runs on **every mouse-move**. Five lane tools adding
  a per-move 512-sample `find_station` scan on top could bite. Measure first.

## GW-2 amendments these findings propose

GW-2 is marked draft ("steps are refined as the owning pillar sprints land").
The proposed amendments, landing with the sprints that implement them:

1. **Step 3 describes a feature that does not exist.** It says *"a junction
   is created automatically at the overlap"*. The Create Road tool only calls
   `create_road` / `create_linked_road`, and `SnapKind` is
   `{Grid, RoadEndpoint, TangentContinuation}` — there is no side or crossing
   snap. Junctions are made only with the Create Junction tool. The kernel ops
   exist (`assembly::cross_onto_road`, `tee_onto_road`, `attach_t_junction`),
   so this is **tool wiring**, and it is the natural sibling of #95's
   tee-on-commit. **P2 owns it; folded into p2-s3.**
2. **Step 11 is self-contradictory.** A road style *defines* the lane
   profile, yet the step preserves *"lane count edits"*. **Decision
   (Armando, 2026-07-15): a style replaces the lane profile and boundary
   marks, and preserves reference-line geometry, elevation, superelevation,
   junction connectivity, name, and placed objects/signals.** Amend the step
   to preserve the elevation profile only.

## GW-2 step ownership

P2 owns **steps 2, 3, 4, 5, 11, 12**, plus its slice of 23 (round-trip).
Step 6 (Surface node graph) and 7–8 (terrain, bridges) are P5; 9 is P4;
10 and 13–15 are P3; 16–20 are P6; 21–22 are P7.

## Sprint cut this implies

| Sprint | Reality-adjusted scope |
|---|---|
| p2-s1 | **New.** Kernel lane-section foundations: `split_lane_section`, `set_lane_width_profile` (fixes the §3 data-loss bug), interior `add_lane` with renumber+remap, public `section_at`/`section_end`, multi-section fixtures first |
| p2-s2 | **New.** Junction regen learns turn-set changes (§4) — highest risk in the pillar; gates Carve |
| [#214](https://github.com/Robomous/RoadMaker/issues/214) p2-s3 | Extend-from-endpoint (new solver math, §7) **+ tee-on-commit (#95) + cross-on-commit (GW-2 step 3) + grade match (#97)** |
| [#216](https://github.com/Robomous/RoadMaker/issues/216) p2-s4 | Lane tool = **fill the 28-line stub**; Lane Width as a new `Editor2DPage` |
| [#217](https://github.com/Robomous/RoadMaker/issues/217) p2-s5 | Lane Add + Lane Form as viewport tools over s1's ops |
| [#218](https://github.com/Robomous/RoadMaker/issues/218) p2-s6 | Lane Carve + lane-boundary hit-testing; depends on s1 **and s2** |
| [#215](https://github.com/Robomous/RoadMaker/issues/215) p2-s7 | Enclosed-area ground surfaces — scope unchanged, but the **representation is now decided**: a `Surface` entity with a derived boundary (§5), not a render-side by-product |
| [#219](https://github.com/Robomous/RoadMaker/issues/219) p2-s8 | Road styles: new `RoadStyle` type (§6), `apply_road_style`, drop-target widening, urban two-lane style (#194) |
| p2-s9 | **New.** GW-2 P2-steps self-check, tool docs, GW-2 amendments |
| help-s1/s2 | **New.** In-app Help (Qt Help Framework) — pipeline + viewer, then F1 context help + coverage test + seed tutorials |

Critical path: **s1 → s2 → s6**. s3 and s7 are independent.
