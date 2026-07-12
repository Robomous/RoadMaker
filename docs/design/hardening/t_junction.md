# T-junctions — split + attach-to-side (hardening sprint)

*Design for issue [#92](https://github.com/Robomous/RoadMaker/issues/92)
(kernel) and the editor side-snap slice; an as-built addendum to the M2
editing tools ([02_editing_tools.md](../m2/02_editing_tools.md)), which this
document extends rather than replaces.*

## The root-cause lesson, first

v0.3.0 could not build a T-intersection: Create Junction connects road
*endpoints* in proximity, but a T attaches an endpoint to the *side* of
another road — which requires splitting the target first, and no tool
exposed that workflow even though the kernel always had `split_road`. The
spec was written from the standard's element vocabulary ("junctions,
connecting roads, lane links exist") instead of user workflows ("draw a
road, then tee another into it — the second thing anyone draws"). The
standing rule that came out of this is in the
[product-parity standard](../../standards/product-parity.md): **every tool
spec must include workflow walkthroughs, not only element coverage.** This
document starts with one.

### Workflow walkthrough (what the user does)

1. User has road **A** and draws road **B** ending near A's side — or picks
   the Create Junction tool and drags B's endpoint toward A's side.
2. Within snap range of A's reference line, a snap indicator appears **on**
   A at the projected station `s`, and the status bar explains what release
   will do.
3. Release: A is split around `s`, a junction appears joining A's two
   halves and B, connecting roads for **all legal turns** are generated.
   ONE undo step returns everything to the pre-release state, byte-identical
   on disk.
4. Esc at any point before release cancels cleanly.

## Kernel: `attach_t_junction`

```
struct TAttachOptions {
  double gap_m = 0.0;             // 0 = auto from road widths
  JunctionGenOptions generation;  // the M2 generator knobs
};

std::unique_ptr<Command>
attach_t_junction(const RoadNetwork& network, RoadEnd end,
                  RoadId target, double s,
                  const TAttachOptions& options = {});
```

### Composition, not new machinery

The command is a **composite of four existing factories**, applied in
order and reverted in reverse:

1. `split_road(target, s + gap)` — head keeps `[0, s+gap)`, road **B'**
   gets the rest.
2. `split_road(target, s − gap)` — head keeps `[0, s−gap)`, a **middle
   stub** `[s−gap, s+gap)` appears.
3. `delete_road(middle stub)` — its referential closure frees the facing
   link slots on both neighbors (and, after the sprint's #89 fix, junction
   arms would be pruned too if any existed).
4. `create_junction({head:End, B':Start, end}, options.generation)` — the
   unmodified M2 generator.

Why the double split: a single split leaves the two facing ends
**coincident**, and the junction generator (correctly) plans connecting
roads across the *gap between* arm ends — a zero gap means zero-length
through-turns. The removed middle stub IS the junction area.

Later children are built **during the first apply** (the tail/stub ids are
assigned by the arena at apply time); redo re-applies the captured
children, so ids resurrect identically (the M2 restore-in-place contract).
Any mid-chain failure reverts the already-applied prefix — a failed apply
leaves the network untouched, per the command-layer invariant.

### Gap auto-sizing

`gap_m = 0` derives the gap from the geometry it must clear:

```
gap = max(half_width(target at s), half_width(end road at its end)) + 1.0 m
```

with `half_width` = the wider side's total lane width. Rationale: the
junction area must at least span the crossing road's body; the +1 m keeps
the arm ends out of the blended surface's steepest region. An explicit
`gap_m` wins (the editor may expose it later; the tool does not in this
sprint).

### Factory-time validation (fast, user-facing errors)

- `target`/`end.road` resolve, and `end.road != target` (self-attach is a
  loop, not a T).
- `end`'s link slot is free (same rule as `create_junction`).
- Neither road is a junction's *connecting* road. Junction-**linked** ends
  are fine: the sprint lifts the M2 "no split near junctions" restriction —
  `split_road` now remaps a successor-side junction's arm, connection table,
  and connecting-road links onto the new tail piece, so the same main road
  can be teed repeatedly (the second thing anyone draws after the first T).
- `s − gap > tol` and `s + gap < length − tol`: attach point too close to
  an end → the user wanted a normal endpoint junction; the error says so.
- paramPoly3 interiors: both splits inherit the M2 restriction; the error
  propagates verbatim.

### Turn policy (ASAM 1.9.0 §12.2–12.4, local reference)

The generator emits **all legal turns** — one `<connection>` per
(incomingRoad, connectingRoad) pair
(`asam.net:xodr:1.8.0:junctions.connection.one_link_to_incoming`),
connecting roads never incoming
(`asam.net:xodr:1.4.0:junctions.connection.connect_road_no_incoming_road`),
`<laneLink>` always written (omission is deprecated by §12.4). T-junctions
in the real world have **asymmetric permissions** (no-left-turn, one-way
arms); the default policy is deliberately *generate everything legal, let
the user prune later* — pruning UI is out of sprint scope and recorded in
the M3a+ backlog. The junction is `type="default"`; the recorded arm list
(`rm:arms`) makes it regenerate like any M2 junction when an incoming road
is edited.

## Editor UX

- **Create Junction tool** (extended, not forked): dragging a road
  endpoint within snap range of another road's *side* projects the cursor
  onto that road's reference line (the picking layer already computes
  (s, t) for hover) and shows a snap indicator at the projected `s`;
  release pushes `attach_t_junction` as ONE undo entry. Endpoint-to-
  endpoint behavior is unchanged and takes precedence inside its own snap
  radius.
- **Create Road tool**: a road may *start* (or end) on another road's
  side; on commit the editor groups `create_road` + `attach_t_junction`
  into one undo macro — same semantics, same single Ctrl+Z.
- **Esc** cancels at every stage (already the tool contract; the T path
  adds no new state that survives Esc).

## Tests

- Kernel: reject cases (near ends, junction road, occupied slot,
  self-attach); apply→revert byte-identical `write_xodr`; double-attach on
  the same road (two junctions); attach onto a two-record road across the
  record joint; regeneration after dragging the attached road's far end.
- Round-trip: build a T, save, reload, compare within tolerance; undo
  returns the exact pre-split file.
- Editor (tool-controller level): side-snap engages/disengages by radius,
  release = one undo entry, Esc mid-drag leaves the document untouched.
- GW-1 step 2 is the by-hand acceptance for the whole path.
