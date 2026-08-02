# Bridges

When a road runs over another without a junction between them, it is an
**overpass** — and RoadMaker can turn that raised span into a real bridge
structure: a deck with piers, abutments and guardrails.

## Building a bridge

1. Give the carrying road some elevation so it clears the road beneath it — open
   its [elevation profile](elevation.md) and raise the span over the crossing.
   Roughly 3 m of clearance or more reads as an overpass.
2. Choose **Edit ▸ Bridge ▸ Generate Bridge Structures**. RoadMaker finds every
   place two roads cross without a junction and enough vertical gap, and builds a
   bridge deck over each. A toast reports how many it built.

You can also let the editor nudge you: when a crossing appears that no bridge
covers, a hint points you at the menu.

## What gets built

Each bridge is a single watertight solid:

- a **deck** with visible thickness, swept along the road so it follows curves
  and superelevation and tapers with a widening cross-section;
- **guardrails** down both edges;
- **piers** underneath — none for a short span, then one every so often as the
  span grows;
- **abutments** at each end, reaching down toward the ground.

The deck sits at the road surface and rides with it: change the road's elevation
and the bridge follows, with no extra undo step.

## When a road moves

Only the *span* is saved — where along the carrying road the bridge starts and
how long it is — and that says nothing about which crossing it was built for. So
when you move either road, RoadMaker works out where the crossing went and slides
the span along to stay over it. The deck keeps its length, its material and its
name; it just ends up in the right place.

If the crossing has gone altogether — you moved the roads apart, or turned the
overpass into a junction — the span is left exactly where you authored it and the
editor tells you it no longer spans anything. **Nothing is deleted**: a move
should never throw away something you made. When you do want the leftovers gone,
choose **Edit ▸ Bridge ▸ Remove Orphaned Spans**, which clears every bridge with
no crossing under it in one undoable step.

Re-running **Generate Bridge Structures** is always safe: it skips crossings a
deck already carries rather than stacking a second one beside it.

## What is saved

Only the **span** — that road *R* is a bridge from *s* for a given length — is
written to the `.xodr`, as a standard `<bridge>` element (ASAM OpenDRIVE §13.12).
The deck, piers, abutments and guardrails are **generated on load**, not stored:
OpenDRIVE has no way to record them, and because the generator is deterministic
nothing is lost. This keeps the file a clean, standards-valid document that other
tools (esmini, CARLA) can read, and it round-trips byte-identically.

Bridge solids are included when you export the scene to glTF or USD.

## Coming next

This first version builds bridges from the menu with a fixed deck style. A
one-click *"Generate bridge structure?"* prompt on detection, an interactive
handle to widen or narrow the bridged extent directly (a bridge cannot yet be
selected — clicking one picks the road beneath), and custom deck profiles are
planned follow-ups.
