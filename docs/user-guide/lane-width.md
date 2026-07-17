# Lane Width

*Shape a lane's width along the road — a smooth taper rather than a single
constant value — in the 2D Editor's Lane Width tab.*

![The 2D Editor docked below the viewport with the Lane Width tab
alongside Vertical Profile](img/lane-width.png)

## Steps

1. Open the **2D Editor** dock (**Shift+L**, or *View ▸ 2D Editor*) and pick the
   **Lane Width** tab. Lane Width is a 2D Editor tab, not a viewport tool.
2. Select the lane whose width you want to shape (click it with the
   [Lane](lane-profile.md) tool). The tab draws the lane's width `w(s)` as an
   editable 2D curve for the current lane section.
3. Drag the control points to taper the width along the section. The 3D road
   follows the edited profile immediately.
4. The result is a per-section width profile — a list of cubic `w(ds) = a + b·ds
   + c·ds² + d·ds³` records with section-local `s` offsets, exactly what
   OpenDRIVE `<width>` stores.

The edit is one undoable command. Width `0` is legal — it is how a lane tapers
in or out of existence (the basis for [Lane Add](lane-add.md),
[Lane Form](lane-form.md), and [Lane Carve](lane-carve.md)).

## Notes

- A constant-width change (one value for the whole lane) is quicker from the
  [Lane](lane-profile.md) tool's Properties panel; the Lane Width tab is for
  width that *varies* along `s`.
- Setting a varying profile never discards an authored taper — the kernel
  refuses to flatten a varying width to a constant behind your back.
- Each lane section carries its own width profile; splitting a section (Merge &
  Split, or a tool that inserts a seam) partitions the profile at the cut.

## Reference

[M2 editing tools §4](../design/m2/02_editing_tools.md) and the
[P2 discovery report](../roadmap/pillars/p2_discovery.md). Width semantics and
the zero-width rule: [OpenDRIVE conventions](../domain/opendrive.md).
