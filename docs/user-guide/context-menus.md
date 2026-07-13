# Right-Click Menus

*A right-click gives you the actions that make sense for whatever is under the
cursor — the fastest way to reach the topology operations.*

## Using them

**Right-click and release without dragging** to open a menu. A right-click that
*drags* orbits the camera as usual, so a quick click is a menu and a drag is a
look-around — there is no mode to switch.

What the menu offers depends on what you clicked:

| You right-clicked… | Menu |
|---|---|
| a **road body** | Remove this lane _(when a lane is under the cursor)_ · Insert bend point here · Split road here · Merge selected roads · Edit lane profile · Edit elevation profile · Frame · Delete road |
| a **node handle** (on a selected road) | Split at this node · Delete node · Frame |
| **empty space** | Create road here · Paste _(coming soon)_ · Frame all |
| a **junction** (from the scene tree) | Frame · Delete junction |

- **Remove this lane** appears when you right-click on a specific lane; it is
  enabled only for the outermost lane of a side (the OpenDRIVE numbering must
  stay contiguous), and the road's own items stay reachable beneath it.
- **Merge selected roads** is enabled only when exactly two roads are selected
  and they can actually merge ([Merge & Split](merge-split.md)).
- **Split at this node** is disabled on a road's end nodes (there is nothing to
  split off).
- **Edit lane / elevation profile** selects the road and switches to that tool.

## Notes

Every menu action goes through the command layer, so **Undo/Redo** reverses it.
The menu is built from a single description shared by the viewport and (in
future) the scene tree and guided tour, so the same right-click means the same
thing everywhere.

## Reference

[M3a topology editing](../design/m3a/06_topology_editing.md) — the
`context_menu` builder and the per-context item matrix.
