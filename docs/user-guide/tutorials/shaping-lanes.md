# Shaping lanes

*Take a plain road and shape its cross-section: add a lane, retype it, widen it
along the road, and drop in a pocket lane. Builds on
[Your first road network](first-road-network.md).*

![A road whose lane count and widths vary along its length in the
editor](img/shaping-lanes.png)

## Before you start

Open a scene with at least one multi-lane road — either the one you drew in the
[first tutorial](first-road-network.md) or a sample road. Select the road so the
lane tools have something to act on.

## 1. Add and retype a lane

Pick the **Lane** tool ([Lane](../lane-profile.md)). It shows the road's
cross-section; from there you can add a lane on either side of the centre,
remove one, change its type (driving, sidewalk, shoulder…), and set its travel
direction. Add one lane and give it a type — the change is a single undoable
command and the mesh updates immediately.

## 2. Vary the width along the road

A lane does not have to keep a constant width. Open the **Lane Width** editor
([Lane Width](../lane-width.md), <kbd>Shift</kbd>+<kbd>L</kbd>): it shows the
selected lane's width as a 2D curve along the road's length. Drag the curve to
taper the lane — narrow at one end, wider at the other — and watch the road
follow.

## 3. Drop in a pocket lane

For a short lane that starts and ends inside the road — a passing pocket or a
lay-by — use **Lane Add** ([Lane Add](../lane-add.md)). Click on the road to drop
a self-contained lane that opens and closes within the road, without touching the
lanes around it.

To grow a lane from a point all the way to the road's end (and have it link
across junction seams), use **Lane Form** ([Lane Form](../lane-form.md)) instead.
To carve a tapering turn lane on a junction approach, see
[Lane Carve](../lane-carve.md).

## 4. Confirm

Check the [Diagnostics](../diagnostics.md) panel — lane widths and links that
stray outside the standard show up here. When it is clean, **File ▸ Save** to
keep your work.

## Where to go next

- [Working with road styles](working-with-road-styles.md) — restyle the whole
  road at once instead of lane by lane.
