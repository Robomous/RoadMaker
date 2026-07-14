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
  it on release (e.g. *"Rotated road 4 by 30°"*). Each completed drag is **one
  undo step**.
- Rotation snaps to **15° detents**; hold <kbd>Shift</kbd> while dragging the
  ring for a free angle.
- <kbd>Esc</kbd> cancels a drag in progress, leaving the network untouched.

### Per-entity behavior

- **Roads** — X/Y translate the whole road; the yaw ring rotates it about the
  selection pivot; the Z arrow applies a uniform elevation offset. A road that
  participates in a junction can't be transformed (its pose is generated) — the
  drop is refused with a toast; move the junction's free end nodes instead.
- **Props / signals** — X/Y translate (re-projected onto the owning road) and
  the yaw ring rotates the instance. Props don't expose a Z arrow yet.

Pitch/roll rings and prop elevation are planned follow-ups.

## See also

- [Bend points](bend-points.md) and [Edit nodes](edit-nodes.md) — reshaping a
  road's geometry rather than moving it whole.
- [Elevation](elevation.md) — editing a road's vertical profile node by node.
- [Context menus](context-menus.md) — the right-click actions per entity.
