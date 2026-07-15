# 02 — Bridge generator

*Carrying one road over another, as solids the kernel can represent and the
standard can record.*

## 1. v1 scope

Locked to the seed's approved default (overview decision 2):

**Deck with thickness · abutments at each bank · piers where the span needs them
· guardrails both sides · one guardrail style.**

Not in v1: tunnels, cable-stayed/arch/truss types, bridge decks that carry
anything but the road that generated them, pedestrian bridges, expansion joints,
bearings, drainage, or any structural analysis. A RoadMaker bridge is a *visual
and semantic* structure — it looks right and it validates, it does not claim to
stand up.

## 2. Data model: what is standard, what is generated

Two distinct things, and conflating them is the trap:

| Thing | Where it lives | Why |
|---|---|---|
| **The span** — that road R is a bridge from s to s+length, of a given type | `<bridge>` in the `.xodr` | ASAM models it; consumers (esmini, CARLA) read it |
| **The solids** — deck, abutments, piers, guardrail meshes | Generated at load, **not serialized** | OpenDRIVE has no vocabulary for them; storing meshes in a road file would be inventing one |
| **The surface material** | `<userData>` on the `<bridge>` | Per `01` §3 — no standard carrier exists |

This follows the seed exactly: "`<bridge>` records the span, the solids are
Manifold geometry not necessarily serialized to xodr". The generator is
**deterministic** — same `<bridge>` record + same road geometry + same
parameters → same solids — so nothing is lost by not storing them, and a
round-trip stays byte-identical because there is nothing extra to write.

### `<bridge>` representation

ASAM OpenDRIVE 1.9.0 §13.12, `t_road_objects_bridge`, `<bridge>` within
`<objects>` (multiplicity 0..*):

```xml
<road id="2" length="120.0">
  <objects>
    <bridge s="50.0" length="24.0" id="1" name="overpass" type="concrete">
      <!-- surface material; no standard carrier (01 §3) -->
      <userData code="rm:material.bridge_deck" value="rm:concrete"/>
    </bridge>
  </objects>
</road>
```

- `s` and `length` (required, `t_grEqZero`) — the span in the carrying road's
  s-coordinate.
- `type` (required, `e_bridgeType`: `concrete` / `steel` / `wood` / `brick`).
  v1 authors `concrete`; the others parse and round-trip but the generator's
  solids do not vary by type (a `steel` bridge gets concrete-shaped geometry —
  documented, not hidden).
- **"Bridges are valid for the complete cross section of a road"** unless a
  `<laneValidity>` child narrows them
  (`asam.net:xodr:1.7.0:road.object.bridges.define_type`). v1 always spans the
  full cross-section and writes no `<laneValidity>`; the parser accepts and
  preserves one on a foreign file.

Parse/represent/write/validate lands in the kernel **before** any generator work
(product-parity rule: nothing renders that the kernel cannot represent first).

## 3. Manifold — first consumer, so it opens with a spike

Manifold 3.5.2 (Apache-2.0) is **pinned in `cmake/deps.cmake:85` and linked into
the kernel target, with zero call sites anywhere in the repo**. The licence and
build cost are already paid; the API integration is entirely greenfield. That is
risk 4, and WS-3 opens by retiring it:

**Spike (first PR of WS-3, before any UX):** extrude one deck solid from a road
span, boolean it against nothing, get a triangle mesh out, render it. Answers
three questions:

1. **Does `MANIFOLD_CROSS_SECTION OFF` block us?** It is off because "we use
   Clipper2 directly for plan-view ops". Extruding a deck cross-section along a
   curved reference line is exactly what `CrossSection` is for. If the spike
   wants it, flipping the flag is a `deps.cmake` edit — no new dependency, no
   new licence review. If Clipper2 + a hand-rolled sweep is cleaner, stay as is.
2. **Does `MANIFOLD_PAR OFF` hurt?** Single-threaded boolean on a
   deck-sized mesh should be milliseconds. Measure before caring.
3. **What is the mesh handoff?** Manifold's output must become a
   `RoadMaker` mesh (positions/normals/uvs) with planar UVs so `01`'s materials
   apply. This is the real integration surface.

The spike's result is recorded as an As-built note in this document.

### Why Manifold at all

The deck is a swept solid; abutments and piers are boxes that must **union**
cleanly with it and **subtract** the clearance volume under the deck. Robust
booleans on generated geometry is precisely Manifold's job, and hand-rolling CSG
is how you get non-manifold meshes that break the exporters. It is already
approved and linked — this is the consumer it was pinned for.

## 4. Generation

### Inputs

