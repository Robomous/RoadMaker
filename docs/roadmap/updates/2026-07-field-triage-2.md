# 2026-07 field triage, batch 2 — six maintainer reports

Tracking issue: [#397](https://github.com/Robomous/RoadMaker/issues/397).
Six items from hands-on use, each diagnosed against the code at `main` @
`d900583` before any task was cut: junction sidewalk interiors, the prop
corridor clamp, welcome-screen recents, elevation continuity at road joints,
camera navigation below Z=0 plus an axis widget, and moving roads with
cascade recalculation. This document records the diagnoses, the resulting
issues, the maintainer's four amendments, the release-gate change, and the
fix queue. The full per-issue scope/acceptance lives in the issues.

Precedent: the [first 2026-07 triage cycle](2026-07-field-triage.md)
(#351 → #352–#358/#360). Dedupe: no existing open task covered any item;
relevant lineage is cross-linked per item below. Diagnoses that *cleared*
suspected regressions are recorded as explicitly as the defects — two of the
six items turned out not to be code regressions at all.

Slug note: the triage drafts proposed `p1-f4`, `p1-f5`, and `p2-s9`; those
slugs were already taken (#371, #375, #264), so the new items carry the next
free ones: `p1-f7` (#404), `p1-f8` (#405), `p2-s10` (#403).

## Maintainer decisions (2026-07-23, Gate A amendments)

1. **Everything in this batch is release-blocking.** Every item identified up
   to and including this triage batch must be complete before v0.1.0 —
   nothing is post-release, including the full move-with-cascade epic (#406)
   and both P1 follow-ups. The [release gate](../README.md#release-gate) is
   amended accordingly and is checkable from the issue tracker alone.
2. **Every pillar reopen is flagged with the same criterion.** Three pillars
   were sprint-complete and reopen: **P1** (#401, #404, #405), **P2**
   (#398, #403), **P4** (#402). Each pillar's discovery doc carries a dated
   status note; GW hand-run status is unaffected until the items land.
3. **#338's spec is extended before release for implementation** with an
   orphaned-anchor policy on road deletion (kernel-level atomic,
   world-position-preserving re-anchor + editor re-anchor/delete flow, landing
   with #338), an accepted-and-known rotation-arc risk note for large-|t|
   props (mitigation deferred to #406's obstruction sprint, cross-linked both
   ways), and an interop position-round-trip acceptance criterion run as an
   external process / CI-only step (never a runtime dependency).
4. **The sidewalk-segmentation bug (#402) carries hard numeric acceptance
   criteria** derived from the triage probe harness — the fix PR cannot
   renegotiate them (details under item 1).

## Item 1 — junction sidewalk interiors → [#402](https://github.com/Robomous/RoadMaker/issues/402) (+ a masking note on [#360](https://github.com/Robomous/RoadMaker/issues/360))

**Not a regression of #385** (which delivered exactly #357's scope) and **not
#356** (reproduces with zero curvature). A straight-approach headless probe
against the real junction path measured the band split: the splitter
classifies floor triangles by centroid with no constrained edge on the band's
inner boundary, so seam error ≈ the Steiner triangle size ≈ the full band
width — sawtooth seams (T tight: 26.8 % mid-band misclassification, teeth
1.5 m into the carriageway; even the roomy 4-way class the existing tests use
shows 4.0 %). Tight N-ways collapse to **zero bands** (a fully sidewalked
5-way renders no sidewalk); single-sided corners and non-outermost sidewalk
(sidewalk+shoulder) get nothing. Floors stay watertight — the defect is the
material seam. Centroid classification cannot be tuned into correctness; the
fix is boundary-aware splitting (the band's inner boundary as constrained CDT
edges) plus a floor-aware band build. #360's black junction floor masked this
class of defect in the CI review artifact — noted there; #360 lands
with/before #402's PR review.

**Hard acceptance thresholds** (decision 4): mid-band misclassification
< 1 % per band on every case in the probe matrix; seam deviation
< 0.5 × `kSteinerStep` (= 1.0 m today, scaling with the step) as max vertex
deviation from the ideal band inner boundary; **zero** zero-band collapses
(every corner adjacent to a sidewalked arm emits its band stub); bands flush
to the arm-face sidewalk mesh. Every current case fails all four; the
justification for each number is recorded in the issue.

## Item 2 — prop corridor clamp → [#338](https://github.com/Robomous/RoadMaker/issues/338) spec approved, hold lifted

The "clamp" is a deliberate snap-radius rejection gate on |t| measured from
the road **reference line** (12 m), introduced with the road-relative
persistence policy — the kernel never constrains t, and ASAM OpenDRIVE leaves
t unbounded (objects stay children of `<road>`), so un-clamping is spec-legal
while detaching props from roads is not representable. The real technical
blocker was height: prop z extrapolates the road's superelevation plane to
arbitrary t; the scene height field (#232) can answer off-corridor but was
never consulted. The approved spec: nearest-road unbounded-t anchoring for
free-canvas placements, the Prop Polygon survival filter removed, a height
fallback chain (road corridor → height field → z = 0), the corridor boundary
defined by the outermost lane edge rather than a fixed radius, ~2 m edge
soft-snap with a suppression modifier, signals/markings/span/curve
distributions unchanged — plus the decision-3 additions (orphaned-anchor
policy, rotation-arc risk, interop round-trip). Implementation may run in
parallel with p5-s4.

## Item 3 — welcome-screen recents → [#399](https://github.com/Robomous/RoadMaker/issues/399)

**No regressing PR exists** — the welcome widget is functionally unchanged
since #295 and every suspected PR was cleared. The production settings store
was polluted and evicted by automated capture runs through three
long-standing gaps: screenshot/capture mode records into the real recents
list, paths are stored verbatim (relative entries die on any cwd change), and
the 10-entry cap evicts with no protection. Green tests plus broken field
behavior are consistent: the fixtures use isolated absolute-path settings.
The issue carries a from-scratch repro; the fix (absolutize, capture-mode
read-only settings, tests in production scope) was explicitly out of scope
for the triage run itself.

## Item 4 — elevation at road joints → [#398](https://github.com/Robomous/RoadMaker/issues/398) + sprint [#403](https://github.com/Robomous/RoadMaker/issues/403) (`p2-s10`)

Two observations, two verdicts:

- **Correct-by-design**: extending from an elevated end pins the contact
  z/grade and extends linearly — a −6 % grade run below Z=0 is exactly the
  coded behavior; the grade-easing variant was the never-implemented half of
  the #95/#97 supersession. Consequence: below-zero geometry is legitimate
  (feeds item 5's full scope), and easing lands in #403.
- **Latent kernel bug** found during verification: `close_gap`'s connector
  branch omits the contact-dependent grade sign flip that the junction
  generator applies, producing a V-kink ramp at "Link Ends" for three of the
  four contact combinations (#398 — first in the fix queue).

Beyond the bug, elevation continuity today exists **only where a connector
road is generated, and only at generation time**: pure-link welds compare
x/y only, profile edits are strictly per-road, and no document states what a
connection guarantees — that absence is itself a finding. Sprint #403 writes
the contract (per-operation plan + elevation guarantees), adds 3D weld
checks and a validator advisory, pins profile edits at welded boundaries,
delivers chain-creation grade matching with easing, and states the move/drag
link policy that the cascade epic then implements. P5's terrain skirt merely
amplifies joint steps into cliffs — fixing the cause is this sprint.

## Item 5 — camera below Z=0 + axis widget → [#404](https://github.com/Robomous/RoadMaker/issues/404) (`p1-f7`) + [#405](https://github.com/Robomous/RoadMaker/issues/405) (`p1-f8`)

**No camera z-floor exists** — the pivot is fully unbounded. The perception
comes from the pitch clamp (the camera can never look *up*, so underpass
ceilings and deck undersides are unviewable) and from z=0 ground-plane
re-anchoring (pan/hover/framing anchors resolve on the datum plane and yank
the pivot back). Since below-zero geometry is legitimate (item 4), #404 keeps
full scope: symmetric pitch range plus real-ground anchoring (terrain height
field or current pivot plane). #405 adds the axis-navigation corner widget on
already-shipped groundwork (cardinal views, the gizmo's constant-pixel axis
math, the overlay block); no dependency between the two. Both amend GW-1 with
their PRs, per the #358 precedent.

## Item 6 — moving roads with cascade recalculation → epic [#406](https://github.com/Robomous/RoadMaker/issues/406) (workstream `cascade`) + bugs [#400](https://github.com/Robomous/RoadMaker/issues/400), [#401](https://github.com/Robomous/RoadMaker/issues/401)

Verified state: node-drag junction follow already works live (#156/#208);
body-drag and gizmo moves sever plain link welds (the gizmo without
confirmation), node drag leaves link records silently stale, elevation edits
leave junction contacts silently stepped, surface sets and bridge spans go
stale after moves, and nothing detects prop obstruction. Two incidental bugs
were cut immediately: #400 (prop instances render at the pre-move position
after translate/rotate — the ops never dirty the objects channel) and #401
(the gizmo discards the kernel's refusal — no feedback, and no sever
confirmation). The capability itself is a five-sprint, dependency-ordered
epic **gated on the #403 contract**: neighbor follow, junction-regen coverage
for all gestures, derived-layer recompute, prop obstruction
flagging/relocation. It spans P2/P4/P5/P6, so it is the fourth cross-pillar
workstream — label `cascade`, own epic, **no pillar milestone** (matching
`help`/`fmt`/`docs-site`; the release gate names the workstream explicitly,
which is why label-only tracking suffices). Sprints are cut after #403 lands;
the epic checklist is the skeleton.

## Release-gate change (decision 1)

The [release gate](../README.md#release-gate) now requires, in addition to
the eight pillars: every cross-pillar workstream issue (`help`, `fmt`,
`docs-site`, `cascade`) and every follow-up/bug closed — concretely, **any
open issue created on or before 2026-07-23 blocks v0.1.0 unless the
maintainer explicitly re-scopes it**. The gate is checkable from the issue
tracker alone.

## Fix queue (approved priority)

1. **#398** — close_gap grade-sign V-kink (P2, bug). Small kernel sign fix,
   visible defect on the basic Link Ends gesture.
2. **#399** — welcome recents (P6, bug). Small; restores the daily launch
   surface and stops ongoing pollution by automated runs.
3. **#400** — prop-instance staleness (P6, bug). Small; silent visual lie
   after any road move.
4. **#402** — sidewalk segmentation (P4, bug). Largest visible defect —
   every default urban-sidewalk junction shows it; lands **before** #356.
5. **#356** — curved-corner collision, after #402 (both touch the corner
   solver; #402's floor-aware band build softens its band-side symptoms).
6. **#360** — CI black floor, with/before #402's PR review.

Running in parallel, unchanged: #234 (p5-s4) continues as the next feature
sprint; #338 implements after this spec-post; #403 after or alongside #234;
#406's sprints are cut after #403, scheduled before/alongside P7. The
docs-site queue (#345–#348) and open follow-ups (#353, #390, #396,
#391–#393, #297, #307, #337) keep their relative order — all release-blocking
under decision 1.

**Golden workflows**: no amendments in this pass — #404/#405 and the cascade
epic amend GW-1/GW-2 when their behavior lands, with the implementing PRs
(the #358 precedent). The item-1 probe oracles become *tests*, not GW steps.
