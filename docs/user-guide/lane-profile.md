# Lane Profile

*Change a road's cross-section — add and remove lanes, and set each lane's
type.*

## Steps

1. Select a road, then activate the **Lane Profile** tool. The road's lanes are
   highlighted in the viewport.
2. **Add a lane** on the left or right with the **Add left** / **Add right**
   buttons; the new lane takes a default width. A road-level selection is
   enough — you don't have to pick a lane first.
3. **Remove a lane** with **Remove left lane** / **Remove right lane**. Each
   button removes the *outermost* lane on that side (the OpenDRIVE numbering
   stays contiguous), so you can peel off a shoulder or sidewalk without first
   selecting it. A button greys out when only the driving lane remains on that
   side ("Only the driving lane remains") or when no road is selected. You can
   also **right-click a lane in the viewport → Remove this lane** — the same
   rule applies, and the road's own menu items stay available alongside it.
4. **Retype** a lane (driving, sidewalk, shoulder, parking, …) from the
   properties panel after selecting it.

Each change is one undoable command that keeps the lane section valid — widths
stay defined for the whole section and lane links to neighbouring sections and
junctions are updated. Removing a lane and undoing restores the very same lane
(id and all).

## Notes

- Lane `0` is the width-less centre line; it carries the centre road marking,
  not traffic.
- The outer boundary of each lane is where its road marking is painted — set
  the marking type and colour (including true double-yellow) in the properties
  panel; see [Objects & signals](objects-signals.md) for painted markings
  that are objects rather than lane marks.

## Reference

[M2 editing tools §4 (Lane Profile)](../design/m2/02_editing_tools.md).
