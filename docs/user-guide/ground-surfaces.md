# Ground Surfaces

*Reshape the ground that fills a block of roads — drag its boundary nodes and
tangent handles instead of accepting the shape the roads imply.*

## What a ground surface is

When a loop of roads encloses an area, RoadMaker fills it with a **ground
surface**: a watertight mesh stitched to the surrounding road edges, with its
elevation solved so the ground meets every kerb without a step. You do not
create one — it appears with the loop and follows the roads as you edit them.

That automatic shape is called a **derived** boundary. It is exactly the block
the roads surround, and it tracks them: move a road and the ground follows.

## Reshaping a boundary

1. Click the ground surface to select it, then activate the **Surface** tool
   (**U**). Its boundary appears as a ring of node handles, each with two
   tangent handles on a whisker.
2. **Drag a node** to move that corner of the boundary.
3. **Drag a tangent tip** to curve the boundary through the node. The tangent's
   *length* controls how far the edge bulges, its *direction* which way.
4. **Click a midpoint marker** — the small dot between two nodes — to insert a
   node there.
5. Click a node, then press **Delete** or **Backspace**, to remove it. A
   boundary needs at least three nodes, so the last three cannot be deleted.
6. **Esc** cancels a drag in progress.

Each gesture is one undo step, and the whole drag commits once on release.

## Derived and authored boundaries

**The first edit detaches the surface.** A derived boundary is a function of the
roads; once you move a node it no longer is, so the surface switches to an
**authored** boundary and stops being re-derived. The Attributes pane shows
which state it is in, and the roads it came from are kept as *provenance* — they
still supply the elevation the ground is pinned to.

To go back, use **Revert to derived**, on the Attributes pane or in the
surface's right-click menu. The boundary returns to whatever the roads currently
enclose — which is not necessarily the shape you started from, if the roads have
moved since. Reverting discards the authored nodes, and is itself undoable.

If every road of the loop is deleted, an authored surface survives (its boundary
is its own data) while a derived one disappears with the loop.

## Notes

- Boundaries are edited in plan view only. Elevation still comes from the
  surrounding roads, so a boundary dragged away from them stays pinned to the
  nearest road edge until the height field lands.
- A boundary that crosses itself is refused — the edit is rejected with a status
  message and the last good shape stays on screen.
- Authored boundaries are saved into the `.xodr` and reloaded exactly; a file
  with no authored surfaces is written byte-for-byte as before.
- To change what the ground is made of rather than its shape, see
  [Materials](materials.md).

## Reference

[P5 discovery report](../roadmap/pillars/p5_discovery.md) and
[GW-2 step 6](../roadmap/golden_workflows/gw2_simple_scene.md).
