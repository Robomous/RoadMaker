# Moving and transforming

*Move and rotate roads and props with the **Move** tool and its 3D transform
gizmo, or drag a road body with the Select tool as a power path.*

## The Move tool

Activate **Move** from the toolbar (the 4-arrow icon) or press <kbd>M</kbd>.
Hovering a movable entity — a road or a placed prop — shows the 4-arrow cursor.

- **Drag the body** of a road or prop to move it freely in plan view. Roads
  translate through the same command as before; a prop re-projects onto its
  owning road as you drag. Release to place — exactly one undoable command
  (<kbd>Ctrl</kbd>+<kbd>Z</kbd> restores it byte-for-byte). <kbd>Esc</kbd>
  mid-drag cancels.
- **Click** an entity to select it and show the transform gizmo.
- Right-click a road or prop and choose **Move** to arm the tool with that
  entity selected.

The **Select** tool (the default, <kbd>V</kbd>) keeps drag-to-move on road
bodies as a power path, and still rubber-band-selects from empty space.

## The transform gizmo

With the Move tool active and a single entity selected, the classic 3D gizmo
appears at the entity's pivot, always on top at a constant on-screen size:

| Handle | Drag does |
|---|---|
| **X arrow** (red) | Translate along world X |
| **Y arrow** (green) | Translate along world Y |
| **Z arrow** (blue) | Raise/lower — *roads only* (uniform elevation offset) |
| **Centre pad** | Free translate in the XY plane |
| **Yaw ring** | Rotate about Z (the vertical axis) |

- Grab a handle and drag; the change **previews live** and a toast summarizes
  it on release (e.g. *"Rotated prop to 30°"*). Each completed drag is **one
  undo step**.
- Rotation snaps to **15° detents**; hold <kbd>Shift</kbd> while dragging the
  ring for a free angle.
- For a prop or a sign the detents are **absolute**: the angle lands on an exact
  multiple of 15° **measured from the road**, not 15° from wherever the drag
  started. Rotating a prop that sits at some odd angle therefore tidies it up —
  turn a bench toward the kerb and it ends up genuinely perpendicular. Roads
  snap the amount they are turned *by* instead, since a curved road has no one
  heading to measure against.
- <kbd>Esc</kbd> cancels a drag in progress, leaving the network untouched.
- Moving or rotating a road that is **linked** to a road staying put no longer
  breaks that connection: the neighbouring road's end is reshaped to stay joined.
  Nothing asks first, because there is nothing to ask about. On the rare
  occasion the neighbour genuinely cannot follow, the connection is cut and the
  editor says which one and why, after the fact.
- What the roads *imply* moves too. A [ground surface](ground-surfaces.md)
  appears, changes shape or disappears as the block its roads enclose does, and a
  [bridge](bridges.md) slides along to stay over the crossing it was built for.
  The two things a move will not do quietly are re-draw a boundary you reshaped
  by hand, and delete a bridge whose crossing has gone — both are reported and
  left as you made them.

### Per-entity behavior

- **Roads** — X/Y translate the whole road; the yaw ring rotates it about the
  selection pivot; the Z arrow applies a uniform elevation offset. With several
  roads selected, the pad and the ring move **all of them** together, about the
  pivot where the gizmo is drawn.
- **Junction arms** — move and rotate freely, and the junction rebuilds itself
  around their new positions, exactly as dragging a shape node has always done.
  Select **every** arm and the junction travels as one rigid piece instead: its
  turns are carried across untouched rather than recalculated, so hand-shaped
  paths, corner radii and stop lines survive the move exactly as they were.
  If a move would put an arm so far out that the junction can no longer be
  rebuilt, the whole gesture is refused and nothing moves — the editor names the
  junction, and the drag simply stops following rather than leaving a wrecked
  junction behind.
- **Junction connecting roads** — the short generated roads *inside* a junction
  can't be moved or rotated: their shape is computed from the arms, so the next
  rebuild would discard anything you dragged them to. Grabbing an arrow, the pad
  or the ring is refused on the spot, with a toast naming the road and the
  junction. Move the arms instead.
- **Props** — X/Y translate (re-projected onto the owning road) and the yaw ring
  rotates the instance. No Z arrow yet. On a **prop span**, the ring turns every
  instance in the series by the same angle relative to the road — the heading is
  an offset the whole series shares.
- **Signs and signals** — the same translate and ring, on the sign itself rather
  than the road it stands beside. The ring edits the **Heading offset**, which
  makes it an override exactly like typing in the Attributes pane:
  [auto-orientation](objects-signals.md) never recomputes it afterwards, and
  **Auto facing** is the only way back to the derived angle. Which traffic the
  sign applies to is left alone — turning a sign never changes what it governs.

Pitch/roll rings and prop elevation are planned follow-ups.

## When a move puts a prop in the way

Props ride the road that owns them. A tree placed beside a road keeps its
position *relative to that road*, so moving the road takes the tree with it —
which is what you want, and which is also how a tree can end up standing in
someone else's carriageway.

Rotating is where this bites hardest. A prop sitting well out to one side sweeps
a wide **arc** when its road turns, because it swings around the pivot rather
than sliding along with the road. Half a turn can carry a prop several times
further than the road itself moves.

When a move does that, the editor says so:

> **Prop 12 is in the way — it now stands in road 3's carriageway**

Nothing is changed for you. The prop stays exactly where the move put it, the
move stays done, and undo puts everything back. Reopening the scene later will
report it again, so it is not something you can lose track of by saving.

You have three ways out:

- **Undo** the move.
- **Edit ▸ Props ▸ Relocate Obstructed Props** — shifts every flagged prop to the
  nearest clear spot on its own road, as a single undoable step. A prop with
  nowhere clear to go is left alone rather than dumped somewhere arbitrary, and
  if nothing needs moving the editor tells you that instead of adding a pointless
  undo entry.
- **Right-click the prop ▸ Re-anchor to nearest road** — keeps the prop exactly
  where it is on screen and hands it to the road it now sits beside, so *that*
  road's future moves carry it. Useful when the prop is where you want it and
  simply belongs to the wrong road. Note that re-anchoring a prop to the road it
  is standing *in* just stops the warning: a prop is never reported against its
  own road. Use Relocate if you actually want it out of the lane.

What is **not** flagged matters as much as what is. A prop is never reported
against its own road, so median trees, bollards and kerbside streetlights are
left in peace; nor against roads joined to it through a junction, so corner
poles at an intersection stay quiet; and only the **driving lanes** are tested,
so a prop on a neighbouring verge or sidewalk is fine. Crosswalks and painted
markings are never treated as obstacles. A prop that declares no size at all is
not checked — the editor will not invent dimensions for it.


## See also

- [Bend points](bend-points.md) and [Edit nodes](edit-nodes.md) — reshaping a
  road's geometry rather than moving it whole.
- [Elevation](elevation.md) — editing a road's vertical profile node by node.
- [Context menus](context-menus.md) — the right-click actions per entity.
- [Props and signals](objects-signals.md) — placing and sizing the props a move can carry.
