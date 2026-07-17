# Working with road styles

*Restyle a whole road in one gesture by dragging a style from the Library. A
faster alternative to editing lanes one at a time.*

![A road being restyled by dropping a Library style onto it in the
editor](img/working-with-road-styles.png)

## Before you start

Open a scene with a road you want to restyle — the road from
[Your first road network](first-road-network.md) is a good candidate. Open the
**Library** panel ([Library](../library.md)); the styles live under the **Road
styles** group.

## 1. Pick a style

A road style is a complete cross-section — lane count, types, widths, and lane
markings — packaged as a Library asset (for example, an urban two-lane road with
dashed centre lines). Browse the **Road styles** group and find one that matches
what you want the road to become.

## 2. Drop it on the road

Drag the style out of the Library and drop it onto the target road in the
viewport. The whole road takes on the new cross-section in a single undoable
step: its lanes are rebuilt to match the style and the road is flattened to one
lane section. See [Road Styles](../road-styles.md) for exactly what the apply
does and its limits (it refuses connecting roads inside a junction).

## 3. Adjust from there

The restyle is a starting point, not a lock. Once applied, you can still shape
individual lanes with the [Lane](../lane-profile.md) and
[Lane Width](../lane-width.md) tools, exactly as in
[Shaping lanes](shaping-lanes.md). Undo with <kbd>Ctrl</kbd>+<kbd>Z</kbd> if the
style is not what you wanted and try another.

## 4. Save

When the road looks right, **File ▸ Save** writes the updated network. The
markings and lane types from the style are part of the OpenDRIVE output.

## Where to go next

- [Shaping lanes](shaping-lanes.md) — fine-tune the cross-section the style gave
  you.
- [Getting around](getting-around.md) — frame the road to inspect the new
  markings up close.
