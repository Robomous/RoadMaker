# Lane

*Edit a road's cross-section — add, remove, and retype lanes, and set each
lane's travel direction.*

![The Lane tool with a road selected, showing the click-select and Delete /
Shift+D gestures](img/lane.png)

## Steps

1. Activate the **Lane** tool (`L`). The road's lanes are highlighted in the
   viewport.
2. **Click a lane** to select it. A road-level selection is enough to add or
   remove lanes — you don't have to pick a lane first.
3. **Add a lane** on the left or right with the **Add left** / **Add right**
   buttons in the Properties panel; the new lane takes a default width.
4. **Remove a lane** with **Remove left lane** / **Remove right lane**, or press
   **Delete** with a lane selected. Each removes the *outermost* lane on that
   side (the OpenDRIVE numbering stays contiguous), so you can peel off a
   shoulder or sidewalk without first selecting it. A button greys out when only
   the driving lane remains on that side, and Delete on a centre or interior
   lane reports why instead of silently doing nothing. You can also
   **right-click a lane in the viewport → Remove this lane** — the same rule
   applies.
5. **Retype** a lane (driving, sidewalk, shoulder, parking, …) from the
   Properties panel after selecting it.
6. **Set the travel direction** of the selected lane with **Shift+D**, which
   cycles *standard → reversed → both → standard* (OpenDRIVE `@direction`,
   §11). Standard writes nothing; reversed and both are emitted explicitly.

Each change is one undoable command that keeps the lane section valid — widths
stay defined for the whole section and lane links to neighbouring sections and
junctions are updated. Removing a lane and undoing restores the very same lane
(id and all).

## Notes

- Lane `0` is the width-less centre line; it carries the centre road marking,
  not traffic. It has no travel direction.
- The outer boundary of each lane is where its road marking is painted — set
  the marking type and colour (including true double-yellow) in the Properties
  panel; see [Objects & signals](objects-signals.md) for painted markings that
  are objects rather than lane marks.
- To shape a lane's width *along* the road (a taper rather than a constant
  width), use the [Lane Width](lane-width.md) tab of the 2D Editor. To add a new
  lane over just part of a road, see [Lane Add](lane-add.md),
  [Lane Form](lane-form.md), and [Lane Carve](lane-carve.md).

## Reference

[M2 editing tools §4 (Lane Profile)](../design/m2/02_editing_tools.md) and the
[P2 discovery report](../roadmap/pillars/p2_discovery.md) for the lane-editing
tools added in the P2 pillar.
