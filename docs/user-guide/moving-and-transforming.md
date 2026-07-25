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

### Per-entity behavior

- **Roads** — X/Y translate the whole road; the yaw ring rotates it about the
  selection pivot; the Z arrow applies a uniform elevation offset. A road that
  participates in a junction can't be transformed (its pose is generated) — the
  drop is refused with a toast; move the junction's free end nodes instead.
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

## See also

- [Bend points](bend-points.md) and [Edit nodes](edit-nodes.md) — reshaping a
  road's geometry rather than moving it whole.
- [Elevation](elevation.md) — editing a road's vertical profile node by node.
- [Context menus](context-menus.md) — the right-click actions per entity.
