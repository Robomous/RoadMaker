# Road markings: stencils and curves

*Place arrow stencils on a lane and draw free-form line markings and crossings
along a road — the two painted-marking tools that consume Library marking and
crosswalk assets.*

RoadMaker paints road markings as OpenDRIVE `<object>`s. Two tools author them
interactively; both consume an asset selected in the [Library](library.md), and
each placement is one undoable command.

## Marking Point — arrow stencils

*Place and adjust a painted arrow stencil on a lane.*

1. Pick an arrow from the Library's **Stencils** category (the six core glyphs:
   straight, left, right, left-right, straight-left, straight-right).
2. Activate the **Marking Point** tool (**S**).
3. Click a lane. The stencil drops centred on the lane, sized to the lane width
   (the asset's *width fraction* × the lane width) and oriented along the lane's
   travel direction.
4. To adjust a placed stencil, drag it: it slides along and across the lane,
   re-orienting to the travel direction, and snaps back within the lane band.
   Releasing the drag is one undoable move.

You can also drag an arrow straight from the Library onto a lane — the drop
lands exactly where the ghost previews it.

## Marking Curve — free-form lines and crossings

*Draw a painted line or striped crossing that follows a curve you trace along a
road.*

1. Pick a **marking** asset (a lane-line style — solid, broken, double) **or** a
   **crosswalk** asset (a striped band) in the Library.
2. Activate the **Marking Curve** tool (**Shift+W**).
3. Click to place points along the road. The first click anchors the curve to
   the road under it; a live preview shows the fitted curve as you go.
4. Press **Enter** or double-click to commit. **Backspace** removes the last
   point; **Esc** cancels. Nothing enters the network until you commit.

A marking asset paints a solid or dashed line of the asset's width along the
curve; a crosswalk asset paints a striped band — this is the curved crossing of
[GW-5 step 6](../roadmap/golden_workflows/gw5_crosswalk_assets.md).

All points of one curve must lie on the anchor road; a click that leaves it is
rejected with a status-bar message.

### Tight curves

The band is the centreline offset by half its width to each side. If the curve
bends tighter than that half-width, the two offset edges would cross and the
band would fold onto itself, so the commit is refused with a message. Draw a
gentler curve, add intermediate points, or reduce the marking width.

## Notes

- Both tools place OpenDRIVE `<object>`s and round-trip through `.xodr`
  (`rm:stencil` and `rm:markingCurve` `<userData>` carry the authoring
  parameters; the interop outline + `<markings>` let foreign viewers draw them
  too).
- For crosswalks and stop lines placed automatically across a junction
  approach, use the [Crosswalk & Stop Line](objects-signals.md) tool instead.
- Selecting a placed marking shows its width and dash fields in the
  [Properties panel](attributes.md), each editable as an undoable command.
- **Per-instance material override:** drop a material from the Library onto a
  selected marking's **Material** slot in the Attributes pane to repaint *that
  instance only*. The override is pinned — changing the asset's Default Material
  later updates every other instance but leaves the overridden one alone.

## Reference

[M3a road marks](../design/m3a/02_road_marks.md) and the
[GW-5 crosswalk-assets workflow](../roadmap/golden_workflows/gw5_crosswalk_assets.md).
