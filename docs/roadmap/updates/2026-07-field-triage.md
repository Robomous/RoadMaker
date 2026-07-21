# 2026-07 field triage — five maintainer reports

Tracking issue: [#351](https://github.com/Robomous/RoadMaker/issues/351).
Five items from hands-on use (two behavior changes, three bugs), each diagnosed
against the code at `main` @ `950939c` before any task was cut. This document
records the diagnoses, the resulting issues, the golden-workflow amendments,
and the fix queue. The full per-issue scope/acceptance lives in the issues
themselves.

Dedupe: no existing open task covered any item. Relevant closed lineage:
#95/#214 (single tee/cross on commit), #103/#105 (junction geometry quality),
#194/#219 (urban profile / road styles). None of these items block on the
prop-placement spec (#338) or on ADR-0008; the one interaction found is noted
under item 1.

## Item 1 — new roads default to the urban-with-sidewalks template → [#355](https://github.com/Robomous/RoadMaker/issues/355)

The default is a hardcoded constant in two places that must agree:
`editor/src/tools/create_road_tool.hpp:85` (`LaneProfile::two_lane_rural()`)
and the toolbar's initial check in `editor/src/app/actions.cpp:268`. The
target, `LaneProfile::urban_sidewalk()` (`core/src/road/authoring.cpp:25-33`),
already ships and is wired into the toolbar and the Library ("Urban w/
sidewalks"). Two clarifications the diagnosis settled:

- The p2-s8 `RoadStyle::urban_two_lane()` Library asset is a **re-style delta
  for existing roads** and carries no sidewalks — it is not the target. The
  creation-blueprint system (`LaneProfile`) and the re-style system
  (`RoadStyle`) are distinct.
- No project/scene-level default-template concept exists. Decision: flip the
  constant now; a per-project default is a candidate for the fmt-s1 (#325)
  native project container, not a dependency of this change.

Existing scenes and placed roads are untouched; rural stays available.
User-guide wording ships with the code change (in #355's scope), so the docs
never describe unshipped behavior. GW-2 step 2 ("a road with the default
style") is default-agnostic — no GW amendment.

## Item 2 — two-point road creation loops into a teardrop → [#352](https://github.com/Robomous/RoadMaker/issues/352) (+ UX follow-up [#353](https://github.com/Robomous/RoadMaker/issues/353))

The unconstrained two-point fit is straight by construction (the Clothoids
dependency special-cases n==2 to the chord heading at both poses). The loop
appears because a snap silently locks an endpoint heading and the locked fit
is solved as a fixed-heading Hermite clothoid with no fallback and no loop
rejection (`core/src/road/authoring.cpp:224-245`). Two triggers:

- the always-on **tangent-continuation snap** projects clicks onto the
  *infinite forward ray* of every road end (`core/src/edit/snap.cpp:74-93`),
  so a first click near some road's extension line locks the start heading
  without the user knowing;
- the intentional `+π` arrival reversal on a last-click endpoint snap
  (`editor/src/tools/create_road_tool.cpp:77`) forces "arrive from the far
  side" when approaching a snapped end from the wrong direction.

Refuted hypotheses (for the record): stale cross-session heading state
(session state is fully reset), solver branch selection near zero curvature,
and ±π wrap arithmetic errors.

**Redesign verdict: no tool redesign.** The multi-click waypoint model matches
GW-2 and is sound; the failure is constraint *communication*. #352 fixes the
constraint handling (lock only on genuine chaining intent + a kernel bound
that rejects looping locked fits); #353 separately adds the missing snap
affordances (visible heading-lock indicator, modifier to suppress snapping).

## Item 3 — auto-junctions: every crossing + T-intent, one commit → [#354](https://github.com/Robomous/RoadMaker/issues/354)

One root cause covers both reported defects, so one task was cut rather than
two: `CreateRoadTool::commit()` selects exactly one junction-forming assembly
from an if/else ladder (`editor/src/tools/create_road_tool.cpp:232-261`), and
every helper below it is single-result — `first_body_crossing` keeps only the
smallest-s crossing, and kernel `cross_roads` breaks at the first viable
crossing then splits the target road, leaving later crossings structurally
unreachable (`core/src/edit/operations.cpp:5180-5222`). T detection exists on
the draw path but only for the terminal waypoints, mutually exclusive with X
in the same ladder.

The kernel needs no new capability — `CompositeCommand` already applies
lazily-built children atomically. The fix is an enumerate-all-interactions →
one-composite-commit refactor in the tool/assembly layer. P4 interplay was
checked: locked/span junctions and connecting roads are already guarded, and
the fix must preserve those guards plus `junctions_are_current` flagging.
GW-2 step 3 was amended accordingly (see below).

## Item 4 — junction sidewalk corners → [#356](https://github.com/Robomous/RoadMaker/issues/356) + [#357](https://github.com/Robomous/RoadMaker/issues/357)

The two screenshots are two different defects:

- **Curved-approach collision (#356, lands first)** — a generation bug in the
  shared corner solver: `corner_faces` samples each approach at a single end
  station and treats its outer edge as an infinite straight ray
  (`core/src/mesh/junction_corner_detail.cpp:143-149`), so a curved mouth
  diverges from the ray and the fillet/strip geometry interpenetrates.
  Explicitly **not** the p4-s5 escape-valve class: surface spans arbitrate
  elevation/samples inside a fixed union and cannot move the boundary.
- **Missing sidewalk wrap + mixed boundary (#357)** — a generation gap that
  was never scoped: the junction floor *includes* the sidewalk band (the
  boundary uses the outermost lane edges with no type filter) but paves it
  all as one carriageway material (`core/src/mesh/fill_backend.hpp:589`);
  the only corner sidewalk geometry is p4-s2's opt-in material wedge — a
  notch triangle, not a continuous band. No code branches on lane type when
  shaping the junction boundary, so mixed sidewalk/no-sidewalk junctions have
  no specialized handling.

Both tasks name the shared-solver regression surface (p4-s1/s2 corner
edits + materials, p4-s4 lock/regen record-carry, p4-s5 spans, p4-s6
maneuvers); sanitizer runs covering the editor tests are mandatory. With
#355 making sidewalked roads the default, every junction exercises this
path — which raises #357's value.

## Item 5 — wheel zoom anchors to the cursor → [#358](https://github.com/Robomous/RoadMaker/issues/358) (p1-f3)

Orthographic wheel zoom already anchors to the cursor
(`editor/src/viewport/camera.cpp:98-123`); perspective does not — its branch
is a plain pivot dolly while comments (and one unit test,
`ZoomAboutInPerspectiveIsAPlainZoom`) claim cursor-ray travel that does not
happen. #358 makes perspective pin the world point under the cursor, keeps
the chord zoom pivot-centered, and — per the approved push-past decision —
relocates the pivot along the **cursor ray** on wheel-driven push-past so the
subject never lurches away at deepest zoom. The affected camera tests are
named in the issue. GW-1 steps 3–4 were amended (below); user-guide camera
pages update within #358, alongside the code.

## Golden-workflow amendments (this change)

- **GW-1 steps 3–4** (`gw1_camera.md`): the wheel now zooms toward/away from
  the **cursor** in both projections; wheel push-past relocates the pivot
  along the cursor ray; the chord zoom stays pivot-centered. The steps'
  checkboxes were cleared and an amendments note records that the 2026-07-15
  macOS pass predates the change — steps 3–4 re-verify at the next hand-run.
- **GW-2 step 3** (`gw2_simple_scene.md`): extended from the single-crossing
  wording (the p2-s3 delivery) to require a junction at every crossing plus
  T-intent detection, all in one undoable commit, with T-intent indicated
  while drawing.

## Fix queue (approved priority, user-pain × risk)

1. **#352** — teardrop loop (P2, bug). Daily blocker on the most basic
   gesture; small, well-localized fix.
2. **#356** — curved-corner collision (P4, bug). Daily blocker, visible
   corruption; contained in the corner solver.
3. **#354** — multi-crossing + T-intent (P2, bug). Core authoring flow;
   kernel machinery ready.
4. **#357** — sidewalk wrap + mixed boundary (P4). Largest task; lands after
   #356.
5. **#358** — zoom-to-cursor (P1, p1-f3). Small, isolated quality-of-life.
6. **#355** — urban-with-sidewalks default (P2). Trivial flip + docs; can
   ride along any time. (#353, the snap-affordance follow-up, schedules with
   or after #352.)

These are follow-ups and do not displace the P4 sprint queue (#228 next)
unless the maintainer reorders; the recommendation is to run #352 and #356
before or alongside p4-s7.