| Parameter | Default | Notes |
|---|---|---|
| Deck depth | 0.8 m | Visible thickness — GS-4 requires under-deck clearance legible from the fixed camera |
| Deck overhang | 0.5 m each side | Deck extends past the outermost lane edge |
| Pier spacing rule | none if span ≤ 30 m; else piers every ≤ 25 m, evenly divided | A rule, not a count, so a longer span does not need re-authoring |
| Pier section | 1.2 m square | One style |
| Abutment | at each end of the span, to skirt level | Meets the ground |
| Guardrail style | one profile, both sides | Decision 2 — one style in v1 |
| Guardrail height | 1.0 m | |

Parameters live on the generator command, not in the `.xodr` — regenerating with
different parameters is a new command, and the `<bridge>` record is unchanged by
them.

### The grade-separation query

A kernel query, not an editor heuristic:

> Two roads **grade-separate** where their reference lines cross in plan view
> (Clipper2 already does plan-view intersection) and their elevations at the
> crossing differ by more than a clearance threshold (default 3.0 m), with
> **no junction** connecting them.

That last clause is what distinguishes an overpass from an intersection, and it
is why this is a query on the network rather than on geometry alone. The result
is `{upper_road, lower_road, s_upper, s_lower, clearance}` — everything the
generator and the auto-offer UX need.

### Auto-offer UX

Per the seed: *auto-offered on detected grade separation, plus manual.*

- **Detection** runs on the same `DirtySet` channel as other post-edit work,
  when a road is created or its elevation changes. It never generates anything
  on its own.
- **The offer is a toast with an action**: *"These roads cross without a
  junction. **Generate bridge structure?**"* — dismissible, non-modal, and it
  does not re-fire for a crossing the user has dismissed (a dismissal is
  remembered per road-pair for the session). An editor that silently built a
  bridge because two roads crossed would be worse than one that never offered.
- **Manual path**: select a road span → context menu → **Generate bridge
  structure**. Same command, same parameters, no detection required. This is the
  path that works when detection is wrong.
- Generation is **one `edit::Command`, one undo step**, per the M2 invariants.

### Regeneration on elevation edits

A bridge whose road's elevation changes must follow, or the deck floats. The
`<bridge>` record is s-anchored so it survives; the *solids* are regenerated
from the current geometry on the `DirtySet` re-mesh channel — the same way prop
instances rebuild without re-tessellating road surfaces. Because solids are
derived, this is a re-derive, not a mutation: no command, no undo entry.

If an elevation edit **destroys** the grade separation (clearance drops below
threshold), the bridge does not silently vanish — the `<bridge>` record stays,
the solids regenerate at the new (wrong-looking) elevation, and a **validator
warning** cites the clearance. Deleting a bridge is the user's call.

## 5. Degenerate cases

Each gets a strategy and a fixture in WS-3. None may be handled by "it looks
fine on the happy path".

| Case | Strategy | Fixture |
|---|---|---|
| **Skew crossing** (roads cross well off 90°) | Deck spans the *projected* width of the lower road plus clearance, so a shallow skew makes a longer deck, not a clipped one. Abutments sit at the skewed span ends. Below ~20° skew, refuse with a diagnostic — the deck degenerates into a ribbon. | 45° and 20° crossings |
| **Curved deck** (carrying road curves over the span) | Sweep the cross-section along the reference line rather than extruding a prism; piers stay perpendicular to the local tangent. | Bridge on an arc and on a spiral |
| **Very short span** (< ~2× deck depth) | Refuse, diagnostic: the structure would be thicker than it is long. Not a crash, not a sliver solid. | 1 m span |
| **Superelevated deck** | Cross-section inherits the road's superelevation; guardrails follow. | Banked bridge |
| **Span crosses a lane-section boundary** | Allowed; the cross-section is sampled per station, so a width change mid-span is a taper, not a discontinuity. | Bridge over a widening |
| **Two bridges overlapping on one road** | Allowed by the standard (`<bridge>` is 0..*); solids union. Overlapping identical spans → warning. | Two adjacent spans |
| **Bridge over nothing** (manual generation with no lower road) | Allowed — a viaduct over the skirt. Abutments to skirt level, no clearance check. | Standalone span |

## 6. Tests

- **Kernel**: `<bridge>` round-trip byte-identical, including a foreign
  `<laneValidity>` child and unmodeled attributes (`RawXml`); missing required
  `length`/`type` → diagnostic citing the rule id; `e_bridgeType` outside the
  enum preserves its original spelling (the `RawXml` value mechanism).
- **Query**: grade-separation detection true-positives (GS-4's crossing),
  true-negatives (a junction crossing, a crossing below clearance).
- **Generator**: determinism (same inputs → byte-identical mesh); manifoldness of
  the output solid; every degenerate case above.
- **Command**: apply → revert leaves `write_xodr()` byte-identical; failed apply
  (a refused short span) leaves the network untouched.
- **Render**: GS-4's under-deck clearance visible from the fixed camera —
  WS-5's baseline.
- **esmini**: GS-4 loads clean with its `<bridge>` records, through the existing
  `esmini-roundtrip` job (it globs `assets/samples/*.xodr`, so GS-4 is covered
  the moment it lands there).
