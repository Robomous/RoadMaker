# Bend Points

*Add a new node to a road exactly where you click — without changing the road's
shape — then bend it there.*

## Steps

1. With the **Select** tool, **double-click a road body** where you want the
   bend. A node is inserted on the curve at that point (one undoable command),
   and the editor switches to **Edit Nodes** with the new node grabbed.
2. **Drag** to bend the road at the new node, or release to leave it where it
   is. The insert and the bend are separate undo steps, so
   <kbd>Ctrl</kbd>+<kbd>Z</kbd> once undoes the bend and again removes the node.
3. You can also double-click a road body while the **Edit Nodes** tool is
   already active, or click a segment's **midpoint marker** — both insert the
   same shape-preserving node.

Press <kbd>Esc</kbd> after the insert to keep the committed node but cancel the
grab.

## Shape is preserved

Inserting a node does **not** reshape the road. The node is placed on the
current curve and the heading is pinned at every node, so every part of the
road away from the new node is reproduced exactly (straight, arc, and spiral
segments within tolerance). Only then does dragging the node change the shape.

> Imported (foreign) roads with a paramPoly3 segment are re-fitted as clothoids
> the first time you edit them — a small one-time shape change, announced in the
> status bar.

## Limits

- A new node must be at least **2 m** from any existing node. Closer than that
  and the insert is refused:

  > a node already exists within 2 m of this point

## Reference

[Edit Nodes](edit-nodes.md) — the `edit::insert_node_at` kernel API and the
heading-pinning re-fit are covered in the M3a topology-editing design notes
(`docs/design/m3a/06_topology_editing.md`).
