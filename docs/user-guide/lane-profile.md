# Lane Profile

*Change a road's cross-section — add and remove lanes, and set each lane's
type.*

## Steps

1. Select a road, then activate the **Lane Profile** tool. The road's lanes are
   highlighted in the viewport.
2. **Add a lane** to the left or right of the current selection with the
   toolbar buttons; the new lane takes a default width.
3. **Remove** the outermost lane on a side.
4. **Retype** a lane (driving, sidewalk, shoulder, parking, …) from the
   properties panel.

Each change is one undoable command that keeps the lane section valid — widths
stay defined for the whole section and lane links to neighbouring sections and
junctions are updated.

## Notes

- Lane `0` is the width-less centre line; it carries the centre road marking,
  not traffic.
- The outer boundary of each lane is where its road marking is painted — set
  the marking type and colour (including true double-yellow) in the properties
  panel; see [Objects & signals](objects-signals.md) for painted markings
  that are objects rather than lane marks.

## Reference

[M2 editing tools §4 (Lane Profile)](../design/m2/02_editing_tools.md).
