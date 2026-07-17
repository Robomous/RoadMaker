# Scene tree

*The list of everything in your network — roads, lanes, and junctions — kept in
step with the viewport selection.*

## What it shows

The **Scene** panel lists every object in the current network: each road, the
lanes inside it, and each junction. It is the structural view of the same data
the viewport draws, so it is the fastest way to find an object you cannot easily
click — a lane buried under a junction, or a road off-screen.

## Selecting from the tree

- **Click a row** to select that object. The viewport highlights it and the
  **Properties** panel fills with its fields.
- Selection is shared both ways: pick a road in the viewport and its row scrolls
  into view and highlights here. There is one selection, shown in two places.
- Selecting in the tree is view state, not an edit, so it is **not undoable**
  (the same rule as clicking in the viewport).

## Working from here

Once a row is selected, every tool and command applies to it: edit its fields in
[Properties](attributes.md), reshape it with [Edit Nodes](edit-nodes.md), or act
on it through the [right-click menus](context-menus.md). The tree updates live as
you add, split, merge, or delete objects.

## Reference

The tree is backed by a `QAbstractItemModel` over the road network; the model
and its update rules are covered in the editor architecture notes
(`docs/architecture/editor.md`).
