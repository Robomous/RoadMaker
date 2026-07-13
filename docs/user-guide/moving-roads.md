# Moving Roads

*Translate a whole road — or several at once — in plan view by dragging its
body with the Select tool.*

## Steps

1. Activate the **Select** tool (the default). Hover a road; the cursor is the
   normal arrow.
2. **Press on the road body and drag.** The cursor becomes a move cursor, the
   road follows the pointer at 1:1, and a ghost line tethers the press point to
   the current position. If the road was not selected, pressing it selects it
   first, so the drag moves it.
3. **Release** to place the road. This is exactly one undoable command —
   <kbd>Ctrl</kbd>+<kbd>Z</kbd> puts the road back byte-for-byte.
4. To move **several roads together**, select them first (rubber-band, or
   <kbd>Shift</kbd>-click), then drag any one of their bodies. All of them
   translate by the same delta as a single undo step.

Press <kbd>Esc</kbd> mid-drag to cancel; the road snaps back to where it
started and nothing enters the undo history.

## Snapping

A single-road drag snaps its nearer endpoint to other roads' endpoints when it
comes within range — a small marker shows the engaged snap. Moving several
roads together does not snap in this version (a follow-up).

## What a move changes

A move shifts the road's plan-view geometry and its authoring waypoints only.
Headings, lengths, lane sections, elevation, and road marks are untouched — the
road is translated, never reshaped. To reshape a road, use
[Edit Nodes](edit-nodes.md).

## Links and junctions

- A link **between two roads that move together** is preserved.
- A link that **leaves the moved set** — to a road staying put — would no longer
  meet, so it is broken on both sides as part of the same move. The first time a
  move would break a link, the editor asks:

  > Moving this road will break its connection to roads that stay put. Move it
  > anyway?

  with a **Don't ask again this session** switch.

- A road that **participates in a junction** cannot be moved as a free body —
  its pose is generated from the junction's arms. The move is refused with:

  > Road *N* belongs to Junction *M* — junction roads can't be moved. Delete the
  > junction or move its free end nodes instead.

  To reposition a junction, delete it ([Junction](junction.md)) and move the
  free road ends, or move the approach roads' end nodes with
  [Edit Nodes](edit-nodes.md).

## Not yet

Rotation, moving a junction as a unit, and lateral lane-section moves are
follow-ups.

## Reference

The connectivity policy and the `edit::translate_roads` / `translate_road`
kernel API are documented in the M3a topology-editing design notes
(`docs/design/m3a/06_topology_editing.md`).
